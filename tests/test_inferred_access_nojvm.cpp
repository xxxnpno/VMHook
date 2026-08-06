// Standalone (no-JVM) tests for the INFERRED-ACCESS contract: a call site
// never supplies a type to get / set / call, and never spells a borrow.
//
// The promise being pinned here is a small one to state and a wide one to get
// wrong:
//
//     float health = self->get_field("health")->get();       // no <float>
//     std::string n = self->get_field("name")->get();        // no as_string()
//     auto rider    = self->get_field("passenger")->get();   // no to_borrowed<>
//     rider->get_method("dismount")->call();                 // no wrapper type
//
// The signature the JVM already stores decides what a read means; the type the
// caller declared decides what shape it arrives in.  Neither is something the
// caller repeats.
//
// SCOPE: no JVM is in-process, so every decode of a compressed OOP bottoms out
// in decode_oop_pointer() with no VMStructs, which is null-safe and returns
// nullptr rather than faulting (vmhook.hpp — see the no-VMStructs early-outs).
// That makes the RUNTIME half of this file a test of the fail-closed paths:
// every reference-shaped conversion must yield an EMPTY handle / null wrapper,
// and must never fabricate an address out of primitive bits.  The half that
// matters most — which conversion branch a given target type selects — is
// compile-time, and is static_asserted, because a wrong branch does not fail to
// compile: it silently hands back a default-constructed result, which is the
// exact bug this surface was added to remove.
//
// What needs a live VM (a real oop to navigate, a real klass to read members
// off, a real call) is out of scope here by construction and belongs to the JVM
// suite.

#include <vmhook/vmhook.hpp>

#include <concepts>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

namespace
{
    int failures{ 0 };
    int checks{ 0 };

    auto check(const char* name, const bool ok) -> void
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            std::printf("[FAIL] %s\n", name);
        }
    }

    // Two wrappers.  Neither is registered: register_class needs a live JVM to
    // be worth anything, and the conversions under test must work without one
    // (klass_match_ok fails OPEN for an unregistered wrapper, by design).
    class ia_entity : public vmhook::object<ia_entity>
    {
    public:
        using object::object;
    };

    class ia_item : public vmhook::object<ia_item>
    {
    public:
        using object::object;
    };

    // Not a wrapper: derives from nothing, constructible from void*.  Present
    // to prove is_java_wrapper_v keys off object_base and not off "happens to
    // take a pointer".
    struct ia_impostor
    {
        explicit ia_impostor(void*) {}
    };

    using field_value  = vmhook::field_proxy::value_t;
    using method_value = vmhook::method_proxy::value_t;

    // A reference-typed field value: the uint32 alternative plus an object
    // descriptor.  The compressed value is deliberately non-zero so that the
    // empty results below are attributable to the DECODE failing (no VMStructs)
    // and not to a null short-circuit that would hide a wrong branch.
    auto ref_field(const std::string& signature = "Ljava/lang/Object;") -> field_value
    {
        return field_value{ std::uint32_t{ 0x1234u }, signature };
    }

    auto int_field() -> field_value
    {
        return field_value{ std::int32_t{ 42 }, std::string{ "I" } };
    }
}

// ===========================================================================
// COMPILE TIME — which branch does a target type select?
//
// These are the assertions that carry the contract.  Every one of the shapes
// below compiled BEFORE the inferred-access work too; they just produced a
// default-constructed value.  Asserting the classification is the only way to
// tell "decoded and came back empty" apart from "never looked".
// ===========================================================================

// -- the handle traits recognise exactly the handle templates ---------------
static_assert(vmhook::detail::is_borrowed_v<vmhook::borrowed<ia_entity>>,
              "borrowed<W> must be recognised as a borrow");
static_assert(vmhook::detail::is_borrowed_v<vmhook::borrowed<>>,
              "the UNTYPED borrow is a borrow too");
static_assert(vmhook::detail::is_borrowed_v<const vmhook::borrowed<ia_entity>&>,
              "cv-ref qualifiers must not change the classification");
