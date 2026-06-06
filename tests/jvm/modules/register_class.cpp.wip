// register_class JVM test module  (feature area: type registration)
//
// THE authority for vmhook::register_class<T>(name) and the type-registration
// machinery it drives (vmhook.hpp):
//   * type_to_class_map        : unordered_map<type_index, string>   (decl 1440)
//   * g_type_factory_map       : unordered_map<string, factory_fn>   (decl 1462)
//   * registration_mutex       : std::mutex guarding both            (decl 1441)
//   * register_class<T>(name)  : the install routine                 (6915-6952)
//
// register_class<T>(name) (6915-6952) does, in order:
//   1. find_class(name) FIRST (6919).  If it returns null -> log + return false
//      and NEITHER map is touched (6921-6925) -> the type stays UNREGISTERED.
//   2. lock registration_mutex (6936).
//   3. type_to_class_map.insert_or_assign(typeid(T), name) (6938)  -- LAST WINS.
//   4. g_type_factory_map.emplace(name, +[](void* oop){ return new T{oop}; })
//      (6944)  -- std::map::emplace, so it is a NO-OP if `name` is already a key
//      (FIRST WINS).  This asymmetry between (3) insert_or_assign and (4) emplace
//      is a real library defect this module PINS; see BUG notes + lib_bugs.
//
// Downstream consumers of the maps that this module exercises end-to-end:
//   * object_base::resolve_klass(type_index) (14409) -> type_to_class_map.find ->
//     find_class.  Backs static_field / get_field / static_method / get_class_methods.
//   * get_class_methods<W>() (7030-7048) -> type_to_class_map.find (7037).
//   * find_methods_by_signature<W>() (7081-7094) -> get_class_methods<W>().
//   * the FACTORY (g_type_factory_map) is consumed in exactly ONE place:
//     detail::extract_frame_arg<unique_ptr<W>> (7488-7508) -> type_to_class_map
//     [typeid W] -> g_type_factory_map[class] -> factory(oop) -> static_cast<W*>.
//     i.e. a hook callback whose receiver param is unique_ptr<W> is the only API
//     that builds a wrapper THROUGH the registered factory.  (field_proxy::get()
//     -> unique_ptr<W> at 11843 and method_proxy return -> unique_ptr<W> at 12464
//     both `new W{oop}` DIRECTLY off the template param and never touch the factory
//     map -- a distinction this module documents and proves.)
//
// WHAT THIS MODULE PROVES (mostly native; one probe anchors a live instance):
//
//   REGISTERED-TYPE RESOLUTION (HARD, every JDK):
//     register_class<rc>("vmhook/fixtures/RegisterClassFix") returns true, inserts
//     into BOTH maps, and thereafter static_field / get_class_methods /
//     find_methods_by_signature / find_class(name) all resolve.
//
//   UNREGISTERED-TYPE GRACE (HARD, every JDK):
//     a wrapper type NEVER passed to register_class -> static_field == nullopt,
//     get_class_methods<W>() == {}, find_methods_by_signature<W>(...) == {},
//     for_each_instance<W> == 0, and the type_to_class_map has no entry -- no crash.
//
//   BOGUS CLASS NAME (HARD, every JDK):
//     register_class<W>("vmhook/fixtures/NoSuchClass...") returns FALSE, leaves the
//     type UNREGISTERED (no map entry), and every accessor on W returns nullopt/
//     empty without crashing.  (find_class fails before any insert.)
//
//   IDEMPOTENT / LAST-WINS RE-REGISTER (HARD, characterised):
//     re-register the SAME type to the SAME name -> still true, map unchanged, one
//     entry; re-register the SAME type to a DIFFERENT (valid) name -> true and the
//     type_to_class_map value is now the NEW name (insert_or_assign last-wins),
//     proven by reading a field that exists only on the new class.
//
//   TWO WRAPPERS / TWO CLASSES (HARD, every JDK):
//     two distinct wrapper types bound to two distinct fixture classes both
//     resolve to their OWN class -- the map keys on type_index so they don't
//     collide.
//
//   FACTORY ASYMMETRY (BUG PIN, HARD where the precondition holds):
//     register a SECOND distinct wrapper type to an ALREADY-registered class name;
//     type_to_class_map now maps the second type -> that name, but
//     g_type_factory_map STILL holds the FIRST type's factory (emplace no-op).  We
//     assert the factory pointer is unchanged (native map inspection only -- we do
//     NOT route a live oop through the stale factory, because that would
//     static_cast a `new First` to `Second*` and hand Java/`->` an invalid wrapper).
//
//   LIVE FACTORY DECODE (the one Java-coordinated angle; HARD, every JDK):
//     scoped_hook on anchor(int) whose detour receives `const unique_ptr<rc>& self`
//     -- the extract_frame_arg factory path.  Inside the detour we assert self is
//     non-null, is_valid_pointer, and self->marker == the fixture's sentinel,
//     proving the made/decoded oop was wrapped as the REGISTERED type with correct
//     field offsets.  The arg decodes and the original body runs (allow-through).
//
// SAFETY: the ONLY hook is installed via scoped_hook<> and uninstalls on scope
// exit -- nothing armed for later modules.  Every decoded oop is guarded by
// is_valid_pointer before use.  No live oop is ever routed through a stale/mistyped
// factory.  No unrooted-oop sweeps (nothing held across the probe boundary).
// C++17 only: no std::bit_cast, no post-17 API.  MSVC copy-init from value_t/get().
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace
{
    constexpr char RC_CLASS[]{ "vmhook/fixtures/RegisterClassFix" };
    // A SECOND real, always-loaded fixture class used for the "two wrappers / two
    // classes" and "re-register to a different name" angles.  FieldPrimitivesGet
    // is loaded by the harness on every run (its own module registers it), and it
    // declares the static field `sIntZero` we use as a resolution witness -- a
    // field RegisterClassFix does NOT have, so a last-wins re-point is observable.
    constexpr char ALT_CLASS[]{ "vmhook/fixtures/FieldPrimitivesGet" };

    // The fixture's sentinel instance-field value (RegisterClassFix.marker).
    constexpr std::int32_t MARKER{ 0x5AFE7A11 };       // 1526182929
    constexpr std::int32_t CLASS_TOKEN{ 0x1357BD13 };  // 323158291
    constexpr std::int32_t ANCHOR_ARG{ 0x0CA75 };      // 51829

    // ---- Primary wrapper for vmhook.fixtures.RegisterClassFix --------------
    // Deriving from vmhook::object<rc> gives the wrapper the vtable register_class
    // needs and the static_field/get_field accessors.  Handshake via static_field
    // (the GCC-portable path).
    class rc : public vmhook::object<rc>
    {
    public:
        explicit rc(vmhook::oop_t instance) noexcept
            : vmhook::object<rc>{ instance }
        {
        }

        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }
        static auto get_class_token() -> std::int32_t
        {
            const auto fp{ static_field("classToken") };
            if (!fp.has_value()) { return -1; }
            const std::int32_t v = fp->get();
            return v;
        }
        static auto get_anchor_calls() -> std::int32_t
        {
            const auto fp{ static_field("anchorCalls") };
            if (!fp.has_value()) { return -1; }
            const std::int32_t v = fp->get();
            return v;
        }

        // Instance-side read of `marker` (used INSIDE the detour through the
        // factory-built wrapper).  Inherited get_field; safe on a valid oop.
        auto marker() const -> std::int32_t
        {
            const auto fp{ get_field("marker") };
            if (!fp.has_value()) { return -1; }
            const std::int32_t v = fp->get();
            return v;
        }
    };

    // ---- A wrapper type that is registered to a DIFFERENT real class --------
    class alt_w : public vmhook::object<alt_w>
    {
    public:
        explicit alt_w(vmhook::oop_t instance) noexcept
            : vmhook::object<alt_w>{ instance }
        {
        }
    };

    // ---- A wrapper type that is NEVER registered (resolution must miss) -----
    class never_registered : public vmhook::object<never_registered>
    {
    public:
        explicit never_registered(vmhook::oop_t instance) noexcept
            : vmhook::object<never_registered>{ instance }
        {
        }
    };

    // ---- A wrapper type registered only to a BOGUS class name ---------------
    class bogus_w : public vmhook::object<bogus_w>
    {
    public:
        explicit bogus_w(vmhook::oop_t instance) noexcept
            : vmhook::object<bogus_w>{ instance }
        {
        }
    };

    // ---- Re-registration probe types ---------------------------------------
    // reborn_w is registered TWICE: first to RC_CLASS, then to ALT_CLASS, to
    // prove last-wins re-point of type_to_class_map.
    class reborn_w : public vmhook::object<reborn_w>
    {
    public:
        explicit reborn_w(vmhook::oop_t instance) noexcept
            : vmhook::object<reborn_w>{ instance }
        {
        }
    };

    // collide_a is registered to a shared name FIRST; collide_b is registered to
    // the SAME name SECOND, to expose the factory emplace-no-overwrite asymmetry.
    class collide_a : public vmhook::object<collide_a>
    {
    public:
        explicit collide_a(vmhook::oop_t instance) noexcept
            : vmhook::object<collide_a>{ instance }
        {
        }
    };
    class collide_b : public vmhook::object<collide_b>
    {
    public:
        explicit collide_b(vmhook::oop_t instance) noexcept
            : vmhook::object<collide_b>{ instance }
        {
        }
    };

    // ── Map inspection helpers (read the maps directly; same symbols the
    //    library uses -- inline at vmhook:: scope).  Reads are taken under
    //    registration_mutex for parity with the library's documented contract. ──
    auto type_is_registered(const std::type_index ti) -> bool
    {
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
        return vmhook::type_to_class_map.find(ti) != vmhook::type_to_class_map.end();
    }
    auto type_maps_to(const std::type_index ti, std::string& out_name) -> bool
    {
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
        const auto it{ vmhook::type_to_class_map.find(ti) };
        if (it == vmhook::type_to_class_map.end()) { return false; }
        out_name = it->second;
        return true;
    }
    auto factory_for(const std::string& class_name)
        -> vmhook::type_factory_function_t
    {
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
        const auto it{ vmhook::g_type_factory_map.find(class_name) };
        return it == vmhook::g_type_factory_map.end() ? nullptr : it->second;
    }

    // 8-nibble uppercase hex of a 32-bit value (diagnostic record only).
    auto to_hex8(std::int32_t value) -> std::string
    {
        static const char* const hex{ "0123456789ABCDEF" };
        const std::uint32_t v{ static_cast<std::uint32_t>(value) };
        std::string out{};
        out.reserve(8);
        for (int shift{ 28 }; shift >= 0; shift -= 4)
        {
            out.push_back(hex[(v >> shift) & 0xFu]);
        }
        return out;
    }

    // ── Live-factory-decode observation state (filled inside the detour). ──
    std::atomic<int>  g_anchor_fires{ 0 };
    std::atomic<bool> g_self_nonnull{ false };
    std::atomic<bool> g_self_valid{ false };
    std::atomic<bool> g_self_marker_ok{ false };
    std::atomic<std::int32_t> g_decoded_marker{ 0 };
    std::atomic<bool> g_arg_ok{ false };

    // The anchor() detour: receives `this` as a unique_ptr<rc> built by the
    // registered FACTORY (extract_frame_arg, vmhook.hpp:7501-7508).  We validate
    // the wrapper points at a real oop of the registered type by reading its
    // `marker` field (offset resolved against rc's klass).  Non-cancelling, so
    // the original body runs (allow-through).
    auto on_anchor(vmhook::return_value& /*ret*/,
                   const std::unique_ptr<rc>& self,
                   std::int32_t delta) -> void
    {
        g_anchor_fires.fetch_add(1, std::memory_order_relaxed);
        g_arg_ok.store(delta == ANCHOR_ARG, std::memory_order_relaxed);
        if (self != nullptr)
        {
            g_self_nonnull.store(true, std::memory_order_relaxed);
            void* const oop{ self->vmhook::object_base::get_instance() };
            if (oop && vmhook::hotspot::is_valid_pointer(oop))
            {
                g_self_valid.store(true, std::memory_order_relaxed);
                const std::int32_t m{ self->marker() };
                g_decoded_marker.store(m, std::memory_order_relaxed);
                g_self_marker_ok.store(m == MARKER, std::memory_order_relaxed);
            }
        }
    }

    auto drive(vmhook_test::context& ctx) -> bool
    {
        return ctx.run_probe(
            [](bool value)
            {
                if (value) { rc::set_done(false); }
                rc::set_go(value);
            },
            []() { return rc::get_done(); });
    }
}

