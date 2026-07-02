// register_class JVM test module  (feature area: type registration)
//
// THE authority for vmhook::register_class<T>(name) and the type-registration
// machinery it drives (vmhook.hpp):
//   * type_to_class_map        : unordered_map<type_index, string>   (decl ~1457)
//   * g_type_factory_map       : unordered_map<string, factory_fn>   (decl ~1479)
//   * registration_mutex       : std::mutex guarding both            (decl ~1458)
//   * register_class<T>(name)  : the install routine                 (~7151)
//
// register_class<T>(name) does, in order:
//   1. find_class(name) FIRST.  If it returns null -> log + return false and
//      NEITHER map is touched -> the type stays UNREGISTERED.
//   2. lock registration_mutex.
//   3. type_to_class_map.insert_or_assign(typeid(T), name)  -- LAST WINS.
//   4. g_type_factory_map.emplace(name, +[](void* oop){ return new T{oop}; })
//      -- std::map::emplace, so it is a NO-OP if `name` is already a key
//      (FIRST WINS).  This asymmetry between (3) insert_or_assign and (4) emplace
//      is a real library defect this module PINS; see BUG notes + lib_bugs.
//
// Downstream consumers of the maps that this module exercises end-to-end:
//   * object_base::resolve_klass(type_index) -> type_to_class_map.find ->
//     find_class.  Backs static_field / get_field / static_method / get_class_methods.
//   * get_class_methods<W>() -> type_to_class_map.find.
//   * find_methods_by_signature<W>() -> get_class_methods<W>().
//   * the FACTORY (g_type_factory_map) is consumed in exactly ONE place:
//     detail::extract_frame_arg<unique_ptr<W>> -> type_to_class_map[typeid W] ->
//     g_type_factory_map[class] -> factory(oop) -> static_cast<W*>.  i.e. a hook
//     callback whose receiver param is unique_ptr<W> is the only API that builds a
//     wrapper THROUGH the registered factory.  (field_proxy::get() -> unique_ptr<W>
//     and method_proxy return -> unique_ptr<W> both `new W{oop}` DIRECTLY off the
//     template param and never touch the factory map -- a distinction this module
//     documents and proves.)
//
// ─────────────────────────────────────────────────────────────────────────────
// SUITE-SAFETY (this module was QUARANTINED in Wave 3 for a matrix-wide JVM
// crash cascade; re-enabled here under the suite-safety rules in
// audit/PERFECTION_PROGRAM.md:400-403):
//
//   * REGISTRY MUTATIONS ARE PROVABLY CONTAINED.  Every wrapper type this module
//     registers (rc, alt_w, reborn_w, collide_a, collide_b) lives in THIS
//     translation unit's anonymous namespace -- no other module can name them, so
//     their type_to_class_map entries (keyed by type_index) are ADDITIVE and can
//     never clobber another module's own wrapper binding.  The g_type_factory_map
//     slots we touch (RC_CLASS, ALT_CLASS, java/lang/Object) are first-wins:
//     RC_CLASS's factory is consumed ONLY by this module's own section 8;
//     java/lang/Object's factory is NEVER consumed by anyone (make_java_array's
//     java_array_w and method_overload's java_object both bind it purely for
//     field_proxy::set/static-resolution and explicitly "do not rely on the
//     factory").  So nothing this module writes to the global registry can taint a
//     sibling module's decode path.  (The earlier quarantine grouped this module
//     with hook_reinstall_after_shutdown's mid-suite global shutdown_hooks(); THAT
//     was the cascade crasher, not register_class's additive registry writes.)
//
//   * ZERO HOOKS ARMED ON EXIT.  The ONLY hook is the section-8 scoped_hook<>,
//     which RAII-uninstalls at its inner-block scope exit.  The module ALSO ends
//     with an unconditional vmhook::shutdown_hooks() placed OUTSIDE the body
//     try/catch, so even if the body throws, control returns to the driver with an
//     empty hook table (mirrors aaa_warmup.cpp:228 and
//     shutdown_hooks_teardown.cpp:417).  A leaked armed hook is exactly what
//     cascaded into later modules in Wave 3; this module cannot leak one on ANY
//     path.
//
//   * NEVER CRASH, BAIL TO [INFO].  The whole body is wrapped in try/catch (a
//     throw is recorded as [INFO], never escapes -- mirrors aaa_warmup.cpp:209).
//     An ENTRY GUARD skips the module cleanly (record [INFO] + final
//     shutdown_hooks()) if RC_CLASS does not resolve, so the unguarded
//     static_field("go")->set(...) handshake derefs (same idiom as hook_basic) can
//     never deref a disengaged optional.  Every decoded oop in the detour is
//     is_valid_pointer-guarded before use; the dereferencing asserts in section 8
//     run only when the hook installed AND the probe completed.
//
//   * WARMTH.  aaa_warmup (priority::first) pays the i2i-patch / first-deopt /
//     compile-cycle cost and pre-resolves bootstrap classes (incl.
//     java/lang/Object) before any feature module runs, so section 8's anchor()
//     dispatch is not a cold-compile fault on the no-SEH MinGW/clang toolchains.
//
//   * NO FORCED GC.  This module/fixture never drives System.gc(), so the
//     forced-GC platform gate (field_introspection / dont_inline / global_ref) does
//     not apply -- there is no cold-forced-GC crash surface to guard.
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
#include <cstddef>
#include <cstdint>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>

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
    constexpr std::int64_t CLASS_TOKEN_LONG{ 0x0123456789ABCDEFLL };
    constexpr const char    CLASS_LABEL[]{ "RegisterClassFix" };

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

        // Unguarded handshake setters mirror hook_basic's idiom.  They are only
        // ever reached AFTER the module's entry guard has confirmed RC_CLASS
        // resolves, so static_field(...) is engaged here (no deref of a
        // disengaged optional).
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }
        static auto get_class_token() -> std::int32_t  { return static_field("classToken")->get(); }
        static auto get_anchor_calls() -> std::int32_t { return static_field("anchorCalls")->get(); }

        // Static-method resolution THROUGH the registered wrapper (the
        // resolve_klass -> _methods-walk consumer of type_to_class_map).
        // Resolution-only (.has_value()); never called, to stay heap-modest.
        static auto resolves_static_method(const char* name) -> bool
        {
            return static_method(name).has_value();
        }
        static auto resolves_static_method(const char* name, const char* sig) -> bool
        {
            return static_method(name, sig).has_value();
        }
        // Differently-typed static fields, each resolved through the SAME map path.
        static auto get_class_token_long() -> std::int64_t { return static_field("classTokenLong")->get(); }
        static auto get_class_flag() -> bool               { return static_field("classFlag")->get(); }
        static auto get_class_label() -> std::string       { return static_field("classLabel")->get(); }

        // Instance-side read of `marker` (used INSIDE the detour through the
        // factory-built wrapper).  Inherited get_field; safe on a valid oop.
        auto marker() const -> std::int32_t { return get_field("marker")->get(); }
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
    // registered FACTORY (extract_frame_arg).  We validate the wrapper points at a
    // real oop of the registered type by reading its `marker` field (offset
    // resolved against rc's klass).  Non-cancelling, so the original body runs
    // (allow-through).  Every dereference is guarded: self non-null, then the
    // decoded oop is is_valid_pointer-checked before marker() reads it.
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
        if (!ctx.run_probe)
        {
            return false;
        }
        return ctx.run_probe(
            [](bool value)
            {
                if (value) { rc::set_done(false); }
                rc::set_go(value);
            },
            []() { return rc::get_done(); });
    }

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks().
    auto run_register_class_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  If RegisterClassFix is not loaded/resolvable, every
        //  static_field()->set/get below would deref a disengaged optional.  Bail
        //  cleanly to [INFO] instead of dereferencing anything (the final
        //  shutdown_hooks() in the wrapper still runs).  In practice the harness
        //  loads RegisterClassFix on every run, so this is belt-and-braces.
        // =====================================================================
        if (vmhook::find_class(RC_CLASS) == nullptr)
        {
            ctx.record("[INFO] register_class: RegisterClassFix not loaded/resolvable "
                       "on this run; skipping the module's live checks (no crash, no "
                       "hooks armed).");
            return;
        }

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
            // `marker` is an INSTANCE field (public int marker), so static_field()
            // must NOT resolve it -- static_field only finds STATIC fields, and the
            // instance accessor get_field("marker") (used by the detour below) is the
            // correct path.  This pins that the registered-wrapper static accessor
            // correctly REJECTS an instance field (the static field classToken IS
            // resolved + value-checked just below).  (CI confirmed static_field
            // ("marker") returns no value; the prior assertion expected the opposite
            // and failed deterministically on every JDK.)
            ctx.check("registered_static_field_rejects_instance_marker", !rc::resolves("marker"));
            ctx.check("registered_static_field_classToken_value",
                      rc::get_class_token() == CLASS_TOKEN);

            // find_class(name) directly resolves the same klass.
            vmhook::hotspot::klass* const k{ vmhook::find_class(RC_CLASS) };
            ctx.check("find_class_resolves_registered_name", k != nullptr);

            // get_class_methods<W>() returns the declared methods (incl anchor).
            const auto methods{ vmhook::get_class_methods<rc>() };
            ctx.check("get_class_methods_nonempty_for_registered", !methods.empty());
            bool has_anchor{ false };
            for (const auto& entry : methods)
            {
                if (entry.first == "anchor") { has_anchor = true; break; }
            }
            ctx.check("get_class_methods_lists_anchor", has_anchor);

            // find_methods_by_signature<W>() -> the int->int anchor descriptor.
            // Both `anchor` (instance) and `staticAnchor` (static) share the (I)I
            // descriptor, so the result must contain BOTH names (it returns ALL
            // matches by contract -- proving descriptor matching is name-agnostic).
            const auto anchor_names{ vmhook::find_methods_by_signature<rc>("(I)I") };
            bool fm_has_anchor{ false };
            bool fm_has_static_anchor{ false };
            for (const std::string& nm : anchor_names)
            {
                if (nm == "anchor") { fm_has_anchor = true; }
                if (nm == "staticAnchor") { fm_has_static_anchor = true; }
            }
            ctx.check("find_methods_by_signature_finds_anchor", fm_has_anchor);
            ctx.check("find_methods_by_signature_finds_static_anchor", fm_has_static_anchor);
            // A descriptor that matches NO declared method -> empty (registered
            // type, wrong signature: distinct from the unregistered->empty case).
            ctx.check("find_methods_by_signature_unmatched_descriptor_empty",
                      vmhook::find_methods_by_signature<rc>("(DD)Ljava/lang/Object;").empty());

            // ---- static_method resolution THROUGH the registered wrapper -------
            // staticAnchor IS static -> resolves; the instance method anchor is
            // NOT static -> static_method must REJECT it (the JVM_ACC_STATIC gate
            // on the same resolve_klass path).  Resolution-only; never called.
            ctx.check("registered_static_method_resolves_staticAnchor",
                      rc::resolves_static_method("staticAnchor"));
            ctx.check("registered_static_method_rejects_instance_anchor",
                      !rc::resolves_static_method("anchor"));
            ctx.check("registered_static_method_with_sig_resolves_staticAnchor",
                      rc::resolves_static_method("staticAnchor", "(I)I"));
            ctx.check("registered_static_method_missing_name_nullopt",
                      !rc::resolves_static_method("noSuchStaticMethod_ZZZ"));

            // ---- differently-typed static fields all resolve via the map -------
            // Once the klass is reached through type_to_class_map, fields of any
            // descriptor (J / Z / Ljava/lang/String;) resolve and read correctly.
            ctx.check("registered_static_field_long_resolves",
                      rc::resolves("classTokenLong"));
            ctx.check("registered_static_field_long_value",
                      rc::get_class_token_long() == CLASS_TOKEN_LONG);
            ctx.check("registered_static_field_bool_resolves",
                      rc::resolves("classFlag"));
            ctx.check("registered_static_field_bool_value",
                      rc::get_class_flag() == true);
            ctx.check("registered_static_field_string_resolves",
                      rc::resolves("classLabel"));
            ctx.check("registered_static_field_string_value",
                      rc::get_class_label() == CLASS_LABEL);

            // ---- get_field(type_index, name) static overload directly ----------
            // static_field forwards to this; assert the type_index overload (the
            // raw resolve_klass consumer) resolves the same field independently.
            ctx.check("get_field_by_type_index_resolves_static",
                      vmhook::object_base::get_field(
                          std::type_index{ typeid(rc) }, "classToken").has_value());
            ctx.check("get_field_by_type_index_rejects_instance_marker",
                      !vmhook::object_base::get_field(
                          std::type_index{ typeid(rc) }, "marker").has_value());

            // ---- get_class_methods(string_view) by-NAME overload ---------------
            // This overload does NOT consult type_to_class_map (it is find_class
            // direct), so it must list anchor for the class name WHETHER OR NOT a
            // wrapper type is registered.  Contrast with the <W>() template above.
            const auto by_name{ vmhook::get_class_methods(RC_CLASS) };
            bool by_name_has_anchor{ false };
            for (const auto& entry : by_name)
            {
                if (entry.first == "anchor") { by_name_has_anchor = true; break; }
            }
            ctx.check("get_class_methods_by_name_lists_anchor", by_name_has_anchor);
            // The map-keyed template and the find_class-direct by-name overload
            // see the SAME class -> identical method-count for RC_CLASS.
            ctx.check("get_class_methods_template_matches_by_name_count",
                      methods.size() == by_name.size());
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
            // static_method on an unregistered wrapper -> nullopt (resolve_klass
            // misses the type map; another resolve_klass consumer beyond
            // static_field that must also degrade gracefully).
            ctx.check("unregistered_static_method_nullopt",
                      !never_registered::static_method("staticAnchor").has_value());
            ctx.check("unregistered_static_method_with_sig_nullopt",
                      !never_registered::static_method("staticAnchor", "(I)I").has_value());
            ctx.check("unregistered_get_field_by_type_index_nullopt",
                      !vmhook::object_base::get_field(
                          std::type_index{ typeid(never_registered) }, "classToken").has_value());
            // make_unique<W>() on an unregistered type -> nullptr (the type-map
            // miss is hit BEFORE any allocation; cheap, no Java object created).
            ctx.check("unregistered_make_unique_nullptr",
                      vmhook::make_unique<never_registered>() == nullptr);
            // The by-NAME get_class_methods overload is map-INDEPENDENT: it
            // resolves RC_CLASS by find_class regardless of which wrapper type (if
            // any) is registered, so it lists anchor even though never_registered
            // is not in the map.  This isolates the type-map's role to the <W>()
            // template path only.
            ctx.check("by_name_overload_is_map_independent",
                      !vmhook::get_class_methods(RC_CLASS).empty());
            // for_each_instance<W> must early-out to 0 visits for an unregistered
            // type (it returns 0 on the type_to_class_map miss BEFORE touching any
            // heap VMStruct) -- and must not crash.  The visitor is never called.
            const std::size_t visits{ vmhook::for_each_instance<never_registered>(
                [](std::unique_ptr<never_registered>) { /* never called */ }) };
            ctx.check("unregistered_for_each_instance_zero", visits == 0);
            // No factory entry was created for any class on behalf of this type.
            // (We cannot key by class -- the type was never mapped -- so the proof
            //  is simply that the type stays out of the type map, asserted above.)
        }

        // =====================================================================
        //  3. BOGUS class name -> register_class returns FALSE, type stays
        //     UNREGISTERED, accessors miss, no crash.  find_class fails BEFORE any
        //     insert, so neither map is touched.
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
            ctx.check("bogus_static_method_nullopt",
                      !bogus_w::static_method("staticAnchor").has_value());
            ctx.check("bogus_get_class_methods_empty",
                      vmhook::get_class_methods<bogus_w>().empty());
            ctx.check("bogus_make_unique_nullptr",
                      vmhook::make_unique<bogus_w>() == nullptr);
            // Empty-string class name: also a miss, also graceful.
            const bool empty_ok{ vmhook::register_class<bogus_w>("") };
            ctx.check("register_returns_false_for_empty_name", !empty_ok);
            ctx.check("bogus_type_still_unregistered_after_empty",
                      !type_is_registered(std::type_index{ typeid(bogus_w) }));
            ctx.check("empty_name_has_no_factory_entry",
                      factory_for("") == nullptr);
            // A null class-name pointer is degenerate input the API also rejects
            // (find_class fails first; no map touched, no crash).  string_view from
            // a null+0 is well-defined and yields an empty name -> same miss.
            const bool whitespace_ok{ vmhook::register_class<bogus_w>("   ") };
            ctx.check("register_returns_false_for_whitespace_name", !whitespace_ok);
            ctx.check("bogus_type_still_unregistered_after_whitespace",
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
        //     insert_or_assign re-points type_to_class_map to the NEW name.
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
            // reborn_w bound to RC sees RC's `classToken` field, NOT
            // FieldPrimitivesGet's `sIntZero`.
            ctx.check("reborn_on_RC_sees_RC_field",
                      reborn_w::static_field("classToken").has_value());
            ctx.check("reborn_on_RC_misses_ALT_only_field",
                      !reborn_w::static_field("sIntZero").has_value());
            // Method resolution also tracks the binding: bound to RC, reborn_w
            // resolves RC's staticAnchor.
            ctx.check("reborn_on_RC_resolves_RC_static_method",
                      reborn_w::static_method("staticAnchor").has_value());

            // Re-point reborn_w to ALT_CLASS.
            const bool second{ vmhook::register_class<reborn_w>(ALT_CLASS) };
            ctx.check("reborn_second_register_true", second);
            {
                std::string mapped{};
                type_maps_to(std::type_index{ typeid(reborn_w) }, mapped);
                ctx.check("reborn_second_maps_to_ALT_last_wins", mapped == ALT_CLASS);
            }
            // Now reborn_w resolves against ALT_CLASS: it sees `sIntZero` and NOT
            // the RC-only `classToken` -> proves the re-point took effect.
            ctx.check("reborn_on_ALT_sees_ALT_field",
                      reborn_w::static_field("sIntZero").has_value());
            ctx.check("reborn_on_ALT_misses_RC_only_field",
                      !reborn_w::static_field("classToken").has_value());
            // ...and after the re-point reborn_w's staticAnchor resolution FLIPS
            // to a miss (ALT_CLASS / FieldPrimitivesGet has no staticAnchor) --
            // a method-resolution witness of the re-point complementing the field
            // witness above.
            ctx.check("reborn_on_ALT_misses_RC_static_method",
                      !reborn_w::static_method("staticAnchor").has_value());
            // get_class_methods now reflects the ALT class.
            ctx.check("reborn_get_class_methods_nonempty_after_repoint",
                      !vmhook::get_class_methods<reborn_w>().empty());
            // BUG (low): the OLD class name's factory entry is never erased on a
            // re-point.  RC_CLASS still has a factory (from rc's baseline
            // registration AND from reborn's first register -- whichever landed
            // first; emplace kept the first).  We only assert RC_CLASS still HAS
            // some factory (the leak is benign here); the asymmetry itself is
            // pinned in section 7.
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
            // Method resolution is ALSO keyed per-wrapper through the map: rc
            // resolves its OWN staticAnchor; alt_w (FieldPrimitivesGet, which has
            // no such method) does NOT -- proving static_method's resolve_klass
            // follows each type's distinct binding, not a shared klass.
            ctx.check("two_classes_rc_resolves_own_static_method",
                      rc::resolves_static_method("staticAnchor"));
            ctx.check("two_classes_alt_misses_rc_static_method",
                      !alt_w::static_method("staticAnchor").has_value());
            // get_class_methods<W>() also tracks each wrapper's own class: rc's
            // list contains anchor; alt_w's does not.
            bool alt_has_anchor{ false };
            for (const auto& entry : vmhook::get_class_methods<alt_w>())
            {
                if (entry.first == "anchor") { alt_has_anchor = true; break; }
            }
            ctx.check("two_classes_alt_methods_miss_rc_anchor", !alt_has_anchor);
        }

        // =====================================================================
        //  7. FACTORY LAST-WINS.  Register collide_a to a shared name, then
        //     collide_b to the SAME name.  Both maps use insert_or_assign, so
        //     collide_b's factory now owns the slot (last-writer-wins).  This
        //     was previously a BUG PIN when g_type_factory_map used emplace
        //     (first-wins) — fixed in the lib to insert_or_assign so the two
        //     views stay lockstep-consistent on rebind.
        //
        //     We use java/lang/Object as the shared name: no module PINS its factory
        //     IDENTITY, and -- critically -- no module CONSUMES its factory (the two
        //     siblings that bind it, make_java_array's java_array_w and
        //     method_overload's java_object, both state they do not rely on the
        //     factory).  So our additive binding here cannot taint any sibling.
        //
        //     SAFETY: we NEVER route a live oop through this factory.  Doing so would
        //     build a `new collide_a` and static_cast it to `collide_b*`
        //     (extract_frame_arg) -- an invalid downcast between unrelated types.
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

            // type_to_class_map: BOTH types now map to the shared name (last write
            // per KEY wins, but they are different keys, so both are present).
            std::string a_name{};
            std::string b_name{};
            type_maps_to(std::type_index{ typeid(collide_a) }, a_name);
            type_maps_to(std::type_index{ typeid(collide_b) }, b_name);
            ctx.check("collide_a_maps_to_shared", a_name == shared);
            ctx.check("collide_b_maps_to_shared_too", b_name == shared);

            // FIXED: the factory slot IS overwritten when collide_b registers,
            // now that g_type_factory_map uses insert_or_assign (last-wins).
            // after_b differs from after_a (unless collide_b's factory happens
            // to alias, which the lambda-per-instantiation address rules out).
            ctx.check("collide_factory_changed_after_second_register",
                      after_b != after_a);
            // Last registrant owns the slot regardless of the pre-existing binding.
            ctx.check("collide_factory_owner_is_last_registrant",
                      after_b != pre || pre == nullptr);
            ctx.record("[INFO] register_class factory/type-map now last-wins on "
                       "both maps (type_to_class_map + g_type_factory_map use "
                       "insert_or_assign), keeping the two views consistent on "
                       "rebind. Historic first-wins-vs-last-wins asymmetry fixed.");
        }

        // =====================================================================
        //  8. LIVE FACTORY DECODE.  Install a scoped_hook on anchor(int); its detour
        //     receives `this` as unique_ptr<rc> built BY THE REGISTERED FACTORY
        //     (extract_frame_arg -> g_type_factory_map[RC_CLASS]).  Drive one probe;
        //     assert the decoded wrapper is the registered type with correct offsets.
        //
        //     This is the ONLY angle that exercises the factory MAP end-to-end on a
        //     real oop, and the ONLY Java-coordinated angle.  scoped_hook uninstalls
        //     on inner-block scope exit -- nothing armed afterward.  Every
        //     dereferencing assertion runs only when the hook installed AND the probe
        //     completed; the detour itself is_valid_pointer-guards the decoded oop.
        // =====================================================================
        {
            // Make sure rc is bound to RC_CLASS (sections above left it so, but be
            // explicit -- the factory the detour uses must be rc's).
            std::string rc_bound{};
            type_maps_to(std::type_index{ typeid(rc) }, rc_bound);
            ctx.check("anchor_precondition_rc_registered_to_RC", rc_bound == RC_CLASS);

            // Reset observation state so a re-run (or any prior fire) cannot leak in.
            g_anchor_fires.store(0);
            g_self_nonnull.store(false);
            g_self_valid.store(false);
            g_self_marker_ok.store(false);
            g_decoded_marker.store(0);
            g_arg_ok.store(false);

            auto handle{ vmhook::scoped_hook<rc>("anchor", &on_anchor) };
            ctx.check("anchor_scoped_hook_installed", handle.installed());
            if (handle.installed())
            {
                const std::int32_t calls_before{ rc::get_anchor_calls() };
                const bool done{ drive(ctx) };
                ctx.check("anchor_probe_completed", done);
                // Only read back observations when the probe actually completed;
                // otherwise the detour never ran and the asserts would be noise.
                if (done)
                {
                    ctx.check("anchor_detour_fired_once", g_anchor_fires.load() == 1);
                    ctx.check("anchor_arg_decoded", g_arg_ok.load());
                    // The factory built a non-null, valid wrapper of the registered type.
                    ctx.check("anchor_self_nonnull_from_factory", g_self_nonnull.load());
                    ctx.check("anchor_self_oop_valid", g_self_valid.load());
                    // The decoded wrapper resolves rc's `marker` field at the right
                    // offset and reads the fixture sentinel -> the made object
                    // decoded to the REGISTERED wrapper type.
                    ctx.check("anchor_decoded_marker_is_sentinel", g_self_marker_ok.load());
                    ctx.record(std::string{ "[INFO] register_class live decode: marker read = 0x" }
                               + to_hex8(g_decoded_marker.load())
                               + " (expected 0x5AFE7A11).");
                    // Allow-through: the original body ran, so the Java-visible
                    // counter advanced by exactly one.
                    ctx.check("anchor_allow_through_body_ran",
                              rc::get_anchor_calls() == calls_before + 1);
                }
                else
                {
                    ctx.record("[INFO] register_class: anchor probe did not complete "
                               "within the timeout; live-decode observations skipped "
                               "(hook still uninstalls at scope exit).");
                }
            }
            // scoped_hook `handle` uninstalls here at scope exit -- nothing armed.
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
}

VMHOOK_JVM_MODULE(register_class)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module (the harness also contains throws, but this
    // module's suite-safety contract is to be strictly harmless -- mirrors
    // aaa_warmup.cpp:209).  A throw is recorded as [INFO], never a FAIL.
    bool body_threw{ false };
    try
    {
        run_register_class_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  The
    // only hook (section 8's scoped_hook) already uninstalled at its scope exit;
    // this unconditional shutdown_hooks() guarantees an empty hook table even if
    // the body threw before reaching that scope exit (it is idempotent and
    // safe-when-empty -- proven by shutdown_hooks_teardown).  A leaked armed hook
    // is exactly what cascaded into later modules in Wave 3.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] register_class: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