static_assert(!vmhook::detail::is_borrowed_v<vmhook::ref<ia_entity>>,
              "a ref is not a borrow");

static_assert(vmhook::detail::is_ref_v<vmhook::ref<ia_entity>>,
              "ref<W> must be recognised as a ref");
static_assert(vmhook::detail::is_ref_v<vmhook::ref<>>,
              "the UNTYPED ref is a ref too");
static_assert(vmhook::detail::is_ref_v<const vmhook::ref<ia_entity>&>,
              "cv-ref qualifiers must not change the classification");
static_assert(!vmhook::detail::is_ref_v<vmhook::borrowed<ia_entity>>,
              "a borrow is not a ref");
static_assert(!vmhook::detail::is_ref_v<int>,
              "a primitive is neither");

// -- the wrapper trait keys off object_base, not off shape ------------------
static_assert(vmhook::detail::is_java_wrapper_v<ia_entity>,
              "a user wrapper deriving object<T> must be a wrapper target");
static_assert(vmhook::detail::is_java_wrapper_v<vmhook::collection>,
              "the built-in collection wrappers are wrapper targets as well");
static_assert(vmhook::detail::is_java_wrapper_v<vmhook::any_object>,
              "so is any_object -- it is an oop-reflective wrapper like the rest");
static_assert(!vmhook::detail::is_java_wrapper_v<vmhook::object_base>,
              "object_base itself is never what a caller declares");
static_assert(!vmhook::detail::is_java_wrapper_v<ia_impostor>,
              "taking a void* does NOT make a type a Java wrapper");
static_assert(!vmhook::detail::is_java_wrapper_v<int>,
              "a primitive is not a wrapper target");
static_assert(!vmhook::detail::is_java_wrapper_v<std::string>,
              "and neither is std::string -- it has its own conversion arm");

// -- the handle's wrapper is recoverable ------------------------------------
static_assert(std::is_same_v<vmhook::detail::handle_wrapper_t<vmhook::borrowed<ia_entity>>, ia_entity>,
              "borrowed<W> must yield W for the klass gate");
static_assert(std::is_same_v<vmhook::detail::handle_wrapper_t<vmhook::ref<ia_item>>, ia_item>,
              "ref<W> must yield W for the klass gate");
static_assert(std::is_same_v<vmhook::detail::handle_wrapper_t<vmhook::borrowed<>>, void>,
              "an untyped handle yields void -- there is no klass to gate on");

// -- every promised spelling is actually a conversion, on BOTH value types --
// (is_convertible_v, not is_constructible_v: the point is that no cast, no
// template argument and no named extraction appears at the call site.)
static_assert(std::is_convertible_v<field_value, float>,
              "a primitive field must read with no template argument");
static_assert(std::is_convertible_v<field_value, std::string>,
              "a String field must read with no as_string()");
static_assert(std::is_convertible_v<field_value, vmhook::borrowed<ia_entity>>,
              "a reference field must read into a borrow with no to_borrowed<>");
static_assert(std::is_convertible_v<field_value, vmhook::ref<ia_entity>>,
              "... and into a ref");
static_assert(std::is_convertible_v<field_value, ia_entity>,
              "... and straight into the wrapper the caller declared");
static_assert(std::is_convertible_v<field_value, std::unique_ptr<ia_entity>>,
              "the pre-existing unique_ptr arm must survive all of that");

static_assert(std::is_convertible_v<method_value, double>,
              "a primitive result must read with no template argument");
static_assert(std::is_convertible_v<method_value, std::string>,
              "a String result must read with no as_string()");
static_assert(std::is_convertible_v<method_value, vmhook::borrowed<ia_item>>,
              "a reference result must read into a borrow");
static_assert(std::is_convertible_v<method_value, vmhook::ref<ia_item>>,
              "... and into a ref");
static_assert(std::is_convertible_v<method_value, ia_item>,
              "... and straight into the wrapper the caller declared");

