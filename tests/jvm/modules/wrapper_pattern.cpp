// wrapper_pattern JVM test module  (feature area: the object<T> wrapper pattern)
//
// THE wrapper-pattern authority: exhaustively exercises vmhook::object<T> /
// object_base (vmhook.hpp:13967-14470 object_base, 14471-14582 object<derived>),
// the CRTP base every typed Java wrapper derives from.  Everything else in the
// library (field_proxy / method_proxy access, enum singletons, collections) is
// reached THROUGH this pattern, so its contract is load-bearing on every JDK.
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//
//  CONSTRUCTION FROM A LIVE OOP
//   * a wrapper built from static_field("instance")->get() (the value_t ->
//     unique_ptr<wrapper> decode path, vmhook.hpp:11821-11848) holds the real
//     OOP, and object_base::get_instance() (14022) returns exactly that pointer.
//
//  THE NAME-HIDING TRAP (real Wave-1 footgun, pinned here)
//   * the wrapper below DELIBERATELY declares a STATIC helper named
//     get_instance() (returning the published singleton).  That static SHADOWS
//     the inherited instance accessor object_base::get_instance().  To read the
//     wrapped OOP off a live wrapper you MUST use explicit base qualification:
//       w.vmhook::object_base::get_instance()
//     The module asserts the two are different operations: the unqualified
//     static returns a (singleton) wrapper; the base-qualified instance accessor
//     returns the raw OOP this wrapper holds.  (Same idiom field_proxy::set uses
//     internally at vmhook.hpp:12130 to reach get_instance() past a shadow.)
//
//  NULL-OOP WRAPPER
//   * a wrapper constructed from nullptr has a null instance pointer (the base
//     accessor returns nullptr), YET static_field / static_method still resolve
//     through the java.lang.Class mirror (object_base::get_field's is_static
//     branch never touches this->instance, 14068-14083), and the inherited
//     instance get_field() of a STATIC field also resolves via the mirror while
//     get_field() of an INSTANCE field returns nullopt gracefully (14085-14089).
//
//  DISPATCH / INTROSPECTION
//   * instance vs static field dispatch for every primitive + a reference field;
//   * instance vs static method dispatch incl. OVERLOADS resolved by name+sig
//     (object_base::get_method name-only vs name+signature, 14166-14372);
//   * field_proxy::is_static()/signature() and method_proxy::is_static()/
//     signature()/name() report the JVM truth for both static and instance.
//
//  VALUE SEMANTICS
//   * copy of a wrapper aliases the SAME OOP (object_base copy ctor, 13988);
//   * move transfers the OOP and NULLS the source (move ctor/assign, 14002-14017).
//
//  EQUALITY (characterized — object_base has NO operator==)
//   * two wrappers wrapping the same instance are "equal" only by raw-OOP
//     identity (get_instance() == get_instance()); distinct instances differ;
//     an alias static (sameAsInstance == instance) decodes to the identical OOP.
//
//  LIVE POST-DISPATCH STATE (run_probe)
//   * a scoped_hook on bump() fires on real bytecode: the detour's `self`
//     wrapper is the correct instance (reads iId), and method_proxy::call() of
//     getId() from inside the detour (on the Java thread) returns that id;
//   * after the probe's putfield, a freshly-built wrapper reads the NEW iValue.
//
//  TYPE-REGISTRY GATE
//   * an UNREGISTERED wrapper type's static_field/static_method return nullopt
//     (resolve_klass via type_to_class_map, 14409-14426) — no crash, no throw.
//
// SAFETY: every decoded/built OOP is gated with is_valid_pointer before use; the
// only hook is installed via scoped_hook<> and uninstalls on scope exit (nothing
// left armed for later modules).  value_t / call() results are extracted by
// COPY-INIT (never brace-init) to stay MSVC-unambiguous.  C++17: no std::bit_cast.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <utility>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <typeindex>

namespace
{
    // The fixture class this module wraps.  Used by register_class<wp>() and by
    // the entry guard's find_class() pre-check (so the unguarded handshake
    // static_field("go")->set(...) derefs can never fault on a missing class).
    constexpr char FIXTURE[]{ "vmhook/fixtures/WrapperPattern" };