VMHOOK_JVM_MODULE(register_class)
{
    // =====================================================================
    //  0. BASELINE: register the primary wrapper -> true, both maps populated.
    // =====================================================================
    const bool reg_ok{ vmhook::register_class<rc>(RC_CLASS) };
    ctx.check("register_returns_true_for_loaded_class", reg_ok);
    ctx.check("registered_type_in_type_to_class_map",
              type_is_registered(std::type_index{ typeid(rc) }));
    {
        std::string mapped{};
        const bool present{ type_maps_to(std::type_index{ typeid(rc) }, mapped) };
        ctx.check("registered_type_maps_to_exact_class_name",
                  present && mapped == RC_CLASS);
    }
    ctx.check("registered_class_has_factory_entry",
              factory_for(RC_CLASS) != nullptr);

    // =====================================================================
    //  1. REGISTERED -> resolution works through every map consumer.
    // =====================================================================
    {
        // static_field path (resolve_klass -> type_to_class_map -> find_class).
        ctx.check("registered_static_field_go_resolves", rc::resolves("go"));
        ctx.check("registered_static_field_marker_resolves", rc::resolves("marker"));
        ctx.check("registered_static_field_classToken_value",
                  rc::get_class_token() == CLASS_TOKEN);

        // find_class(name) directly resolves the same klass.
        vmhook::hotspot::klass* const k{ vmhook::find_class(RC_CLASS) };
        ctx.check("find_class_resolves_registered_name", k != nullptr);

        // get_class_methods<W>() (7030) returns the declared methods (incl anchor).
        const auto methods{ vmhook::get_class_methods<rc>() };
        ctx.check("get_class_methods_nonempty_for_registered", !methods.empty());
        bool has_anchor{ false };
        for (const auto& entry : methods)
        {
            if (entry.first == "anchor") { has_anchor = true; break; }
        }
        ctx.check("get_class_methods_lists_anchor", has_anchor);

        // find_methods_by_signature<W>() (7081) -> the int->int anchor descriptor.
        const auto anchor_names{ vmhook::find_methods_by_signature<rc>("(I)I") };
        bool fm_has_anchor{ false };
        for (const std::string& nm : anchor_names)
        {
            if (nm == "anchor") { fm_has_anchor = true; break; }
        }
        ctx.check("find_methods_by_signature_finds_anchor", fm_has_anchor);
    }

    // =====================================================================
    //  2. UNREGISTERED wrapper type -> every accessor misses GRACEFULLY.
    //     never_registered is never passed to register_class.
    // =====================================================================
    {
        ctx.check("unregistered_type_not_in_map",
                  !type_is_registered(std::type_index{ typeid(never_registered) }));
        ctx.check("unregistered_static_field_nullopt",
                  !never_registered::static_field("go").has_value());
        ctx.check("unregistered_get_class_methods_empty",
                  vmhook::get_class_methods<never_registered>().empty());
        ctx.check("unregistered_find_methods_by_signature_empty",
                  vmhook::find_methods_by_signature<never_registered>("(I)I").empty());
        // for_each_instance<W> must early-out to 0 visits for an unregistered type
        // (vmhook.hpp:6787-6793) -- and must not crash.
        const std::size_t visits{ vmhook::for_each_instance<never_registered>(
            [](std::unique_ptr<never_registered>) { /* never called */ }) };
        ctx.check("unregistered_for_each_instance_zero", visits == 0);
        // No factory entry was created for any class on behalf of this type.
        // (We cannot key by class -- the type was never mapped -- so the proof is
        //  simply that the type stays out of the type map, asserted above.)
    }

    // =====================================================================
    //  3. BOGUS class name -> register_class returns FALSE, type stays
    //     UNREGISTERED, accessors miss, no crash.  find_class fails BEFORE any
    //     insert (vmhook.hpp:6919-6925), so neither map is touched.
    // =====================================================================
    {
        const char* bogus{ "vmhook/fixtures/NoSuchRegisterClass_ZZZ_12345" };
        const bool bogus_ok{ vmhook::register_class<bogus_w>(bogus) };
        ctx.check("register_returns_false_for_bogus_class", !bogus_ok);
        ctx.check("bogus_type_not_in_type_to_class_map",
                  !type_is_registered(std::type_index{ typeid(bogus_w) }));
        ctx.check("bogus_class_has_no_factory_entry",
                  factory_for(bogus) == nullptr);
        ctx.check("bogus_static_field_nullopt",
                  !bogus_w::static_field("go").has_value());
        ctx.check("bogus_get_class_methods_empty",
                  vmhook::get_class_methods<bogus_w>().empty());
        // Empty-string class name: also a miss, also graceful.
        const bool empty_ok{ vmhook::register_class<bogus_w>("") };
        ctx.check("register_returns_false_for_empty_name", !empty_ok);
        ctx.check("bogus_type_still_unregistered_after_empty",
                  !type_is_registered(std::type_index{ typeid(bogus_w) }));
    }

    // =====================================================================
    //  4. IDEMPOTENT re-register: SAME type, SAME name -> true, map unchanged,
    //     still exactly one entry pointing at the same name; factory unchanged.
    // =====================================================================
    {
        const vmhook::type_factory_function_t factory_before{ factory_for(RC_CLASS) };
        const bool again{ vmhook::register_class<rc>(RC_CLASS) };
        ctx.check("reregister_same_type_same_name_true", again);
        std::string mapped{};
        const bool present{ type_maps_to(std::type_index{ typeid(rc) }, mapped) };
        ctx.check("reregister_same_name_map_value_unchanged",
                  present && mapped == RC_CLASS);
        // emplace is a no-op on an existing key, so the factory pointer is stable.
        ctx.check("reregister_same_name_factory_pointer_stable",
                  factory_for(RC_CLASS) == factory_before && factory_before != nullptr);
        // Resolution still works after the redundant register.
        ctx.check("reregister_same_name_static_field_still_resolves",
                  rc::resolves("go"));
    }

    // =====================================================================
    //  5. LAST-WINS re-register: SAME type, DIFFERENT (valid) name.
    //     insert_or_assign (6938) re-points type_to_class_map to the NEW name.
    //     Proven by a field that exists ONLY on the new class.
    // =====================================================================
    {
        // First bind reborn_w to RC_CLASS, confirm RC resolution.
        const bool first{ vmhook::register_class<reborn_w>(RC_CLASS) };
        ctx.check("reborn_first_register_true", first);
        {
            std::string mapped{};
            type_maps_to(std::type_index{ typeid(reborn_w) }, mapped);
            ctx.check("reborn_first_maps_to_RC", mapped == RC_CLASS);
        }
        // reborn_w bound to RC sees RC's `marker` field, NOT FieldPrimitivesGet's
        // `sIntZero`.
        ctx.check("reborn_on_RC_sees_RC_field",
                  reborn_w::static_field("classToken").has_value());
        ctx.check("reborn_on_RC_misses_ALT_only_field",
                  !reborn_w::static_field("sIntZero").has_value());

        // Re-point reborn_w to ALT_CLASS.
        const bool second{ vmhook::register_class<reborn_w>(ALT_CLASS) };
        ctx.check("reborn_second_register_true", second);
        {
            std::string mapped{};
            type_maps_to(std::type_index{ typeid(reborn_w) }, mapped);
            ctx.check("reborn_second_maps_to_ALT_last_wins", mapped == ALT_CLASS);
        }
        // Now reborn_w resolves against ALT_CLASS: it sees `sIntZero` and NOT the
        // RC-only `classToken` -> proves the re-point took effect.
        ctx.check("reborn_on_ALT_sees_ALT_field",
                  reborn_w::static_field("sIntZero").has_value());
        ctx.check("reborn_on_ALT_misses_RC_only_field",
                  !reborn_w::static_field("classToken").has_value());
        // get_class_methods now reflects the ALT class.
        ctx.check("reborn_get_class_methods_nonempty_after_repoint",
                  !vmhook::get_class_methods<reborn_w>().empty());
        // BUG (low): the OLD class name's factory entry is never erased on a
        // re-point.  RC_CLASS still has a factory (from rc's baseline registration
        // AND from reborn's first register -- whichever landed first; emplace kept
        // the first).  We only assert RC_CLASS still HAS some factory (the leak is
        // benign here); the asymmetry itself is pinned in section 7.
        ctx.check("repoint_leaves_old_name_factory_present",
                  factory_for(RC_CLASS) != nullptr);
    }

    // =====================================================================
    //  6. TWO wrappers / TWO classes: distinct types, distinct names, both
    //     resolve to their OWN class.  Keyed by type_index, so no collision.
    // =====================================================================
    {
        const bool a{ vmhook::register_class<rc>(RC_CLASS) };       // already bound
        const bool b{ vmhook::register_class<alt_w>(ALT_CLASS) };
        ctx.check("two_classes_rc_register_true", a);
        ctx.check("two_classes_alt_register_true", b);
        std::string rc_name{};
        std::string alt_name{};
        type_maps_to(std::type_index{ typeid(rc) }, rc_name);
        type_maps_to(std::type_index{ typeid(alt_w) }, alt_name);
        ctx.check("two_classes_rc_maps_to_RC", rc_name == RC_CLASS);
        ctx.check("two_classes_alt_maps_to_ALT", alt_name == ALT_CLASS);
        ctx.check("two_classes_distinct_names", rc_name != alt_name);
        // Each wrapper resolves its OWN class's distinctive field.
        ctx.check("two_classes_rc_resolves_own_field", rc::resolves("classToken"));
        ctx.check("two_classes_alt_resolves_own_field",
                  alt_w::static_field("sIntZero").has_value());
        ctx.check("two_classes_rc_misses_alt_field",
                  !rc::resolves("sIntZero"));
        ctx.check("two_classes_alt_misses_rc_field",
                  !alt_w::static_field("classToken").has_value());
        // Distinct factory entries for the two distinct class names.
        ctx.check("two_classes_distinct_factories",
                  factory_for(RC_CLASS) != nullptr
                  && factory_for(ALT_CLASS) != nullptr
                  && factory_for(RC_CLASS) != factory_for(ALT_CLASS));
    }

    // =====================================================================
    //  7. FACTORY ASYMMETRY (BUG PIN).  Register collide_a to a shared name, then
    //     collide_b to the SAME name.  type_to_class_map (insert_or_assign) now
    //     maps collide_b -> name, but g_type_factory_map (emplace) STILL holds
    //     collide_a's factory.  We assert the factory pointer is IDENTICAL before
    //     and after the second register (proving emplace did not overwrite).
    //
    //     We deliberately use a SHARED name that is its OWN dedicated class so we
    //     do not perturb the RC/ALT factories other sections rely on.  ALT_CLASS
    //     already has a factory (alt_w), so binding both collide_a and collide_b
    //     to ALT_CLASS proves the no-overwrite without creating a new mapping for
    //     a class another section reads a factory identity on... so instead use a
    //     THIRD real class, java/lang/Object, which no other section pins by
    //     factory identity.
    //
    //     SAFETY: we NEVER route a live oop through this factory.  Doing so would
    //     build a `new collide_a` and static_cast it to `collide_b*`
    //     (extract_frame_arg, 7508) -- an invalid downcast between unrelated types.
    //     This section is pure native map inspection.
    // =====================================================================
    {
        const char* shared{ "java/lang/Object" };
        // Snapshot whatever factory java/lang/Object already has (some other
        // module may have bound a wrapper to it; emplace means the FIRST such
        // binding owns the slot for the whole process).
        const vmhook::type_factory_function_t pre{ factory_for(shared) };

        const bool ra{ vmhook::register_class<collide_a>(shared) };
        ctx.check("collide_a_register_true", ra);
        const vmhook::type_factory_function_t after_a{ factory_for(shared) };
        ctx.check("collide_shared_name_has_factory", after_a != nullptr);

        const bool rb{ vmhook::register_class<collide_b>(shared) };
        ctx.check("collide_b_register_true", rb);
        const vmhook::type_factory_function_t after_b{ factory_for(shared) };

        // type_to_class_map: BOTH types now map to the shared name (last write per
        // KEY wins, but they are different keys, so both are present).
        std::string a_name{};
        std::string b_name{};
        type_maps_to(std::type_index{ typeid(collide_a) }, a_name);
        type_maps_to(std::type_index{ typeid(collide_b) }, b_name);
        ctx.check("collide_a_maps_to_shared", a_name == shared);
        ctx.check("collide_b_maps_to_shared_too", b_name == shared);

        // THE BUG: the factory slot did NOT change when collide_b registered.
        // g_type_factory_map.emplace (6944) is a no-op on an existing key, so the
        // factory still builds whatever type owned the slot first (`pre` if a
        // prior module bound java/lang/Object, else collide_a).
        ctx.check("collide_factory_unchanged_after_second_register",
                  after_b == after_a);
        // Whichever owned it first still owns it: if java/lang/Object was unbound
        // before, after_a==collide_a's factory and stays so; if it was bound, the
        // pre-existing factory survived collide_a too.
        ctx.check("collide_factory_owner_is_first_registrant",
                  (pre == nullptr) ? (after_b == after_a)
                                   : (after_b == pre));
        ctx.record("[INFO] register_class factory/type-map asymmetry: "
                   "type_to_class_map.insert_or_assign (vmhook.hpp:6938) is "
                   "last-wins, but g_type_factory_map.emplace (6944) is first-wins. "
                   "Two distinct wrapper types sharing one class name end up with "
                   "the SECOND type mapped but the FIRST type's factory -- a "
                   "unique_ptr<Second> hook arg would be built as `new First` and "
                   "static_cast to Second* (extract_frame_arg, 7508): UB. Pinned, "
                   "not routed through a live oop.");
    }

    // =====================================================================
    //  8. LIVE FACTORY DECODE.  Install a scoped_hook on anchor(int); its detour
    //     receives `this` as unique_ptr<rc> built BY THE REGISTERED FACTORY
    //     (extract_frame_arg -> g_type_factory_map[RC_CLASS]).  Drive one probe;
    //     assert the decoded wrapper is the registered type with correct offsets.
    //
    //     This is the ONLY angle that exercises the factory MAP end-to-end on a
    //     real oop, and the ONLY Java-coordinated angle.  scoped_hook uninstalls
    //     on scope exit -- nothing armed afterward.
    // =====================================================================
    {
        // Make sure rc is bound to RC_CLASS (sections above left it so, but be
        // explicit -- the factory the detour uses must be rc's).
        std::string rc_bound{};
        type_maps_to(std::type_index{ typeid(rc) }, rc_bound);
        ctx.check("anchor_precondition_rc_registered_to_RC", rc_bound == RC_CLASS);

        auto handle{ vmhook::scoped_hook<rc>("anchor", &on_anchor) };
        ctx.check("anchor_scoped_hook_installed", handle.installed());
        if (handle.installed())
        {
            const std::int32_t calls_before{ rc::get_anchor_calls() };
            const bool done{ drive(ctx) };
            ctx.check("anchor_probe_completed", done);
            ctx.check("anchor_detour_fired_once", g_anchor_fires.load() == 1);
            ctx.check("anchor_arg_decoded", g_arg_ok.load());
            // The factory built a non-null, valid wrapper of the registered type.
            ctx.check("anchor_self_nonnull_from_factory", g_self_nonnull.load());
            ctx.check("anchor_self_oop_valid", g_self_valid.load());
            // The decoded wrapper resolves rc's `marker` field at the right offset
            // and reads the fixture sentinel -> the made object decoded to the
            // REGISTERED wrapper type.
            ctx.check("anchor_decoded_marker_is_sentinel", g_self_marker_ok.load());
            ctx.record(std::string{ "[INFO] register_class live decode: marker read = 0x" }
                       + to_hex8(g_decoded_marker.load())
                       + " (expected 0x5AFE7A11).");
            // Allow-through: the original body ran, so the Java-visible counter
            // advanced by exactly one.
            ctx.check("anchor_allow_through_body_ran",
                      rc::get_anchor_calls() == calls_before + 1);
        }
        // scoped_hook `handle` uninstalls here at scope exit -- nothing left armed.
    }

    // =====================================================================
    //  9. POST-HOOK resolution still intact (registration state survives the
    //     hook lifecycle; nothing the hook did perturbed the maps).
    // =====================================================================
    {
        ctx.check("post_hook_rc_still_registered",
                  type_is_registered(std::type_index{ typeid(rc) }));
        ctx.check("post_hook_rc_static_field_still_resolves", rc::resolves("go"));
        ctx.check("post_hook_unregistered_still_unregistered",
                  !type_is_registered(std::type_index{ typeid(never_registered) }));
    }
}