// -- the pointer targets that were excised must STAY excised ----------------
// (this is the MSVC /permissive- ambiguity guard; a new conversion arm is
// exactly the kind of change that could resurrect one of them)
static_assert(!std::is_convertible_v<field_value, const char*>,
              "const char* must remain excluded or std::string targets go ambiguous");
static_assert(!std::is_convertible_v<field_value, ia_entity*>,
              "a raw wrapper pointer must remain excluded");
static_assert(!std::is_convertible_v<method_value, const char*>,
              "same on the call-result value");
static_assert(std::is_convertible_v<field_value, void*>,
              "void* is the one permitted pointer target");

// -- untyped navigation exists on both value types and both untyped handles --
template<typename type>
concept ia_navigable = requires(const type& value)
{
    { value.operator->() } -> std::same_as<vmhook::detail::access<vmhook::any_object>>;
};

static_assert(ia_navigable<field_value>,
              "a field value must navigate without a wrapper type");
static_assert(ia_navigable<method_value>,
              "a call result must navigate without a wrapper type");
static_assert(ia_navigable<vmhook::borrowed<>>,
              "an untyped borrow must navigate too");
static_assert(ia_navigable<vmhook::ref<>>,
              "an untyped ref must navigate too");
static_assert(!ia_navigable<vmhook::borrowed<ia_entity>>,
              "a TYPED borrow still binds its own wrapper, not any_object");

// -- any_object's own surface ------------------------------------------------
static_assert(std::is_base_of_v<vmhook::oop_reflective_base, vmhook::any_object>,
              "any_object must resolve members from the LIVE klass, not the registry");
static_assert(std::is_constructible_v<vmhook::any_object, vmhook::oop_t>,
              "any_object must be constructible from a decoded address");
static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::any_object>,
              "... but that constructor must be explicit, like every other wrapper's");

// -- get_method(name)->call(args...) is THE call spelling --------------------
// There is deliberately no `call(name, args...)` shortcut on a wrapper: one way
// to call a Java method, and it is the one that shows the lookup.
// Written through a named concept: GCC hard-errors on a failed member lookup
// inside a requires-expression whose operand type is not dependent.
template<typename type>
concept ia_has_call_shortcut = requires(const type& value) { value.call("m"); };

static_assert(!ia_has_call_shortcut<ia_entity>,
              "a wrapper must NOT grow a call(name) shortcut past get_method()");
static_assert(!ia_has_call_shortcut<vmhook::any_object>,
              "and neither must any_object");
static_assert(requires(const ia_entity& e)
              { { e.get_method("m")->call(1, 2.0) } -> std::same_as<method_value>; },
              "get_method(name)->call(args...) must deduce argument types");
static_assert(requires(const vmhook::any_object& a)
              { { a.get_method("m")->call() } -> std::same_as<method_value>; },
              "and any_object must offer the same spelling, so untyped chains keep going");

// -- get_field / get_method cover STATIC and INSTANCE members alike ----------
// One name, one call, whatever JVM_ACC_STATIC says.  The static_* names exist
// only for the no-instance case (and for static C++ methods on GCC / Clang>=20,
// where the language cannot resolve the instance overload away).
static_assert(requires(const ia_entity& e)
              { { e.get_field("anyKind") } -> std::same_as<std::optional<vmhook::field_proxy>>; },
              "get_field(name) must be the single field spelling");
static_assert(requires { { ia_entity::static_field("k") } -> std::same_as<std::optional<vmhook::field_proxy>>; },
              "static_field(name) must exist for the no-instance case");
static_assert(requires { { ia_entity::static_method("m") } -> std::same_as<std::optional<vmhook::method_proxy>>; },
              "static_method(name) likewise");

// -- create() runs a real Java constructor -----------------------------------
// It hands back a std::unique_ptr, not a handle type: an object is ALWAYS a
// unique_ptr in this API, so constructing one must not introduce a second
// reference type the caller would otherwise never meet.
static_assert(requires { { ia_entity::create() } -> std::same_as<std::unique_ptr<ia_entity>>; },
              "create() must hand back std::unique_ptr, like every other object");
