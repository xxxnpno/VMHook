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
#include <string>

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