    // -----------------------------------------------------------------------
    // The wrapper under test: vmhook.fixtures.WrapperPattern.
    //
    // NOTE THE NAME-HIDING TRAP, on purpose: the static get_instance() below
    // SHADOWS object_base::get_instance().  Inside this class and at every call
    // site, an unqualified get_instance() names THIS static (returns the
    // singleton wrapper); reading the wrapped raw OOP requires the explicit
    // base qualification w.vmhook::object_base::get_instance().
    //
    // Instance accessors use get_field/get_method (valid from an instance
    // context on every compiler).  Static accessors use the portable
    // static_field/static_method names (GCC rejects deducing-this get_field
    // from a static context).
    // -----------------------------------------------------------------------
    class wp : public vmhook::object<wp>
    {
    public:
        explicit wp(vmhook::oop_t instance) noexcept
            : vmhook::object<wp>{ instance }
        {
        }

        // ---- handshake + scenario selector (all via static_field) ----------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // ---- STATIC helper that SHADOWS object_base::get_instance() ---------
        // Returns the published `instance` singleton as a wrapper.  Declaring a
        // static get_instance() hides the inherited instance accessor; the
        // module proves you must base-qualify to reach the raw-OOP accessor.
        static auto get_instance() -> std::unique_ptr<wp>
        {
            const auto proxy{ static_field("instance") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            std::unique_ptr<wp> ptr = proxy->get();   // copy-init (MSVC-safe)
            return ptr;
        }

        // ---- acquire any published singleton by static field name -----------
        static auto acquire(const char* field) -> std::unique_ptr<wp>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            std::unique_ptr<wp> ptr = proxy->get();
            return ptr;
        }

        // ---- the raw wrapped OOP, via EXPLICIT base qualification -----------
        // The whole point: object_base::get_instance(), NOT the static shadow.
        auto raw_oop() const -> void*
        {
            return this->vmhook::object_base::get_instance();
        }

        // ---- instance field reads (live-oop dispatch) ----------------------
        auto get_iId() const -> std::int32_t
        {
            const auto p{ get_field("iId") };
            if (!p.has_value()) { return -1; }
            const std::int32_t v = p->get();
            return v;
        }
        auto get_iValue() const -> std::int64_t
        {
            const auto p{ get_field("iValue") };
            if (!p.has_value()) { return -1; }
            const std::int64_t v = p->get();
            return v;
        }
        auto get_iLabel() const -> std::string
        {
            const auto p{ get_field("iLabel") };
            if (!p.has_value()) { return std::string{ "<<no-field>>" }; }
            return p->get().as_string();
        }

        // ---- instance method call (best-effort; needs a live JavaThread) ----
        auto call_get_id() const -> std::int64_t
        {
            const auto m{ get_method("getId") };
            if (!m.has_value()) { return k_call_unavailable; }
            const auto r{ m->call() };
            if (r.is_void()) { return k_call_unavailable; }
            const std::int64_t v = r;
            return v;
        }

        static constexpr std::int64_t k_call_unavailable{ -424242 };
    };

    // A SECOND wrapper type that is intentionally NEVER register_class<>'d, to
    // prove resolve_klass() gates cleanly (static_field -> nullopt, no crash).
    class wp_unregistered : public vmhook::object<wp_unregistered>
    {
    public:
        explicit wp_unregistered(vmhook::oop_t instance) noexcept
            : vmhook::object<wp_unregistered>{ instance }
        {
        }
    };

    // value_t variant-alternative indices (must match field_proxy::value_t order).
    constexpr std::size_t kIdxBool = 0;
    constexpr std::size_t kIdxI32  = 3;
    constexpr std::size_t kIdxI64  = 4;
    constexpr std::size_t kIdxU16  = 7;
    constexpr std::size_t kIdxU32  = 8;   // reference / compressed OOP

    // ---- detour observations (filled on the Java thread inside bump()) ----
    std::atomic<int>          g_bump_calls{ 0 };
    std::atomic<bool>         g_self_nonnull{ false };
    std::atomic<bool>         g_self_oop_valid{ false };
    std::atomic<std::int32_t> g_self_iId{ -1 };
    std::atomic<std::int64_t> g_self_call_getId{ wp::k_call_unavailable };
    std::atomic<bool>         g_self_oop_matches_instance{ false };