static_assert(requires { { ia_entity::create("Bob", 12) } -> std::same_as<std::unique_ptr<ia_entity>>; },
              "create(args...) must deduce the <init> overload from the arguments");

// ===========================================================================
// THE UNIQUE_PTR-ONLY SURFACE.
//
// A user never writes `borrowed` anywhere: an object is a std::unique_ptr<T>
// when it is read from a field, returned from a call, passed as an argument,
// stored into a field, constructed, or received as a hook parameter.  These
// assert that every one of those six positions accepts it, because a single
// gap forces the whole vocabulary back into user code.
// ===========================================================================
static_assert(std::is_convertible_v<field_value, std::unique_ptr<ia_entity>>,
              "1. an object FIELD reads as unique_ptr");
static_assert(std::is_convertible_v<method_value, std::unique_ptr<ia_entity>>,
              "2. an object RESULT reads as unique_ptr");
static_assert(requires(const vmhook::method_proxy& m, const std::unique_ptr<ia_entity>& o)
              { m.call(o); },
              "3. a unique_ptr is accepted as a call ARGUMENT");
static_assert(requires(const vmhook::field_proxy& f, const std::unique_ptr<ia_entity>& o)
              { f.set(o); },
              "4. a unique_ptr is accepted by set()");
static_assert(requires { { ia_entity::create() } -> std::same_as<std::unique_ptr<ia_entity>>; },
              "5. construction yields a unique_ptr");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<ia_entity>>,
              "6. and the detour-argument extractor recognises the same shape");

// -- set() accepts anything that names a live object -------------------------
static_assert(requires(const vmhook::field_proxy& f, const vmhook::borrowed<ia_entity>& b) { f.set(b); },
              "set(borrowed) must store the object");
static_assert(requires(const vmhook::field_proxy& f, const vmhook::ref<ia_entity>& r) { f.set(r); },
              "set(ref) must store the object");
static_assert(requires(const vmhook::field_proxy& f, const ia_entity& w) { f.set(w); },
              "set(wrapper) must store the object");
static_assert(requires(const vmhook::field_proxy& f, const field_value& v) { f.set(v); },
              "set(another field's value) must store the object");
static_assert(requires(const vmhook::field_proxy& f, const method_value& v) { f.set(v); },
              "set(a call result) must store the object");
static_assert(requires(const vmhook::field_proxy& f) { f.set(42); f.set(1.5f); f.set("text"); },
              "and the primitive / string writes must be untouched");

