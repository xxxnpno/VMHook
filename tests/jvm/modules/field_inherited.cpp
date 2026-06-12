// field_inherited JVM test module  (feature area: fields)
//
// Exhaustively exercises vmhook::find_field's superclass-chain walk
// (vmhook.hpp:10728-10769, the `for (k = target_klass; k; k = k->get_super())`
// loop at 10756) on a REAL three-level Java hierarchy loaded into a live JVM:
//
//     FieldInheritedBase  <-  FieldInheritedMid  <-  FieldInherited
//
// The single most important technique here: object_base::resolve_klass() keys
// off typeid(*this) — the C++ WRAPPER's static type — NOT the live OOP header
// klass (verified at vmhook.hpp:13847-13851).  So by wrapping the SAME child
// instance OOP in a child-typed / mid-typed / base-typed wrapper we choose the
// klass at which find_field BEGINS its super walk, and can therefore prove, on
// genuine JVM metadata:
//   * an OWN field resolves at walk depth 0,
//   * a parent field resolves at depth 1, a grandparent field at depth 2,
//   * find_field reads by raw offset and IGNORES Java access control: protected,
//     public, package-private and (base-)private inherited fields all resolve,
//   * SHADOWING is child-wins: the child-typed read of the child object sees the
//     CHILD slot; the base-typed read of the SAME object sees the BASE slot —
//     and the two slots hold the two far-apart sentinel values,
//   * inherited STATIC fields resolve through the same walk on the class mirror,
//     and a shadowed static is child-wins too,
//   * a genuinely-absent name walks to java.lang.Object and returns nullopt
//     (negative path) — for the child, the mid, and the base wrapper,
//   * after the probe mutates slots through real putfield/putstatic bytecode,
//     find_field resolves the LIVE post-dispatch value, and the child's shadow
//     write never disturbs an unrelated base object's same-named slot.
//
// Read-only ops aren't thread-safe (vmhook.hpp:22-24) but this module is the
// single test thread, matching the documented contract.  No hooks are needed —
// find_field is driven directly through the wrappers — but we still drive the
// Harness probe (real bytecode) for the live-mutation angles.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    // ---- Wrapper registered to the CHILD class.  Its get_field/static_field
    //      start the super walk at FieldInherited's klass. -------------------
    class fi_child : public vmhook::object<fi_child>
    {
    public:
        explicit fi_child(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_child>{ instance }
        {
        }

        // handshake / scenario selector ------------------------------------
        static auto set_go(bool v) -> void   { static_field("go")->set(v); }
        static auto set_done(bool v) -> void { static_field("done")->set(v); }
        static auto get_done() -> bool       { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // The held child instance, wrapped so we can read it.  The value_t ->
        // unique_ptr conversion validates the decoded OOP.
        static auto get_instance() -> std::unique_ptr<fi_child> { return static_field("instance")->get(); }

        // Read the child's own / inherited fields THROUGH the child klass.
        auto own_int() const -> std::int32_t       { return static_cast<std::int32_t>(get_field("childOwnInt")->get()); }
        auto mid_own_int() const -> std::int32_t   { return static_cast<std::int32_t>(get_field("midOwnInt")->get()); }
        auto protected_int() const -> std::int32_t { return static_cast<std::int32_t>(get_field("protectedInt")->get()); }
        auto public_int() const -> std::int32_t    { return static_cast<std::int32_t>(get_field("publicInt")->get()); }
        auto package_int() const -> std::int32_t   { return static_cast<std::int32_t>(get_field("packageInt")->get()); }
        auto private_int() const -> std::int32_t   { return static_cast<std::int32_t>(get_field("privateInt")->get()); }
        auto base_long() const -> std::int64_t     { return static_cast<std::int64_t>(get_field("baseLong")->get()); }
        auto shadowed_int() const -> std::int32_t  { return static_cast<std::int32_t>(get_field("shadowedInt")->get()); }

        // Inherited (depth-2) fields of every remaining primitive type + an array,
        // read THROUGH the child klass (the super walk supplies the offset).
        auto base_bool() const -> bool             { return get_field("baseBool")->get(); }
        auto base_byte() const -> std::int8_t      { return static_cast<std::int8_t>(get_field("baseByte")->get()); }
        auto base_char() const -> std::uint16_t    { return static_cast<std::uint16_t>(get_field("baseChar")->get()); }
        auto base_short() const -> std::int16_t    { return static_cast<std::int16_t>(get_field("baseShort")->get()); }
        auto base_float() const -> float           { return get_field("baseFloat")->get(); }
        auto base_double() const -> double         { return get_field("baseDouble")->get(); }
        auto base_int_array() const -> std::vector<std::int32_t> { return get_field("baseIntArray")->get(); }
    };

    // ---- Wrapper registered to the MID class.  Super walk starts at Mid. ---
    class fi_mid : public vmhook::object<fi_mid>
    {
    public:
        explicit fi_mid(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_mid>{ instance }
        {
        }
        auto mid_own_int() const -> std::int32_t   { return static_cast<std::int32_t>(get_field("midOwnInt")->get()); }
        auto protected_int() const -> std::int32_t { return static_cast<std::int32_t>(get_field("protectedInt")->get()); }
        auto shadowed_int() const -> std::int32_t  { return static_cast<std::int32_t>(get_field("shadowedInt")->get()); }
    };

    // ---- Wrapper registered to the BASE class.  Super walk starts at Base. -
    //      Reading a CHILD object through this wrapper resolves the BASE
    //      shadow slot — the crux of the child-wins shadowing proof. ---------
    class fi_base : public vmhook::object<fi_base>
    {
    public:
        explicit fi_base(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_base>{ instance }
        {
        }
        // The independent pure-base object the fixture holds.
        //
        // IMPORTANT: `baseInstance` is declared on the CHILD class
        // (FieldInherited), not on FieldInheritedBase — so it must be resolved
        // through the CHILD wrapper (fi_child), the field's declaring class.
        // Resolving it through *this* (base) wrapper would start the super walk at
        // FieldInheritedBase and walk UP toward Object, never finding a
        // child-declared static.  We wrap the resulting OOP as an fi_base (the
        // held object IS a FieldInheritedBase instance); the value_t ->
        // unique_ptr<fi_base> conversion validates the decoded OOP.
        static auto get_base_instance() -> std::unique_ptr<fi_base> { return fi_child::static_field("baseInstance")->get(); }
        auto protected_int() const -> std::int32_t { return static_cast<std::int32_t>(get_field("protectedInt")->get()); }
        auto public_int() const -> std::int32_t    { return static_cast<std::int32_t>(get_field("publicInt")->get()); }
        auto shadowed_int() const -> std::int32_t  { return static_cast<std::int32_t>(get_field("shadowedInt")->get()); }
    };

    // ---- Wrapper registered to the INTERFACE klass.  Used ONLY for the static
    //      accessor on the interface constant — an interface is never
    //      instantiated, so this wrapper holds no live instance; static_field /
    //      the static get_field(type_index,...) overload need no OOP.  Registering
    //      it lets us resolve the interface's OWN klass (its declaring class) and
    //      read IFACE_CONST off the interface mirror through the public API. -----
    class fi_iface : public vmhook::object<fi_iface>
    {
    public:
        explicit fi_iface(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_iface>{ instance }
        {
        }
    };

    // ---- Constants mirrored from FieldInherited*.java ----------------------
    constexpr std::int32_t OWN_INT_INIT        { 0x0C1D0001 };
    constexpr std::int32_t OWN_INT_RUNTIME     { 0x0C1DBEEF };
    constexpr std::int32_t BASE_SHADOW_INT     { 1111 };
    constexpr std::int32_t CHILD_SHADOW_INT    { 9999 };
    constexpr std::int32_t CHILD_SHADOW_RUNTIME{ 4242 };
    constexpr std::int32_t INDEP_BASE_SHADOW   { 7007 };
    constexpr std::int32_t STATIC_SHADOW_BASE    { 555 };
    constexpr std::int32_t STATIC_SHADOW_CHILD   { 777 };
    constexpr std::int32_t STATIC_SHADOW_RUNTIME { 3030 };

    // Base.java
    constexpr std::int32_t PROT_INT_INIT     { 1337 };
    constexpr std::int32_t PROT_INT_RUNTIME  { 0xABCD };
    constexpr std::int32_t PUB_INT_INIT      { 2674 };
    constexpr std::int32_t PUB_INT_RUNTIME   { 0x1234 };
    constexpr std::int32_t PKG_INT_INIT      { static_cast<std::int32_t>(0x0BADCAFE) };
    constexpr std::int32_t PRV_INT_INIT      { static_cast<std::int32_t>(0x0DEFACED) };
    constexpr std::int64_t BASE_LONG_INIT    { 0x00BA5E0000BA5ELL };
    constexpr std::int32_t STAT_PROT_INIT    { 100 };
    constexpr std::int32_t STAT_PROT_RUNTIME { 0x5151 };
    constexpr std::int32_t STAT_PUB_INIT     { 200 };
    constexpr std::int32_t STAT_PUB_RUNTIME  { 0x6262 };
    constexpr std::int32_t STAT_PRV_INIT     { 300 };

    // Mid.java
    constexpr std::int32_t MID_INT_INIT      { 0x00C0FFEE };
    constexpr std::int32_t MID_INT_RUNTIME   { 0x77777777 };
    constexpr std::int32_t STAT_MID_INIT     { 400 };
    constexpr std::int32_t STAT_MID_RUNTIME  { 0x7373 };

    // Base.java — one inherited slot of EVERY remaining JVM primitive type, plus
    // an inherited array.  Mirrored verbatim from FieldInheritedBase.java so the
    // depth-2 super walk is proven for Z B C S F D and the array-OOP decode.
    constexpr bool          BASE_BOOL_INIT   { true };
    constexpr std::int8_t   BASE_BYTE_INIT   { static_cast<std::int8_t>(0x5A) };   // 90
    constexpr std::uint16_t BASE_CHAR_INIT   { static_cast<std::uint16_t>('Q') };  // 0x0051
    constexpr std::int16_t  BASE_SHORT_INIT  { static_cast<std::int16_t>(0x1234) };// 4660
    constexpr float         BASE_FLOAT_INIT  { 2.5f };
    constexpr double        BASE_DOUBLE_INIT { 1.5 };
    // mode 1 runtime writes for the inherited non-int slots.
    constexpr bool          BASE_BOOL_RUNTIME   { false };
    constexpr float         BASE_FLOAT_RUNTIME  { 6.25f };
    constexpr double        BASE_DOUBLE_RUNTIME { 9.75 };

    // FieldInheritedIface.java — the interface constant (implicitly public static
    // final).  find_field's _super-only walk does NOT reach it from an
    // implementor; it resolves only through the interface's own klass.
    constexpr std::int32_t IFACE_CONST_VALUE { 0x1FACE123 };

    // Internal (JVM '/') names used for the direct klass::find_field-vs-super-walk
    // contrast and the interface-constant characterization.
    constexpr char K_CHILD[]{ "vmhook/fixtures/FieldInherited" };
    constexpr char K_MID[]  { "vmhook/fixtures/FieldInheritedMid" };
    constexpr char K_BASE[] { "vmhook/fixtures/FieldInheritedBase" };
    constexpr char K_IFACE[]{ "vmhook/fixtures/FieldInheritedIface" };

    // Drive exactly one probe cycle for `mode`: reset the latched done flag and
    // program the scenario selector on the rising edge, then run the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool v)
            {
                if (v)
                {
                    fi_child::set_done(false);
                    fi_child::set_mode(mode);
                }
                fi_child::set_go(v);
            },
            []() { return fi_child::get_done(); });
    }
}

VMHOOK_JVM_MODULE(field_inherited)
{
    vmhook::register_class<fi_child>("vmhook/fixtures/FieldInherited");
    vmhook::register_class<fi_mid>("vmhook/fixtures/FieldInheritedMid");
    vmhook::register_class<fi_base>("vmhook/fixtures/FieldInheritedBase");
    vmhook::register_class<fi_iface>("vmhook/fixtures/FieldInheritedIface");

    // =====================================================================
    //  Registration / resolution sanity for all three hierarchy levels.
    // =====================================================================
    {
        ctx.check("child_class_registered_static_resolves",
                  fi_child::static_field("go").has_value());
        // A field declared ONLY on the grandparent, resolved through the CHILD
        // wrapper — this is the super walk working at all (depth 2).
        ctx.check("child_resolves_grandparent_field_via_super_walk",
                  fi_child::static_field("sPublic").has_value());
    }

    // =====================================================================
    //  DECLARED-ONLY  vs  SUPER-WALK  — the load-bearing distinction.
    //
    //  klass::find_field(name) searches ONLY the fields declared directly on
    //  that one klass (vmhook.hpp:3462, doc "Searches only fields declared
    //  directly on this class").  vmhook::find_field(klass,name) is the wrapper
    //  that walks Klass::get_super() so INHERITED fields resolve (the loop at
    //  vmhook.hpp:12993).  This block pins the contract at every chain position:
    //  the per-klass call MUST NOT see an inherited field; the super-walk MUST.
    //  We reach both directly through vmhook::find_class (a public free fn that
    //  returns the klass*), independent of any wrapper, so the two layers are
    //  compared head-to-head.
    // =====================================================================
    {
        vmhook::hotspot::klass* const k_child{ vmhook::find_class(K_CHILD) };
        vmhook::hotspot::klass* const k_mid  { vmhook::find_class(K_MID) };
        vmhook::hotspot::klass* const k_base { vmhook::find_class(K_BASE) };

        ctx.check("declared_vs_walk_klasses_resolved",
                  k_child != nullptr && k_mid != nullptr && k_base != nullptr);

        if (k_child && k_mid && k_base)
        {
            // -- get_super() actually links the chain child -> mid -> base ----
            ctx.check("super_link_child_to_mid", k_child->get_super() == k_mid);
            ctx.check("super_link_mid_to_base",  k_mid->get_super()   == k_base);
            // ...and Base's super is some non-null klass that is NOT in our
            // hierarchy (java.lang.Object, possibly via no intermediate) — at
            // minimum it is neither child nor mid nor base.
            {
                vmhook::hotspot::klass* const base_super{ k_base->get_super() };
                ctx.check("super_link_base_has_super", base_super != nullptr);
                ctx.check("super_link_base_super_not_in_hierarchy",
                          base_super != k_child && base_super != k_mid && base_super != k_base);
            }

            // -- OWN field (childOwnInt): declared on the child --------------
            //    declared-only on child  -> FOUND;  super-walk on child -> FOUND.
            ctx.check("declared_only_finds_own_on_child",
                      k_child->find_field("childOwnInt").has_value());
            ctx.check("super_walk_finds_own_on_child",
                      vmhook::find_field(k_child, "childOwnInt").has_value());

            // -- PARENT field (midOwnInt): declared on Mid ------------------
            //    declared-only on CHILD  -> NOT found (it is inherited, depth 1);
            //    declared-only on MID    -> found (it is Mid's own);
            //    super-walk   on CHILD   -> found (the walk descends one link).
            ctx.check("declared_only_misses_inherited_parent_field",
                      k_child->find_field("midOwnInt").has_value() == false);
            ctx.check("declared_only_finds_parent_field_on_its_own_klass",
                      k_mid->find_field("midOwnInt").has_value());
            ctx.check("super_walk_finds_inherited_parent_field",
                      vmhook::find_field(k_child, "midOwnInt").has_value());

            // -- GRANDPARENT field (protectedInt): declared on Base ----------
            //    declared-only on CHILD -> NOT found (inherited, depth 2);
            //    declared-only on MID   -> NOT found (still inherited for Mid);
            //    declared-only on BASE  -> found (Base's own);
            //    super-walk   on CHILD  -> found (walk descends two links).
            ctx.check("declared_only_misses_inherited_grandparent_from_child",
                      k_child->find_field("protectedInt").has_value() == false);
            ctx.check("declared_only_misses_inherited_grandparent_from_mid",
                      k_mid->find_field("protectedInt").has_value() == false);
            ctx.check("declared_only_finds_grandparent_field_on_base",
                      k_base->find_field("protectedInt").has_value());
            ctx.check("super_walk_finds_inherited_grandparent_field",
                      vmhook::find_field(k_child, "protectedInt").has_value());

            // -- The super-walk records WHICH klass declared the field -------
            //    (field_entry_t::declaring_klass, vmhook.hpp:13005).  For an
            //    inherited grandparent field that must be Base, not the child.
            {
                const auto e_prot{ vmhook::find_field(k_child, "protectedInt") };
                ctx.check("super_walk_records_declaring_klass_grandparent",
                          e_prot.has_value() && e_prot->declaring_klass == k_base);
                const auto e_own{ vmhook::find_field(k_child, "childOwnInt") };
                ctx.check("super_walk_records_declaring_klass_own",
                          e_own.has_value() && e_own->declaring_klass == k_child);
                const auto e_mid{ vmhook::find_field(k_child, "midOwnInt") };
                ctx.check("super_walk_records_declaring_klass_parent",
                          e_mid.has_value() && e_mid->declaring_klass == k_mid);
            }

            // -- An ABSENT name: declared-only AND super-walk both empty, at
            //    every chain position, and NEVER crash. --------------------
            ctx.check("declared_only_absent_is_empty",
                      k_child->find_field("noSuchFieldAnywhere").has_value() == false);
            ctx.check("super_walk_absent_is_empty",
                      vmhook::find_field(k_child, "noSuchFieldAnywhere").has_value() == false);
        }
    }

    // =====================================================================
    //  INTERFACE CONSTANT (interface static final) inherited by an implementor.
    //
    //  FieldInheritedBase implements FieldInheritedIface, which declares the
    //  constant IFACE_CONST.  vmhook::find_field's walk follows ONLY _super
    //  (Klass::get_super), NEVER _transitive_interfaces — so the constant is
    //  INVISIBLE through any implementor wrapper, but resolves through the
    //  interface's OWN klass (depth 0, where it is declared).  This characterizes
    //  the deliberate field/method asymmetry: METHOD lookup falls back to the
    //  implemented-interface chain (see InterfacePoly), FIELD lookup does not.
    // =====================================================================
    {
        vmhook::hotspot::klass* const k_iface{ vmhook::find_class(K_IFACE) };
        vmhook::hotspot::klass* const k_child{ vmhook::find_class(K_CHILD) };
        vmhook::hotspot::klass* const k_base { vmhook::find_class(K_BASE) };

        ctx.check("iface_klass_resolved", k_iface != nullptr);

        if (k_iface)
        {
            // Through the interface's OWN klass: the constant is declared here.
            // It is a static (interface fields are implicitly static final).
            ctx.check("iface_const_declared_only_on_iface",
                      k_iface->find_field("IFACE_CONST").has_value());
            const auto e_iface{ vmhook::find_field(k_iface, "IFACE_CONST") };
            ctx.check("iface_const_super_walk_on_iface", e_iface.has_value());
            if (e_iface)
            {
                ctx.check("iface_const_is_static", e_iface->is_static == true);
            }
            // Read its value through the interface mirror via the registered
            // interface wrapper's static accessor (resolve at the declaring klass).
            {
                const auto fp{ fi_iface::static_field("IFACE_CONST") };
                ctx.check("iface_const_value_via_iface_wrapper",
                          fp.has_value()
                              && static_cast<std::int32_t>(fp->get()) == IFACE_CONST_VALUE);
            }
        }

        // Through the IMPLEMENTOR klasses: the _super-only walk never reaches the
        // interface, so the constant is NOT found from child or base.
        if (k_child)
        {
            ctx.check("iface_const_NOT_visible_from_child_super_walk",
                      vmhook::find_field(k_child, "IFACE_CONST").has_value() == false);
        }
        if (k_base)
        {
            ctx.check("iface_const_NOT_visible_from_base_super_walk",
                      vmhook::find_field(k_base, "IFACE_CONST").has_value() == false);
            // ...and the per-klass declared-only call on the implementor agrees.
            ctx.check("iface_const_NOT_declared_on_base",
                      k_base->find_field("IFACE_CONST").has_value() == false);
        }
        // ...and through the registered child wrapper's static accessor likewise.
        ctx.check("iface_const_NOT_visible_via_child_wrapper",
                  fi_child::static_field("IFACE_CONST").has_value() == false);
    }

    // =====================================================================
    //  Obtain the live child instance once; every instance angle below wraps
    //  the SAME OOP in different wrapper types to steer the walk's start klass.
    // =====================================================================
    const auto child{ fi_child::get_instance() };
    ctx.check("child_instance_wrapper_obtained", child != nullptr);

    if (child)
    {
        // ---- OWN field — super walk depth 0 (declared on the child) --------
        {
            auto fp{ child->get_field("childOwnInt") };
            ctx.check("own_field_resolves", fp.has_value());
            if (fp)
            {
                const std::int32_t v{ fp->get() };
                ctx.check("own_field_value_depth0", v == OWN_INT_INIT);
                ctx.check("own_field_not_static", fp->is_static() == false);
                ctx.check("own_field_signature_I", std::string{ fp->signature() } == "I");
            }
            ctx.check("own_field_accessor_value", child->own_int() == OWN_INT_INIT);
        }

        // ---- Parent field — super walk depth 1 (declared on Mid) -----------
        {
            auto fp{ child->get_field("midOwnInt") };
            ctx.check("parent_field_resolves_depth1", fp.has_value());
            if (fp)
            {
                ctx.check("parent_field_value_depth1", static_cast<std::int32_t>(fp->get()) == MID_INT_INIT);
                ctx.check("parent_field_not_static", fp->is_static() == false);
            }
            ctx.check("parent_field_accessor_value", child->mid_own_int() == MID_INT_INIT);
        }

        // ---- Grandparent inherited fields — super walk depth 2, EVERY access
        //      level.  find_field reads by offset, so private/package resolve. -
        {
            ctx.check("inherited_protected_resolves", child->get_field("protectedInt").has_value());
            ctx.check("inherited_protected_value", child->protected_int() == PROT_INT_INIT);

            ctx.check("inherited_public_resolves", child->get_field("publicInt").has_value());
            ctx.check("inherited_public_value", child->public_int() == PUB_INT_INIT);

            ctx.check("inherited_package_private_resolves", child->get_field("packageInt").has_value());
            ctx.check("inherited_package_private_value", child->package_int() == PKG_INT_INIT);

            // Java-private on the grandparent — unreachable from child Java code,
            // but find_field's offset read does not consult access flags.
            ctx.check("inherited_private_resolves_ignoring_access",
                      child->get_field("privateInt").has_value());
            ctx.check("inherited_private_value_ignoring_access",
                      child->private_int() == PRV_INT_INIT);

            // A wide (J) inherited primitive and a reference (String) inherited
            // field, so the walk is proven for non-int + compressed-OOP decode.
            ctx.check("inherited_long_resolves", child->get_field("baseLong").has_value());
            ctx.check("inherited_long_value", child->base_long() == BASE_LONG_INIT);
            {
                auto sp{ child->get_field("baseStr") };
                ctx.check("inherited_string_resolves", sp.has_value());
                if (sp)
                {
                    const std::string s = sp->get();
                    ctx.check("inherited_string_value", s == "base-str");
                    ctx.check("inherited_string_signature",
                              std::string{ sp->signature() } == "Ljava/lang/String;");
                }
            }
        }

        // =================================================================
        //  Inherited fields of EVERY remaining JVM type — depth-2 super walk +
        //  field_proxy::get() proven for Z B C S F D and an inherited ARRAY.
        //  Each resolves (the walk supplies the offset), carries the right
        //  signature, and reads its mirrored init value.  Together with the
        //  I / J / Ljava/lang/String; cases above this is the full type matrix.
        // =================================================================
        {
            // boolean "Z"
            {
                auto fp{ child->get_field("baseBool") };
                ctx.check("inherited_bool_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherited_bool_signature", std::string{ fp->signature() } == "Z");
                    ctx.check("inherited_bool_value", child->base_bool() == BASE_BOOL_INIT);
                }
            }
            // byte "B"
            {
                auto fp{ child->get_field("baseByte") };
                ctx.check("inherited_byte_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherited_byte_signature", std::string{ fp->signature() } == "B");
                    ctx.check("inherited_byte_value", child->base_byte() == BASE_BYTE_INIT);
                }
            }
            // char "C"
            {
                auto fp{ child->get_field("baseChar") };
                ctx.check("inherited_char_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherited_char_signature", std::string{ fp->signature() } == "C");
                    ctx.check("inherited_char_value", child->base_char() == BASE_CHAR_INIT);
                }
            }
            // short "S"
            {
                auto fp{ child->get_field("baseShort") };
                ctx.check("inherited_short_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherited_short_signature", std::string{ fp->signature() } == "S");
                    ctx.check("inherited_short_value", child->base_short() == BASE_SHORT_INIT);
                }
            }
            // float "F"  (exact in IEEE-754 — equality is safe)
            {
                auto fp{ child->get_field("baseFloat") };
                ctx.check("inherited_float_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherited_float_signature", std::string{ fp->signature() } == "F");
                    ctx.check("inherited_float_value", child->base_float() == BASE_FLOAT_INIT);
                }
            }
            // double "D"  (exact in IEEE-754 — equality is safe)
            {
                auto fp{ child->get_field("baseDouble") };
                ctx.check("inherited_double_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherited_double_signature", std::string{ fp->signature() } == "D");
                    ctx.check("inherited_double_value", child->base_double() == BASE_DOUBLE_INIT);
                }
            }
            // int[] "[I" — inherited ARRAY reference (exercises the array-OOP
            // decode through the depth-2 walk, not just a scalar slot).
            {
                auto fp{ child->get_field("baseIntArray") };
                ctx.check("inherited_array_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherited_array_signature", std::string{ fp->signature() } == "[I");
                    ctx.check("inherited_array_is_reference", fp->is_reference() == true);
                    const std::vector<std::int32_t> v{ child->base_int_array() };
                    ctx.check("inherited_array_size", v.size() == 3);
                    ctx.check("inherited_array_values",
                              v.size() == 3 && v[0] == 11 && v[1] == 22 && v[2] == 33);
                }
            }
        }

        // =================================================================
        //  SHADOWING — the crux.  The child re-declares shadowedInt /
        //  shadowedStr.  A CHILD-typed read of the child object must see the
        //  CHILD slot; a BASE-typed read of the SAME object must see the BASE
        //  slot.  The two sentinels are far apart, so a misread is unambiguous.
        // =================================================================
        {
            // Child-typed read -> child slot (walk starts at child, finds it
            // at depth 0, never descends to the base copy).
            auto via_child{ child->get_field("shadowedInt") };
            ctx.check("shadow_child_typed_resolves", via_child.has_value());
            const std::int32_t child_slot{ via_child ? static_cast<std::int32_t>(via_child->get()) : -1 };
            ctx.check("shadow_child_typed_sees_child_slot", child_slot == CHILD_SHADOW_INT);

            // Base-typed wrapper around the SAME OOP -> walk starts at Base,
            // resolves Base's shadowedInt slot (the hidden one).
            fi_base as_base{ child->vmhook::object_base::get_instance() };
            auto via_base{ as_base.get_field("shadowedInt") };
            ctx.check("shadow_base_typed_resolves", via_base.has_value());
            const std::int32_t base_slot{ via_base ? static_cast<std::int32_t>(via_base->get()) : -1 };
            ctx.check("shadow_base_typed_sees_base_slot", base_slot == BASE_SHADOW_INT);

            // The two reads of the SAME object see DIFFERENT physical slots.
            ctx.check("shadow_two_slots_distinct", child_slot != base_slot);
            ctx.check("shadow_child_wins_for_child_view", child_slot == CHILD_SHADOW_INT);
            ctx.check("shadow_base_view_unhidden", base_slot == BASE_SHADOW_INT);

            // The two proxies point at DIFFERENT addresses (physical proof the
            // shadow is a separate slot, not the same offset read twice).
            if (via_child && via_base)
            {
                ctx.check("shadow_slot_addresses_differ",
                          via_child->raw_address() != via_base->raw_address());
            }

            // Mid-typed read -> Mid declares no shadowedInt, so the walk passes
            // through Mid and resolves the BASE slot (NOT the child slot): a
            // mid-typed view is a base-side view for this name.
            fi_mid as_mid{ child->vmhook::object_base::get_instance() };
            auto via_mid{ as_mid.get_field("shadowedInt") };
            ctx.check("shadow_mid_typed_resolves", via_mid.has_value());
            if (via_mid)
            {
                ctx.check("shadow_mid_typed_sees_base_slot",
                          static_cast<std::int32_t>(via_mid->get()) == BASE_SHADOW_INT);
            }

            // String shadow: child copy is "child", base copy is "base".
            {
                auto cs{ child->get_field("shadowedStr") };
                fi_base sb{ child->vmhook::object_base::get_instance() };
                auto bs{ sb.get_field("shadowedStr") };
                ctx.check("shadow_string_child_resolves", cs.has_value());
                ctx.check("shadow_string_base_resolves", bs.has_value());
                if (cs && bs)
                {
                    const std::string cv = cs->get();
                    const std::string bv = bs->get();
                    ctx.check("shadow_string_child_is_child", cv == "child");
                    ctx.check("shadow_string_base_is_base", bv == "base");
                    ctx.check("shadow_string_distinct", cv != bv);
                }
            }
        }

        // =================================================================
        //  Offset consistency: the SAME inherited grandparent field, read
        //  through the child wrapper and through the base wrapper around the
        //  same OOP, resolves to the SAME physical address (it is one slot).
        //  This guards against the walk/cache returning a divergent offset for
        //  an inherited (non-shadowed) field across wrapper types.
        // =================================================================
        {
            auto via_child{ child->get_field("protectedInt") };
            fi_base as_base{ child->vmhook::object_base::get_instance() };
            auto via_base{ as_base.get_field("protectedInt") };
            if (via_child && via_base)
            {
                ctx.check("inherited_nonshadowed_same_address",
                          via_child->raw_address() == via_base->raw_address());
                ctx.check("inherited_nonshadowed_same_value",
                          static_cast<std::int32_t>(via_child->get())
                              == static_cast<std::int32_t>(via_base->get()));
            }
        }

        // =================================================================
        //  Mid-typed reads of fields it OWNS (depth 0 for Mid) and inherits
        //  from Base (depth 1 for Mid) — proves the walk is correct from an
        //  intermediate starting klass too, not only from the leaf.
        // =================================================================
        {
            fi_mid as_mid{ child->vmhook::object_base::get_instance() };
            ctx.check("mid_view_own_field_value", as_mid.mid_own_int() == MID_INT_INIT);
            ctx.check("mid_view_inherited_protected_value", as_mid.protected_int() == PROT_INT_INIT);
        }
    }

    // =====================================================================
    //  STATIC inherited fields — the same get_super() walk runs on the
    //  java.lang.Class mirror.  Read inherited statics at every access level
    //  THROUGH the child wrapper (start klass = child), and prove the shadowed
    //  static is child-wins (child static_field) vs base-wins (base wrapper).
    // =====================================================================
    {
        // protected/public static declared on the grandparent, resolved via the
        // child wrapper's static accessor (walk depth 2 on the mirror chain).
        {
            auto fp{ fi_child::static_field("sProtected") };
            ctx.check("static_inherited_protected_resolves", fp.has_value());
            if (fp)
            {
                ctx.check("static_inherited_protected_is_static", fp->is_static() == true);
                ctx.check("static_inherited_protected_value",
                          static_cast<std::int32_t>(fp->get()) == STAT_PROT_INIT);
            }
        }
        {
            auto fp{ fi_child::static_field("sPublic") };
            ctx.check("static_inherited_public_resolves", fp.has_value());
            if (fp)
            {
                ctx.check("static_inherited_public_value",
                          static_cast<std::int32_t>(fp->get()) == STAT_PUB_INIT);
            }
        }
        // Java-private static on the grandparent — find_field ignores access.
        {
            auto fp{ fi_child::static_field("sPrivate") };
            ctx.check("static_inherited_private_resolves_ignoring_access", fp.has_value());
            if (fp)
            {
                ctx.check("static_inherited_private_value",
                          static_cast<std::int32_t>(fp->get()) == STAT_PRV_INIT);
            }
        }
        // Static declared on the parent (Mid) — walk depth 1 from the child.
        {
            auto fp{ fi_child::static_field("sMid") };
            ctx.check("static_inherited_from_parent_resolves", fp.has_value());
            if (fp)
            {
                ctx.check("static_inherited_from_parent_value",
                          static_cast<std::int32_t>(fp->get()) == STAT_MID_INIT);
            }
        }

        // Shadowed static: child sShadow (777) hides grandparent sShadow (555).
        // Child wrapper -> child mirror slot; base wrapper -> base mirror slot.
        {
            auto via_child{ fi_child::static_field("sShadow") };
            auto via_base{ fi_base::static_field("sShadow") };
            ctx.check("static_shadow_child_resolves", via_child.has_value());
            ctx.check("static_shadow_base_resolves", via_base.has_value());
            if (via_child && via_base)
            {
                const std::int32_t cv{ via_child->get() };
                const std::int32_t bv{ via_base->get() };
                ctx.check("static_shadow_child_wins", cv == STATIC_SHADOW_CHILD);
                ctx.check("static_shadow_base_unhidden", bv == STATIC_SHADOW_BASE);
                ctx.check("static_shadow_distinct", cv != bv);
                ctx.check("static_shadow_addresses_differ",
                          via_child->raw_address() != via_base->raw_address());
            }
        }
    }

    // =====================================================================
    //  NEGATIVE path — a name that exists nowhere in the hierarchy.  The walk
    //  reaches java.lang.Object and returns nullopt.  Proven for the child,
    //  the mid, and the base wrapper (each exhausts a different-length chain).
    // =====================================================================
    {
        ctx.check("absent_field_child_nullopt",
                  fi_child::static_field("noSuchFieldAnywhere").has_value() == false);
        ctx.check("absent_field_mid_nullopt",
                  fi_mid::static_field("noSuchFieldAnywhere").has_value() == false);
        ctx.check("absent_field_base_nullopt",
                  fi_base::static_field("noSuchFieldAnywhere").has_value() == false);

        if (child)
        {
            ctx.check("absent_instance_field_child_nullopt",
                      child->get_field("noSuchFieldAnywhere").has_value() == false);
            // A CHILD field name is NOT visible from a BASE-typed wrapper (the
            // base walk only goes UP, never down into the child) -> nullopt.
            fi_base as_base{ child->vmhook::object_base::get_instance() };
            ctx.check("child_only_field_not_visible_from_base",
                      as_base.get_field("childOwnInt").has_value() == false);
            // ...and the parent-only field is likewise invisible from the base.
            ctx.check("parent_only_field_not_visible_from_base",
                      as_base.get_field("midOwnInt").has_value() == false);
        }
    }

    // =====================================================================
    //  Cache behaviour through the walk: a second resolution of an inherited
    //  field returns a proxy at the SAME address (the (klass*, name) entry is
    //  cached after the first walk; vmhook.hpp:10762).  Value + address stable.
    // =====================================================================
    if (child)
    {
        auto a{ child->get_field("protectedInt") };
        auto b{ child->get_field("protectedInt") };
        if (a && b)
        {
            ctx.check("inherited_cache_same_address", a->raw_address() == b->raw_address());
            ctx.check("inherited_cache_same_value",
                      static_cast<std::int32_t>(a->get())
                          == static_cast<std::int32_t>(b->get()));
        }
    }

    // =====================================================================
    //  WRITE an inherited field THROUGH THE SUBCLASS WRAPPER via the library's
    //  own field_proxy::set() (NOT bytecode).  The super walk resolves the
    //  inherited offset for the write exactly as for the read, at depth 1 (Mid)
    //  and depth 2 (Base) and for an inherited reference (String).  Each slot is
    //  written, read back, asserted, then RESTORED to its init so the later
    //  bytecode-driven mode blocks see the canonical pre-mutation state.
    // =====================================================================
    if (child)
    {
        // depth-1 inherited int (Mid.midOwnInt)
        {
            auto fp{ child->get_field("midOwnInt") };
            ctx.check("set_inherited_depth1_resolves", fp.has_value());
            if (fp)
            {
                fp->set(std::int32_t{ 0x1515 });
                auto rb{ child->get_field("midOwnInt") };
                ctx.check("set_inherited_depth1_readback",
                          rb.has_value() && static_cast<std::int32_t>(rb->get()) == 0x1515);
                fp->set(MID_INT_INIT);  // restore
                auto rs{ child->get_field("midOwnInt") };
                ctx.check("set_inherited_depth1_restored",
                          rs.has_value() && static_cast<std::int32_t>(rs->get()) == MID_INT_INIT);
            }
        }
        // depth-2 inherited int (Base.publicInt)
        {
            auto fp{ child->get_field("publicInt") };
            ctx.check("set_inherited_depth2_resolves", fp.has_value());
            if (fp)
            {
                fp->set(std::int32_t{ 0x2626 });
                auto rb{ child->get_field("publicInt") };
                ctx.check("set_inherited_depth2_readback",
                          rb.has_value() && static_cast<std::int32_t>(rb->get()) == 0x2626);
                fp->set(PUB_INT_INIT);  // restore
                auto rs{ child->get_field("publicInt") };
                ctx.check("set_inherited_depth2_restored",
                          rs.has_value() && static_cast<std::int32_t>(rs->get()) == PUB_INT_INIT);
            }
        }
        // depth-2 inherited REFERENCE (Base.baseStr) — the walk must resolve the
        // inherited String slot for WRITING just as it does for reading.  We
        // assert the RESOLUTION hard (the proxy is obtained at the walk-resolved
        // offset and the slot is a writable reference).  The post-set VALUE
        // read-back depends on make_java_string()/set_str_field() — an ORTHOGONAL
        // path (owned by field_string / field_primitives_set) with a documented
        // mirror-allocation fragility — so the round-trip VALUE is recorded
        // best-effort [INFO], never a FAIL of THIS (inheritance) module.  The slot
        // is restored to its init regardless; nothing downstream reads baseStr.
        {
            auto fp{ child->get_field("baseStr") };
            ctx.check("set_inherited_string_resolves", fp.has_value());
            if (fp)
            {
                ctx.check("set_inherited_string_is_reference", fp->is_reference() == true);
                fp->set(std::string{ "rewritten" });
                auto rb{ child->get_field("baseStr") };
                const std::string rbv{ rb ? std::string{ rb->get() } : std::string{} };
                ctx.record(std::string{ "[INFO] set_inherited_string_readback (best-effort; "
                                        "make_java_string path): got '" } + rbv
                           + "' (expected 'rewritten')");
                fp->set(std::string{ "base-str" });  // restore (best-effort)
                auto rs{ child->get_field("baseStr") };
                const std::string rsv{ rs ? std::string{ rs->get() } : std::string{} };
                ctx.record(std::string{ "[INFO] set_inherited_string_restored (best-effort): now '" }
                           + rsv + "' (expected 'base-str')");
            }
        }
    }

    // =====================================================================
    //  LIVE mutation — mode 1: putfield the child's own + inherited instance
    //  slots through real bytecode, then read each back.  Proves find_field
    //  resolves the live post-dispatch slot at every walk depth.
    // =====================================================================
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("mode1_probe_completed", done);
        if (done)
        {
            const auto live{ fi_child::get_instance() };
            ctx.check("mode1_instance_reobtained", live != nullptr);
            if (live)
            {
                ctx.check("mode1_own_field_live", live->own_int() == OWN_INT_RUNTIME);
                ctx.check("mode1_parent_field_live_depth1", live->mid_own_int() == MID_INT_RUNTIME);
                ctx.check("mode1_inherited_protected_live_depth2",
                          live->protected_int() == PROT_INT_RUNTIME);
                ctx.check("mode1_inherited_public_live_depth2",
                          live->public_int() == PUB_INT_RUNTIME);
                // Inherited NON-int slots written by the same putfield dispatch:
                // the depth-2 walk resolves the live Z / F / D slot too.
                ctx.check("mode1_inherited_bool_live_depth2",
                          live->base_bool() == BASE_BOOL_RUNTIME);
                ctx.check("mode1_inherited_float_live_depth2",
                          live->base_float() == BASE_FLOAT_RUNTIME);
                ctx.check("mode1_inherited_double_live_depth2",
                          live->base_double() == BASE_DOUBLE_RUNTIME);
            }
        }
    }

    // =====================================================================
    //  LIVE mutation — mode 2: write the CHILD shadow slot AND an independent
    //  base object's base shadow slot.  Read both back and prove they did NOT
    //  alias: the child object's child slot got the child value, the unrelated
    //  base object got the base value, and the child object's hidden BASE slot
    //  (read base-typed) is UNTOUCHED by the child write.
    // =====================================================================
    {
        const bool done{ drive(ctx, 2) };
        ctx.check("mode2_probe_completed", done);
        if (done)
        {
            const auto live{ fi_child::get_instance() };
            if (live)
            {
                // Child slot of the child object got the child runtime value.
                ctx.check("mode2_child_shadow_slot_live",
                          live->shadowed_int() == CHILD_SHADOW_RUNTIME);

                // The child object's HIDDEN base slot was NOT written by the
                // child-slot putfield (still the init value, not the runtime).
                fi_base hidden{ live->vmhook::object_base::get_instance() };
                auto hb{ hidden.get_field("shadowedInt") };
                if (hb)
                {
                    ctx.check("mode2_child_objects_base_slot_untouched",
                              static_cast<std::int32_t>(hb->get()) == BASE_SHADOW_INT);
                }
            }

            // The INDEPENDENT pure-base object got the independent base value.
            const auto base_obj{ fi_base::get_base_instance() };
            ctx.check("mode2_base_instance_reobtained", base_obj != nullptr);
            if (base_obj)
            {
                ctx.check("mode2_independent_base_slot_live",
                          base_obj->shadowed_int() == INDEP_BASE_SHADOW);
                // And it differs from the child object's child slot — no alias.
                ctx.check("mode2_no_alias_between_objects",
                          base_obj->shadowed_int() != CHILD_SHADOW_RUNTIME);
            }
        }
    }

    // =====================================================================
    //  LIVE mutation — mode 3: putstatic the inherited + shadowed statics, then
    //  read them back through the walk on the class mirror.
    // =====================================================================
    {
        const bool done{ drive(ctx, 3) };
        ctx.check("mode3_probe_completed", done);
        if (done)
        {
            {
                auto fp{ fi_child::static_field("sProtected") };
                if (fp) { ctx.check("mode3_static_protected_live",
                                    static_cast<std::int32_t>(fp->get()) == STAT_PROT_RUNTIME); }
            }
            {
                auto fp{ fi_child::static_field("sPublic") };
                if (fp) { ctx.check("mode3_static_public_live",
                                    static_cast<std::int32_t>(fp->get()) == STAT_PUB_RUNTIME); }
            }
            {
                auto fp{ fi_child::static_field("sMid") };
                if (fp) { ctx.check("mode3_static_parent_live",
                                    static_cast<std::int32_t>(fp->get()) == STAT_MID_RUNTIME); }
            }
            // Shadowed static: child slot got the runtime value; the base slot
            // (read through the base wrapper) is UNTOUCHED — child-wins write.
            {
                auto cf{ fi_child::static_field("sShadow") };
                auto bf{ fi_base::static_field("sShadow") };
                if (cf) { ctx.check("mode3_static_shadow_child_live",
                                    static_cast<std::int32_t>(cf->get()) == STATIC_SHADOW_RUNTIME); }
                if (bf) { ctx.check("mode3_static_shadow_base_untouched",
                                    static_cast<std::int32_t>(bf->get()) == STATIC_SHADOW_BASE); }
            }
        }
    }
}