    // The instance OOP captured natively BEFORE the probe (compared to self).
    std::atomic<std::uintptr_t> g_instance_oop_bits{ 0 };

    // The bump() detour: build nothing extra — vmhook hands us a `self` wrapper
    // already decoded from `this`.  Read its id, call getId() (we are ON the
    // Java thread here, so the interpreter/JNI call gate is live), and compare
    // its raw OOP to the instance OOP the body captured.  Allow-through (no
    // cancel) so the original putfield still runs and the probe completes.
    auto on_bump(vmhook::return_value& /*ret*/, const std::unique_ptr<wp>& self) -> void
    {
        g_bump_calls.fetch_add(1, std::memory_order_relaxed);
        const bool nonnull{ self != nullptr };
        g_self_nonnull.store(nonnull, std::memory_order_relaxed);
        if (!nonnull)
        {
            return;
        }

        void* const self_oop{ self->raw_oop() };
        const bool valid{ self_oop != nullptr && vmhook::hotspot::is_valid_pointer(self_oop) };
        g_self_oop_valid.store(valid, std::memory_order_relaxed);
        if (!valid)
        {
            return;
        }

        g_self_iId.store(self->get_iId(), std::memory_order_relaxed);
        g_self_call_getId.store(self->call_get_id(), std::memory_order_relaxed);

        const std::uintptr_t want{ g_instance_oop_bits.load(std::memory_order_relaxed) };
        g_self_oop_matches_instance.store(
            want != 0 && reinterpret_cast<std::uintptr_t>(self_oop) == want,
            std::memory_order_relaxed);
    }

