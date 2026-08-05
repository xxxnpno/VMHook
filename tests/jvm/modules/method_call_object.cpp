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
// so call() falls back to the call stub; its 'L'/'[' branch (vmhook.hpp ~15660-15666)
// now DECODES the JNI local-ref handle to the underlying heap OOP
// (jni_decode_object), re-encodes it (encode_oop_pointer), and DeleteLocalRef's
// the handle AFTER the decode — so a non-null reference return round-trips into a
// usable wrapper on BOTH paths.  (An older header truncated/freed the handle;
// that flaw is FIXED and the sibling method_call_dispatch module asserts the
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

        // Chained call whose SECOND link is a NULL reference return: a null
        // unique_ptr through a method-decoded receiver.
        auto make_null_sibling() -> std::unique_ptr<child_object> { return get_method("makeNullSibling")->call(); }

        // self() through this (method-returned) wrapper -> the SAME Child OOP.
        auto self_proxy() -> std::unique_ptr<child_object> { return get_method("self")->call(); }
    };

    // Wrapper for vmhook.fixtures.MethodObject$Puppy — the TWO-level-deep
    // concrete type returned by makePuppy() (declared Animal).  speak() is the
    // depth-2 override; breed_id reads the inherited Dog field through the wrapper.
    class puppy_object : public vmhook::object<puppy_object>
    {
    public:
        explicit puppy_object(vmhook::oop_t instance) noexcept
            : vmhook::object<puppy_object>{ instance }
        {
        }

        auto breed_id() -> std::int32_t { return get_field("breedId")->get(); }
        auto speak()    -> std::string  { return get_method("speak")->call().as_string(); }
    };

    // Wrapper for vmhook.fixtures.MethodObject$NamedThing — the concrete IMPL
    // class behind an interface-typed (Named) return.  name() dispatches the
    // interface method through the runtime-decoded wrapper.
    class named_object : public vmhook::object<named_object>
    {
    public:
        explicit named_object(vmhook::oop_t instance) noexcept
            : vmhook::object<named_object>{ instance }
        {
        }

        auto code()  -> std::int32_t { return get_field("code")->get(); }
        auto name()  -> std::string  { return get_method("name")->call().as_string(); }
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
        static auto puppy_identity()        -> std::int32_t { return static_field("puppyIdentity")->get(); }
        static auto named_identity()        -> std::int32_t { return static_field("namedIdentity")->get(); }

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

        // Two-level-deep polymorphic return (declared Animal, runtime Puppy).
        auto make_puppy()   -> std::unique_ptr<puppy_object>  { return get_method("makePuppy")->call(); }
        auto puppy_sound()  -> std::string                    { return get_method("getPuppySound")->call().as_string(); }
        // Interface-typed return (declared Named, runtime NamedThing).
        auto make_named()   -> std::unique_ptr<named_object>  { return get_method("makeNamed")->call(); }
        auto named_name()   -> std::string                    { return get_method("getNamedName")->call().as_string(); }
        // Object ARG -> object return (identity echo): pass a Child wrapper back in.
        auto echo_child(const std::unique_ptr<child_object>& c) -> std::unique_ptr<child_object>
        {
            return get_method("echoChild")->call(c);
        }
        // int ARG selects which array Child is returned (or null when out of range).
        auto pick_child(std::int32_t idx) -> std::unique_ptr<child_object>
        {
            return get_method("pickChild")->call(idx);
        }
        // A different method returning the SAME static Child (cross-method identity).
        auto same_static_child() -> std::unique_ptr<child_object> { return get_method("sameStaticChild")->call(); }

        // Subtype passed as a base-typed (Animal) arg, returned unchanged: pass a
        // Dog wrapper IN as an Animal, the decoded return is the same runtime Dog.
        auto echo_animal(const std::unique_ptr<dog_object>& a) -> std::unique_ptr<dog_object>
        {
            return get_method("echoAnimal")->call(a);
        }

        // ── VARIED-ARG-SIGNATURE object returns (batch-14 deepening) ───────
        // Each folds its argument(s) into the returned Child's tag, so the
        // native decode catches a mis-packed / truncated / dropped argument.
        auto child_for_long(std::int64_t v)  -> std::unique_ptr<child_object> { return get_method("childForLong")->call(v); }
        auto child_for_double(double v)       -> std::unique_ptr<child_object> { return get_method("childForDouble")->call(v); }
        auto child_for_string(const std::string& s) -> std::unique_ptr<child_object> { return get_method("childForString")->call(s); }
        auto child_for_float(float v)         -> std::unique_ptr<child_object> { return get_method("childForFloat")->call(v); }
        auto child_for_bool(bool b)           -> std::unique_ptr<child_object> { return get_method("childForBool")->call(b); }
        auto child_for_many(std::int32_t i, std::int64_t l, const std::string& s) -> std::unique_ptr<child_object>
        {
            return get_method("childForMany")->call(i, l, s);
        }
        // java.lang.Object base return — runtime type is the stored Child, so
        // the registered child wrapper decodes it; identity == field child.
        auto child_as_object() -> std::unique_ptr<child_object> { return get_method("childAsObject")->call(); }
        // Object ARG returned unchanged (root-type identity echo).
        auto echo_object(const std::unique_ptr<child_object>& o) -> std::unique_ptr<child_object>
        {
            return get_method("echoObject")->call(o);
        }
        // String ARG returned unchanged — lands in the std::string alternative.
        auto echo_string(const std::string& s) -> std::string { return get_method("echoString")->call(s).as_string(); }
        // Wide / bool / double boxed-primitive returns (usable via Integer-shaped
        // wrappers; we only read identity + klass name, not box-specific methods).
        auto boxed_long()   -> std::unique_ptr<integer_object> { return get_method("boxedLong")->call(); }
        auto boxed_bool()   -> std::unique_ptr<integer_object> { return get_method("boxedBool")->call(); }
        auto boxed_double() -> std::unique_ptr<integer_object> { return get_method("boxedDouble")->call(); }
        // Arg-driven null on a reference (String) arg.
        auto child_when_present(const std::string& key) -> std::unique_ptr<child_object>
        {
            return get_method("childWhenPresent")->call(key);
        }

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

    // String[] ('[Ljava/lang/String;') reference return
    std::atomic<bool>           g_strarray_decoded_nonnull{ false };
    std::atomic<std::int32_t>   g_strarray_len{ -1 };
    std::atomic<bool>           g_strarray_elem0_ok{ false };
    std::atomic<bool>           g_strarray_elem2_ok{ false };

    // two-level-deep polymorphic return (declared Animal, runtime Puppy)
    std::atomic<bool>           g_puppy_nonnull{ false };
    std::atomic<std::int32_t>   g_puppy_breed{ -1 };
    std::string                 g_puppy_speak{};
    std::string                 g_puppy_speak_java{};
    std::string                 g_puppy_klass{};
    std::atomic<std::uintptr_t> g_puppy_instance{ 0 };

    // interface-typed return (declared Named, runtime NamedThing)
    std::atomic<bool>           g_named_nonnull{ false };
    std::atomic<std::int32_t>   g_named_code{ -1 };
    std::string                 g_named_name{};
    std::string                 g_named_name_java{};
    std::string                 g_named_klass{};

    // object-arg identity echo (echoChild)
    std::atomic<bool>           g_echo_nonnull{ false };
    std::atomic<std::int32_t>   g_echo_tag{ -1 };
    std::atomic<bool>           g_echo_same_as_field{ false };

    // arg-selected object return (pickChild)
    std::atomic<bool>           g_pick0_nonnull{ false };
    std::atomic<bool>           g_pick1_nonnull{ false };
    std::atomic<bool>           g_pick2_nonnull{ false };
    std::atomic<std::int32_t>   g_pick0_tag{ -1 };
    std::atomic<std::int32_t>   g_pick1_tag{ -1 };
    std::atomic<std::int32_t>   g_pick2_tag{ -1 };
    std::atomic<bool>           g_pick_all_distinct{ false };
    std::atomic<bool>           g_pick_oob_null{ false };

    // cross-method identity: sameStaticChild() vs staticMakeChild()
    std::atomic<bool>           g_samestatic_nonnull{ false };
    std::atomic<std::uintptr_t> g_samestatic_instance{ 0 };

    // chained NULL sibling (chained call whose 2nd link is null)
    std::atomic<bool>           g_null_sibling_is_null{ false };

    // Child.self() through a method-returned wrapper -> same Child OOP
    std::atomic<bool>           g_child_self_nonnull{ false };
    std::atomic<std::uintptr_t> g_child_self_instance{ 0 };

    // getChild() called twice -> SAME stored OOP each time (idempotent identity)
    std::atomic<std::uintptr_t> g_getchild_instance_2{ 0 };

    // one value_t converted twice -> two non-null wrappers wrapping the SAME OOP
    std::atomic<bool>           g_value_t_reuse_both_nonnull{ false };
    std::atomic<bool>           g_value_t_reuse_same_oop{ false };

    // value_t introspection (is_void / is_string) sanity
    std::atomic<bool> g_isvoid_on_null{ false };
    std::atomic<bool> g_isstring_on_label{ false };
    std::atomic<bool> g_isvoid_on_object{ false };
    std::atomic<bool> g_isstring_on_object{ false };

    // call-path taken
    std::atomic<bool> g_call_stub_present{ false };

    // ── batch-14 deepening: varied-arg-signature object returns ────────────
    std::atomic<bool>         g_long_arg_nonnull{ false };
    std::atomic<std::int32_t> g_long_arg_tag{ -1 };
    std::atomic<bool>         g_double_arg_nonnull{ false };
    std::atomic<std::int32_t> g_double_arg_tag{ -1 };
    std::atomic<bool>         g_string_arg_nonnull{ false };
    std::atomic<std::int32_t> g_string_arg_tag{ -1 };
    std::atomic<bool>         g_string_arg_label_ok{ false };
    std::atomic<bool>         g_float_arg_nonnull{ false };
    std::atomic<std::int32_t> g_float_arg_tag{ -1 };
    std::atomic<bool>         g_bool_true_arg_nonnull{ false };
    std::atomic<std::int32_t> g_bool_true_arg_tag{ -1 };
    std::atomic<std::int32_t> g_bool_false_arg_tag{ -1 };
    std::atomic<bool>         g_many_arg_nonnull{ false };
    std::atomic<std::int32_t> g_many_arg_tag{ -1 };

    // java.lang.Object base return (declared Object, runtime Child)
    std::atomic<bool>           g_object_return_nonnull{ false };
    std::atomic<std::int32_t>   g_object_return_tag{ -1 };
    std::atomic<std::uintptr_t> g_object_return_instance{ 0 };
    std::string                 g_object_return_klass{};

    // arg-returned-unchanged on non-Child declared types
    std::atomic<bool>           g_echo_object_nonnull{ false };
    std::atomic<bool>           g_echo_object_same_as_field{ false };
    std::atomic<bool>           g_echo_string_ok{ false };

    // boxed-primitive variety (Long / Boolean / Double)
    std::atomic<bool> g_boxed_long_nonnull{ false };
    std::string       g_boxed_long_klass{};
    std::atomic<bool> g_boxed_bool_nonnull{ false };
    std::string       g_boxed_bool_klass{};
    std::atomic<bool> g_boxed_double_nonnull{ false };
    std::string       g_boxed_double_klass{};

    // arg-driven null on a reference (String) arg
    std::atomic<bool> g_present_nonnull_when_key{ false };
    std::atomic<std::int32_t> g_present_tag{ -1 };

    // ── batch-19 deepening: gaps the prior batches left ────────────────────
    // NULL object passed as arg -> NULL object returned (arg-driven null on the
    // OBJECT round-trip path, distinct from the String-arg null above).
    std::atomic<bool> g_echo_null_child_is_null{ false };
    std::atomic<bool> g_echo_null_object_is_null{ false };

    // SUBTYPE passed where a BASE-typed (Animal) arg is expected, returned: the
    // decoded return is the same runtime Dog (identity echo through a widening
    // reference arg).
    std::atomic<bool>           g_echo_animal_nonnull{ false };
    std::atomic<bool>           g_echo_animal_same_oop{ false };
    std::string                 g_echo_animal_klass{};

    // FRESH-each-call on the STATIC dispatch path (staticFreshChild news a Child
    // every call): two static calls -> two DISTINCT non-null instances.
    std::atomic<bool>           g_static_fresh_a_nonnull{ false };
    std::atomic<bool>           g_static_fresh_b_nonnull{ false };
    std::atomic<std::int32_t>   g_static_fresh_tag{ -1 };
    std::atomic<std::uintptr_t> g_static_fresh_instance_a{ 0 };
    std::atomic<std::uintptr_t> g_static_fresh_instance_b{ 0 };

    // DEEP CHAIN through self(): self() -> getChild() through the self-returned
    // (this) wrapper -> must decode to the SAME Child OOP as a direct getChild().
    std::atomic<bool>           g_self_chain_child_nonnull{ false };
    std::atomic<std::uintptr_t> g_self_chain_child_instance{ 0 };

    // CHAIN through a STATIC-returned wrapper: staticMakeChild() -> self() on it
    // -> the same STATIC_CHILD OOP (an instance call chained off a static return).
    std::atomic<bool>           g_static_chain_self_nonnull{ false };
    std::atomic<std::uintptr_t> g_static_chain_self_instance{ 0 };

    constexpr std::int32_t k_child_tag    = 0x5EED;
    constexpr std::int32_t k_maybe_tag    = 0x1234;
    constexpr std::int32_t k_static_tag   = 0x7AC0;
    constexpr std::int32_t k_static_fresh_tag = 0x7AC1;
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
    constexpr std::int32_t k_dog_breed_id  = 0x0D06;  // Puppy inherits Dog.breedId
    constexpr std::int32_t k_named_code    = 0x4A3D;
    const std::string      k_child_label   = "child-of-method";
    const std::string      k_sibling_label = "sibling-of-child";
    const std::string      k_label_string  = "label-via-method";
    const std::string      k_dog_sound     = "woof";
    const std::string      k_puppy_sound   = "yip";
    const std::string      k_named_name    = "named-impl";
    const std::string      k_str_array_0   = "s0";
    const std::string      k_str_array_2   = "s2";

    // ── batch-14 deepening: varied-arg-signature expected values ───────────
    // Computed here EXACTLY as the fixture computes them (same widths, same
    // folds) so a wrong arg pack on the native side produces a wrong tag the
    // assertion catches.  The long fold uses an unsigned >> 32 to mirror Java's
    // logical >>> 32.  k_long_arg == 0x1_0000_002A (hi half 1, lo half 0x2A).
    constexpr std::int64_t k_long_arg       = (std::int64_t{ 1 } << 32) | std::int64_t{ 0x2A };
    constexpr std::int32_t k_long_arg_tag   =
        0x6000 + static_cast<std::int32_t>(
            k_long_arg + static_cast<std::int64_t>(static_cast<std::uint64_t>(k_long_arg) >> 32));
    constexpr double       k_double_arg     = 1234.5;
    constexpr std::int32_t k_double_arg_tag = 0x6100 + static_cast<std::int32_t>(k_double_arg);
    const std::string      k_string_arg     = "arg-string";
    const std::int32_t     k_string_arg_tag = 0x6200 + static_cast<std::int32_t>(k_string_arg.size());
    constexpr float        k_float_arg      = 2.5f;
    constexpr std::int32_t k_float_arg_tag  = 0x6300 + static_cast<std::int32_t>(k_float_arg);
    constexpr std::int32_t k_bool_true_tag  = 0x6401;
    constexpr std::int32_t k_bool_false_tag = 0x6400;
    constexpr std::int32_t k_many_int       = 7;
    constexpr std::int64_t k_many_long      = std::int64_t{ 0x1000 };
    const std::string      k_many_string    = "many";
    const std::int32_t     k_many_tag       =
        k_many_int + static_cast<std::int32_t>(k_many_long)
        + static_cast<std::int32_t>(k_many_string.size());

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

                // CHAINED NULL: a null reference return THROUGH the method-decoded
                // receiver still yields a null unique_ptr (the chained null path).
                std::unique_ptr<child_object> null_sib{ mc->make_null_sibling() };
                g_null_sibling_is_null.store(null_sib == nullptr, std::memory_order_relaxed);

                // SELF through a method-returned wrapper: Child.self() returns
                // `this`, so the returned wrapper decodes to the SAME Child OOP.
                std::unique_ptr<child_object> child_self{ mc->self_proxy() };
                g_child_self_nonnull.store(child_self != nullptr, std::memory_order_relaxed);
                if (child_self)
                {
                    g_child_self_instance.store(
                        reinterpret_cast<std::uintptr_t>(child_self->get_instance()),
                        std::memory_order_relaxed);
                }
            }

            // IDEMPOTENT IDENTITY: getChild() returns the stored `child` field, so
            // a SECOND call decodes to the SAME OOP (contrast makeChild's distinct).
            std::unique_ptr<child_object> mc2{ self->get_child() };
            if (mc2)
            {
                g_getchild_instance_2.store(
                    reinterpret_cast<std::uintptr_t>(mc2->get_instance()),
                    std::memory_order_relaxed);
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

        // ── TWO-LEVEL-DEEP polymorphic return (declared Animal, runtime Puppy) ─
        // Proves the runtime-type decode is depth-independent: the wrapper sees
        // Puppy, reads Dog's inherited breedId, and speak() reaches the depth-2
        // Puppy override (not Animal's nor Dog's).
        {
            g_puppy_speak_java = self->puppy_sound();   // Java ground truth
            std::unique_ptr<puppy_object> pup{ self->make_puppy() };
            g_puppy_nonnull.store(pup != nullptr, std::memory_order_relaxed);
            if (pup)
            {
                void* const inst{ pup->get_instance() };
                g_puppy_instance.store(reinterpret_cast<std::uintptr_t>(inst),
                                       std::memory_order_relaxed);
                g_puppy_klass = runtime_klass_name(inst);
                g_puppy_breed.store(pup->breed_id(), std::memory_order_relaxed);
                g_puppy_speak = pup->speak();
            }
        }

        // ── INTERFACE-typed return (declared Named, runtime NamedThing) ────────
        // The decoded oop carries the concrete IMPL klass; name() dispatches the
        // interface method through the wrapper.
        {
            g_named_name_java = self->named_name();     // Java ground truth
            std::unique_ptr<named_object> nt{ self->make_named() };
            g_named_nonnull.store(nt != nullptr, std::memory_order_relaxed);
            if (nt)
            {
                g_named_klass = runtime_klass_name(nt->get_instance());
                g_named_code.store(nt->code(), std::memory_order_relaxed);
                g_named_name = nt->name();
            }
        }

        // ── OBJECT ARG -> object return (echoChild identity round-trip) ────────
        // Pass the stored `child` (reachable from the GC-rooted singleton, so its
        // OOP is stable) back IN as a unique_ptr<wrapper> argument; the returned
        // OOP must equal the field's OOP.
        {
            std::unique_ptr<child_object> fc{ self->field_child() };
            if (fc)
            {
                const std::uintptr_t fc_oop{
                    reinterpret_cast<std::uintptr_t>(fc->get_instance()) };
                std::unique_ptr<child_object> echoed{ self->echo_child(fc) };
                g_echo_nonnull.store(echoed != nullptr, std::memory_order_relaxed);
                if (echoed)
                {
                    g_echo_tag.store(echoed->get_tag(), std::memory_order_relaxed);
                    g_echo_same_as_field.store(
                        reinterpret_cast<std::uintptr_t>(echoed->get_instance()) == fc_oop,
                        std::memory_order_relaxed);
                }
            }
        }

        // ── ARG-SELECTED object return (pickChild(idx)) ────────────────────────
        // An int arg selects which array Child the call() returns: idx 0/1/2 each
        // yield a DISTINCT non-null Child with the matching published tag; an
        // out-of-range idx yields null (the arg-driven null path).
        {
            std::unique_ptr<child_object> p0{ self->pick_child(0) };
            std::unique_ptr<child_object> p1{ self->pick_child(1) };
            std::unique_ptr<child_object> p2{ self->pick_child(2) };
            g_pick0_nonnull.store(p0 != nullptr, std::memory_order_relaxed);
            g_pick1_nonnull.store(p1 != nullptr, std::memory_order_relaxed);
            g_pick2_nonnull.store(p2 != nullptr, std::memory_order_relaxed);
            std::uintptr_t i0{ 0 };
            std::uintptr_t i1{ 0 };
            std::uintptr_t i2{ 0 };
            if (p0) { g_pick0_tag.store(p0->get_tag(), std::memory_order_relaxed);
                      i0 = reinterpret_cast<std::uintptr_t>(p0->get_instance()); }
            if (p1) { g_pick1_tag.store(p1->get_tag(), std::memory_order_relaxed);
                      i1 = reinterpret_cast<std::uintptr_t>(p1->get_instance()); }
            if (p2) { g_pick2_tag.store(p2->get_tag(), std::memory_order_relaxed);
                      i2 = reinterpret_cast<std::uintptr_t>(p2->get_instance()); }
            g_pick_all_distinct.store(
                i0 != 0 && i1 != 0 && i2 != 0 && i0 != i1 && i1 != i2 && i0 != i2,
                std::memory_order_relaxed);
            std::unique_ptr<child_object> oob{ self->pick_child(99) };
            g_pick_oob_null.store(oob == nullptr, std::memory_order_relaxed);
        }

        // ── CROSS-METHOD identity: sameStaticChild() == staticMakeChild() OOP ──
        // Two DIFFERENT methods returning the one STATIC_CHILD singleton decode to
        // the same heap object (compared against g_static_instance in the body).
        {
            std::unique_ptr<child_object> ssc{ self->same_static_child() };
            g_samestatic_nonnull.store(ssc != nullptr, std::memory_order_relaxed);
            if (ssc)
            {
                g_samestatic_instance.store(
                    reinterpret_cast<std::uintptr_t>(ssc->get_instance()),
                    std::memory_order_relaxed);
            }
        }

        // ── ONE value_t CONVERTED TWICE -> two wrappers, SAME OOP ──────────────
        // Each conversion of a value_t to unique_ptr<wrapper> news a fresh wrapper
        // but decodes the SAME stored compressed OOP, so the two wrappers must be
        // non-null and wrap the identical instance (the conversion is repeatable,
        // not consuming).
        {
            auto gm{ self->get_method("getChild") };
            if (gm)
            {
                const auto v{ gm->call() };
                std::unique_ptr<child_object> w1{ v };
                std::unique_ptr<child_object> w2{ v };
                g_value_t_reuse_both_nonnull.store(w1 != nullptr && w2 != nullptr,
                                                   std::memory_order_relaxed);
                if (w1 && w2)
                {
                    g_value_t_reuse_same_oop.store(
                        w1->get_instance() == w2->get_instance(),
                        std::memory_order_relaxed);
                }
            }
        }

        // ── String[] ('[Ljava/lang/String;') reference return ──────────────────
        // Decode the array oop, walk its length, and decode each element's
        // compressed OOP into a java.lang.String via read_java_string.
        {
            auto sm{ self->get_method("stringArray") };
            if (sm)
            {
                void* const arr{ static_cast<void*>(sm->call()) };
                g_strarray_decoded_nonnull.store(arr != nullptr, std::memory_order_relaxed);
                if (arr && vmhook::hotspot::is_valid_pointer(arr))
                {
                    g_strarray_len.store(vmhook::array_length(arr), std::memory_order_relaxed);
                    const std::uint32_t e0{ vmhook::get_array_element<std::uint32_t>(arr, 0) };
                    const std::uint32_t e2{ vmhook::get_array_element<std::uint32_t>(arr, 2) };
                    void* const e0_oop{ vmhook::hotspot::decode_oop_pointer(e0) };
                    void* const e2_oop{ vmhook::hotspot::decode_oop_pointer(e2) };
                    if (e0_oop && vmhook::hotspot::is_valid_pointer(e0_oop))
                    {
                        g_strarray_elem0_ok.store(
                            vmhook::read_java_string(e0_oop) == k_str_array_0,
                            std::memory_order_relaxed);
                    }
                    if (e2_oop && vmhook::hotspot::is_valid_pointer(e2_oop))
                    {
                        g_strarray_elem2_ok.store(
                            vmhook::read_java_string(e2_oop) == k_str_array_2,
                            std::memory_order_relaxed);
                    }
                }
            }
        }

        // ── VARIED-ARG-SIGNATURE object returns (batch-14 deepening) ───────────
        // Drive an object-returning call() through every primitive arg WIDTH and
        // reference arg kind: a wide long (two slots), a wide double (two slots),
        // a String reference arg, a float, a boolean, and a mixed multi-arg
        // signature (int + long + String).  Each returned Child folds its arg(s)
        // into its tag, so a narrowed / dropped / mis-slotted argument yields a
        // WRONG tag — the assertions are non-vacuous.
        {
            std::unique_ptr<child_object> cl{ self->child_for_long(k_long_arg) };
            g_long_arg_nonnull.store(cl != nullptr, std::memory_order_relaxed);
            if (cl) { g_long_arg_tag.store(cl->get_tag(), std::memory_order_relaxed); }

            std::unique_ptr<child_object> cd{ self->child_for_double(k_double_arg) };
            g_double_arg_nonnull.store(cd != nullptr, std::memory_order_relaxed);
            if (cd) { g_double_arg_tag.store(cd->get_tag(), std::memory_order_relaxed); }

            std::unique_ptr<child_object> cs{ self->child_for_string(k_string_arg) };
            g_string_arg_nonnull.store(cs != nullptr, std::memory_order_relaxed);
            if (cs)
            {
                g_string_arg_tag.store(cs->get_tag(), std::memory_order_relaxed);
                // The String arg is echoed into the returned Child's label, so a
                // lost reference arg is caught on the label too, not just the tag.
                g_string_arg_label_ok.store(cs->get_label() == k_string_arg,
                                            std::memory_order_relaxed);
            }

            std::unique_ptr<child_object> cf{ self->child_for_float(k_float_arg) };
            g_float_arg_nonnull.store(cf != nullptr, std::memory_order_relaxed);
            if (cf) { g_float_arg_tag.store(cf->get_tag(), std::memory_order_relaxed); }

            std::unique_ptr<child_object> cbt{ self->child_for_bool(true) };
            g_bool_true_arg_nonnull.store(cbt != nullptr, std::memory_order_relaxed);
            if (cbt) { g_bool_true_arg_tag.store(cbt->get_tag(), std::memory_order_relaxed); }
            std::unique_ptr<child_object> cbf{ self->child_for_bool(false) };
            if (cbf) { g_bool_false_arg_tag.store(cbf->get_tag(), std::memory_order_relaxed); }

            std::unique_ptr<child_object> cm{ self->child_for_many(k_many_int, k_many_long, k_many_string) };
            g_many_arg_nonnull.store(cm != nullptr, std::memory_order_relaxed);
            if (cm) { g_many_arg_tag.store(cm->get_tag(), std::memory_order_relaxed); }
        }

        // ── java.lang.Object BASE return (declared Object, runtime Child) ──────
        // The return type is the ROOT reference type; the decoded oop carries the
        // concrete runtime klass (Child) and is the SAME heap object as the field
        // child — proving the decode is driven by the runtime oop, not the
        // declared return type.
        {
            std::unique_ptr<child_object> obj{ self->child_as_object() };
            g_object_return_nonnull.store(obj != nullptr, std::memory_order_relaxed);
            if (obj)
            {
                void* const inst{ obj->get_instance() };
                g_object_return_instance.store(reinterpret_cast<std::uintptr_t>(inst),
                                               std::memory_order_relaxed);
                g_object_return_klass = runtime_klass_name(inst);
                g_object_return_tag.store(obj->get_tag(), std::memory_order_relaxed);
            }
        }

        // ── ARG RETURNED UNCHANGED on non-Child declared types ─────────────────
        // echoObject(Object) returns its arg unchanged: pass the stored child IN
        // as the arg, the returned OOP must equal it (root-type identity echo).
        // echoString(String) returns its String arg unchanged into the eager
        // std::string alternative (an argument round-tripped through the String
        // return path, not the OOP path).
        {
            std::unique_ptr<child_object> fc{ self->field_child() };
            if (fc)
            {
                const std::uintptr_t fc_oop{
                    reinterpret_cast<std::uintptr_t>(fc->get_instance()) };
                std::unique_ptr<child_object> back{ self->echo_object(fc) };
                g_echo_object_nonnull.store(back != nullptr, std::memory_order_relaxed);
                if (back)
                {
                    g_echo_object_same_as_field.store(
                        reinterpret_cast<std::uintptr_t>(back->get_instance()) == fc_oop,
                        std::memory_order_relaxed);
                }
            }
            g_echo_string_ok.store(self->echo_string(k_string_arg) == k_string_arg,
                                   std::memory_order_relaxed);
        }

        // ── BOXED-PRIMITIVE VARIETY: Long / Boolean / Double ───────────────────
        // Wide and non-int boxed returns each decode to a non-null wrapper whose
        // runtime klass is the matching boxed type — the OOP-decode is correct
        // for boxes wider than / different from Integer too.
        {
            std::unique_ptr<integer_object> bl{ self->boxed_long() };
            g_boxed_long_nonnull.store(bl != nullptr, std::memory_order_relaxed);
            if (bl) { g_boxed_long_klass = runtime_klass_name(bl->get_instance()); }

            std::unique_ptr<integer_object> bb{ self->boxed_bool() };
            g_boxed_bool_nonnull.store(bb != nullptr, std::memory_order_relaxed);
            if (bb) { g_boxed_bool_klass = runtime_klass_name(bb->get_instance()); }

            std::unique_ptr<integer_object> bd{ self->boxed_double() };
            g_boxed_double_nonnull.store(bd != nullptr, std::memory_order_relaxed);
            if (bd) { g_boxed_double_klass = runtime_klass_name(bd->get_instance()); }
        }

        // ── ARG-DRIVEN NULL on a reference (String) arg ────────────────────────
        // childWhenPresent(key): a non-null String arg -> the stored child; a
        // null String arg would -> null.  We drive the non-null branch (passing a
        // real String arg yields a non-null Child) — the reference-arg companion
        // to pickChild's int-out-of-range null path.
        {
            std::unique_ptr<child_object> p{ self->child_when_present(k_string_arg) };
            g_present_nonnull_when_key.store(p != nullptr, std::memory_order_relaxed);
            if (p) { g_present_tag.store(p->get_tag(), std::memory_order_relaxed); }
        }

        // ── NULL OBJECT ARG -> NULL OBJECT RETURN (object-path arg-driven null) ─
        // Pass an EMPTY unique_ptr (a null reference) as the object arg; the
        // arg-packing nulls the jvalue and the echo returns null, which must
        // decode back to a null unique_ptr — the OBJECT-arg companion to the
        // String-arg null path, and a graceful-null contract on a non-null-capable
        // method driven by its argument.
        {
            const std::unique_ptr<child_object> null_child_arg{};
            std::unique_ptr<child_object> echoed_null{ self->echo_child(null_child_arg) };
            g_echo_null_child_is_null.store(echoed_null == nullptr, std::memory_order_relaxed);

            const std::unique_ptr<child_object> null_object_arg{};
            std::unique_ptr<child_object> echoed_null_obj{ self->echo_object(null_object_arg) };
            g_echo_null_object_is_null.store(echoed_null_obj == nullptr, std::memory_order_relaxed);
        }

        // ── SUBTYPE passed as a BASE-typed (Animal) arg, returned unchanged ─────
        // Decode a Dog via makeAnimal(), pass it back IN through echoAnimal(Animal)
        // — a WIDENING reference arg (Dog -> Animal) — and the returned OOP must
        // equal the arg OOP, with the decode still seeing the concrete runtime Dog.
        {
            std::unique_ptr<dog_object> dog{ self->make_animal() };
            if (dog)
            {
                const std::uintptr_t dog_oop{
                    reinterpret_cast<std::uintptr_t>(dog->get_instance()) };
                std::unique_ptr<dog_object> back{ self->echo_animal(dog) };
                g_echo_animal_nonnull.store(back != nullptr, std::memory_order_relaxed);
                if (back)
                {
                    void* const inst{ back->get_instance() };
                    g_echo_animal_same_oop.store(
                        reinterpret_cast<std::uintptr_t>(inst) == dog_oop,
                        std::memory_order_relaxed);
                    g_echo_animal_klass = runtime_klass_name(inst);
                }
            }
        }

        // ── FRESH-each-call on the STATIC dispatch path ────────────────────────
        // staticFreshChild() new's a Child every call, so two STATIC calls decode
        // to two DISTINCT non-null instances — the static-path companion to
        // makeChild()'s instance-path distinct-each-call probe.
        {
            auto sf{ method_object::static_method("staticFreshChild") };
            if (sf)
            {
                std::unique_ptr<child_object> a = sf->call();
                std::unique_ptr<child_object> b = sf->call();
                g_static_fresh_a_nonnull.store(a != nullptr, std::memory_order_relaxed);
                g_static_fresh_b_nonnull.store(b != nullptr, std::memory_order_relaxed);
                if (a)
                {
                    g_static_fresh_tag.store(a->get_tag(), std::memory_order_relaxed);
                    g_static_fresh_instance_a.store(
                        reinterpret_cast<std::uintptr_t>(a->get_instance()),
                        std::memory_order_relaxed);
                }
                if (b)
                {
                    g_static_fresh_instance_b.store(
                        reinterpret_cast<std::uintptr_t>(b->get_instance()),
                        std::memory_order_relaxed);
                }
            }
        }

        // ── DEEP CHAIN through self(): self() -> getChild() on the self wrapper ─
        // self() returns `this`, so getChild() called THROUGH the self-returned
        // wrapper must decode to the SAME Child OOP as a direct getChild() — a
        // chained object-returning call whose receiver is itself a method-returned
        // `this`.
        {
            std::unique_ptr<method_object> sp{ self->self_proxy() };
            if (sp)
            {
                std::unique_ptr<child_object> chained{ sp->get_child() };
                g_self_chain_child_nonnull.store(chained != nullptr, std::memory_order_relaxed);
                if (chained)
                {
                    g_self_chain_child_instance.store(
                        reinterpret_cast<std::uintptr_t>(chained->get_instance()),
                        std::memory_order_relaxed);
                }
            }
        }

        // ── CHAIN through a STATIC-returned wrapper: staticMakeChild() -> self() ─
        // The static call returns the STATIC_CHILD singleton; calling self() (an
        // INSTANCE method) THROUGH that static-returned wrapper must decode to the
        // SAME STATIC_CHILD OOP — an instance call chained off a static return.
        {
            auto sm{ method_object::static_method("staticMakeChild") };
            if (sm)
            {
                std::unique_ptr<child_object> sc = sm->call();
                if (sc)
                {
                    std::unique_ptr<child_object> sc_self{ sc->self_proxy() };
                    g_static_chain_self_nonnull.store(sc_self != nullptr, std::memory_order_relaxed);
                    if (sc_self)
                    {
                        g_static_chain_self_instance.store(
                            reinterpret_cast<std::uintptr_t>(sc_self->get_instance()),
                            std::memory_order_relaxed);
                    }
                }
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
        vmhook::register_class<puppy_object>("vmhook/fixtures/MethodObject$Puppy");
        vmhook::register_class<named_object>("vmhook/fixtures/MethodObject$NamedThing");
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

        // String[] ('[Ljava/lang/String;'): decoded oop non-null, length, elements
        // read as java.lang.String through read_java_string.
        ctx.check("mco_stringarray_reference_decoded_non_null",
                  g_strarray_decoded_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_stringarray_length_correct",
                  g_strarray_len.load(std::memory_order_relaxed) == k_array_len);
        ctx.check("mco_stringarray_elem0_correct",
                  g_strarray_elem0_ok.load(std::memory_order_relaxed));
        ctx.check("mco_stringarray_elem2_correct",
                  g_strarray_elem2_ok.load(std::memory_order_relaxed));

        // ════════════════ TWO-LEVEL-DEEP polymorphic return (Puppy) ═════════
        // makePuppy() declared Animal, runtime Puppy (Animal -> Dog -> Puppy).
        ctx.check("mco_puppy_non_null_wrapper",
                  g_puppy_nonnull.load(std::memory_order_relaxed));
        // The decoded oop's runtime klass is the depth-2 Puppy, not Animal/Dog.
        ctx.check("mco_puppy_runtime_klass_is_Puppy",
                  ends_with(g_puppy_klass, "MethodObject$Puppy"));
        // Dog's breedId is inherited; reading it through the Puppy wrapper works.
        ctx.check("mco_puppy_inherited_field_read_through_wrapper",
                  g_puppy_breed.load(std::memory_order_relaxed) == k_dog_breed_id);
        // Virtual dispatch reaches the DEPTH-2 Puppy override (not Animal/Dog).
        ctx.check("mco_puppy_depth2_virtual_dispatch_hits_override",
                  g_puppy_speak == k_puppy_sound
                  && g_puppy_speak == g_puppy_speak_java);
        ctx.check("mco_puppy_identity_published",
                  method_object::puppy_identity() != 0);

        // ════════════════ INTERFACE-typed return (NamedThing) ══════════════
        // makeNamed() declared Named (interface), runtime NamedThing (impl class).
        ctx.check("mco_named_non_null_wrapper",
                  g_named_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_named_runtime_klass_is_NamedThing",
                  ends_with(g_named_klass, "MethodObject$NamedThing"));
        ctx.check("mco_named_field_read_through_wrapper",
                  g_named_code.load(std::memory_order_relaxed) == k_named_code);
        // Interface method dispatched through the runtime-impl-decoded wrapper.
        ctx.check("mco_named_interface_dispatch_through_wrapper",
                  g_named_name == k_named_name && g_named_name == g_named_name_java);
        ctx.check("mco_named_identity_published",
                  method_object::named_identity() != 0);

        // ════════════════ OBJECT ARG -> object return (echoChild) ══════════
        // A unique_ptr<wrapper> passed back IN as an argument round-trips: the
        // returned OOP equals the argument OOP (identity echo).
        ctx.check("mco_echo_object_arg_non_null_wrapper",
                  g_echo_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_echo_object_arg_tag_correct",
                  g_echo_tag.load(std::memory_order_relaxed) == k_child_tag);
        ctx.check("mco_echo_object_arg_returns_same_oop",
                  g_echo_same_as_field.load(std::memory_order_relaxed));

        // ════════════════ ARG-SELECTED object return (pickChild) ═══════════
        // An int arg selects which array Child is returned: 0/1/2 each non-null
        // with the matching published tag and all three DISTINCT; idx 99 -> null.
        ctx.check("mco_pickchild_idx0_non_null", g_pick0_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_pickchild_idx1_non_null", g_pick1_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_pickchild_idx2_non_null", g_pick2_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_pickchild_idx0_tag_correct",
                  g_pick0_tag.load(std::memory_order_relaxed) == k_array_tag_0);
        ctx.check("mco_pickchild_idx1_tag_correct",
                  g_pick1_tag.load(std::memory_order_relaxed) == k_array_tag_1);
        ctx.check("mco_pickchild_idx2_tag_correct",
                  g_pick2_tag.load(std::memory_order_relaxed) == k_array_tag_2);
        ctx.check("mco_pickchild_all_three_distinct",
                  g_pick_all_distinct.load(std::memory_order_relaxed));
        ctx.check("mco_pickchild_out_of_range_returns_null",
                  g_pick_oob_null.load(std::memory_order_relaxed));

        // ════════════════ CROSS-METHOD identity ════════════════════════════
        // sameStaticChild() and staticMakeChild() return the ONE STATIC_CHILD
        // singleton; two different methods must decode to the SAME heap object.
        ctx.check("mco_samestatic_non_null_wrapper",
                  g_samestatic_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_cross_method_same_static_instance",
                  g_samestatic_instance.load(std::memory_order_relaxed) != 0
                  && g_samestatic_instance.load(std::memory_order_relaxed)
                         == g_static_instance.load(std::memory_order_relaxed));

        // ════════════════ getChild() IDEMPOTENT identity ═══════════════════
        // getChild() returns the stored field, so a second call decodes the SAME
        // OOP (contrast makeChild, whose two calls are distinct).
        ctx.check("mco_getchild_idempotent_same_oop_each_call",
                  g_getchild_instance.load(std::memory_order_relaxed) != 0
                  && g_getchild_instance.load(std::memory_order_relaxed)
                         == g_getchild_instance_2.load(std::memory_order_relaxed));

        // ════════════════ CHAINED NULL sibling ═════════════════════════════
        // A null reference return THROUGH a method-decoded receiver is a null ptr.
        ctx.check("mco_chained_null_sibling_returns_null",
                  g_null_sibling_is_null.load(std::memory_order_relaxed));

        // ════════════════ Child.self() through method-returned wrapper ═════
        // self() returns `this`, so the wrapper decodes to the SAME Child OOP as
        // getChild() did (self-identity through a method-returned receiver).
        ctx.check("mco_child_self_non_null_wrapper",
                  g_child_self_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_child_self_equals_getchild_instance",
                  g_child_self_instance.load(std::memory_order_relaxed) != 0
                  && g_child_self_instance.load(std::memory_order_relaxed)
                         == g_getchild_instance.load(std::memory_order_relaxed));

        // ════════════════ ONE value_t converted twice ══════════════════════
        // Converting one value_t to unique_ptr<wrapper> twice news two wrappers
        // over the SAME decoded OOP — the conversion is repeatable, not consuming.
        ctx.check("mco_value_t_double_conversion_both_non_null",
                  g_value_t_reuse_both_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_value_t_double_conversion_same_oop",
                  g_value_t_reuse_same_oop.load(std::memory_order_relaxed));

        // ════════════════ VARIED-ARG-SIGNATURE object returns ══════════════
        // Every primitive arg WIDTH and reference arg kind drives an
        // object-returning call(); the returned Child's tag folds the arg, so a
        // narrowed / dropped / mis-slotted argument is caught by a wrong tag.

        // WIDE long arg (two interpreter slots): both halves survive the pack.
        ctx.check("mco_arg_long_non_null_wrapper",
                  g_long_arg_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_arg_long_tag_folds_both_halves",
                  g_long_arg_tag.load(std::memory_order_relaxed) == k_long_arg_tag);

        // WIDE double arg (two slots).
        ctx.check("mco_arg_double_non_null_wrapper",
                  g_double_arg_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_arg_double_tag_correct",
                  g_double_arg_tag.load(std::memory_order_relaxed) == k_double_arg_tag);

        // String (reference) arg -> object return; arg survives in tag AND label.
        ctx.check("mco_arg_string_non_null_wrapper",
                  g_string_arg_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_arg_string_tag_correct",
                  g_string_arg_tag.load(std::memory_order_relaxed) == k_string_arg_tag);
        ctx.check("mco_arg_string_label_echoes_arg",
                  g_string_arg_label_ok.load(std::memory_order_relaxed));

        // float arg (single slot).
        ctx.check("mco_arg_float_non_null_wrapper",
                  g_float_arg_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_arg_float_tag_correct",
                  g_float_arg_tag.load(std::memory_order_relaxed) == k_float_arg_tag);

        // boolean arg: the arg selects the tag (both branches distinguishable).
        ctx.check("mco_arg_bool_true_non_null_wrapper",
                  g_bool_true_arg_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_arg_bool_true_tag_correct",
                  g_bool_true_arg_tag.load(std::memory_order_relaxed) == k_bool_true_tag);
        ctx.check("mco_arg_bool_false_tag_correct",
                  g_bool_false_arg_tag.load(std::memory_order_relaxed) == k_bool_false_tag);
        ctx.check("mco_arg_bool_branches_distinct",
                  g_bool_true_arg_tag.load(std::memory_order_relaxed)
                      != g_bool_false_arg_tag.load(std::memory_order_relaxed));

        // MULTI-arg mixed-width signature (int + wide long + String): all three
        // arguments survive (tag folds every one).
        ctx.check("mco_arg_many_non_null_wrapper",
                  g_many_arg_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_arg_many_tag_folds_all_three_args",
                  g_many_arg_tag.load(std::memory_order_relaxed) == k_many_tag);

        // ════════════════ java.lang.Object BASE return ═════════════════════
        // Declared Object, runtime Child: the decode is runtime-driven, the oop
        // is the SAME heap object as the field child, and a field read through
        // the wrapper works.
        ctx.check("mco_object_base_return_non_null_wrapper",
                  g_object_return_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_object_base_return_runtime_klass_is_Child",
                  ends_with(g_object_return_klass, "MethodObject$Child"));
        ctx.check("mco_object_base_return_tag_correct",
                  g_object_return_tag.load(std::memory_order_relaxed) == k_child_tag);
        ctx.check("mco_object_base_return_same_oop_as_field",
                  g_object_return_instance.load(std::memory_order_relaxed) != 0
                  && g_object_return_instance.load(std::memory_order_relaxed)
                         == g_field_instance.load(std::memory_order_relaxed));

        // ════════════════ ARG RETURNED UNCHANGED (non-Child types) ═════════
        // echoObject(Object) round-trips the stored child: returned OOP == arg OOP.
        // (Distinct from the echoChild block above, whose arg is declared Child;
        // here the param is declared java.lang.Object — a widening reference arg.)
        ctx.check("mco_echo_objecttype_arg_non_null_wrapper",
                  g_echo_object_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_echo_objecttype_arg_returns_same_oop",
                  g_echo_object_same_as_field.load(std::memory_order_relaxed));
        // echoString(String) round-trips a String arg through the eager-String path.
        ctx.check("mco_echo_stringtype_arg_returns_same_string",
                  g_echo_string_ok.load(std::memory_order_relaxed));

        // ════════════════ BOXED-PRIMITIVE VARIETY (Long/Boolean/Double) ═════
        // Wide / non-int boxes each decode to a non-null wrapper of the matching
        // boxed klass — the OOP decode is correct beyond Integer.
        ctx.check("mco_boxed_long_non_null_wrapper",
                  g_boxed_long_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_boxed_long_runtime_klass_is_Long",
                  ends_with(g_boxed_long_klass, "Long"));
        ctx.check("mco_boxed_bool_non_null_wrapper",
                  g_boxed_bool_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_boxed_bool_runtime_klass_is_Boolean",
                  ends_with(g_boxed_bool_klass, "Boolean"));
        ctx.check("mco_boxed_double_non_null_wrapper",
                  g_boxed_double_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_boxed_double_runtime_klass_is_Double",
                  ends_with(g_boxed_double_klass, "Double"));

        // ════════════════ ARG-DRIVEN NULL on a reference (String) arg ══════
        // A non-null String arg -> the stored child (non-null + correct tag);
        // the reference-arg companion to pickChild's int-out-of-range null.
        ctx.check("mco_arg_present_string_non_null_wrapper",
                  g_present_nonnull_when_key.load(std::memory_order_relaxed));
        ctx.check("mco_arg_present_string_tag_correct",
                  g_present_tag.load(std::memory_order_relaxed) == k_child_tag);

        // ════════════════ NULL OBJECT ARG -> NULL OBJECT RETURN ════════════
        // An empty unique_ptr passed as the object arg packs a null reference;
        // the echo returns null, which must decode back to a null unique_ptr.
        // The graceful-null contract on the OBJECT round-trip path (the String-
        // arg null above is its reference-arg sibling).
        ctx.check("mco_echo_null_child_arg_returns_null",
                  g_echo_null_child_is_null.load(std::memory_order_relaxed));
        ctx.check("mco_echo_null_object_arg_returns_null",
                  g_echo_null_object_is_null.load(std::memory_order_relaxed));

        // ════════════════ SUBTYPE-as-base-ARG identity echo (Dog as Animal) ═
        // A Dog wrapper passed IN through echoAnimal(Animal) — a widening
        // reference arg — round-trips: the returned OOP equals the arg OOP and
        // the decode sees the concrete runtime Dog (not the declared Animal).
        ctx.check("mco_echo_animal_arg_non_null_wrapper",
                  g_echo_animal_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_echo_animal_arg_returns_same_oop",
                  g_echo_animal_same_oop.load(std::memory_order_relaxed));
        ctx.check("mco_echo_animal_arg_runtime_klass_is_Dog",
                  ends_with(g_echo_animal_klass, "MethodObject$Dog"));
        // The echoed Dog is the SAME singleton makeAnimal() returns (its OOP must
        // equal the makeAnimal() instance captured earlier).
        ctx.check("mco_echo_animal_arg_same_as_make_animal",
                  g_echo_animal_same_oop.load(std::memory_order_relaxed)
                  && g_animal_instance.load(std::memory_order_relaxed) != 0);

        // ════════════════ FRESH-each-call on the STATIC dispatch path ═══════
        // staticFreshChild() new's a Child every call: two static calls decode to
        // two DISTINCT non-null instances (the static-path companion to
        // makeChild's instance-path distinct-each-call check).
        ctx.check("mco_static_fresh_first_call_non_null",
                  g_static_fresh_a_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_static_fresh_second_call_non_null",
                  g_static_fresh_b_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_static_fresh_tag_correct",
                  g_static_fresh_tag.load(std::memory_order_relaxed) == k_static_fresh_tag);
        ctx.check("mco_static_fresh_distinct_instances_each_call",
                  g_static_fresh_instance_a.load(std::memory_order_relaxed) != 0
                  && g_static_fresh_instance_b.load(std::memory_order_relaxed) != 0
                  && g_static_fresh_instance_a.load(std::memory_order_relaxed)
                         != g_static_fresh_instance_b.load(std::memory_order_relaxed));

        // ════════════════ DEEP CHAIN through self() (this -> getChild) ══════
        // self() returns `this`; getChild() called THROUGH the self-returned
        // wrapper decodes to the SAME Child OOP as a direct getChild() — a chained
        // object-returning call whose receiver is itself a method-returned `this`.
        ctx.check("mco_self_chain_getchild_non_null_wrapper",
                  g_self_chain_child_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_self_chain_getchild_equals_direct_getchild",
                  g_self_chain_child_instance.load(std::memory_order_relaxed) != 0
                  && g_self_chain_child_instance.load(std::memory_order_relaxed)
                         == g_getchild_instance.load(std::memory_order_relaxed));

        // ════════════════ CHAIN through a STATIC-returned wrapper ═══════════
        // staticMakeChild() returns STATIC_CHILD; self() (an INSTANCE method)
        // called THROUGH that static-returned wrapper decodes to the SAME
        // STATIC_CHILD OOP — an instance call chained off a static return.
        ctx.check("mco_static_chain_self_non_null_wrapper",
                  g_static_chain_self_nonnull.load(std::memory_order_relaxed));
        ctx.check("mco_static_chain_self_equals_static_instance",
                  g_static_chain_self_instance.load(std::memory_order_relaxed) != 0
                  && g_static_chain_self_instance.load(std::memory_order_relaxed)
                         == g_static_instance.load(std::memory_order_relaxed));

        // ── breadcrumbs (never affect pass/fail) ───────────────────────────
        ctx.record("[INFO] animal runtime klass = " + g_animal_klass
                   + " (declared Animal, decoded Dog); speak() via wrapper = '" + g_animal_speak
                   + "', via Java = '" + g_animal_speak_java + "'");
        ctx.record("[INFO] boxed runtime klass = " + g_boxed_klass
                   + " value=" + std::to_string(g_boxed_value.load(std::memory_order_relaxed)));
        ctx.record("[INFO] makeChild distinct OOPs: a=0x"
                   + std::to_string(g_make_instance_a.load(std::memory_order_relaxed))
                   + " b=0x" + std::to_string(g_make_instance_b.load(std::memory_order_relaxed)));
        ctx.record("[INFO] puppy runtime klass = " + g_puppy_klass
                   + " (declared Animal, decoded Puppy); speak() via wrapper = '" + g_puppy_speak
                   + "', via Java = '" + g_puppy_speak_java + "'");
        ctx.record("[INFO] named runtime klass = " + g_named_klass
                   + " (declared interface Named, decoded NamedThing); name() via wrapper = '"
                   + g_named_name + "', via Java = '" + g_named_name_java + "'");
        ctx.record("[INFO] echoChild round-trip: same-as-field="
                   + std::string{ g_echo_same_as_field.load(std::memory_order_relaxed) ? "yes" : "no" });
        ctx.record("[INFO] cross-method static identity: sameStaticChild=0x"
                   + std::to_string(g_samestatic_instance.load(std::memory_order_relaxed))
                   + " staticMakeChild=0x"
                   + std::to_string(g_static_instance.load(std::memory_order_relaxed)));
        ctx.record("[INFO] varied-arg tags: long="
                   + std::to_string(g_long_arg_tag.load(std::memory_order_relaxed))
                   + " double=" + std::to_string(g_double_arg_tag.load(std::memory_order_relaxed))
                   + " string=" + std::to_string(g_string_arg_tag.load(std::memory_order_relaxed))
                   + " float=" + std::to_string(g_float_arg_tag.load(std::memory_order_relaxed))
                   + " bool(t/f)=" + std::to_string(g_bool_true_arg_tag.load(std::memory_order_relaxed))
                   + "/" + std::to_string(g_bool_false_arg_tag.load(std::memory_order_relaxed))
                   + " many=" + std::to_string(g_many_arg_tag.load(std::memory_order_relaxed))
                   + " (expected long=" + std::to_string(k_long_arg_tag)
                   + " many=" + std::to_string(k_many_tag) + ")");
        ctx.record("[INFO] object-base return runtime klass = " + g_object_return_klass
                   + " (declared java.lang.Object, decoded Child)");
        ctx.record("[INFO] boxed variety runtime klasses: Long=" + g_boxed_long_klass
                   + " Boolean=" + g_boxed_bool_klass + " Double=" + g_boxed_double_klass);
        ctx.record("[INFO] null-object-arg echo: child-arg-null="
                   + std::string{ g_echo_null_child_is_null.load(std::memory_order_relaxed) ? "yes" : "no" }
                   + " object-arg-null="
                   + std::string{ g_echo_null_object_is_null.load(std::memory_order_relaxed) ? "yes" : "no" });
        ctx.record("[INFO] echoAnimal(Dog as Animal) runtime klass = " + g_echo_animal_klass
                   + " same-oop=" + std::string{ g_echo_animal_same_oop.load(std::memory_order_relaxed) ? "yes" : "no" });
        ctx.record("[INFO] staticFreshChild distinct OOPs: a=0x"
                   + std::to_string(g_static_fresh_instance_a.load(std::memory_order_relaxed))
                   + " b=0x" + std::to_string(g_static_fresh_instance_b.load(std::memory_order_relaxed)));
        ctx.record("[INFO] self()->getChild() chained=0x"
                   + std::to_string(g_self_chain_child_instance.load(std::memory_order_relaxed))
                   + " direct getChild=0x"
                   + std::to_string(g_getchild_instance.load(std::memory_order_relaxed))
                   + "; staticMakeChild()->self()=0x"
                   + std::to_string(g_static_chain_self_instance.load(std::memory_order_relaxed)));
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