int main()
{
    // =======================================================================
    // SECTION 1 — a PRIMITIVE value is not an object, in every shape.
    //
    // The single worst outcome for this surface would be reading an int field
    // into a handle and getting a handle to address 42.  Each of these asserts
    // the opposite: empty, and — for the borrow — "never borrowed" rather than
    // "borrowed and since expired", because those need different handling.
    // =======================================================================
    {
        const field_value primitive{ int_field() };

        // COPY-init throughout, deliberately.  `T x{ value }` is direct-list-
        // init, which reconsiders T's own constructors alongside the conversion
        // — MSVC rejects the resulting overload set outright for the handle and
        // wrapper targets, and for std::string it silently picks
        // initializer_list<char>.  `T x = value;` is the spelling this surface
        // promises and the only one that means the same thing on every compiler.
        const vmhook::borrowed<ia_entity> as_borrow = primitive;
        check("prim_to_borrow_empty",   !static_cast<bool>(as_borrow));
        check("prim_to_borrow_not_expired", !as_borrow.expired());
        check("prim_to_borrow_no_address",  as_borrow.raw_unsafe() == nullptr);

        const vmhook::ref<ia_entity> as_ref = primitive;
        check("prim_to_ref_empty", as_ref.resolve() == nullptr);

        const ia_entity as_wrapper = primitive;
        check("prim_to_wrapper_null", as_wrapper.get_instance() == nullptr);

        const vmhook::any_object untyped{ static_cast<void*>(primitive) };
        check("prim_to_any_object_null", !static_cast<bool>(untyped));

        // The primitive still reads as a primitive, unchanged.
        const int as_int = primitive;
        check("prim_still_reads_as_int", as_int == 42);
        const double as_double = primitive;
        check("prim_still_reads_as_double", as_double == 42.0);
    }

    // =======================================================================
    // SECTION 2 — a REFERENCE value with no VM decodes to nothing, not to the
    //   compressed bits reinterpreted as an address.
    // =======================================================================
    {
        const field_value reference{ ref_field() };

        const vmhook::borrowed<ia_entity> as_borrow = reference;
        check("ref_nodecode_borrow_empty", !static_cast<bool>(as_borrow));
        check("ref_nodecode_borrow_no_raw_bits",
              as_borrow.raw_unsafe() != reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234u)));

        const ia_entity as_wrapper = reference;
        check("ref_nodecode_wrapper_null", as_wrapper.get_instance() == nullptr);

        // is_reference() still reports what the value HOLDS, independently of
        // whether it could be decoded.
        check("ref_nodecode_is_reference", reference.is_reference());
    }

    // =======================================================================
    // SECTION 3 — an ARRAY descriptor is not a single object.
    //
    // '[' decodes to the ARRAY oop, so wrapping it would resolve every later
    // field at an offset that means nothing.  reference_target refuses it by
    // descriptor, BEFORE any decode — which is the one guard here that a live
    // VM is not needed to observe.
    // =======================================================================
    {
        const field_value array_value{ ref_field("[Ljava/lang/String;") };

        const vmhook::borrowed<ia_entity> as_borrow = array_value;
        check("array_to_borrow_empty", !static_cast<bool>(as_borrow));

        const ia_entity as_wrapper = array_value;
        check("array_to_wrapper_null", as_wrapper.get_instance() == nullptr);

        // A primitive-array descriptor is refused on the same rule.
        const field_value int_array{ ref_field("[I") };
        const ia_entity from_int_array = int_array;
        check("prim_array_to_wrapper_null", from_int_array.get_instance() == nullptr);
    }

    // =======================================================================
    // SECTION 4 — the call-result value behaves identically.
    //
    // It has no descriptor to consult, so the array rule cannot apply; what
    // must hold is that void / primitive / String alternatives never produce a
    // handle, and that a compressed OOP that will not decode produces an empty
    // one rather than a fabricated address.
    // =======================================================================
    {
        const method_value returned_void{ std::monostate{} };
        const vmhook::borrowed<ia_item> void_borrow   = returned_void;
        const ia_item                   void_wrapper  = returned_void;
        check("void_result_is_void",          returned_void.is_void());
        check("void_result_to_borrow_empty",  !static_cast<bool>(void_borrow));
        check("void_result_to_wrapper_null",  void_wrapper.get_instance() == nullptr);

        const method_value returned_int{ std::int32_t{ 7 } };
        const vmhook::borrowed<ia_item> int_borrow = returned_int;
        check("int_result_to_borrow_empty", !static_cast<bool>(int_borrow));
        const int seven = returned_int;
        check("int_result_still_reads_as_int", seven == 7);

        // A String result was decoded EAGERLY by call(); the address is gone,
        // so a handle for it would have to be invented.  It must not be.
        const method_value returned_string{ std::string{ "hello" } };
        const vmhook::borrowed<ia_item> string_borrow = returned_string;
        check("string_result_to_borrow_empty", !static_cast<bool>(string_borrow));
        const std::string text = returned_string;
        check("string_result_still_reads_as_string", text == "hello");

        const method_value returned_ref{ std::uint32_t{ 0x1234u } };
        const vmhook::borrowed<ia_item> ref_borrow  = returned_ref;
        const ia_item                   ref_wrapper = returned_ref;
        check("ref_result_nodecode_borrow_empty",  !static_cast<bool>(ref_borrow));
        check("ref_result_nodecode_wrapper_null",  ref_wrapper.get_instance() == nullptr);
    }

    // =======================================================================
    // SECTION 5 — navigating through nothing ends quietly.
    //
    // The reason `a->call("b")->call("c")` is allowed to be written as a chain
    // is that a missing link binds a NULL any_object instead of faulting.  With
    // no VM, EVERY link is missing, which makes this the exact shape to test.
    // =======================================================================
    {
        const field_value reference{ ref_field() };
        check("nav_field_value_binds_null", !static_cast<bool>(**reference));
        check("nav_field_value_field_is_nullopt",
              !reference->get_field("anything").has_value());
        check("nav_field_value_method_is_nullopt",
              !reference->get_method("anything").has_value());
        check("nav_field_value_class_name_empty", reference->class_name().empty());
        check("nav_field_value_instance_of_false", !reference->instance_of("java/lang/Object"));

        // A chained call through nothing yields the same empty result a void
        // call does — and is still navigable, so the chain type-checks.
        check("nav_chained_call_nullopt",
              !reference->get_method("anything").has_value());

        const method_value result{ std::uint32_t{ 0x1234u } };
        check("nav_method_value_binds_null", !static_cast<bool>(**result));
        check("nav_method_value_field_is_nullopt",
              !result->get_field("anything").has_value());

        // A primitive navigates to null too, rather than to its own bits.
        const field_value primitive{ int_field() };
        check("nav_primitive_binds_null", !static_cast<bool>(**primitive));
    }

    // =======================================================================
    // SECTION 6 — the untyped handles navigate, and revalidate first.
    // =======================================================================
    {
        const vmhook::borrowed<> empty_borrow{};
        check("untyped_empty_borrow_binds_null", !static_cast<bool>(**empty_borrow));
        check("untyped_empty_borrow_field_nullopt",
              !empty_borrow->get_field("x").has_value());

        const vmhook::ref<> empty_ref{};
        check("untyped_empty_ref_binds_null", !static_cast<bool>(**empty_ref));
        check("untyped_empty_ref_method_nullopt",
              !empty_ref->get_method("x").has_value());
    }

    // =======================================================================
    // SECTION 7 — a missed lookup is an EMPTY OPTIONAL, on every path.
    //
    // With no VM every lookup misses, which makes this the shape to test: each
    // accessor must hand back nullopt rather than a proxy aimed at nothing.
    // =======================================================================
    {
        const ia_entity wrapper{ nullptr };
        check("get_field_miss_nullopt",     !wrapper.get_field("nope").has_value());
        check("get_method_miss_nullopt",    !wrapper.get_method("nope").has_value());
        check("static_field_miss_nullopt",  !ia_entity::static_field("nope").has_value());
        check("static_method_miss_nullopt", !ia_entity::static_method("nope").has_value());

        const vmhook::any_object untyped{ nullptr };
        check("any_object_get_method_nullopt",     !untyped.get_method("nope").has_value());
        check("any_object_class_name_empty",       untyped.class_name().empty());
        check("any_object_get_method_sig_nullopt",
              !untyped.get_method("nope", "()V").has_value());
    }

    // =======================================================================
    // SECTION 8 — create() refuses BEFORE allocating.
    //
    // The order matters more than the result: an unregistered type, an unloaded
    // class, a missing <init> and an unavailable call stub must each be found
    // before anything is allocated, so a refusal cannot leave a raw,
    // constructor-less object on the Java heap.  With no VM every one of those
    // gates trips, and the observable contract is an EMPTY handle that was
    // never borrowed.
    // =======================================================================
    {
        const std::unique_ptr<ia_entity> made{ ia_entity::create() };
        check("create_no_vm_wrapper_arrived", made != nullptr);
        check("create_no_vm_instance_null", made && made->get_instance() == nullptr);

        const std::unique_ptr<ia_entity> with_args{ ia_entity::create("Bob", 12) };
        check("create_args_no_vm_instance_null",
              with_args && with_args->get_instance() == nullptr);
    }

    std::printf(failures == 0 ? "[PASS] %d checks\n" : "[DONE] %d checks\n", checks);
    return failures == 0 ? 0 : 1;
}