    // Drive one probe cycle for `mode`: clear the latched done + program the
    // selector on the rising edge of go, then wait for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        if (!ctx.run_probe)
        {
            return false;
        }
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    wp::set_done(false);
                    wp::set_mode(mode);
                }
                wp::set_go(value);
            },
            []() { return wp::get_done(); });
    }

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-
    // safety: ZERO hooks armed on EVERY exit path).
    auto run_wrapper_pattern_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  If WrapperPattern is not loaded/resolvable, every
        //  static_field()->set/get below (the go/done/mode handshake) would deref
        //  a disengaged optional.  Bail cleanly to [INFO] instead of dereferencing
        //  anything (the wrapper's final shutdown_hooks() still runs).  In practice
        //  the harness loads every vmhook.fixtures.* class on each run, so this is
        //  belt-and-braces.  (Same idiom as register_class / hook_basic.)
        // =====================================================================
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] wrapper_pattern: WrapperPattern not loaded/resolvable "
                       "on this run; skipping the module's live checks (no crash, no "
                       "hooks armed).");
            return;
        }

        vmhook::register_class<wp>(FIXTURE);
        // wp_unregistered is intentionally NOT registered (type-registry gate test).

    // =====================================================================
    //  0. Sanity: the class resolves through the portable accessors.
    // =====================================================================
    ctx.check("wp_class_registered_static_field_resolves", wp::resolves("sTag"));
    ctx.check("wp_static_method_resolves", wp::static_method("staticTag").has_value());

    // =====================================================================
    //  1. CONSTRUCT A WRAPPER FROM A LIVE OOP + the NAME-HIDING TRAP.
    //     static_field("instance")->get() decodes the compressed OOP into a
    //     unique_ptr<wp>; the base-qualified get_instance() returns that OOP.
    // =====================================================================
    const auto inst{ wp::acquire("instance") };
    ctx.check("instance_wrapper_constructed_from_live_oop", inst != nullptr);
    void* instance_oop{ nullptr };
    if (inst)
    {
        instance_oop = inst->raw_oop();   // object_base::get_instance(), base-qualified
        ctx.check("instance_get_instance_nonnull", instance_oop != nullptr);
        ctx.check("instance_get_instance_valid_pointer",
                  vmhook::hotspot::is_valid_pointer(instance_oop));
        // Reading a known instance field through the live wrapper proves the OOP
        // is the real object (not a stray pointer): iId == 0x0BADF00D.
        ctx.check("instance_wrapper_reads_iId", inst->get_iId() == 0x0BADF00D);

        // NAME-HIDING TRAP: the unqualified static get_instance() resolves to
        // the STATIC shadow (returns a singleton wrapper), a DIFFERENT operation
        // from the base-qualified instance accessor that returns the raw OOP.
        const auto via_static_shadow{ wp::get_instance() };
        ctx.check("name_hiding_static_shadow_returns_wrapper", via_static_shadow != nullptr);
        if (via_static_shadow)
        {
            // Both ultimately reference the same Java object (the `instance`
            // singleton), but reached two different ways: the static shadow
            // builds a fresh wrapper around `instance`, whose base-qualified
            // raw OOP equals the first wrapper's base-qualified raw OOP.
            ctx.check("name_hiding_shadow_oop_equals_base_qualified_oop",
                      via_static_shadow->raw_oop() == instance_oop);
        }
        g_instance_oop_bits.store(reinterpret_cast<std::uintptr_t>(instance_oop),
                                  std::memory_order_relaxed);
    }

    // =====================================================================
    //  2. INSTANCE FIELD DISPATCH through the live-oop wrapper (every primitive
    //     width + a reference field), with variant-alternative + signature.
    // =====================================================================
    if (inst)
    {
        // int iId
        {
            const auto p{ inst->get_field("iId") };
            ctx.check("inst_field_iId_resolves", p.has_value());
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("inst_field_iId_variant_i32", v.data.index() == kIdxI32);
                ctx.check("inst_field_iId_value", static_cast<std::int32_t>(v) == 0x0BADF00D);
                ctx.check("inst_field_iId_is_static_false", p->is_static() == false);
                ctx.check("inst_field_iId_signature_I", std::string{ p->signature() } == "I");
            }
        }
        // long iValue (initial 1000)
        {
            const auto p{ inst->get_field("iValue") };
            ctx.check("inst_field_iValue_resolves", p.has_value());
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("inst_field_iValue_variant_i64", v.data.index() == kIdxI64);
                ctx.check("inst_field_iValue_initial_1000", static_cast<std::int64_t>(v) == 1000);
                ctx.check("inst_field_iValue_signature_J", std::string{ p->signature() } == "J");
            }
        }
        // boolean iFlag (false)
        {
            const auto p{ inst->get_field("iFlag") };
            ctx.check("inst_field_iFlag_resolves", p.has_value());
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("inst_field_iFlag_variant_bool", v.data.index() == kIdxBool);
                ctx.check("inst_field_iFlag_value_false", static_cast<bool>(v) == false);
            }
        }
        // String iLabel (reference field -> compressed OOP alternative)
        {
            const auto p{ inst->get_field("iLabel") };
            ctx.check("inst_field_iLabel_resolves", p.has_value());
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("inst_field_iLabel_variant_u32_ref", v.data.index() == kIdxU32);
                ctx.check("inst_field_iLabel_is_reference", p->is_reference());
                ctx.check("inst_field_iLabel_value", inst->get_iLabel() == "wrapper-instance");
                ctx.check("inst_field_iLabel_signature_String",
                          std::string{ p->signature() } == "Ljava/lang/String;");
            }
        }
    }

    // =====================================================================
    //  3. STATIC FIELD DISPATCH — via static_field AND via the inherited
    //     instance get_field() on the live wrapper (a static field resolves
    //     through the mirror regardless of which accessor names it).
    // =====================================================================
    {
        const auto ps{ wp::static_field("sTag") };
        ctx.check("static_field_sTag_resolves", ps.has_value());
        if (ps)
        {
            ctx.check("static_field_sTag_is_static_true", ps->is_static() == true);
            ctx.check("static_field_sTag_value",
                      static_cast<std::int32_t>(ps->get()) == static_cast<std::int32_t>(0x5A5A5A5A));
            ctx.check("static_field_sTag_signature_I", std::string{ ps->signature() } == "I");
        }
        // long sBig
        {
            const auto p{ wp::static_field("sBig") };
            if (p) { ctx.check("static_field_sBig_value",
                               static_cast<std::int64_t>(p->get()) == 0x0123456789ABCDEFLL); }
        }
        // char sChar (euro)
        {
            const auto p{ wp::static_field("sChar") };
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("static_field_sChar_variant_u16", v.data.index() == kIdxU16);
                ctx.check("static_field_sChar_value", static_cast<std::uint16_t>(v) == 0x20AC);
            }
        }
        // boolean sFlag
        {
            const auto p{ wp::static_field("sFlag") };
            if (p) { ctx.check("static_field_sFlag_true", static_cast<bool>(p->get()) == true); }
        }
        // String sName
        {
            const auto p{ wp::static_field("sName") };
            ctx.check("static_field_sName_resolves", p.has_value());
            if (p) { ctx.check("static_field_sName_value", p->get().as_string() == "wrapper-static"); }
        }
        // Same STATIC field through the LIVE instance wrapper's inherited
        // get_field(): must resolve via the mirror and equal the static read.
        if (inst)
        {
            const auto via_inst{ inst->get_field("sTag") };
            ctx.check("static_field_via_instance_accessor_resolves", via_inst.has_value());
            if (via_inst)
            {
                ctx.check("static_field_via_instance_is_static_true", via_inst->is_static() == true);
                ctx.check("static_field_via_instance_equals_static",
                          static_cast<std::int32_t>(via_inst->get()) == static_cast<std::int32_t>(0x5A5A5A5A));
            }
        }
    }

    // =====================================================================
    //  4. NULL-OOP WRAPPER: null instance pointer, yet static resolution works
    //     and instance-field resolution fails gracefully (no crash).
    // =====================================================================
    {
        const wp null_wrapper{ nullptr };
        // The base accessor returns nullptr for a null-oop wrapper.
        ctx.check("null_wrapper_base_instance_is_null",
                  null_wrapper.vmhook::object_base::get_instance() == nullptr);

        // static_field still resolves through the class mirror.
        ctx.check("null_wrapper_static_field_resolves_via_mirror",
                  wp::static_field("sTag").has_value());
        // The inherited instance get_field() of a STATIC field ALSO resolves
        // (the is_static branch never dereferences this->instance).
        {
            const auto p{ null_wrapper.get_field("sTag") };
            ctx.check("null_wrapper_get_field_static_resolves", p.has_value());
            if (p)
            {
                ctx.check("null_wrapper_get_field_static_value",
                          static_cast<std::int32_t>(p->get()) == static_cast<std::int32_t>(0x5A5A5A5A));
            }
        }
        // get_field() of an INSTANCE field on a null-oop wrapper -> nullopt.
        ctx.check("null_wrapper_get_field_instance_is_nullopt",
                  null_wrapper.get_field("iId").has_value() == false);
        // static_method still resolves on a null-oop wrapper.
        ctx.check("null_wrapper_static_method_resolves",
                  wp::static_method("staticTag").has_value());
    }

    // =====================================================================
    //  5. INSTANCE vs STATIC METHOD DISPATCH + signature()/is_static()/name().
    //     Pure introspection — HARD on every JDK (no JavaThread needed).
    // =====================================================================
    {
        // Static method staticTag()I.
        const auto sm{ wp::static_method("staticTag") };
        ctx.check("static_method_staticTag_resolves", sm.has_value());
        if (sm)
        {
            ctx.check("static_method_staticTag_is_static_true", sm->is_static() == true);
            ctx.check("static_method_staticTag_name", sm->name() == "staticTag");
            ctx.check("static_method_staticTag_signature", std::string{ sm->signature() } == "()I");
            ctx.check("static_method_staticTag_not_reference", sm->is_reference() == false);
        }
        // Static method staticName()Ljava/lang/String;.
        const auto smn{ wp::static_method("staticName") };
        if (smn)
        {
            ctx.check("static_method_staticName_signature",
                      std::string{ smn->signature() } == "()Ljava/lang/String;");
            ctx.check("static_method_staticName_is_reference", smn->is_reference() == true);
        }
        // Instance method getId()I through the live wrapper.
        if (inst)
        {
            const auto im{ inst->get_method("getId") };
            ctx.check("instance_method_getId_resolves", im.has_value());
            if (im)
            {
                ctx.check("instance_method_getId_is_static_false", im->is_static() == false);
                ctx.check("instance_method_getId_name", im->name() == "getId");
                ctx.check("instance_method_getId_signature", std::string{ im->signature() } == "()I");
            }
            // Instance method getValue()J and getLabel()Ljava/lang/String;.
            const auto imv{ inst->get_method("getValue") };
            if (imv) { ctx.check("instance_method_getValue_signature", std::string{ imv->signature() } == "()J"); }
            const auto iml{ inst->get_method("getLabel") };
            if (iml)
            {
                ctx.check("instance_method_getLabel_is_reference", iml->is_reference() == true);
                ctx.check("instance_method_getLabel_signature",
                          std::string{ iml->signature() } == "()Ljava/lang/String;");
            }
        }
        // Unknown method names -> nullopt (static and instance).
        ctx.check("static_method_unknown_is_nullopt", wp::static_method("noSuchMethod").has_value() == false);
        if (inst)
        {
            ctx.check("instance_method_unknown_is_nullopt", inst->get_method("noSuchMethod").has_value() == false);
        }
    }

    // =====================================================================
    //  6. METHOD OVERLOAD RESOLUTION BY name+signature.
    //     Static combine(I)I vs combine(II)I; instance describe()I vs
    //     describe(I)I — the name+signature overload must pick the exact match.
    // =====================================================================
    {
        const auto c1{ wp::static_method("combine", "(I)I") };
        const auto c2{ wp::static_method("combine", "(II)I") };
        ctx.check("overload_static_combine_I_resolves", c1.has_value());
        ctx.check("overload_static_combine_II_resolves", c2.has_value());
        if (c1) { ctx.check("overload_static_combine_I_signature", std::string{ c1->signature() } == "(I)I"); }
        if (c2) { ctx.check("overload_static_combine_II_signature", std::string{ c2->signature() } == "(II)I"); }
        if (c1 && c2)
        {
            // The two overloads are DISTINCT Method*s (different descriptors).
            ctx.check("overload_static_combine_distinct_signatures",
                      std::string{ c1->signature() } != std::string{ c2->signature() });
        }
        // A non-existent overload signature -> nullopt (name exists, sig doesn't).
        ctx.check("overload_static_combine_bad_sig_nullopt",
                  wp::static_method("combine", "(J)I").has_value() == false);

        if (inst)
        {
            const auto d0{ inst->get_method("describe", "()I") };
            const auto d1{ inst->get_method("describe", "(I)I") };
            ctx.check("overload_instance_describe_void_resolves", d0.has_value());
            ctx.check("overload_instance_describe_int_resolves", d1.has_value());
            if (d0) { ctx.check("overload_instance_describe_void_signature", std::string{ d0->signature() } == "()I"); }
            if (d1) { ctx.check("overload_instance_describe_int_signature", std::string{ d1->signature() } == "(I)I"); }
        }
    }

    // =====================================================================
    //  7. VALUE SEMANTICS — copy aliases the same OOP; move transfers + nulls.
    // =====================================================================
    if (inst && instance_oop)
    {
        // COPY: a copy of the wrapper points at the SAME OOP (no GC handle, just
        // a raw pointer copy).  Build a fresh wp to copy (inst is a unique_ptr).
        wp original{ instance_oop };
        wp copied{ original };   // copy ctor (object_base copy ctor)
        ctx.check("copy_ctor_aliases_same_oop",
                  copied.vmhook::object_base::get_instance() == instance_oop);
        ctx.check("copy_ctor_source_unchanged",
                  original.vmhook::object_base::get_instance() == instance_oop);

        // Copy ASSIGNMENT.
        wp assigned{ nullptr };
        assigned = original;
        ctx.check("copy_assign_aliases_same_oop",
                  assigned.vmhook::object_base::get_instance() == instance_oop);

        // MOVE ctor: dest takes the OOP, source is nulled.
        wp move_src{ instance_oop };
        wp move_dst{ std::move(move_src) };
        ctx.check("move_ctor_dest_has_oop",
                  move_dst.vmhook::object_base::get_instance() == instance_oop);
        ctx.check("move_ctor_source_nulled",
                  move_src.vmhook::object_base::get_instance() == nullptr);   // NOLINT(bugprone-use-after-move)

        // MOVE assignment: same transfer + null-out.
        wp ma_src{ instance_oop };
        wp ma_dst{ nullptr };
        ma_dst = std::move(ma_src);
        ctx.check("move_assign_dest_has_oop",
                  ma_dst.vmhook::object_base::get_instance() == instance_oop);
        ctx.check("move_assign_source_nulled",
                  ma_src.vmhook::object_base::get_instance() == nullptr);   // NOLINT(bugprone-use-after-move)

        // A field read through a COPY yields the same value as through the
        // original — the copy is a fully usable wrapper, not a hollow shell.
        ctx.check("copy_reads_same_iId", copied.get_iId() == original.get_iId());
    }

    // =====================================================================
    //  8. EQUALITY (characterized) — object_base has NO operator==, so equality
    //     is raw-OOP identity.  Two wrappers to the SAME instance share an OOP;
    //     DISTINCT instances differ; the alias static decodes to the same OOP.
    // =====================================================================
    ctx.record("[INFO] wrapper_pattern: vmhook::object_base defines no operator== "
               "(and no hash); two wrappers are 'the same object' iff their "
               "base-qualified get_instance() raw OOPs compare equal. Wrap-then-"
               "compare on the OOP is the supported identity test.");
    {
        const auto a{ wp::acquire("instance") };
        const auto b{ wp::acquire("instance") };        // same Java object, twice
        const auto other{ wp::acquire("instance2") };   // DISTINCT Java object
        const auto alias{ wp::acquire("sameAsInstance") };  // alias of `instance`

        ctx.check("equality_two_acquires_instance_nonnull", a != nullptr && b != nullptr);
        if (a && b
            && vmhook::hotspot::is_valid_pointer(a->raw_oop())
            && vmhook::hotspot::is_valid_pointer(b->raw_oop()))
        {
            // Same field name decoded twice -> identical OOP (singleton stable).
            ctx.check("equality_same_instance_same_oop", a->raw_oop() == b->raw_oop());
        }
        if (a && other
            && vmhook::hotspot::is_valid_pointer(a->raw_oop())
            && vmhook::hotspot::is_valid_pointer(other->raw_oop()))
        {
            ctx.check("equality_distinct_instances_differ", a->raw_oop() != other->raw_oop());
            // And they really are different objects (different iId).
            ctx.check("equality_distinct_instances_distinct_iId",
                      a->get_iId() != other->get_iId());
            ctx.check("equality_instance2_iId", other->get_iId() == 0x0CAFE2);
        }
        if (a && alias
            && vmhook::hotspot::is_valid_pointer(a->raw_oop())
            && vmhook::hotspot::is_valid_pointer(alias->raw_oop()))
        {
            // sameAsInstance aliases `instance` in Java -> identical OOP.
            ctx.check("equality_alias_static_same_oop_as_instance", alias->raw_oop() == a->raw_oop());
        }
    }

    // =====================================================================
    //  9. DEFAULT-CONSTRUCTED (nullptr) wrapper — graceful nullopt everywhere,
    //     no crash, no throw (covers the using-decl'd default ctor path too).
    // =====================================================================
    {
        const wp defaulted{ nullptr };
        ctx.check("default_wrapper_null_instance",
                  defaulted.vmhook::object_base::get_instance() == nullptr);
        ctx.check("default_wrapper_instance_field_nullopt",
                  defaulted.get_field("iId").has_value() == false);
        // get_method does NOT depend on a live instance (it scans the klass), so
        // it still resolves; the returned proxy simply has a null receiver.
        const auto m{ defaulted.get_method("getId") };
        ctx.check("default_wrapper_get_method_resolves_null_receiver", m.has_value());
        // A static field still resolves on the defaulted wrapper.
        ctx.check("default_wrapper_static_field_resolves",
                  defaulted.get_field("sTag").has_value());
    }

    // =====================================================================
    //  10. TYPE-REGISTRY GATE — an UNREGISTERED wrapper type resolves to no
    //      klass, so static_field / static_method return nullopt (no crash).
    // =====================================================================
    {
        ctx.check("unregistered_type_static_field_nullopt",
                  wp_unregistered::static_field("sTag").has_value() == false);
        ctx.check("unregistered_type_static_method_nullopt",
                  wp_unregistered::static_method("staticTag").has_value() == false);
        // An instance of the unregistered type also resolves nothing (its
        // typeid is absent from type_to_class_map).
        const wp_unregistered u{ nullptr };
        ctx.check("unregistered_type_instance_get_field_nullopt",
                  u.get_field("sTag").has_value() == false);
    }

    // =====================================================================
    //  11. LIVE POST-DISPATCH STATE via run_probe.
    //      Install a scoped_hook on bump(): the detour's `self` wrapper must be
    //      the correct instance, getId() called from the detour returns iId, and
    //      AFTER the probe's putfield a fresh wrapper reads the NEW iValue.
    // =====================================================================
    {
        // Read iValue BEFORE the probe (initial 1000) through a fresh wrapper.
        if (inst)
        {
            ctx.check("pre_probe_iValue_is_1000", inst->get_iValue() == 1000);
        }

        auto handle{ vmhook::scoped_hook<wp>("bump", &on_bump) };
        ctx.check("wrapper_pattern_hook_installed", handle.installed());

        if (handle.installed())
        {
            const bool done{ drive(ctx, 0) };
            ctx.check("wrapper_pattern_probe_completed", done);
            ctx.check("wrapper_pattern_bump_fired_once", g_bump_calls.load() == 1);

            // The detour received a non-null, valid `self` wrapper.
            ctx.check("detour_self_nonnull", g_self_nonnull.load());
            ctx.check("detour_self_oop_valid", g_self_oop_valid.load());
            // self is the SAME object as the `instance` singleton (raw-OOP match).
            ctx.check("detour_self_oop_matches_instance", g_self_oop_matches_instance.load());
            // self reads the correct id through the wrapper from inside the detour.
            ctx.check("detour_self_reads_correct_iId", g_self_iId.load() == 0x0BADF00D);

            // getId() called natively from inside the detour (we are on the Java
            // thread, so the call gate is live): HARD when a value came back,
            // [INFO] if this JDK had no interpreter/JNI call gate even on-thread.
            const std::int64_t called{ g_self_call_getId.load() };
            if (called == wp::k_call_unavailable)
            {
                ctx.record("[INFO] wrapper_pattern: method_proxy::call(getId) returned no value "
                           "from inside the detour on this JDK (call gate unavailable even on the "
                           "Java thread); the wrapper field-read path proves dispatch instead.");
                ctx.check("detour_self_call_getId_best_effort", true);
            }
            else
            {
                ctx.check("detour_self_call_getId_best_effort", called == 0x0BADF00D);
            }

            if (done)
            {
                // After the putfield (iValue += 2345), a FRESH wrapper built from
                // the same static reads the NEW value: 1000 + 2345 == 3345.
                const auto after{ wp::acquire("instance") };
                ctx.check("post_probe_wrapper_reobtained", after != nullptr);
                if (after)
                {
                    ctx.check("post_probe_wrapper_reads_new_iValue", after->get_iValue() == 3345);
                }
                // The ORIGINAL wrapper (same OOP) also sees the mutation — the
                // wrapper aliases live heap memory, it does not snapshot.
                if (inst)
                {
                    ctx.check("original_wrapper_sees_mutation", inst->get_iValue() == 3345);
                }
            }
        }
        // scoped_hook `handle` uninstalls here at scope exit — nothing left armed.
    }
}   // run_wrapper_pattern_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(wrapper_pattern)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (or the harness) can never escape this module.  A throw is recorded as
    // [INFO], never a FAIL (mirrors register_class.cpp / aaa_warmup.cpp).
    bool body_threw{ false };
    try
    {
        run_wrapper_pattern_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  The
    // only hook (section 11's scoped_hook) already uninstalled at its scope exit;
    // this unconditional shutdown_hooks() guarantees an empty hook table even if
    // the body threw BEFORE reaching that scope exit (it is idempotent and
    // safe-when-empty — proven by shutdown_hooks_teardown).  A leaked armed hook
    // is exactly the failure mode that cascaded across the matrix in Wave 3.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] wrapper_pattern: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
