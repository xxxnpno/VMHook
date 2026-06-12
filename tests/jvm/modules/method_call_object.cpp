// method_call_object JVM test module — area: methods.
//
// Feature under test: method_proxy::call() that returns a Java reference type,
// converted to std::unique_ptr<wrapper>.  This is the "method-vs-field parity"
// path: field_proxy::value_t has always decoded a compressed OOP into a
// unique_ptr<wrapper> (vmhook.hpp, cast_for_variant ~14073); method_proxy::value_t
// now mirrors it (vmhook.hpp, operator target_type ~14935) so an Object-returning
// Java method can be assigned straight into a unique_ptr<wrapper> instead of
// silently yielding null.  This module is the EXHAUSTIVE "every possible object
// return" exercise of that conversion on a live JVM.
//
// EVERY OBJECT-RETURN SHAPE this module drives through method_proxy::call():
//
//   * NON-NULL object       -> usable wrapper: read a field (Child.tag/.label)
//                              AND call a method (Child.getTag()) through it,
//   * NULL object           -> null unique_ptr (monostate), on a method that can
//                              also be non-null (maybeChild) AND one that is
//                              unconditionally null (nullChild) AND a static one,
//   * FRESH each call       -> makeChild() new's a Child every call; two calls
//                              decode to two DISTINCT instances (distinct OOPs),
//   * method-vs-field PARITY -> getChild() (method) and the `child` field (field)
//                              decode to the SAME heap object (pointer compare AND
//                              published identityHashCode cross-check),
//   * SELF identity         -> self() returns `this`; wrapper instance == receiver,
//   * POLYMORPHIC           -> makeAnimal() is declared Animal, returns a Dog; the
//                              decoded wrapper sees the RUNTIME klass (Dog) and a
//                              virtual method (speak()) dispatches to the override,
//   * BOXED                 -> boxedInt() returns Integer.valueOf(N) as Object; the
//                              wrapper is usable and intValue() dispatches through it,
//   * STATIC                -> staticMakeChild()/staticNullChild() (the static-call
//                              path of an Object-returning call()),
//   * ARRAY references      -> childArray() ('[L'), intArray() ('[I'), objectArray()
//                              ('[Ljava/lang/Object;'): the value_t decodes the
//                              array oop (void* conversion), and the module walks
//                              array_length()/get_array_element() to read it,
//   * CHAINED               -> getChild() -> Child, then Child.makeSibling() -> Child:
//                              the unique_ptr<wrapper> from the first call is the
//                              receiver of a SECOND object-returning call,
//   * STRING reference      -> childLabel() lands in the std::string alternative,
//                              NOT the uint32 OOP alternative (routing proof).
//
// PATH NOTE (no longer a correctness gate).  method_proxy::call() uses HotSpot's
// _call_stub_entry when present (JDK 8-20), where a non-String reference return
// is stored as a real compressed OOP (encode_oop_pointer, vmhook.hpp ~15955).
// On JDK 21+ that VMStruct is absent (and CI exercises NO JDK that exports it),
// so call() falls back to call_jni(); its 'L'/'[' branch (vmhook.hpp ~15660-15666)
// now DECODES the JNI local-ref handle to the underlying heap OOP
// (jni_decode_object), re-encodes it (encode_oop_pointer), and DeleteLocalRef's
// the handle AFTER the decode — so a non-null reference return round-trips into a
// usable wrapper on BOTH paths.  (An older header truncated/freed the handle;
// that flaw is FIXED and the sibling method_call_jni_fallback module asserts the
// same usable-wrapper contract unconditionally.)  This module therefore HARD-
// asserts the full usable-wrapper contract on every JDK and only RECORDS which
// dispatch path was taken as [INFO].
//
// SUITE-SAFETY (this module runs inside the shared suite; later modules run after
// it, so it must leave NOTHING armed and never crash the process):
//   * The whole body runs under try/catch -> a stray throw becomes [INFO], never
//     escapes (mirrors field_object_ref.cpp / register_class.cpp).
//   * An UNCONDITIONAL vmhook::shutdown_hooks() runs OUTSIDE the try, so even a
//     throw before the scoped_hook's scope-exit leaves an empty hook table.
//   * An ENTRY GUARD bails cleanly to [INFO] if the fixture is not loaded, so the
//     unguarded static_field()->... handshake derefs never touch a disengaged
//     optional.
//   * Every raw deref of a decoded oop / klass is gated by is_valid_pointer.
//   * The ONLY hook is a scoped_hook<> that RAII-uninstalls at its block scope.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    constexpr char FIXTURE[]{ "vmhook/fixtures/MethodObject" };

    // Wrapper for vmhook.fixtures.MethodObject$Child.  Has the fields/method the
    // returned-wrapper usability checks read THROUGH a method-returned wrapper,
    // plus makeSibling() so a returned Child can itself be the receiver of a
    // SECOND object-returning call (the chained-call angle).  Accessors are the
    // documented clean one-liner idiom (no sentinel guards inside accessors).
    class child_object : public vmhook::object<child_object>
    {
    public:
        explicit child_object(vmhook::oop_t instance) noexcept
            : vmhook::object<child_object>{ instance }
        {
        }

        // Read a field through this wrapper (the "read a field through it" half
        // of the usable-wrapper contract).
        auto get_tag()   -> std::int32_t { return get_field("tag")->get(); }
        auto get_label() -> std::string  { return get_field("label")->get(); }

        // Call a method through this wrapper (the "call a method through it"
        // half — proves the decoded OOP is a real, dispatch-capable object).
        auto call_get_tag() -> std::int32_t { return get_method("getTag")->call(); }

        // Object-returning call THROUGH this (itself method-returned) wrapper:
        // the chained-call probe.  The result is another unique_ptr<child_object>.
        auto make_sibling() -> std::unique_ptr<child_object> { return get_method("makeSibling")->call(); }
    };

    // Wrapper registered for vmhook.fixtures.MethodObject$Dog — the CONCRETE
    // runtime type returned by makeAnimal() (declared Animal).  speak() is the
    // overridden virtual method dispatched through the runtime-typed wrapper.
    class dog_object : public vmhook::object<dog_object>
    {
    public:
        explicit dog_object(vmhook::oop_t instance) noexcept
            : vmhook::object<dog_object>{ instance }
        {
        }

        auto breed_id() -> std::int32_t { return get_field("breedId")->get(); }
        auto speak()    -> std::string  { return get_method("speak")->call().as_string(); }
    };

    // Wrapper for java.lang.Integer — the boxed-type Object return.  int_value()
    // dispatches Integer.intValue() through the method-decoded wrapper.
    class integer_object : public vmhook::object<integer_object>
    {
    public:
        explicit integer_object(vmhook::oop_t instance) noexcept
            : vmhook::object<integer_object>{ instance }
        {
        }

        auto int_value() -> std::int32_t { return get_method("intValue")->call(); }
    };

    // Wrapper for vmhook.fixtures.MethodObject.  Drives the object-returning
    // calls and the field-path baseline.  Accessors are the clean one-liner idiom.
    class method_object : public vmhook::object<method_object>
    {
    public:
        explicit method_object(vmhook::oop_t instance) noexcept
            : vmhook::object<method_object>{ instance }
        {
        }

        // ── go/done handshake ──────────────────────────────────────────────
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ── published identities (exact cross-checks) ──────────────────────
        static auto self_identity()         -> std::int32_t { return static_field("selfIdentity")->get(); }
        static auto child_identity()        -> std::int32_t { return static_field("childIdentity")->get(); }
        static auto static_child_identity() -> std::int32_t { return static_field("staticChildIdentity")->get(); }
        static auto animal_identity()       -> std::int32_t { return static_field("animalIdentity")->get(); }

        // ── method-path object returns (the FEATURE) ───────────────────────
        auto make_child()   -> std::unique_ptr<child_object>  { return get_method("makeChild")->call(); }
        auto get_child()    -> std::unique_ptr<child_object>  { return get_method("getChild")->call(); }
        auto self_proxy()   -> std::unique_ptr<method_object> { return get_method("self")->call(); }
        auto make_animal()  -> std::unique_ptr<dog_object>    { return get_method("makeAnimal")->call(); }
        auto boxed_int()    -> std::unique_ptr<integer_object> { return get_method("boxedInt")->call(); }
        auto maybe_child(bool present) -> std::unique_ptr<child_object>
        {
            return get_method("maybeChild")->call(present);
        }
        auto null_child()   -> std::unique_ptr<child_object>  { return get_method("nullChild")->call(); }
        // as_string() (not the implicit conversion) disambiguates std::string vs
        // const char* on MSVC — the value_t conversion operator can yield both.
        auto child_label()  -> std::string                    { return get_method("childLabel")->call().as_string(); }
        // The polymorphic Dog's overridden speak(), via the base receiver — the
        // Java-side ground truth for the override-dispatch cross-check.
        auto animal_sound() -> std::string                    { return get_method("getAnimalSound")->call().as_string(); }

        // ── field-path baseline (always works; no call_stub dependency) ────
        auto field_child() -> std::unique_ptr<child_object> { return get_field("child")->get(); }
    };

    // ── observations captured inside the tick() detour ─────────────────────
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_self_ok{ false };

    // make_child(): non-null usable wrapper + distinct-each-call identity
    std::atomic<bool>           g_make_nonnull{ false };
    std::atomic<std::int32_t>   g_make_tag{ -1 };
    std::atomic<bool>           g_make_label_ok{ false };
    std::atomic<std::int32_t>   g_make_method_tag{ -1 };
    std::atomic<std::uintptr_t> g_make_instance_a{ 0 };
    std::atomic<std::uintptr_t> g_make_instance_b{ 0 };
    std::atomic<bool>           g_make_b_nonnull{ false };

    // get_child() (method) vs field_child (field): parity
    std::atomic<bool>           g_getchild_nonnull{ false };
    std::atomic<std::uintptr_t> g_getchild_instance{ 0 };
    std::atomic<std::int32_t>   g_getchild_tag{ -1 };
    std::atomic<bool>           g_field_nonnull{ false };
    std::atomic<std::uintptr_t> g_field_instance{ 0 };
    std::atomic<std::int32_t>   g_field_tag{ -1 };

    // self(): identity
    std::atomic<bool>           g_selfproxy_nonnull{ false };
    std::atomic<std::uintptr_t> g_selfproxy_instance{ 0 };
    std::atomic<std::uintptr_t> g_receiver_instance{ 0 };

    // null contract
    std::atomic<bool> g_maybe_true_nonnull{ false };
    std::atomic<std::int32_t> g_maybe_true_tag{ -1 };
    std::atomic<bool> g_maybe_false_null{ false };
    std::atomic<bool> g_nullchild_null{ false };

    // static object returns
    std::atomic<bool>           g_static_nonnull{ false };
    std::atomic<std::int32_t>   g_static_tag{ -1 };
    std::atomic<std::uintptr_t> g_static_instance{ 0 };
    std::atomic<bool>           g_static_null_is_null{ false };

    // polymorphic return (declared Animal, runtime Dog)
    std::atomic<bool>           g_animal_nonnull{ false };
    std::atomic<std::int32_t>   g_animal_breed{ -1 };
    std::string                 g_animal_speak{};            // override result via wrapper
    std::string                 g_animal_speak_java{};       // ground truth via base receiver
    std::string                 g_animal_klass{};            // runtime klass name of decoded oop
    std::atomic<std::uintptr_t> g_animal_instance{ 0 };

    // boxed Integer return
    std::atomic<bool>           g_boxed_nonnull{ false };
    std::atomic<std::int32_t>   g_boxed_value{ -1 };
    std::string                 g_boxed_klass{};

    // chained call: getChild() -> makeSibling()
    std::atomic<bool>           g_sibling_nonnull{ false };
    std::atomic<std::int32_t>   g_sibling_tag{ -1 };
    std::atomic<bool>           g_sibling_label_ok{ false };
    std::atomic<std::uintptr_t> g_sibling_instance{ 0 };

    // array reference returns ('[L' Child[], '[I' int[], '[L Object;' Object[])
    std::atomic<bool>           g_childarray_decoded_nonnull{ false };
    std::atomic<std::int32_t>   g_childarray_len{ -1 };
    std::atomic<std::int32_t>   g_childarray_elem0_tag{ -1 };
    std::atomic<std::int32_t>   g_childarray_elem2_tag{ -1 };
    std::atomic<bool>           g_intarray_decoded_nonnull{ false };
    std::atomic<std::int32_t>   g_intarray_len{ -1 };
    std::atomic<std::int32_t>   g_intarray_elem0{ -1 };
    std::atomic<std::int32_t>   g_intarray_elem3{ -1 };
    std::atomic<bool>           g_objectarray_decoded_nonnull{ false };
    std::atomic<std::int32_t>   g_objectarray_len{ -1 };
    std::atomic<std::int32_t>   g_objectarray_elem1_tag{ -1 };

    // String reference return (std::string alternative)
    std::atomic<bool> g_label_ok{ false };

    // value_t introspection (is_void / is_string) sanity
    std::atomic<bool> g_isvoid_on_null{ false };
    std::atomic<bool> g_isstring_on_label{ false };
    std::atomic<bool> g_isvoid_on_object{ false };
    std::atomic<bool> g_isstring_on_object{ false };

    // call-path taken
    std::atomic<bool> g_call_stub_present{ false };

    constexpr std::int32_t k_child_tag    = 0x5EED;
    constexpr std::int32_t k_maybe_tag    = 0x1234;
    constexpr std::int32_t k_static_tag   = 0x7AC0;
    constexpr std::int32_t k_sibling_tag  = 0x51B;
    constexpr std::int32_t k_dog_breed    = 0x0D06;
    constexpr std::int32_t k_boxed_value  = 0x07E5;
    constexpr std::int32_t k_array_tag_0  = 100;
    constexpr std::int32_t k_array_tag_1  = 200;
    constexpr std::int32_t k_array_tag_2  = 300;
    constexpr std::int32_t k_array_len    = 3;
    constexpr std::int32_t k_int_array_0  = 11;
    constexpr std::int32_t k_int_array_3  = 44;
    constexpr std::int32_t k_int_array_len = 4;
    const std::string      k_child_label   = "child-of-method";
    const std::string      k_sibling_label = "sibling-of-child";
    const std::string      k_label_string  = "label-via-method";
    const std::string      k_dog_sound     = "woof";

    // Internal name of the runtime klass behind an oop, or "" if unresolvable.
    // Used to prove a polymorphic / boxed return's decoded oop carries the
    // CONCRETE runtime type (never the declared type).  Fully guarded.
    auto runtime_klass_name(void* oop) -> std::string
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

    // True if `haystack` ends with `suffix` (small helper for klass-name checks).
    auto ends_with(const std::string& haystack, const std::string& suffix) -> bool
    {
        return haystack.size() >= suffix.size()
            && haystack.compare(haystack.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // ── the detour body: drive every object-returning call on `self` ────────
    // Runs on the Java thread inside the tick() detour, where current_java_thread
    // is set (call() requires it).  Captures into file-scope atomics; the module
    // body asserts.  GC-SENSITIVITY: every OOP a call() returns is decoded from a
    // fresh JNI local ref (call_jni) / the call-stub result holder and is only
    // tick-scoped.  We read it IMMEDIATELY (no Java allocation between the call()
    // and the reads through the wrapper) so no GC can relocate it underneath us;
    // a hypothetical relocation mid-read is the documented best-effort edge (see
    // the [INFO] in the module body).
    auto drive_calls(const std::unique_ptr<method_object>& self) -> void
    {
        g_detour_calls.fetch_add(1, std::memory_order_relaxed);
        if (!self)
        {
            return;
        }
        g_self_ok.store(true, std::memory_order_relaxed);
        g_receiver_instance.store(
            reinterpret_cast<std::uintptr_t>(self->get_instance()),
            std::memory_order_relaxed);

        // ── make_child(): non-null usable wrapper + distinct-each-call ─────
        {
            std::unique_ptr<child_object> made{ self->make_child() };
            g_make_nonnull.store(made != nullptr, std::memory_order_relaxed);
            if (made)
            {
                g_make_instance_a.store(
                    reinterpret_cast<std::uintptr_t>(made->get_instance()),
                    std::memory_order_relaxed);
                g_make_tag.store(made->get_tag(), std::memory_order_relaxed);
                g_make_label_ok.store(made->get_label() == k_child_label,
                                      std::memory_order_relaxed);
                // method THROUGH the method-returned wrapper:
                g_make_method_tag.store(made->call_get_tag(),
                                        std::memory_order_relaxed);
            }
            // A SECOND makeChild() allocates a DISTINCT Child -> distinct OOP.
            std::unique_ptr<child_object> made_b{ self->make_child() };
            g_make_b_nonnull.store(made_b != nullptr, std::memory_order_relaxed);
            if (made_b)
            {
                g_make_instance_b.store(
                    reinterpret_cast<std::uintptr_t>(made_b->get_instance()),
                    std::memory_order_relaxed);
            }
        }

        // ── get_child() (method) instance + tag, and CHAINED makeSibling() ─
        {
            std::unique_ptr<child_object> mc{ self->get_child() };
            g_getchild_nonnull.store(mc != nullptr, std::memory_order_relaxed);
            if (mc)
            {
                g_getchild_instance.store(
                    reinterpret_cast<std::uintptr_t>(mc->get_instance()),
                    std::memory_order_relaxed);
                g_getchild_tag.store(mc->get_tag(), std::memory_order_relaxed);

                // CHAINED: an object-returning call THROUGH the method-returned
                // wrapper.  The unique_ptr<child_object> from get_child() is the
                // receiver of a second method_proxy::call() returning an object.
                std::unique_ptr<child_object> sib{ mc->make_sibling() };
                g_sibling_nonnull.store(sib != nullptr, std::memory_order_relaxed);
                if (sib)
                {
                    g_sibling_instance.store(
                        reinterpret_cast<std::uintptr_t>(sib->get_instance()),
                        std::memory_order_relaxed);
                    g_sibling_tag.store(sib->get_tag(), std::memory_order_relaxed);
                    g_sibling_label_ok.store(sib->get_label() == k_sibling_label,
                                             std::memory_order_relaxed);
                }
            }
        }

        // ── field_child (field path baseline) instance + tag ───────────────
        {
            std::unique_ptr<child_object> fc{ self->field_child() };
            g_field_nonnull.store(fc != nullptr, std::memory_order_relaxed);
            if (fc)
            {
                g_field_instance.store(
                    reinterpret_cast<std::uintptr_t>(fc->get_instance()),
                    std::memory_order_relaxed);
                g_field_tag.store(fc->get_tag(), std::memory_order_relaxed);
            }
        }

        // ── self(): returned wrapper instance == receiver ──────────────────
        {
            std::unique_ptr<method_object> sp{ self->self_proxy() };
            g_selfproxy_nonnull.store(sp != nullptr, std::memory_order_relaxed);
            if (sp)
            {
                g_selfproxy_instance.store(
                    reinterpret_cast<std::uintptr_t>(sp->get_instance()),
                    std::memory_order_relaxed);
            }
        }

        // ── polymorphic return: declared Animal, runtime Dog ───────────────
        {
            g_animal_speak_java = self->animal_sound();   // Java ground truth
            std::unique_ptr<dog_object> animal{ self->make_animal() };
            g_animal_nonnull.store(animal != nullptr, std::memory_order_relaxed);
            if (animal)
            {
                void* const inst{ animal->get_instance() };
                g_animal_instance.store(reinterpret_cast<std::uintptr_t>(inst),
                                        std::memory_order_relaxed);
                g_animal_klass = runtime_klass_name(inst);
                g_animal_breed.store(animal->breed_id(), std::memory_order_relaxed);
                // Virtual dispatch THROUGH the runtime-typed wrapper: speak()
                // must reach the Dog override, not Animal's base.
                g_animal_speak = animal->speak();
            }
        }

        // ── boxed Integer return ───────────────────────────────────────────
        {
            std::unique_ptr<integer_object> boxed{ self->boxed_int() };
            g_boxed_nonnull.store(boxed != nullptr, std::memory_order_relaxed);
            if (boxed)
            {
                g_boxed_klass = runtime_klass_name(boxed->get_instance());
                g_boxed_value.store(boxed->int_value(), std::memory_order_relaxed);
            }
        }

        // ── null contract on a method that can be non-null too ─────────────
        {
            std::unique_ptr<child_object> present{ self->maybe_child(true) };
            g_maybe_true_nonnull.store(present != nullptr, std::memory_order_relaxed);
            if (present)
            {
                g_maybe_true_tag.store(present->get_tag(), std::memory_order_relaxed);
            }
            std::unique_ptr<child_object> absent{ self->maybe_child(false) };
            g_maybe_false_null.store(absent == nullptr, std::memory_order_relaxed);
        }

        // ── unconditional null return ──────────────────────────────────────
        {
            std::unique_ptr<child_object> nc{ self->null_child() };
            g_nullchild_null.store(nc == nullptr, std::memory_order_relaxed);

            // is_void() must be true for a null reference return (call stores
            // monostate when the OOP is null).
            auto nm{ self->get_method("nullChild") };
            if (nm)
            {
                const auto v{ nm->call() };
                g_isvoid_on_null.store(v.is_void(), std::memory_order_relaxed);
            }
        }

        // ── static object returns ──────────────────────────────────────────
        {
            auto sm{ method_object::static_method("staticMakeChild") };
            if (sm)
            {
                std::unique_ptr<child_object> sc = sm->call();
                g_static_nonnull.store(sc != nullptr, std::memory_order_relaxed);
                if (sc)
                {
                    g_static_instance.store(
                        reinterpret_cast<std::uintptr_t>(sc->get_instance()),
                        std::memory_order_relaxed);
                    g_static_tag.store(sc->get_tag(), std::memory_order_relaxed);
                }
            }
            auto sn{ method_object::static_method("staticNullChild") };
            if (sn)
            {
                std::unique_ptr<child_object> sc = sn->call();
                g_static_null_is_null.store(sc == nullptr, std::memory_order_relaxed);
            }
        }

        // ── array reference returns: walk the decoded array oop ────────────
        // For each array-returning method we take the value_t's void* conversion
        // (decode_oop_pointer) to the ARRAY oop, then use the public array
        // helpers to read length + elements.  '[L' object arrays store one
        // compressed OOP per element (decode each, wrap as a Child, read tag);
        // '[I' stores raw int32 elements.
        {
            // Child[] ('[L' descriptor).
            auto am{ self->get_method("childArray") };
            if (am)
            {
                void* const arr{ static_cast<void*>(am->call()) };
                g_childarray_decoded_nonnull.store(arr != nullptr,
                                                   std::memory_order_relaxed);
                if (arr && vmhook::hotspot::is_valid_pointer(arr))
                {
                    g_childarray_len.store(vmhook::array_length(arr),
                                           std::memory_order_relaxed);
                    const std::uint32_t e0{ vmhook::get_array_element<std::uint32_t>(arr, 0) };
                    const std::uint32_t e2{ vmhook::get_array_element<std::uint32_t>(arr, 2) };
                    void* const e0_oop{ vmhook::hotspot::decode_oop_pointer(e0) };
                    void* const e2_oop{ vmhook::hotspot::decode_oop_pointer(e2) };
                    if (e0_oop && vmhook::hotspot::is_valid_pointer(e0_oop))
                    {
                        child_object c0{ e0_oop };
                        g_childarray_elem0_tag.store(c0.get_tag(), std::memory_order_relaxed);
                    }
                    if (e2_oop && vmhook::hotspot::is_valid_pointer(e2_oop))
                    {
                        child_object c2{ e2_oop };
                        g_childarray_elem2_tag.store(c2.get_tag(), std::memory_order_relaxed);
                    }
                }
            }

            // int[] ('[I' descriptor) — primitive elements read directly.
            auto im{ self->get_method("intArray") };
            if (im)
            {
                void* const arr{ static_cast<void*>(im->call()) };
                g_intarray_decoded_nonnull.store(arr != nullptr,
                                                 std::memory_order_relaxed);
                if (arr && vmhook::hotspot::is_valid_pointer(arr))
                {
                    g_intarray_len.store(vmhook::array_length(arr),
                                         std::memory_order_relaxed);
                    g_intarray_elem0.store(vmhook::get_array_element<std::int32_t>(arr, 0),
                                           std::memory_order_relaxed);
                    g_intarray_elem3.store(vmhook::get_array_element<std::int32_t>(arr, 3),
                                           std::memory_order_relaxed);
                }
            }

            // Object[] ('[Ljava/lang/Object;' descriptor).
            auto om{ self->get_method("objectArray") };
            if (om)
            {
                void* const arr{ static_cast<void*>(om->call()) };
                g_objectarray_decoded_nonnull.store(arr != nullptr,
                                                    std::memory_order_relaxed);
                if (arr && vmhook::hotspot::is_valid_pointer(arr))
                {
                    g_objectarray_len.store(vmhook::array_length(arr),
                                            std::memory_order_relaxed);
                    const std::uint32_t e1{ vmhook::get_array_element<std::uint32_t>(arr, 1) };
                    void* const e1_oop{ vmhook::hotspot::decode_oop_pointer(e1) };
                    if (e1_oop && vmhook::hotspot::is_valid_pointer(e1_oop))
                    {
                        child_object c1{ e1_oop };
                        g_objectarray_elem1_tag.store(c1.get_tag(), std::memory_order_relaxed);
                    }
                }
            }
        }

        // ── String reference return + value_t routing introspection ────────
        {
            g_label_ok.store(self->child_label() == k_label_string,
                             std::memory_order_relaxed);
            auto lm{ self->get_method("childLabel") };
            if (lm)
            {
                const auto v{ lm->call() };
                g_isstring_on_label.store(v.is_string(), std::memory_order_relaxed);
            }
            auto im{ self->get_method("getChild") };
            if (im)
            {
                const auto v{ im->call() };
                // An Object (non-String) return must NOT be is_void() when the
                // OOP is non-null, and must NOT be is_string() (it lands in the
                // uint32 OOP alternative).
                g_isvoid_on_object.store(v.is_void(), std::memory_order_relaxed);
                g_isstring_on_object.store(v.is_string(), std::memory_order_relaxed);
            }
        }
    }

    // The whole body, factored out so the module wrapper can run it under a
    // try/catch and ALWAYS follow it with shutdown_hooks().
    auto run_method_call_object_checks(vmhook_test::context& ctx) -> void
    {
        // ── ENTRY GUARD ────────────────────────────────────────────────────
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] method_call_object: MethodObject not loaded/resolvable "
                       "on this run; skipping live checks (no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<method_object>(FIXTURE);
        // The nested types are "Outer$Inner" in JVM internal form.
        vmhook::register_class<child_object>("vmhook/fixtures/MethodObject$Child");
        vmhook::register_class<dog_object>("vmhook/fixtures/MethodObject$Dog");
        // Boxed-type wrapper: java.lang.Integer is a bootstrap class, always loaded.
        vmhook::register_class<integer_object>("java/lang/Integer");

        g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr,
                                  std::memory_order_relaxed);

        {
            // Hook tick(); inside the detour, drive every object-returning call on
            // `self` via method_proxy::call() so they dispatch on a live OOP with
            // current_java_thread set (call() requires being inside a detour).
            auto handle{ vmhook::scoped_hook<method_object>(
                "tick",
                [](vmhook::return_value&,
                   const std::unique_ptr<method_object>& self,
                   std::int32_t /*nonce*/)
                {
                    drive_calls(self);
                }) };

            ctx.check("mco_hook_installed", handle.installed());

            method_object::set_mode(0);
            const bool done{ ctx.run_probe(
                [](bool value) { method_object::set_go(value); },
                []() { return method_object::get_done(); }) };

            ctx.check("mco_probe_completed", done);
            ctx.check("mco_detour_fired",
                      g_detour_calls.load(std::memory_order_relaxed) >= 1);
            ctx.check("mco_detour_saw_self",
                      g_self_ok.load(std::memory_order_relaxed));
            // scoped_hook `handle` uninstalls here at scope exit — nothing armed.
        }

        const bool stub{ g_call_stub_present.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] call path: " }
                   + (stub ? "call_stub fast path (StubRoutines::_call_stub_entry present) — "
                             "reference returns are real compressed OOPs"
                           : "call_jni fallback (Call(Static)?ObjectMethodA) — reference returns "
                             "are JNI handles decoded to the heap OOP then re-encoded; CI's path"));
        ctx.record("[INFO] java identities: self=" + std::to_string(method_object::self_identity())
                   + " child=" + std::to_string(method_object::child_identity())
                   + " staticChild=" + std::to_string(method_object::static_child_identity())
                   + " animal=" + std::to_string(method_object::animal_identity()));
        ctx.record("[INFO] GC-sensitivity: every method-returned OOP is tick-scoped; the detour "
                   "reads through each wrapper IMMEDIATELY (no intervening Java allocation), so a "
                   "GC relocation mid-read is a documented best-effort edge, not exercised here.");

        // The whole point of this module — and the reason every assertion below
        // is HARD on both paths — is that the call_jni 'L'/'[' arm now decodes the
        // JNI handle to the real heap OOP (jni_decode_object) and re-encodes it,
        // so a non-null reference return is a usable wrapper on EVERY JDK.

        // ════════════════ NULL CONTRACT (the key invariant) ════════════════
        // A null Java return must NEVER fabricate a wrapper, and must be is_void().
        ctx.check("mco_nullchild_returns_null_unique_ptr",
                  g_nullchild_null.load(std::memory_order_relaxed));
        ctx.check("mco_maybechild_false_returns_null_unique_ptr",
                  g_maybe_false_null.load(std::memory_order_relaxed));
        ctx.check("mco_staticnullchild_returns_null_unique_ptr",
                  g_static_null_is_null.load(std::memory_order_relaxed));
        ctx.check("mco_value_t_is_void_true_for_null_reference_return",
                  g_isvoid_on_null.load(std::memory_order_relaxed));

        // ════════════════ value_t ALTERNATIVE ROUTING (String vs Object) ════
        // String returns are eagerly decoded to the std::string alternative; a
        // non-null Object return lands in the uint32 OOP alternative.
        ctx.check("mco_string_return_value_equals_expected",
                  g_label_ok.load(std::memory_order_relaxed));
        ctx.check("mco_value_t_is_string_true_for_string_return",
                  g_isstring_on_label.load(std::memory_order_relaxed));
        ctx.check("mco_value_t_object_return_not_void",
                  !g_isvoid_on_object.load(std::memory_order_relaxed));
        ctx.check("mco_value_t_object_return_not_string",
                  !g_isstring_on_object.load(std::memory_order_relaxed));

        // ════════════════ FIELD-PATH BASELINE (no call_stub dependency) ═════
        // Proves the fixture, the registration, and the value_t unique_ptr decode
        // are all sound, independent of the call path.
        ctx.check("mco_field_path_child_non_null",
                  g_field_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_field_path_child_tag_correct",
                  g_field_tag.load(std::memory_order_relaxed) == k_child_tag);

        // ════════════════ NON-NULL -> USABLE WRAPPER (make_child) ═══════════
        ctx.check("mco_makechild_non_null_wrapper",
                  g_make_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_makechild_field_read_through_wrapper",
                  g_make_tag.load(std::memory_order_relaxed) == k_child_tag);
        ctx.check("mco_makechild_label_read_through_wrapper",
                  g_make_label_ok.load(std::memory_order_relaxed));
        ctx.check("mco_makechild_method_call_through_wrapper",
                  g_make_method_tag.load(std::memory_order_relaxed) == k_child_tag);

        // ════════════════ FRESH-EACH-CALL -> DISTINCT IDENTITIES ════════════
        // makeChild() new's a Child every call: the two calls must decode to two
        // DIFFERENT non-null instances.
        ctx.check("mco_makechild_second_call_non_null",
                  g_make_b_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_makechild_distinct_instances_each_call",
                  g_make_instance_a.load(std::memory_order_relaxed) != 0
                  && g_make_instance_b.load(std::memory_order_relaxed) != 0
                  && g_make_instance_a.load(std::memory_order_relaxed)
                         != g_make_instance_b.load(std::memory_order_relaxed));

        // ════════════════ getChild() + method-vs-field PARITY ══════════════
        ctx.check("mco_getchild_non_null_wrapper",
                  g_getchild_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_getchild_tag_correct",
                  g_getchild_tag.load(std::memory_order_relaxed) == k_child_tag);
        // getChild() (method) and `child` (field) decode to the SAME heap object.
        ctx.check("mco_method_vs_field_same_instance",
                  g_getchild_instance.load(std::memory_order_relaxed) != 0
                  && g_getchild_instance.load(std::memory_order_relaxed)
                         == g_field_instance.load(std::memory_order_relaxed));
        // And Java's published identityHashCode for `child` is non-zero (proving
        // the probe ran and the parity object is the one the fixture published).
        ctx.check("mco_child_identity_published",
                  method_object::child_identity() != 0);

        // ════════════════ self() IDENTITY ══════════════════════════════════
        ctx.check("mco_self_return_non_null_wrapper",
                  g_selfproxy_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_self_return_instance_equals_receiver",
                  g_selfproxy_instance.load(std::memory_order_relaxed) != 0
                  && g_selfproxy_instance.load(std::memory_order_relaxed)
                         == g_receiver_instance.load(std::memory_order_relaxed));

        // ════════════════ maybeChild(true): non-null branch ════════════════
        ctx.check("mco_maybechild_true_non_null_wrapper",
                  g_maybe_true_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_maybechild_true_tag_correct",
                  g_maybe_true_tag.load(std::memory_order_relaxed) == k_maybe_tag);

        // ════════════════ STATIC object return ═════════════════════════════
        ctx.check("mco_staticmakechild_non_null_wrapper",
                  g_static_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_staticmakechild_tag_correct",
                  g_static_tag.load(std::memory_order_relaxed) == k_static_tag);

        // ════════════════ POLYMORPHIC return (declared Animal, runtime Dog) ══
        ctx.check("mco_animal_non_null_wrapper",
                  g_animal_nonnull.load(std::memory_order_relaxed));
        // The decoded oop's RUNTIME klass is Dog (not the declared Animal) — the
        // wrapper sees the concrete type.
        ctx.check("mco_animal_runtime_klass_is_Dog",
                  ends_with(g_animal_klass, "MethodObject$Dog"));
        // A Dog-only field read through the wrapper (Animal has no breedId).
        ctx.check("mco_animal_subclass_field_read_through_wrapper",
                  g_animal_breed.load(std::memory_order_relaxed) == k_dog_breed);
        // Virtual dispatch THROUGH the wrapper reaches the Dog override; it must
        // equal both the expected constant AND the Java-side ground truth.
        ctx.check("mco_animal_virtual_dispatch_hits_override",
                  g_animal_speak == k_dog_sound
                  && g_animal_speak == g_animal_speak_java);

        // ════════════════ BOXED Integer return ═════════════════════════════
        ctx.check("mco_boxed_int_non_null_wrapper",
                  g_boxed_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_boxed_int_runtime_klass_is_Integer",
                  ends_with(g_boxed_klass, "Integer"));
        // intValue() dispatched through the method-decoded Integer wrapper.
        ctx.check("mco_boxed_int_value_through_wrapper",
                  g_boxed_value.load(std::memory_order_relaxed) == k_boxed_value);

        // ════════════════ CHAINED call (getChild -> makeSibling) ═══════════
        // The unique_ptr<child_object> from getChild() was itself the receiver of
        // a SECOND object-returning call().
        ctx.check("mco_chained_sibling_non_null_wrapper",
                  g_sibling_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_chained_sibling_tag_correct",
                  g_sibling_tag.load(std::memory_order_relaxed) == k_sibling_tag);
        ctx.check("mco_chained_sibling_label_correct",
                  g_sibling_label_ok.load(std::memory_order_relaxed));
        // The sibling is a DISTINCT object from the Child it was made on.
        ctx.check("mco_chained_sibling_distinct_from_child",
                  g_sibling_instance.load(std::memory_order_relaxed) != 0
                  && g_sibling_instance.load(std::memory_order_relaxed)
                         != g_getchild_instance.load(std::memory_order_relaxed));

        // ════════════════ ARRAY reference returns ('[L', '[I', '[LObject;') ══
        // Child[] ('[L'): decoded oop non-null, length, and element tags read by
        // walking the array (decode each element's compressed OOP, wrap, read tag).
        ctx.check("mco_childarray_reference_decoded_non_null",
                  g_childarray_decoded_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_childarray_length_correct",
                  g_childarray_len.load(std::memory_order_relaxed) == k_array_len);
        ctx.check("mco_childarray_elem0_tag_correct",
                  g_childarray_elem0_tag.load(std::memory_order_relaxed) == k_array_tag_0);
        ctx.check("mco_childarray_elem2_tag_correct",
                  g_childarray_elem2_tag.load(std::memory_order_relaxed) == k_array_tag_2);

        // int[] ('[I'): decoded oop non-null, length, primitive elements.
        ctx.check("mco_intarray_reference_decoded_non_null",
                  g_intarray_decoded_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_intarray_length_correct",
                  g_intarray_len.load(std::memory_order_relaxed) == k_int_array_len);
        ctx.check("mco_intarray_elem0_correct",
                  g_intarray_elem0.load(std::memory_order_relaxed) == k_int_array_0);
        ctx.check("mco_intarray_elem3_correct",
                  g_intarray_elem3.load(std::memory_order_relaxed) == k_int_array_3);

        // Object[] ('[Ljava/lang/Object;'): decoded oop non-null, length, an
        // element (a Child) read through the array.
        ctx.check("mco_objectarray_reference_decoded_non_null",
                  g_objectarray_decoded_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_objectarray_length_correct",
                  g_objectarray_len.load(std::memory_order_relaxed) == k_array_len);
        ctx.check("mco_objectarray_elem1_tag_correct",
                  g_objectarray_elem1_tag.load(std::memory_order_relaxed) == k_array_tag_1);

        // ── breadcrumbs (never affect pass/fail) ───────────────────────────
        ctx.record("[INFO] animal runtime klass = " + g_animal_klass
                   + " (declared Animal, decoded Dog); speak() via wrapper = '" + g_animal_speak
                   + "', via Java = '" + g_animal_speak_java + "'");
        ctx.record("[INFO] boxed runtime klass = " + g_boxed_klass
                   + " value=" + std::to_string(g_boxed_value.load(std::memory_order_relaxed)));
        ctx.record("[INFO] makeChild distinct OOPs: a=0x"
                   + std::to_string(g_make_instance_a.load(std::memory_order_relaxed))
                   + " b=0x" + std::to_string(g_make_instance_b.load(std::memory_order_relaxed)));
    }
}

VMHOOK_JVM_MODULE(method_call_object)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module (mirrors field_object_ref.cpp's suite-safety
    // contract).  A throw is recorded as [INFO], never a [FAIL].
    bool body_threw{ false };
    try
    {
        run_method_call_object_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs.  Later modules run after
    // this one, so it MUST leave ZERO hooks armed.  The only hook (the tick
    // scoped_hook) already uninstalled at its scope exit; this unconditional
    // shutdown_hooks() guarantees an empty hook table even if the body threw
    // before reaching that scope exit (idempotent + safe-when-empty).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] method_call_object: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial results.");
    }
    ctx.check("mco_module_left_clean", true);
}
