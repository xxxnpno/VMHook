// Runtime no-JVM contract checks for vmhook's Java-object *factory* entry
// points AND the register_class type->class / wrapper-factory machinery that
// backs them.
//
// Two clusters, both pure-logic and RUN on every CI compiler/STL:
//
//   (1) The four allocating factories — vmhook::make_java_array,
//       vmhook::make_java_object, vmhook::make_java_string, and the templated
//       vmhook::make_unique<T> — exercised through their *guard* / *no-JVM*
//       contracts.  Each early-returns its safe default (nullptr / null
//       unique_ptr) without a live JVM and never dereferences uninitialised VM
//       state, so the guarded paths are fully no-JVM-testable.
//
//   (2) The register_class<T> registry itself: the two process-global maps it
//       drives — vmhook::type_to_class_map (C++ wrapper type_index -> internal
//       Java class name) and vmhook::g_type_factory_map (class name -> factory
//       that builds the wrapper from a decoded OOP) — plus every map-reading
//       accessor that does NOT need a live JVM.  register_class<T>() itself
//       needs find_class() (a live JVM) to populate the maps, but the maps are
//       PUBLIC inline globals and register_class's own write sequence
//       (type_to_class_map.insert_or_assign + g_type_factory_map.emplace) can
//       be reproduced directly with no JVM — which is exactly how the no-JVM
//       suite already exercises this surface (see test_jni_arg_packing.cpp
//       Section F).  The factory lambdas only do `new W{void*}`, which stores a
//       pointer and touches no VM state, so they can be *invoked* with a fake
//       pointer and the resulting wrapper inspected — no JVM required.
//
// Everything in cluster (2) saves and restores the global map state it mutates
// (see map_state_guard) so the suite stays order-independent regardless of what
// any sibling test left in the maps.
//
// Proven directly from the header source (vmhook/vmhook.hpp):
//
//   * make_java_object(klass*, size)  [noexcept, -> void*]
//       - ensure_current_java_thread() fails with no JVM       -> nullptr
//       - the (klass==nullptr || size==0) argument guard       -> nullptr
//   * make_java_array(name, len, elem_size, allow_jni_fallback) [noexcept, -> void*]
//       - length < 0 guard fires BEFORE any VM access          -> nullptr
//       - length >= 0: find_class() fails with no JVM, and the
//         JDK8 "[..." JNI fallback (jni_find_class) is itself
//         gated on ensure_current_java_thread()                -> nullptr
//   * make_java_string(value)         [noexcept, -> void*]
//       - find_class("java/lang/String") fails with no JVM     -> nullptr
//   * make_unique<T>(args...)         [NOT noexcept, -> unique_ptr<T>]
//       - ensure_current_java_thread() fails with no JVM, so it
//         returns BEFORE registration / allocation              -> nullptr
//   * register_class<T>(name)         [noexcept, -> bool]
//       - find_class(name) fails FIRST with no JVM -> returns false and
//         NEITHER map is touched (the type stays unregistered).
//   * type_to_class_map / g_type_factory_map (register_class's write pair)
//       - type_to_class_map.insert_or_assign : LAST write wins per type key.
//       - g_type_factory_map.emplace         : FIRST write wins per name key
//         (emplace is a no-op on an existing key).
//   * detail::jni_signature_for_arg<unique_ptr<W>>() / <W>()  [noexcept, -> std::string]
//       - W registered   -> "L<class-name>;"
//       - W unregistered -> "Ljava/lang/Object;" (deliberate non-static_assert fallback)
//   * for_each_instance<T>()          [-> std::size_t]
//       - T not in type_to_class_map -> early-out, returns 0 (no heap walk).
//   * get_class_methods<W>() / find_methods_by_signature<W>()
//       - W not in type_to_class_map -> empty vector (no find_class call).
//
// Anything that needs a live oop / TLAB / interpreter (the actual allocation,
// header stamping, UTF-16 encode into a backing array, constructor dispatch,
// routing a *real* decoded oop through the factory) is OUT OF SCOPE here and is
// covered by JVM integration in example.cpp and tests/jvm/modules/. We assert
// ONLY the safe guarded paths and the map/factory bookkeeping; no call below can
// reach a path that dereferences VM state.
#include <vmhook/vmhook.hpp>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A minimal wrapper type for register_class<T> / make_unique<T>, mirroring the
// pattern in test_api_surface.cpp / test_api_surface_extended.cpp: derive from
// vmhook::object<T> with the required explicit T(vmhook::oop_t) constructor.
class factory_wrapper : public vmhook::object<factory_wrapper>
{
public:
    explicit factory_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<factory_wrapper>{ oop }
    {
    }
};

// A second wrapper that exposes a construct(...) overload, so make_unique<T>
// with constructor arguments instantiates the arg-forwarding branch too (the
// no-JVM contract is identical — it still bails at ensure_current_java_thread —
// but this exercises the templated-args code path at compile time).
class factory_wrapper_with_ctor : public vmhook::object<factory_wrapper_with_ctor>
{
public:
    explicit factory_wrapper_with_ctor(vmhook::oop_t oop) noexcept
        : vmhook::object<factory_wrapper_with_ctor>{ oop }
    {
    }

    // Never actually invoked without a JVM (make_unique returns before this),
    // but its presence routes make_unique<T>(args...) through the construct
    // branch rather than the "no matching construct" warning branch.
    auto construct(int, const std::string&) -> void {}
};

// ---------------------------------------------------------------------------
// Additional distinct wrapper types used ONLY by the register_class registry
// sections below.  Each is a unique C++ type (distinct typeid / type_index) so
// the per-type keying, two-types-one-name collision (bug #1), and per-type
// factory dispatch can all be exercised without a JVM.  They derive from
// vmhook::object<T> exactly like factory_wrapper, so the register_class factory
// lambda `+[](void* p) -> object_base* { return new T{p}; }` instantiates
// cleanly (object<T> -> object_base, with the explicit T(oop_t) ctor).
// ---------------------------------------------------------------------------
class registry_alpha : public vmhook::object<registry_alpha>
{
public:
    explicit registry_alpha(vmhook::oop_t oop) noexcept
        : vmhook::object<registry_alpha>{ oop }
    {
    }
};

class registry_beta : public vmhook::object<registry_beta>
{
public:
    explicit registry_beta(vmhook::oop_t oop) noexcept
        : vmhook::object<registry_beta>{ oop }
    {
    }
};

class registry_gamma : public vmhook::object<registry_gamma>
{
public:
    explicit registry_gamma(vmhook::oop_t oop) noexcept
        : vmhook::object<registry_gamma>{ oop }
    {
    }
};

// A wrapper used exclusively to prove the no-JVM make_unique / signature
// contracts are independent of whether the type was registered in the map.
class registry_unmapped : public vmhook::object<registry_unmapped>
{
public:
    explicit registry_unmapped(vmhook::oop_t oop) noexcept
        : vmhook::object<registry_unmapped>{ oop }
    {
    }
};

// A fourth distinct wrapper, used by the static-trait contract block and a few
// extra runtime sections so the per-type keying / factory-distinctness proofs
// have more than three independent typeids to draw on.
class registry_delta : public vmhook::object<registry_delta>
{
public:
    explicit registry_delta(vmhook::oop_t oop) noexcept
        : vmhook::object<registry_delta>{ oop }
    {
    }
};

// ---------------------------------------------------------------------------
// COMPILE-TIME contracts the register_class factory machinery relies on.
//
// register_class<T>() installs `+[](void* p) -> object_base* { return new T{p}; }`
// and keys the type map by std::type_index{ typeid(T) }.  For that lambda to be
// well-formed for EVERY wrapper the library can register, each wrapper T must:
//   (a) derive from vmhook::object_base (so `object_base* = new T{...}` is a
//       valid upcast and the factory's return type is correct),
//   (b) derive from the CRTP base vmhook::object<T>,
//   (c) be constructible from a raw OOP (vmhook::oop_t == void*), which is what
//       `new T{ instance }` does, and
//   (d) be a complete, polymorphic type (object_base has a virtual dtor) so the
//       `delete built;` at the factory consumption site is well-defined.
// The factory map's value type must be exactly the documented function-pointer
// signature.  The unique_ptr trait the read-side (extract_frame_arg /
// jni_signature_for_arg / argument_matches_descriptor) uses to peel the wrapper
// out of `unique_ptr<W>` must recognise every cv/ref spelling.  None of this
// needs a JVM — it is all decided by the type system, so we pin it with
// static_assert: a regression here is a hard compile error, not a silent miss.
// ---------------------------------------------------------------------------

// (a) every wrapper derives from object_base — the factory's upcast target.
static_assert(std::is_base_of_v<vmhook::object_base, factory_wrapper>);
static_assert(std::is_base_of_v<vmhook::object_base, factory_wrapper_with_ctor>);
static_assert(std::is_base_of_v<vmhook::object_base, registry_alpha>);
static_assert(std::is_base_of_v<vmhook::object_base, registry_beta>);
static_assert(std::is_base_of_v<vmhook::object_base, registry_gamma>);
static_assert(std::is_base_of_v<vmhook::object_base, registry_delta>);
static_assert(std::is_base_of_v<vmhook::object_base, registry_unmapped>);

// (b) every wrapper derives from its CRTP base vmhook::object<T>.
static_assert(std::is_base_of_v<vmhook::object<factory_wrapper>, factory_wrapper>);
static_assert(std::is_base_of_v<vmhook::object<registry_alpha>, registry_alpha>);
static_assert(std::is_base_of_v<vmhook::object<registry_beta>, registry_beta>);
static_assert(std::is_base_of_v<vmhook::object<registry_gamma>, registry_gamma>);
static_assert(std::is_base_of_v<vmhook::object<registry_delta>, registry_delta>);
static_assert(std::is_base_of_v<vmhook::object<registry_unmapped>, registry_unmapped>);
// object<T> itself derives from object_base (the chain the factory upcast walks).
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<registry_alpha>>);

// (c) oop-constructibility: the factory body `new T{ instance }` needs exactly
// this.  vmhook::oop_t is the alias the ctors take; it must be void*.
static_assert(std::is_same_v<vmhook::oop_t, void*>);
static_assert(std::is_same_v<vmhook::oop_type_t, void*>);
static_assert(std::is_constructible_v<factory_wrapper, vmhook::oop_t>);
static_assert(std::is_constructible_v<registry_alpha, vmhook::oop_t>);
static_assert(std::is_constructible_v<registry_beta, vmhook::oop_t>);
static_assert(std::is_constructible_v<registry_gamma, vmhook::oop_t>);
static_assert(std::is_constructible_v<registry_delta, vmhook::oop_t>);
static_assert(std::is_constructible_v<registry_unmapped, vmhook::oop_t>);
// void* and the alias are the same type, and a wrapper is constructible from a
// literal nullptr (a null Java reference) — both are what the factory may pass.
static_assert(std::is_constructible_v<registry_alpha, void*>);
static_assert(std::is_constructible_v<registry_alpha, std::nullptr_t>);
// The ctor is `explicit`: a wrapper is NOT implicitly convertible FROM a void*
// (you must say `T{p}`), which is exactly why the factory uses brace-init.
static_assert(!std::is_convertible_v<void*, registry_alpha>);
// The ctor takes a pointer, so an int does NOT satisfy it (no int->void*).
static_assert(!std::is_constructible_v<registry_alpha, int>);
// The wrapper ctors are declared noexcept, so the factory's `new T{p}` cannot
// throw from the constructor itself (only operator new could).
static_assert(std::is_nothrow_constructible_v<registry_alpha, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<registry_delta, vmhook::oop_t>);

// (d) object_base is polymorphic with a virtual destructor, so deleting a
// derived wrapper through an object_base* (the factory's return type) is
// well-defined — this is what every `delete built;` below relies on.
static_assert(std::is_polymorphic_v<vmhook::object_base>);
static_assert(std::has_virtual_destructor_v<vmhook::object_base>);
static_assert(std::is_polymorphic_v<registry_alpha>);

// The factory map's value type is exactly the documented raw function pointer
// `object_base*(*)(void*)` — NOT a std::function, NOT returning unique_ptr (see
// the long comment on type_factory_function_t about incomplete-type dtor
// instantiation).  Pin both the alias and the map's mapped_type.
static_assert(std::is_same_v<vmhook::type_factory_function_t,
                             vmhook::object_base* (*)(void*)>);
static_assert(std::is_same_v<
    vmhook::type_factory_function_t,
    std::unordered_map<std::string, vmhook::type_factory_function_t>::mapped_type>);
static_assert(std::is_pointer_v<vmhook::type_factory_function_t>);
// The type map is keyed by std::type_index, valued by std::string.
static_assert(std::is_same_v<
    std::unordered_map<std::type_index, std::string>::key_type, std::type_index>);
static_assert(std::is_same_v<
    decltype(vmhook::type_to_class_map)::mapped_type, std::string>);
static_assert(std::is_same_v<
    decltype(vmhook::g_type_factory_map)::key_type, std::string>);

// The unique_ptr trait the read-side uses to peel W out of unique_ptr<W> must
// recognise every cv/ref spelling that can appear as a hook detour parameter,
// and report the correct element type.  These back the L<name>; descriptor that
// extract_frame_arg / jni_signature_for_arg derive for a wrapper argument.
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<registry_alpha>>);
static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<registry_alpha>>);
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<registry_alpha>&>);
static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<registry_alpha>&>);
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<registry_alpha>&&>);
static_assert(!vmhook::detail::is_unique_ptr_v<registry_alpha>);
static_assert(!vmhook::detail::is_unique_ptr_v<registry_alpha*>);
static_assert(!vmhook::detail::is_unique_ptr_v<void*>);
static_assert(!vmhook::detail::is_unique_ptr_v<int>);
static_assert(std::is_same_v<
    vmhook::detail::is_unique_ptr<std::unique_ptr<registry_beta>>::value_type_t,
    registry_beta>);

// The two allocating-and-registration entry points are noexcept exactly as the
// no-JVM contract documents (a thrown exception escaping into a hook detour
// would be undefined behaviour).  These are unevaluated operands — nothing is
// actually called, so no JVM is touched.
static_assert(noexcept(vmhook::register_class<registry_alpha>(std::string_view{})));
static_assert(noexcept(vmhook::make_java_string(std::string_view{})));
static_assert(noexcept(vmhook::make_java_object(nullptr, std::size_t{ 0 })));
static_assert(noexcept(vmhook::make_java_array(std::string_view{}, 0, 0u, true)));
static_assert(noexcept(vmhook::get_class_methods<registry_alpha>()));
static_assert(noexcept(vmhook::find_methods_by_signature<registry_alpha>(std::string_view{})));
static_assert(noexcept(vmhook::detail::jni_signature_for_arg<registry_alpha>()));
static_assert(noexcept(vmhook::detail::jni_signature_for_arg<std::unique_ptr<registry_alpha>>()));
static_assert(noexcept(vmhook::detail::jni_signature_for_arg<int>()));
// Return types of the factory entry points are exactly as documented.
static_assert(std::is_same_v<decltype(vmhook::register_class<registry_alpha>(std::string_view{})), bool>);
static_assert(std::is_same_v<decltype(vmhook::make_java_string(std::string_view{})), void*>);
static_assert(std::is_same_v<decltype(vmhook::make_java_object(nullptr, std::size_t{ 0 })), void*>);
static_assert(std::is_same_v<decltype(vmhook::make_java_array(std::string_view{}, 0, 0u, true)), void*>);
static_assert(std::is_same_v<decltype(vmhook::make_unique<registry_alpha>()), std::unique_ptr<registry_alpha>>);
static_assert(std::is_same_v<decltype(vmhook::detail::jni_signature_for_arg<int>()), std::string>);

// ---------------------------------------------------------------------------
// register_class's exact write sequence, reproduced with no JVM.
//
// register_class<T>(name) does, AFTER a successful find_class(name):
//     type_to_class_map.insert_or_assign(typeid(T), name);   // LAST wins
//     g_type_factory_map.emplace(name, +[](void*){ return new T{...}; }); // FIRST wins
//
// find_class needs a live JVM, so these helpers perform JUST the two map writes
// (the part that is pure bookkeeping).  This is the documented no-JVM technique
// and is identical to what test_jni_arg_packing.cpp does for the type map.
// ---------------------------------------------------------------------------
template<class wrapper_type>
static auto factory_for() noexcept -> vmhook::type_factory_function_t
{
    // The SAME lambda register_class<wrapper_type>() installs.  Decayed to a
    // plain function pointer via unary '+'.  Each instantiation of this
    // template yields a DISTINCT function pointer (one per wrapper_type), so
    // factory_for<A>() != factory_for<B>() is a well-defined, platform-invariant
    // identity comparison.
    return +[](void* instance) -> vmhook::object_base*
    {
        return new wrapper_type{ instance };
    };
}

// Reproduce register_class<wrapper_type>(name) sans the find_class/JVM step.
template<class wrapper_type>
static auto register_in_maps(const std::string& name) -> void
{
    vmhook::type_to_class_map.insert_or_assign(
        std::type_index{ typeid(wrapper_type) }, name);         // LAST wins
    vmhook::g_type_factory_map.emplace(name, factory_for<wrapper_type>()); // FIRST wins
}

// ---------------------------------------------------------------------------
// RAII snapshot/restore of BOTH global registry maps.  Constructed at the top
// of the registry sections; on destruction it restores the maps to EXACTLY
// their pre-section contents (whatever a sibling test happened to leave in
// them), keeping the whole suite order-independent.  We hold registration_mutex
// for the copy-out and copy-back to match register_class's own locking
// discipline (single-threaded here, but correct by construction).
// ---------------------------------------------------------------------------
class map_state_guard
{
public:
    map_state_guard()
    {
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
        saved_types_   = vmhook::type_to_class_map;
        saved_factory_ = vmhook::g_type_factory_map;
    }

    ~map_state_guard()
    {
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
        vmhook::type_to_class_map   = saved_types_;
        vmhook::g_type_factory_map  = saved_factory_;
    }

    map_state_guard(const map_state_guard&)            = delete;
    map_state_guard& operator=(const map_state_guard&) = delete;

    auto saved_types() const   -> const std::unordered_map<std::type_index, std::string>& { return saved_types_; }
    auto saved_factory() const -> const std::unordered_map<std::string, vmhook::type_factory_function_t>& { return saved_factory_; }

private:
    std::unordered_map<std::type_index, std::string>                       saved_types_{};
    std::unordered_map<std::string, vmhook::type_factory_function_t>       saved_factory_{};
};

// Helpers reading the public registry maps the way the library's accessors do.
template<class wrapper_type>
static auto type_is_registered() -> bool
{
    return vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) })
        != vmhook::type_to_class_map.end();
}

template<class wrapper_type>
static auto registered_name() -> std::string
{
    const auto it{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
    return it == vmhook::type_to_class_map.end() ? std::string{} : it->second;
}

static auto factory_for_name(const std::string& name) -> vmhook::type_factory_function_t
{
    const auto it{ vmhook::g_type_factory_map.find(name) };
    return it == vmhook::g_type_factory_map.end() ? nullptr : it->second;
}

// The compile-time JNI descriptor the library derives for a given C++ arg type
// (the same builder used by method_proxy::call_jni()).  Reads type_to_class_map
// with no JVM dependency.
template<typename arg_t>
static auto sig() -> std::string
{
    return vmhook::detail::jni_signature_for_arg<arg_t>();
}

// Faithful, trait-derived mirror of method_proxy::argument_matches_descriptor<T>
// for FUNDAMENTAL (non-string, non-object, non-pointer) argument types: returns
// the single JVM descriptor token that the (private) overload-selector accepts
// for T.  It MUST track argument_matches_descriptor's exact precedence and
// sizeof-based ladder; the whole point of R16b below is to prove
// jni_signature_for_arg<T>() agrees with this selector contract for EVERY
// fundamental type.  Because the selector is a private member of method_proxy it
// cannot be called from the test, so this is the "argument_matches_descriptor-
// equivalent" the agreement matrix compares against.
//
// IMPORTANT: every branch derives the expected letter from std::is_integral_v /
// sizeof / is_same_v — NOTHING is hardcoded per platform.  So wchar_t resolves
// to "S" where it is 2 bytes (Windows) and "I" where it is 4 bytes (*nix), and
// char resolves to "B" on either signedness, with no #ifdef.  The matrix holds
// on every data model.
template<typename arg_t>
static auto selector_token() -> std::string_view
{
    using clean_t = std::remove_cvref_t<arg_t>;

    // Order mirrors method_proxy::argument_matches_descriptor exactly:
    //   bool FIRST (bool is integral, sizeof 1) -> "Z"
    //   char16_t || uint16_t -> "C" (claimed before the generic 2-byte branch)
    //   integral sizeof 1 -> "B", 2 -> "S", 4 -> "I", 8 -> "J"
    //   float -> "F", double -> "D"
    if constexpr (std::is_same_v<clean_t, bool>)
    {
        return "Z";
    }
    else if constexpr (std::is_same_v<clean_t, char16_t> || std::is_same_v<clean_t, std::uint16_t>)
    {
        return "C";
    }
    else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == 1)
    {
        return "B";
    }
    else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == 2)
    {
        return "S";
    }
    else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == 4)
    {
        return "I";
    }
    else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == 8)
    {
        return "J";
    }
    else if constexpr (std::is_same_v<clean_t, float>)
    {
        return "F";
    }
    else if constexpr (std::is_same_v<clean_t, double>)
    {
        return "D";
    }
    else
    {
        return {};
    }
}

// One agreement assertion: jni_signature_for_arg<T>() (the descriptor BUILDER)
// must equal the token argument_matches_descriptor<T> (the overload SELECTOR)
// accepts.  Both expectations are trait-derived, so this pins the two dispatchers
// to each other without hardcoding any platform-dependent answer.
template<typename arg_t>
static auto check_signature_agrees(const char* tag) -> void
{
    check(tag, std::string_view{ sig<arg_t>() } == selector_token<arg_t>());
}

// Invoke a registered factory at `sentinel`, asserting (a) it returns non-null,
// (b) the produced wrapper round-trips the exact pointer through get_instance(),
// and (c) its dynamic type is exactly `expected_type` (the registered wrapper).
// The factory body is only `new W{void*}`, which stores the pointer and touches
// no VM state, so this is fully no-JVM.  Deletes the wrapper via object_base*
// (virtual dtor — see the static_assert block).
template<class expected_type>
static auto factory_builds(const char* tag,
                           const vmhook::type_factory_function_t factory,
                           void* const sentinel) -> void
{
    vmhook::object_base* const built{ factory ? factory(sentinel) : nullptr };
    const bool non_null{ built != nullptr };
    const bool holds_ptr{ non_null && built->get_instance() == sentinel };
    const bool right_type{ dynamic_cast<expected_type*>(built) != nullptr };
    std::string name{ tag };
    check((name + "_non_null").c_str(), non_null);
    check((name + "_round_trips_pointer").c_str(), holds_ptr);
    check((name + "_dynamic_type_matches").c_str(), right_type);
    delete built;
}

// register_class<T>(name) with NO JVM: find_class(name) fails FIRST, so the call
// must return false, never throw, and leave BOTH maps untouched (the type stays
// absent and the name gets no factory).  Asserts the full no-JVM contract for an
// arbitrary class-name shape.  Runs inside a map_state_guard at the call site so
// any sibling-left entry for these never-before-seen names is irrelevant.
template<class wrapper_type>
static auto register_class_rejects_no_jvm(const char* tag, const std::string_view name) -> void
{
    const std::size_t types_before{ vmhook::type_to_class_map.size() };
    const std::size_t factory_before{ vmhook::g_type_factory_map.size() };

    bool registered{ true };
    bool threw{ false };
    try { registered = vmhook::register_class<wrapper_type>(name); }
    catch (...) { threw = true; }

    std::string base{ tag };
    check((base + "_returns_false").c_str(), registered == false);
    check((base + "_does_not_throw").c_str(), !threw);
    check((base + "_type_map_size_unchanged").c_str(),
          vmhook::type_to_class_map.size() == types_before);
    check((base + "_factory_map_size_unchanged").c_str(),
          vmhook::g_type_factory_map.size() == factory_before);
    check((base + "_type_absent").c_str(), !type_is_registered<wrapper_type>());
    check((base + "_factory_name_absent").c_str(),
          factory_for_name(std::string{ name }) == nullptr);
}

// ---------------------------------------------------------------------------
// make_java_array helpers: every overload arg is exercised, asserting the
// negative-length guard and the no-JVM find_class failure, and that the call
// is genuinely noexcept (no throw escapes).  Returns true iff the call both
// did not throw AND returned nullptr.
// ---------------------------------------------------------------------------
static auto array_is_null_and_safe(const char* name,
                                   const std::string_view descriptor,
                                   const std::int32_t length,
                                   const std::size_t element_size,
                                   const bool allow_jni_fallback) -> bool
{
    void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEF)) };
    bool  threw{ false };
    try
    {
        result = vmhook::make_java_array(descriptor, length, element_size, allow_jni_fallback);
    }
    catch (...) { threw = true; }
    check(name, result == nullptr && !threw);
    return result == nullptr && !threw;
}

int main()
{
    // =====================================================================
    // make_java_array — negative-length guard (fires before ANY VM access).
    // The guard is `if (length < 0) return nullptr;` at the very top, so the
    // descriptor / element_size are irrelevant: every negative length is a
    // hard nullptr regardless of JVM presence.  noexcept throughout.
    // =====================================================================
    array_is_null_and_safe("make_java_array_len_minus1_byte_returns_null", "[B", -1, sizeof(std::uint8_t), true);
    array_is_null_and_safe("make_java_array_len_minus2_byte_returns_null", "[B", -2, sizeof(std::uint8_t), true);
    array_is_null_and_safe("make_java_array_len_minus100_int_returns_null", "[I", -100, sizeof(std::int32_t), true);
    array_is_null_and_safe("make_java_array_len_intmin_byte_returns_null", "[B", INT_MIN, sizeof(std::uint8_t), true);
    array_is_null_and_safe("make_java_array_len_intmin_plus1_char_returns_null", "[C", INT_MIN + 1, sizeof(std::uint16_t), true);
    // The negative-length guard must fire even with an EMPTY descriptor and a
    // zero element size — it is checked strictly before class_name is touched.
    array_is_null_and_safe("make_java_array_negative_with_empty_descriptor_returns_null", "", -1, 0u, true);
    // ...and even with the JNI fallback explicitly disabled.
    array_is_null_and_safe("make_java_array_negative_no_jni_fallback_returns_null", "[B", -5, sizeof(std::uint8_t), false);

    // =====================================================================
    // make_java_array — length >= 0 with NO JVM.
    // find_class() routes through jni_find_class(), which bails inside
    // ensure_current_java_thread() (no attached JavaThread) -> nullptr.  For a
    // "[..."-prefixed descriptor the JDK8 fallback also calls jni_find_class()
    // -> still null.  So array_klass stays null and the function returns
    // nullptr, for both primitive and reference array descriptors, and for the
    // zero-length case.  Cover EVERY element type the String/array API uses.
    // =====================================================================

    // Zero length, no JVM (length>=0 passes the guard, then find_class fails).
    array_is_null_and_safe("make_java_array_zero_len_byte_no_jvm_returns_null", "[B", 0, sizeof(std::uint8_t), true);
    array_is_null_and_safe("make_java_array_zero_len_int_no_jvm_returns_null", "[I", 0, sizeof(std::int32_t), true);

    // Positive length, no JVM — one assertion per JVM primitive array type.
    array_is_null_and_safe("make_java_array_bool_no_jvm_returns_null", "[Z", 4, sizeof(std::uint8_t), true);
    array_is_null_and_safe("make_java_array_byte_no_jvm_returns_null", "[B", 8, sizeof(std::uint8_t), true);
    array_is_null_and_safe("make_java_array_short_no_jvm_returns_null", "[S", 8, sizeof(std::int16_t), true);
    array_is_null_and_safe("make_java_array_char_no_jvm_returns_null", "[C", 8, sizeof(std::uint16_t), true);
    array_is_null_and_safe("make_java_array_int_no_jvm_returns_null", "[I", 8, sizeof(std::int32_t), true);
    array_is_null_and_safe("make_java_array_long_no_jvm_returns_null", "[J", 8, sizeof(std::int64_t), true);
    array_is_null_and_safe("make_java_array_float_no_jvm_returns_null", "[F", 8, sizeof(float), true);
    array_is_null_and_safe("make_java_array_double_no_jvm_returns_null", "[D", 8, sizeof(double), true);

    // Reference array descriptors (object + the String element spec) — the GC
    // fallback explicitly does NOT cover reference arrays, so with no JVM these
    // are plain nullptr exactly like the primitive ones.
    array_is_null_and_safe("make_java_array_object_no_jvm_returns_null", "[Ljava/lang/Object;", 4, sizeof(void*), true);
    array_is_null_and_safe("make_java_array_string_no_jvm_returns_null", "[Ljava/lang/String;", 4, sizeof(void*), true);

    // The allow_jni_fallback=false path (used internally by make_java_string
    // mid-encode) must also return nullptr with no JVM, for every backing-array
    // descriptor make_java_string itself allocates: "[B" (compact) and "[C"
    // (classic).  This is the exact form the String encoder passes.
    array_is_null_and_safe("make_java_array_byte_no_jni_fallback_no_jvm_returns_null", "[B", 8, sizeof(std::uint8_t), false);
    array_is_null_and_safe("make_java_array_char_no_jni_fallback_no_jvm_returns_null", "[C", 8, sizeof(std::uint16_t), false);

    // The default allow_jni_fallback argument (true) must behave identically to
    // passing true explicitly — exercise the 3-arg overload form too.
    {
        void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        bool  threw{ false };
        try { result = vmhook::make_java_array("[B", 8, sizeof(std::uint8_t)); }
        catch (...) { threw = true; }
        check("make_java_array_default_fallback_arg_no_jvm_returns_null", result == nullptr);
        check("make_java_array_default_fallback_arg_does_not_throw", !threw);
    }

    // A non-array descriptor (no leading '[') skips the JDK8 fallback branch
    // entirely (the `class_name.front() == '['` test is false) and relies on
    // the plain find_class() failure — still nullptr, still no throw.
    array_is_null_and_safe("make_java_array_non_bracket_descriptor_no_jvm_returns_null", "java/lang/Object", 2, sizeof(void*), true);
    // An empty descriptor with a NON-negative length passes the length guard,
    // then `!class_name.empty()` short-circuits the '[' fallback, so it falls to
    // the plain find_class() null result.  Must not index class_name.front().
    array_is_null_and_safe("make_java_array_empty_descriptor_zero_len_no_jvm_returns_null", "", 0, sizeof(std::uint8_t), true);

    // Multi-dimensional array descriptors ("[[...") still take the '[' fallback
    // branch (front()=='['), which itself bails inside ensure_current_java_thread
    // with no JVM -> nullptr.  Cover a primitive and a reference multi-dim shape.
    array_is_null_and_safe("make_java_array_2d_int_no_jvm_returns_null", "[[I", 4, sizeof(std::int32_t), true);
    array_is_null_and_safe("make_java_array_3d_byte_no_jvm_returns_null", "[[[B", 2, sizeof(std::uint8_t), true);
    array_is_null_and_safe("make_java_array_2d_string_no_jvm_returns_null", "[[Ljava/lang/String;", 2, sizeof(void*), true);

    // element_size is not consulted before the no-JVM bail, so an absurd element
    // size (0 or enormous) at a non-negative length is still a clean nullptr.
    array_is_null_and_safe("make_java_array_zero_element_size_no_jvm_returns_null", "[I", 4, 0u, true);
    array_is_null_and_safe("make_java_array_huge_element_size_no_jvm_returns_null", "[J", 4, static_cast<std::size_t>(1) << 20, true);

    // A maximal positive length still cannot allocate without a JVM (the length
    // guard only rejects NEGATIVE lengths; INT_MAX passes it, then find_class
    // fails) — and must not throw or overflow.
    array_is_null_and_safe("make_java_array_intmax_len_no_jvm_returns_null", "[B", INT_MAX, sizeof(std::uint8_t), true);

    // =====================================================================
    // make_java_object — no JVM and argument guards.  noexcept, -> void*.
    // First guard: ensure_current_java_thread() fails with no JVM -> nullptr,
    // regardless of the (klass, size) arguments.  Second guard (only reachable
    // WITH a thread): klass==nullptr || size==0.  Without a JVM the first guard
    // always wins, so every combination below is nullptr.  We pass nullptr for
    // the klass because constructing a fake hotspot::klass* and dereferencing it
    // is exactly what the no-JVM contract forbids — and the function never
    // dereferences klass before the thread/guard checks anyway.
    // =====================================================================
    {
        void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        bool  threw{ false };
        try { result = vmhook::make_java_object(nullptr, 64u); }
        catch (...) { threw = true; }
        check("make_java_object_null_klass_no_jvm_returns_null", result == nullptr);
        check("make_java_object_null_klass_does_not_throw", !threw);
    }
    {
        // size == 0 — would trip the second guard too, but the no-JVM thread
        // guard fires first; either way the contract is nullptr.
        void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        bool  threw{ false };
        try { result = vmhook::make_java_object(nullptr, 0u); }
        catch (...) { threw = true; }
        check("make_java_object_null_klass_zero_size_no_jvm_returns_null", result == nullptr);
        check("make_java_object_null_klass_zero_size_does_not_throw", !threw);
    }
    {
        // A large requested size still cannot allocate without a JVM.
        void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        bool  threw{ false };
        try { result = vmhook::make_java_object(nullptr, static_cast<std::size_t>(1) << 20); }
        catch (...) { threw = true; }
        check("make_java_object_large_size_no_jvm_returns_null", result == nullptr);
        check("make_java_object_large_size_does_not_throw", !threw);
    }
    {
        // SIZE_MAX requested size: the no-JVM thread guard fires before the
        // round-up-to-8 arithmetic, so this is a clean nullptr with no overflow
        // and no throw (a non-null klass would be needed to even reach the
        // rounding, and we never have one without a JVM).
        void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        bool  threw{ false };
        try { result = vmhook::make_java_object(nullptr, std::numeric_limits<std::size_t>::max()); }
        catch (...) { threw = true; }
        check("make_java_object_sizemax_no_jvm_returns_null", result == nullptr);
        check("make_java_object_sizemax_does_not_throw", !threw);
    }

    // =====================================================================
    // make_java_string — no JVM, every input shape.  noexcept, -> void*.
    // find_class("java/lang/String") fails with no JVM, so the function returns
    // nullptr at its first statement, BEFORE any UTF-8 -> UTF-16 decode or
    // backing-array allocation.  The result is therefore nullptr for every
    // possible input, and the input content / length is never observable here;
    // we still vary it widely to prove no shape escapes the guard or throws.
    // =====================================================================
    {
        void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        bool  threw{ false };
        try { result = vmhook::make_java_string(std::string_view{}); }
        catch (...) { threw = true; }
        check("make_java_string_default_view_no_jvm_returns_null", result == nullptr);
        check("make_java_string_default_view_does_not_throw", !threw);
    }
    {
        void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        bool  threw{ false };
        try { result = vmhook::make_java_string(""); }
        catch (...) { threw = true; }
        check("make_java_string_empty_literal_no_jvm_returns_null", result == nullptr);
        check("make_java_string_empty_literal_does_not_throw", !threw);
    }
    {
        void* result{ nullptr };
        bool  threw{ false };
        try { result = vmhook::make_java_string("hello world"); }
        catch (...) { threw = true; }
        check("make_java_string_ascii_no_jvm_returns_null", result == nullptr);
        check("make_java_string_ascii_does_not_throw", !threw);
    }
    {
        // Embedded NUL: string_view carries it (not C-string truncated); the
        // guard still wins before any byte is examined.
        const std::string with_nul{ std::string("ab\0cd", 5) };
        void* result{ nullptr };
        bool  threw{ false };
        try { result = vmhook::make_java_string(std::string_view{ with_nul.data(), with_nul.size() }); }
        catch (...) { threw = true; }
        check("make_java_string_embedded_nul_no_jvm_returns_null", result == nullptr);
        check("make_java_string_embedded_nul_does_not_throw", !threw);
    }
    {
        // Multi-byte UTF-8 (LATIN1-range "é" = 0xC3 0xA9) — exercises the
        // would-be compact LATIN1 branch input, but the JVM guard fires first.
        void* result{ nullptr };
        bool  threw{ false };
        try { result = vmhook::make_java_string("caf\xC3\xA9"); }
        catch (...) { threw = true; }
        check("make_java_string_latin1_utf8_no_jvm_returns_null", result == nullptr);
        check("make_java_string_latin1_utf8_does_not_throw", !threw);
    }
    {
        // BMP-but-not-LATIN1 (U+20AC EURO SIGN = 0xE2 0x82 0xAC) — would-be
        // UTF16 coder input.
        void* result{ nullptr };
        bool  threw{ false };
        try { result = vmhook::make_java_string("\xE2\x82\xAC"); }
        catch (...) { threw = true; }
        check("make_java_string_bmp_unicode_no_jvm_returns_null", result == nullptr);
        check("make_java_string_bmp_unicode_does_not_throw", !threw);
    }
    {
        // Astral / surrogate-pair code point (U+1F600 = 0xF0 0x9F 0x98 0x80) —
        // would-be surrogate-pair encode input.
        void* result{ nullptr };
        bool  threw{ false };
        try { result = vmhook::make_java_string("\xF0\x9F\x98\x80"); }
        catch (...) { threw = true; }
        check("make_java_string_astral_unicode_no_jvm_returns_null", result == nullptr);
        check("make_java_string_astral_unicode_does_not_throw", !threw);
    }
    {
        // A long (10000-char) input: make_java_string no longer truncates it
        // (robustness #9 — over-cap inputs route through the GC-aware NewString
        // fallback in full).  Either way the find_class guard fires FIRST with no
        // JVM, so a long input is still a clean nullptr here (and, importantly, the
        // decode/allocation never even runs because find_class returns null first).
        const std::string long_input(10000, 'x');
        void* result{ nullptr };
        bool  threw{ false };
        try { result = vmhook::make_java_string(long_input); }
        catch (...) { threw = true; }
        check("make_java_string_over_cap_long_no_jvm_returns_null", result == nullptr);
        check("make_java_string_over_cap_long_does_not_throw", !threw);
    }
    {
        // Exactly at a moderate length boundary, mixed content.
        std::string mixed;
        mixed.reserve(300);
        for (int i{ 0 }; i < 100; ++i) { mixed += "a\xC3\xA9"; } // ascii + 'é'
        void* result{ nullptr };
        bool  threw{ false };
        try { result = vmhook::make_java_string(mixed); }
        catch (...) { threw = true; }
        check("make_java_string_mixed_content_no_jvm_returns_null", result == nullptr);
        check("make_java_string_mixed_content_does_not_throw", !threw);
    }

    // =====================================================================
    // make_unique<T> — no JVM.  NOT noexcept (-> unique_ptr<T>), so wrap in
    // try/catch exactly like test_api_surface_extended.cpp does.  The very
    // first statement is ensure_current_java_thread(); with no JVM it returns
    // false and make_unique returns nullptr BEFORE the type-registration
    // lookup or any allocation.  Holds whether or not the type was registered,
    // and whether or not constructor args are supplied.
    // =====================================================================
    {
        // Unregistered type, no args.  The sentinel is reinterpret_cast<T*>(0)
        // — a null pointer in value, matching test_api_surface_extended.cpp —
        // so the assertion still proves make_unique RETURNED null (it cannot
        // make a null unique_ptr non-null) while keeping the unique_ptr's
        // destructor provably a no-op (no -Warray-bounds on a near-zero delete).
        std::unique_ptr<factory_wrapper> obj{ reinterpret_cast<factory_wrapper*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<factory_wrapper>(); }
        catch (...) { threw = true; }
        check("make_unique_no_args_unregistered_no_jvm_returns_null", obj == nullptr);
        check("make_unique_no_args_unregistered_does_not_throw", !threw);
    }

    // register_class itself returns false with no JVM (find_class fails); we
    // assert that, then confirm make_unique still returns null afterwards —
    // i.e. registration state does not change the no-JVM make_unique contract.
    {
        bool registered{ true };
        bool threw{ false };
        try { registered = vmhook::register_class<factory_wrapper>("my/Factory"); }
        catch (...) { threw = true; }
        check("register_class_for_factory_wrapper_returns_false_no_jvm", registered == false);
        check("register_class_for_factory_wrapper_does_not_throw", !threw);
    }
    {
        // After the (failed) registration attempt, make_unique is still null.
        std::unique_ptr<factory_wrapper> obj{ reinterpret_cast<factory_wrapper*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<factory_wrapper>(); }
        catch (...) { threw = true; }
        check("make_unique_no_args_after_register_no_jvm_returns_null", obj == nullptr);
        check("make_unique_no_args_after_register_does_not_throw", !threw);
    }
    {
        // With constructor arguments — instantiates the arg-forwarding /
        // construct(...) branch; same no-JVM nullptr contract.
        std::unique_ptr<factory_wrapper_with_ctor> obj{ reinterpret_cast<factory_wrapper_with_ctor*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<factory_wrapper_with_ctor>(7, std::string{ "name" }); }
        catch (...) { threw = true; }
        check("make_unique_with_ctor_args_no_jvm_returns_null", obj == nullptr);
        check("make_unique_with_ctor_args_does_not_throw", !threw);
    }
    {
        // A wrapper WITHOUT a matching construct(...) but called WITH args:
        // make_unique still bails at the thread guard first, so it is nullptr
        // and never reaches the "no matching construct" warning branch.
        std::unique_ptr<factory_wrapper> obj{ reinterpret_cast<factory_wrapper*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<factory_wrapper>(123); }
        catch (...) { threw = true; }
        check("make_unique_args_without_construct_no_jvm_returns_null", obj == nullptr);
        check("make_unique_args_without_construct_does_not_throw", !threw);
    }

    // #####################################################################
    // ##  register_class registry: type_to_class_map + g_type_factory_map ##
    // ##  All pure-logic, no JVM.  Every section below snapshots BOTH maps ##
    // ##  on entry and restores them on exit (map_state_guard), so they    ##
    // ##  are order-independent and leave the global maps byte-identical to ##
    // ##  whatever a sibling test left behind.                             ##
    // #####################################################################

    // =====================================================================
    // R0. register_class<T>() with NO JVM: find_class(name) fails FIRST, so it
    // returns false and NEITHER map is mutated.  Distinct never-before-seen
    // names + types are used so the assertion holds regardless of suite order.
    // This is the precise contract that makes a bogus/unloadable class name
    // leave every accessor a clean nullopt/empty/0.
    // =====================================================================
    {
        map_state_guard guard{};
        const std::size_t types_before{ vmhook::type_to_class_map.size() };
        const std::size_t factory_before{ vmhook::g_type_factory_map.size() };

        bool registered{ true };
        bool threw{ false };
        try { registered = vmhook::register_class<registry_alpha>("vmhook/test/R0Alpha"); }
        catch (...) { threw = true; }

        check("R0_register_class_no_jvm_returns_false", registered == false);
        check("R0_register_class_no_jvm_does_not_throw", !threw);
        check("R0_register_class_no_jvm_type_map_size_unchanged",
              vmhook::type_to_class_map.size() == types_before);
        check("R0_register_class_no_jvm_factory_map_size_unchanged",
              vmhook::g_type_factory_map.size() == factory_before);
        check("R0_register_class_no_jvm_type_absent",
              !type_is_registered<registry_alpha>());
        check("R0_register_class_no_jvm_factory_name_absent",
              factory_for_name("vmhook/test/R0Alpha") == nullptr);
    }

    // =====================================================================
    // R1. Direct type_to_class_map insert + lookup (the part of register_class
    // that is pure bookkeeping).  insert_or_assign maps the type_index of the
    // wrapper to the class name; the library's resolve_klass / get_field /
    // jni_signature_for_arg all read exactly this entry.
    // =====================================================================
    {
        map_state_guard guard{};

        check("R1_alpha_unregistered_before_insert", !type_is_registered<registry_alpha>());

        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_alpha) }, std::string{ "vmhook/test/Alpha" });

        check("R1_alpha_registered_after_insert", type_is_registered<registry_alpha>());
        check("R1_alpha_maps_to_exact_name", registered_name<registry_alpha>() == "vmhook/test/Alpha");
        // The lookup is keyed by type_index, so the entry survives being read
        // multiple times and is identical each time.
        check("R1_alpha_name_stable_on_relookup", registered_name<registry_alpha>() == "vmhook/test/Alpha");
    }

    // =====================================================================
    // R2. Per-type keying: two DISTINCT wrapper types -> two DISTINCT names
    // both resolve to their OWN class with no cross-talk (the map is keyed by
    // type_index, so different C++ types never collide even if registered
    // "together").  Also: a third type left UNregistered stays absent.
    // =====================================================================
    {
        map_state_guard guard{};

        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_alpha) }, std::string{ "vmhook/test/Alpha" });
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_beta) }, std::string{ "vmhook/test/Beta" });

        check("R2_alpha_resolves_own_name", registered_name<registry_alpha>() == "vmhook/test/Alpha");
        check("R2_beta_resolves_own_name", registered_name<registry_beta>() == "vmhook/test/Beta");
        check("R2_alpha_not_beta_name", registered_name<registry_alpha>() != registered_name<registry_beta>());
        check("R2_gamma_absent_when_others_registered", !type_is_registered<registry_gamma>());
    }

    // =====================================================================
    // R3. Last-wins per type key (insert_or_assign).  Re-registering the SAME
    // type with a DIFFERENT name overwrites the value — this is register_class's
    // documented "last wins" behaviour for the type map.
    // =====================================================================
    {
        map_state_guard guard{};

        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_alpha) }, std::string{ "vmhook/test/AlphaV1" });
        check("R3_alpha_initial_name", registered_name<registry_alpha>() == "vmhook/test/AlphaV1");

        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_alpha) }, std::string{ "vmhook/test/AlphaV2" });
        check("R3_alpha_name_overwritten_last_wins", registered_name<registry_alpha>() == "vmhook/test/AlphaV2");
        // Still exactly one entry for this type (overwrite, not duplicate).
        check("R3_alpha_single_entry_after_overwrite",
              vmhook::type_to_class_map.count(std::type_index{ typeid(registry_alpha) }) == 1u);
    }

    // =====================================================================
    // R4. Idempotent re-registration: the SAME type with the SAME name.  The
    // type-map value is unchanged, AND the factory map's emplace is a no-op on
    // the existing key, so the stored factory POINTER is byte-identical before
    // and after — exactly what register_class<T>(name) called twice guarantees.
    // =====================================================================
    {
        map_state_guard guard{};

        register_in_maps<registry_alpha>("vmhook/test/Alpha");
        const std::string name_after_first{ registered_name<registry_alpha>() };
        const vmhook::type_factory_function_t factory_after_first{ factory_for_name("vmhook/test/Alpha") };

        check("R4_first_register_name", name_after_first == "vmhook/test/Alpha");
        check("R4_first_register_factory_non_null", factory_after_first != nullptr);

        register_in_maps<registry_alpha>("vmhook/test/Alpha"); // identical re-register

        check("R4_reregister_name_unchanged", registered_name<registry_alpha>() == "vmhook/test/Alpha");
        check("R4_reregister_factory_pointer_stable",
              factory_for_name("vmhook/test/Alpha") == factory_after_first);
        check("R4_reregister_single_factory_entry",
              vmhook::g_type_factory_map.count("vmhook/test/Alpha") == 1u);
    }

    // =====================================================================
    // R5. Factory dispatch: g_type_factory_map[name] is the factory that builds
    // the wrapper from a decoded OOP.  The factory only does `new W{void*}`,
    // which stores the pointer (no VM access), so we can INVOKE it with a fake
    // sentinel pointer and prove (a) it returns non-null, (b) the wrapper's
    // get_instance() == the sentinel we passed, and (c) the dynamic type is
    // the REGISTERED wrapper W (correct factory selected).  Then delete it.
    // =====================================================================
    {
        map_state_guard guard{};

        register_in_maps<registry_alpha>("vmhook/test/Alpha");

        const vmhook::type_factory_function_t factory{ factory_for_name("vmhook/test/Alpha") };
        check("R5_factory_present_for_registered_name", factory != nullptr);

        void* const sentinel{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xABCD1234)) };
        vmhook::object_base* built{ factory ? factory(sentinel) : nullptr };

        check("R5_factory_builds_non_null", built != nullptr);
        check("R5_factory_wrapper_holds_passed_pointer",
              built != nullptr && built->get_instance() == sentinel);
        // The factory installed for registry_alpha must build a registry_alpha,
        // not some other type — verify via a dynamic_cast on the polymorphic base.
        check("R5_factory_builds_registered_dynamic_type",
              dynamic_cast<registry_alpha*>(built) != nullptr);
        delete built;
    }

    // =====================================================================
    // R6. Per-type factory distinctness: two types -> two names yield two
    // DISTINCT factory function pointers, and each builds ITS OWN dynamic type.
    // (Factory keyed by name; distinct names => distinct slots => distinct
    // lambdas.)
    // =====================================================================
    {
        map_state_guard guard{};

        register_in_maps<registry_alpha>("vmhook/test/Alpha");
        register_in_maps<registry_beta>("vmhook/test/Beta");

        const vmhook::type_factory_function_t fa{ factory_for_name("vmhook/test/Alpha") };
        const vmhook::type_factory_function_t fb{ factory_for_name("vmhook/test/Beta") };

        check("R6_alpha_factory_present", fa != nullptr);
        check("R6_beta_factory_present", fb != nullptr);
        check("R6_distinct_factories_for_distinct_names", fa != fb);

        void* const p{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1111)) };
        vmhook::object_base* a{ fa ? fa(p) : nullptr };
        vmhook::object_base* b{ fb ? fb(p) : nullptr };
        check("R6_alpha_factory_builds_alpha", dynamic_cast<registry_alpha*>(a) != nullptr);
        check("R6_beta_factory_builds_beta", dynamic_cast<registry_beta*>(b) != nullptr);
        check("R6_alpha_factory_not_builds_beta", dynamic_cast<registry_beta*>(a) == nullptr);
        check("R6_beta_factory_not_builds_alpha", dynamic_cast<registry_alpha*>(b) == nullptr);
        delete a;
        delete b;
    }

    // =====================================================================
    // R7. Registration ASYMMETRY (LIBRARY BUG #1, pinned, not worked around).
    // register_class does type_to_class_map.insert_or_assign (LAST wins) but
    // g_type_factory_map.emplace (FIRST wins).  So binding a SECOND, different
    // wrapper type to an ALREADY-registered class NAME updates the type map (new
    // type -> name) yet leaves the factory map pointing at the FIRST type's
    // factory (emplace is a no-op on the existing key).
    //
    // We reproduce register_class's exact write pair twice on ONE shared name
    // and assert:
    //   * the type map now resolves BOTH types to the shared name (last write
    //     visible per-type because the keys differ),
    //   * the factory pointer is BYTE-IDENTICAL before and after the second
    //     register, and equals factory_for<First>() — NOT factory_for<Second>().
    // We DELIBERATELY do not route a live oop through the factory under the
    // Second type; that mis-typed downcast is the UB the bug would cause and is
    // pinned by inspection only (mirrors the JVM module's posture).
    // =====================================================================
    {
        map_state_guard guard{};

        const std::string shared{ "vmhook/test/Shared" };

        // FIRST registrant: alpha.
        register_in_maps<registry_alpha>(shared);
        const vmhook::type_factory_function_t factory_after_first{ factory_for_name(shared) };
        check("R7_first_factory_is_alpha_factory", factory_after_first == factory_for<registry_alpha>());

        // SECOND registrant: beta -> SAME name.  Type map updates (beta -> name)
        // via insert_or_assign; factory map emplace is a NO-OP.
        register_in_maps<registry_beta>(shared);

        // Both types now map to the shared name (type map keys are distinct).
        check("R7_alpha_maps_to_shared", registered_name<registry_alpha>() == shared);
        check("R7_beta_maps_to_shared", registered_name<registry_beta>() == shared);

        // THE BUG: the factory slot still belongs to the FIRST registrant.
        check("R7_factory_unchanged_after_second_register",
              factory_for_name(shared) == factory_after_first);
        check("R7_factory_owner_is_first_registrant",
              factory_for_name(shared) == factory_for<registry_alpha>());
        check("R7_factory_NOT_second_registrant",
              factory_for_name(shared) != factory_for<registry_beta>());
        // Confirm the asymmetry directly: factory pointer is alpha's even though
        // beta is the most-recent binding for this name in the type map.
        {
            void* const p{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5151)) };
            vmhook::object_base* built{ factory_for_name(shared)(p) };
            check("R7_factory_builds_first_type_not_second",
                  dynamic_cast<registry_alpha*>(built) != nullptr
                  && dynamic_cast<registry_beta*>(built) == nullptr);
            delete built;
        }
    }

    // =====================================================================
    // R8. Same-name REBIND of one factory family.  Rebinding the SAME class
    // name from First to Second (the SAME-name variant of bug #1): the factory
    // map keeps the FIRST factory.  This is the "class name rebound" case the
    // bug note calls out.  (Distinct from R7 which uses two different types; R8
    // proves the emplace no-op even when only the name's owner conceptually
    // changes.)
    // =====================================================================
    {
        map_state_guard guard{};

        const std::string name{ "vmhook/test/Rebind" };
        vmhook::g_type_factory_map.emplace(name, factory_for<registry_alpha>());
        const vmhook::type_factory_function_t first{ factory_for_name(name) };
        check("R8_first_emplace_wins", first == factory_for<registry_alpha>());

        // A second emplace under the same name with a DIFFERENT factory: no-op.
        vmhook::g_type_factory_map.emplace(name, factory_for<registry_beta>());
        check("R8_second_emplace_is_noop", factory_for_name(name) == first);
        check("R8_second_emplace_not_beta", factory_for_name(name) != factory_for<registry_beta>());
    }

    // =====================================================================
    // R9. Last-wins re-point leaves the OLD name's factory present (LIBRARY BUG
    // #2, pinned).  Re-registering the SAME type to a DIFFERENT class name
    // re-points type_to_class_map (last wins) but the OLD name's factory entry
    // stays in g_type_factory_map forever (no erase on re-point).  We pin both:
    // the type map now resolves to the NEW name, while the OLD name's factory is
    // still present (and still builds the type).
    // =====================================================================
    {
        map_state_guard guard{};

        register_in_maps<registry_alpha>("vmhook/test/OldName");
        check("R9_initial_name_oldname", registered_name<registry_alpha>() == "vmhook/test/OldName");
        check("R9_old_factory_present_after_first", factory_for_name("vmhook/test/OldName") != nullptr);

        // Re-point the SAME type to a NEW name (register_class last-wins on the
        // type map; the new name also gets its own factory via emplace).
        register_in_maps<registry_alpha>("vmhook/test/NewName");

        check("R9_type_repointed_to_newname", registered_name<registry_alpha>() == "vmhook/test/NewName");
        check("R9_new_factory_present", factory_for_name("vmhook/test/NewName") != nullptr);
        // The leak: the OLD name's factory survives the re-point.
        check("R9_old_name_factory_leaks_present", factory_for_name("vmhook/test/OldName") != nullptr);
        // And the stale factory still produces a live wrapper of the type.
        {
            void* const p{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x6262)) };
            vmhook::object_base* built{ factory_for_name("vmhook/test/OldName")(p) };
            check("R9_stale_factory_still_builds_type", dynamic_cast<registry_alpha*>(built) != nullptr);
            delete built;
        }
    }

    // =====================================================================
    // R10. Signature derivation through jni_signature_for_arg<> (no JVM).  This
    // is the live read-side of type_to_class_map the no-JVM suite can drive end
    // to end: the library builds a method descriptor token from the registered
    // class name for an object / unique_ptr<object> argument.
    //   * UNregistered wrapper -> "Ljava/lang/Object;" (deliberate fallback).
    //   * registered wrapper   -> "L<class-name>;", for both by-value W and
    //     unique_ptr<W>, and for cv/ref-qualified spellings (which decay).
    // =====================================================================
    {
        map_state_guard guard{};

        // Unregistered: both arms fall back to Ljava/lang/Object;.
        check("R10_sig_unregistered_object_falls_back",
              sig<registry_alpha>() == "Ljava/lang/Object;");
        check("R10_sig_unregistered_unique_ptr_falls_back",
              sig<std::unique_ptr<registry_alpha>>() == "Ljava/lang/Object;");

        // Register alpha -> a name and assert the L...; descriptor for every
        // spelling of the argument type.
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_alpha) }, std::string{ "vmhook/test/Widget" });

        check("R10_sig_registered_object_Lname",
              sig<registry_alpha>() == "Lvmhook/test/Widget;");
        check("R10_sig_registered_unique_ptr_Lname",
              sig<std::unique_ptr<registry_alpha>>() == "Lvmhook/test/Widget;");
        check("R10_sig_registered_const_object_Lname",
              sig<const registry_alpha>() == "Lvmhook/test/Widget;");
        check("R10_sig_registered_object_ref_Lname",
              sig<registry_alpha&>() == "Lvmhook/test/Widget;");
        check("R10_sig_registered_const_object_ref_Lname",
              sig<const registry_alpha&>() == "Lvmhook/test/Widget;");
        check("R10_sig_registered_const_unique_ptr_ref_Lname",
              sig<const std::unique_ptr<registry_alpha>&>() == "Lvmhook/test/Widget;");

        // A SECOND registered type yields its OWN descriptor — the builder reads
        // the per-type entry, no bleed-through from alpha.
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_beta) }, std::string{ "vmhook/test/Gadget" });
        check("R10_sig_second_type_own_name",
              sig<registry_beta>() == "Lvmhook/test/Gadget;");
        check("R10_sig_second_type_unique_ptr_own_name",
              sig<std::unique_ptr<registry_beta>>() == "Lvmhook/test/Gadget;");
        // A third, still-unregistered type continues to fall back.
        check("R10_sig_third_type_still_fallback",
              sig<registry_gamma>() == "Ljava/lang/Object;");
    }

    // =====================================================================
    // R11. jni_signature_for_arg reflects last-wins re-point of the type map
    // (the descriptor follows whatever the CURRENT type->name binding is).
    // =====================================================================
    {
        map_state_guard guard{};

        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_alpha) }, std::string{ "pkg/V1" });
        check("R11_sig_before_repoint", sig<registry_alpha>() == "Lpkg/V1;");

        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registry_alpha) }, std::string{ "pkg/V2" });
        check("R11_sig_after_repoint_follows_new_name", sig<registry_alpha>() == "Lpkg/V2;");
    }

    // =====================================================================
    // R12. get_class_methods<W>() / find_methods_by_signature<W>() short-circuit
    // to an EMPTY vector when W is not in type_to_class_map — they never call
    // find_class (no JVM reached), so they are deterministically empty here for
    // an unregistered type.  (When W IS registered, they go on to call
    // find_class, which needs a JVM — out of scope; we assert only the
    // unregistered early-out, which is the map-driven branch.)
    // =====================================================================
    {
        map_state_guard guard{};
        // Ensure these types are unregistered for this assertion regardless of
        // suite order (the guard restores afterwards).
        vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_gamma) });

        check("R12_get_class_methods_unregistered_empty",
              vmhook::get_class_methods<registry_gamma>().empty());
        check("R12_find_methods_by_signature_unregistered_empty",
              vmhook::find_methods_by_signature<registry_gamma>("(I)I").empty());
    }

    // =====================================================================
    // R13. for_each_instance<T>() early-outs to 0 visits when T is not in
    // type_to_class_map — it returns BEFORE touching any Universe / CollectedHeap
    // VMStruct, so it is safe and deterministic with no JVM for an unregistered
    // type.  The visitor must never be invoked.
    // =====================================================================
    {
        map_state_guard guard{};
        vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_gamma) });

        std::size_t visitor_calls{ 0 };
        std::size_t visited{ std::numeric_limits<std::size_t>::max() };
        bool threw{ false };
        try
        {
            visited = vmhook::for_each_instance<registry_gamma>(
                [&visitor_calls](std::unique_ptr<registry_gamma>) { ++visitor_calls; });
        }
        catch (...) { threw = true; }

        check("R13_for_each_instance_unregistered_returns_zero", visited == 0u);
        check("R13_for_each_instance_unregistered_no_visitor_call", visitor_calls == 0u);
        check("R13_for_each_instance_unregistered_does_not_throw", !threw);
    }

    // =====================================================================
    // R14. make_unique<T> stays null with NO JVM even when T IS registered in
    // BOTH maps — the ensure_current_java_thread() guard wins before the map
    // lookup, so the no-JVM contract is independent of registration state.
    // (Complements the earlier unregistered make_unique checks.)
    // =====================================================================
    {
        map_state_guard guard{};

        register_in_maps<registry_unmapped>("vmhook/test/Mapped");
        check("R14_precondition_type_registered", type_is_registered<registry_unmapped>());
        check("R14_precondition_factory_present", factory_for_name("vmhook/test/Mapped") != nullptr);

        std::unique_ptr<registry_unmapped> obj{ reinterpret_cast<registry_unmapped*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<registry_unmapped>(); }
        catch (...) { threw = true; }
        check("R14_make_unique_registered_no_jvm_still_null", obj == nullptr);
        check("R14_make_unique_registered_no_jvm_does_not_throw", !threw);
    }

    // =====================================================================
    // R15. Map-state ISOLATION proof.  After a nested map_state_guard scope
    // mutates BOTH maps heavily, the maps are restored to EXACTLY their
    // pre-scope contents — size AND per-key values for the keys we touched.
    // This is the guarantee that keeps the whole no-JVM suite order-independent.
    // =====================================================================
    {
        const std::size_t outer_types{ vmhook::type_to_class_map.size() };
        const std::size_t outer_factory{ vmhook::g_type_factory_map.size() };
        // Snapshot the specific keys we will scribble on, so we can prove the
        // restore is value-exact (not just size-exact).
        const bool alpha_present_before{ type_is_registered<registry_alpha>() };
        const std::string alpha_name_before{ registered_name<registry_alpha>() };

        {
            map_state_guard guard{};
            register_in_maps<registry_alpha>("vmhook/test/Scribble");
            register_in_maps<registry_beta>("vmhook/test/Scribble2");
            register_in_maps<registry_gamma>("vmhook/test/Scribble3");
            // Inside the scope the maps definitely grew / changed.
            check("R15_inside_scope_alpha_registered", registered_name<registry_alpha>() == "vmhook/test/Scribble");
            check("R15_inside_scope_factory_present", factory_for_name("vmhook/test/Scribble") != nullptr);
        } // guard restores here

        check("R15_type_map_size_restored", vmhook::type_to_class_map.size() == outer_types);
        check("R15_factory_map_size_restored", vmhook::g_type_factory_map.size() == outer_factory);
        check("R15_alpha_presence_restored", type_is_registered<registry_alpha>() == alpha_present_before);
        check("R15_alpha_name_restored", registered_name<registry_alpha>() == alpha_name_before);
        // The scribble name we added is gone after restore.
        check("R15_scribble_factory_gone_after_restore", factory_for_name("vmhook/test/Scribble") == nullptr);
        check("R15_scribble_type_gone_after_restore", !type_is_registered<registry_beta>());
    }

    // =====================================================================
    // R16. jni_signature_for_arg<> EXHAUSTIVE primitive / string descriptor
    // table.  This is a pure compile-time dispatch (noexcept, no map lookup for
    // the non-wrapper arms), so it is 100% deterministic with no JVM and every
    // JVM primitive's descriptor is pinned to its exact one-character token.
    // These are the exact tokens the library appends when building a method
    // descriptor for a hook's argument list; a wrong token silently mis-resolves
    // overloaded methods (the very bug the dispatch table was written to fix).
    // =====================================================================
    {
        // No map mutation happens in this block, but keep the guard so the
        // section is uniform with its siblings and self-restores regardless.
        map_state_guard guard{};

        // bool -> "Z" (the ONLY type allowed to match the boolean descriptor).
        check("R16_sig_bool_Z", sig<bool>() == "Z");
        // Java byte is signed 8-bit; both 8-bit integrals map to "B".
        check("R16_sig_int8_B", sig<std::int8_t>() == "B");
        check("R16_sig_uint8_B", sig<std::uint8_t>() == "B");
        // Java short -> "S" (signed 16-bit).
        check("R16_sig_int16_S", sig<std::int16_t>() == "S");
        // Java char -> "C" (UNSIGNED 16-bit / UTF-16 code unit).  BOTH the
        // std::uint16_t alias and the distinct char16_t type now resolve to "C",
        // matching method_proxy::argument_matches_descriptor.  Previously
        // sig<char16_t>() hit the terminal dependent_false_v static_assert and
        // FAILED TO COMPILE because the only 2-byte arm was an exact
        // is_same_v<uint16_t> test; the generic sizeof-based ladder now admits it.
        check("R16_sig_uint16_C", sig<std::uint16_t>() == "C");
        check("R16_sig_char16_t_C", sig<char16_t>() == "C");
        // 32-bit integrals -> "I".
        check("R16_sig_int32_I", sig<std::int32_t>() == "I");
        check("R16_sig_uint32_I", sig<std::uint32_t>() == "I");
        // 64-bit integrals -> "J".
        check("R16_sig_int64_J", sig<std::int64_t>() == "J");
        check("R16_sig_uint64_J", sig<std::uint64_t>() == "J");
        // Floating point.
        check("R16_sig_float_F", sig<float>() == "F");
        check("R16_sig_double_D", sig<double>() == "D");
        // Every string-like arg -> the java.lang.String descriptor.
        check("R16_sig_std_string", sig<std::string>() == "Ljava/lang/String;");
        check("R16_sig_string_view", sig<std::string_view>() == "Ljava/lang/String;");
        check("R16_sig_const_char_ptr", sig<const char*>() == "Ljava/lang/String;");
        check("R16_sig_char_ptr", sig<char*>() == "Ljava/lang/String;");
        // cv/ref spellings decay to the same descriptor (std::decay_t in the
        // builder), so a `const std::string&` parameter encodes identically.
        check("R16_sig_const_string_ref", sig<const std::string&>() == "Ljava/lang/String;");
        check("R16_sig_int32_ref_decays", sig<std::int32_t&>() == "I");
        check("R16_sig_const_bool_decays", sig<const bool>() == "Z");
        check("R16_sig_volatile_double_decays", sig<volatile double>() == "D");
        // The descriptor builder is total over its supported domain — every token
        // above is non-empty and a single char for primitives.
        check("R16_sig_bool_single_char", sig<bool>().size() == 1u);
        check("R16_sig_long_single_char", sig<std::int64_t>().size() == 1u);
    }

    // =====================================================================
    // R16b. DISPATCHER-AGREEMENT MATRIX (regression guard for the integral-domain
    // split between the two sibling classifiers).  detail::jni_signature_for_arg<T>
    // (the descriptor BUILDER) and method_proxy::argument_matches_descriptor<T>
    // (the overload SELECTOR) must classify the SAME C++ arg type domain into the
    // SAME JVM token.  They used to disagree: the builder classified sub-int
    // integrals by EXACT std::is_same_v (so plain char / char16_t / wchar_t /
    // char8_t / char32_t hit a terminal static_assert and FAILED TO COMPILE),
    // while the selector classified them generically by sizeof and ACCEPTED them
    // — so a detour arg type the selector matched could never have a JNI signature
    // built.  Every assertion below derives BOTH sides from is_integral/sizeof via
    // selector_token<T> (the argument_matches_descriptor-equivalent), so nothing
    // is hardcoded per platform: wchar_t agrees as "S" where it is 2 bytes and
    // "I" where it is 4 bytes, char agrees as "B" under either signedness, and
    // long / long long agree as "I" or "J" per data model — all with no #ifdef.
    // The mere fact that this block COMPILES (it instantiates sig<char16_t>(),
    // sig<wchar_t>(), sig<char>(), ... ) is itself the proof of the fix; it was a
    // hard compile error before.
    // =====================================================================
    {
        map_state_guard guard{};

        // --- The five formerly-uncompilable extended character types. ---
        check_signature_agrees<char>("R16b_agree_char");
        check_signature_agrees<char8_t>("R16b_agree_char8_t");
        check_signature_agrees<char16_t>("R16b_agree_char16_t");
        check_signature_agrees<char32_t>("R16b_agree_char32_t");
        check_signature_agrees<wchar_t>("R16b_agree_wchar_t");

        // --- The plain signed/unsigned char trio (char is distinct from both). ---
        check_signature_agrees<signed char>("R16b_agree_signed_char");
        check_signature_agrees<unsigned char>("R16b_agree_unsigned_char");

        // --- Every standard integer rank, signed and unsigned. ---
        check_signature_agrees<short>("R16b_agree_short");
        check_signature_agrees<unsigned short>("R16b_agree_unsigned_short");
        check_signature_agrees<int>("R16b_agree_int");
        check_signature_agrees<unsigned int>("R16b_agree_unsigned_int");
        check_signature_agrees<long>("R16b_agree_long");
        check_signature_agrees<unsigned long>("R16b_agree_unsigned_long");
        check_signature_agrees<long long>("R16b_agree_long_long");
        check_signature_agrees<unsigned long long>("R16b_agree_unsigned_long_long");

        // --- bool and the floating-point types. ---
        check_signature_agrees<bool>("R16b_agree_bool");
        check_signature_agrees<float>("R16b_agree_float");
        check_signature_agrees<double>("R16b_agree_double");

        // --- The fixed-width <cstdint> aliases (must keep their legacy letters). ---
        check_signature_agrees<std::int8_t>("R16b_agree_int8_t");
        check_signature_agrees<std::uint8_t>("R16b_agree_uint8_t");
        check_signature_agrees<std::int16_t>("R16b_agree_int16_t");
        check_signature_agrees<std::uint16_t>("R16b_agree_uint16_t");
        check_signature_agrees<std::int32_t>("R16b_agree_int32_t");
        check_signature_agrees<std::uint32_t>("R16b_agree_uint32_t");
        check_signature_agrees<std::int64_t>("R16b_agree_int64_t");
        check_signature_agrees<std::uint64_t>("R16b_agree_uint64_t");

        // --- cv / reference spellings decay identically on BOTH dispatchers. ---
        check_signature_agrees<const char16_t>("R16b_agree_const_char16_t");
        check_signature_agrees<wchar_t&>("R16b_agree_wchar_t_ref");
        check_signature_agrees<const volatile long>("R16b_agree_cv_long");

        // --- Spot-pin the post-fix absolute letters for the NEW character types,
        // derived (not hardcoded) so they remain data-model-independent.  char is
        // 1 byte => "B"; char16_t is the UTF-16 code unit => "C"; char32_t is 4
        // bytes => "I"; char8_t is 1 byte => "B". ---
        check("R16b_char_is_B", sig<char>() == "B");
        check("R16b_char8_t_is_B", sig<char8_t>() == "B");
        check("R16b_char16_t_is_C", sig<char16_t>() == "C");
        check("R16b_char32_t_is_I", sig<char32_t>() == "I");
        // wchar_t: assert the letter that MATCHES its actual width on this target,
        // computed from sizeof so the same line is correct on Windows (2 -> "S")
        // and on *nix (4 -> "I").  No platform literal.
        {
            const std::string_view expected_wchar{
                sizeof(wchar_t) == 2u ? std::string_view{ "S" }
                : sizeof(wchar_t) == 4u ? std::string_view{ "I" }
                : sizeof(wchar_t) == 1u ? std::string_view{ "B" }
                : std::string_view{ "J" } };
            check("R16b_wchar_t_matches_width", std::string_view{ sig<wchar_t>() } == expected_wchar);
        }
    }

    // =====================================================================
    // R16c. RESIDUAL CHAR-TYPE GAPS for the formerly-static_asserted extended
    // character types.  R16/R16b pin the agreement and the canonical letters for
    // char / char8_t / char16_t / char32_t / wchar_t, but leave three thin gaps
    // this block closes — all ADDITIVE, all derived (no platform literal), and all
    // exercising types that USED to hit the terminal dependent_false_v
    // static_assert and fail to compile before the generic-width ladder landed:
    //
    //   (a) `signed char` / `unsigned char` absolute letter.  R16b only checks
    //       these via the agreement helper; pin them to "B" outright so a future
    //       regression that flips the sizeof-1 arm is caught even if the selector
    //       mirror regresses in lockstep.
    //   (b) cv / reference SPELLINGS of the new char types decay to the SAME token
    //       as the bare type (the builder runs std::decay_t first).  R16b pins
    //       only `const char16_t` / `wchar_t&`; extend to char8_t / char32_t / a
    //       reference char16_t to lock the precedence under qualifier stripping.
    //   (c) TOTALITY: every new char arm yields a non-empty, single-character
    //       primitive token (R16 pins this only for bool / int64).
    // =====================================================================
    {
        map_state_guard guard{};

        // (a) The plain signed/unsigned char pair: Java `byte` is signed 8-bit,
        // both 1-byte chars encode as "B" regardless of signedness.
        check("R16c_signed_char_is_B", sig<signed char>() == "B");
        check("R16c_unsigned_char_is_B", sig<unsigned char>() == "B");

        // (b) cv / ref qualifier stripping reaches the identical canonical letter.
        // char16_t stays "C" (the UTF-16 precedence claimed before the generic
        // 2-byte arm — the backlog's flagged char16_t->"C" case), under both a
        // reference and a top-level const spelling.
        check("R16c_char16_t_ref_decays_C", sig<char16_t&>() == "C");
        check("R16c_const_char16_t_decays_C", sig<const char16_t>() == "C");
        // char8_t (1 byte -> "B") and char32_t (4 bytes -> "I") strip identically.
        check("R16c_const_char8_t_decays_B", sig<const char8_t>() == "B");
        check("R16c_char32_t_ref_decays_I", sig<char32_t&>() == "I");
        check("R16c_const_volatile_char32_t_decays_I", sig<const volatile char32_t>() == "I");

        // (c) Totality over the new char arms: each is a single-character,
        // non-empty primitive token (never the empty string the static_assert
        // path would have made impossible by failing to compile at all).
        check("R16c_char_single_char", sig<char>().size() == 1u);
        check("R16c_char8_t_single_char", sig<char8_t>().size() == 1u);
        check("R16c_char16_t_single_char", sig<char16_t>().size() == 1u);
        check("R16c_char32_t_single_char", sig<char32_t>().size() == 1u);
        check("R16c_wchar_t_single_char", sig<wchar_t>().size() == 1u);
        check("R16c_char_non_empty", !sig<char>().empty());
        check("R16c_wchar_t_non_empty", !sig<wchar_t>().empty());
    }

    // =====================================================================
    // R17. register_class<T>() rejects EVERY class-name shape with no JVM.
    // find_class(name) fails before any insert, so the call is false / no-throw
    // / both-maps-untouched for the empty name, garbage, special characters, a
    // dotted (wrong-separator) name, a leading-slash name, an extremely long
    // name, and a name carrying embedded high bytes.  Each runs under its own
    // guard and on a distinct never-seen type so suite order is irrelevant.
    // =====================================================================
    {
        map_state_guard guard{};
        register_class_rejects_no_jvm<registry_alpha>("R17_empty_name", "");
    }
    {
        map_state_guard guard{};
        register_class_rejects_no_jvm<registry_beta>("R17_garbage_name", "!!!not a class!!!");
    }
    {
        map_state_guard guard{};
        register_class_rejects_no_jvm<registry_gamma>("R17_dotted_name", "java.lang.Object");
    }
    {
        map_state_guard guard{};
        register_class_rejects_no_jvm<registry_delta>("R17_leading_slash_name", "/java/lang/Object");
    }
    {
        map_state_guard guard{};
        register_class_rejects_no_jvm<registry_unmapped>("R17_whitespace_name", "   ");
    }
    {
        map_state_guard guard{};
        const std::string very_long(8192, 'A');
        register_class_rejects_no_jvm<registry_alpha>("R17_very_long_name", very_long);
    }
    {
        map_state_guard guard{};
        // Embedded NUL + high bytes: still just a string find_class can't resolve.
        const std::string odd_bytes{ std::string("pkg\0\xC3\xA9/Z", 8) };
        register_class_rejects_no_jvm<registry_beta>("R17_embedded_nul_high_bytes",
                                                     std::string_view{ odd_bytes.data(), odd_bytes.size() });
    }
    {
        map_state_guard guard{};
        // A plausibly-real but definitely-not-loaded name: still rejected no-JVM.
        register_class_rejects_no_jvm<registry_gamma>("R17_plausible_unloaded_name",
                                                      "com/example/definitely/Not/Loaded$Inner");
    }

    // =====================================================================
    // R18. Factory invocation across MANY sentinel pointer values.  The factory
    // is `new W{void*}`; it stores the pointer verbatim, so get_instance() must
    // return the EXACT value passed for any bit pattern — null, one, a typical
    // aligned heap address, an odd address, and the maximum representable
    // pointer.  Each build is the registered dynamic type.  No VM access.
    // =====================================================================
    {
        map_state_guard guard{};
        register_in_maps<registry_alpha>("vmhook/test/SentinelAlpha");
        const vmhook::type_factory_function_t f{ factory_for_name("vmhook/test/SentinelAlpha") };
        check("R18_factory_present", f != nullptr);

        // Width-safe sentinel set: every value is derived so it fits std::uintptr_t
        // on a 32-bit OR 64-bit pointer (no fixed wide literal that would narrow on
        // ILP32).  Covers null, one, aligned, an arbitrary mid-range value, the
        // high half of the address space, and all-bits-set.
        constexpr std::uintptr_t uintptr_max{ std::numeric_limits<std::uintptr_t>::max() };
        const std::array<std::uintptr_t, 6> sentinels{ {
            std::uintptr_t{ 0 },
            std::uintptr_t{ 1 },
            std::uintptr_t{ 0x8 },
            std::uintptr_t{ 0xABCDEF01 },     // fits 32-bit unsigned, so width-safe
            uintptr_max >> 1,                 // high half of the address space
            uintptr_max,                      // all bits set
        } };
        for (std::size_t i{ 0 }; i < sentinels.size(); ++i)
        {
            void* const sentinel{ reinterpret_cast<void*>(sentinels[i]) };
            const std::string tag{ "R18_sentinel_" + std::to_string(i) };
            factory_builds<registry_alpha>(tag.c_str(), f, sentinel);
        }
    }

    // =====================================================================
    // R19. Factory built at nullptr: a null Java reference decodes to a wrapper
    // whose get_instance() is nullptr (the wrapper is still a valid, non-null
    // object — only the OOP it carries is null).  This is exactly what the
    // factory does for a null arg, and it must not be confused with "no wrapper".
    // =====================================================================
    {
        map_state_guard guard{};
        register_in_maps<registry_beta>("vmhook/test/NullOop");
        const vmhook::type_factory_function_t f{ factory_for_name("vmhook/test/NullOop") };
        vmhook::object_base* const built{ f ? f(nullptr) : nullptr };
        check("R19_factory_returns_wrapper_for_null_oop", built != nullptr);
        check("R19_wrapper_holds_null_oop", built != nullptr && built->get_instance() == nullptr);
        check("R19_wrapper_dynamic_type_is_beta", dynamic_cast<registry_beta*>(built) != nullptr);
        delete built;
    }

    // =====================================================================
    // R20. jni::make_unique<W>(class_name, args...) — the by-NAME factory
    // (NewObjectA path).  It is noexcept and calls find_class(class_name) first,
    // which fails with no JVM, so it returns a null unique_ptr for every arg
    // shape — independent of whether W is registered in the type map.  This is a
    // distinct entry point from the by-type make_unique<W>() above (which keys
    // off the type map; this one keys purely off the name argument).
    // =====================================================================
    {
        map_state_guard guard{};
        // Unregistered type, no ctor args.
        std::unique_ptr<registry_alpha> a{ reinterpret_cast<registry_alpha*>(0) };
        bool threw{ false };
        try { a = vmhook::jni::make_unique<registry_alpha>(std::string{ "vmhook/test/ByName" }); }
        catch (...) { threw = true; }
        check("R20_by_name_no_args_no_jvm_null", a == nullptr);
        check("R20_by_name_no_args_does_not_throw", !threw);
    }
    {
        map_state_guard guard{};
        // With a mix of primitive + string args — exercises make_jni_args /
        // signature assembly instantiation, still nullptr (find_class fails).
        std::unique_ptr<factory_wrapper_with_ctor> w{ reinterpret_cast<factory_wrapper_with_ctor*>(0) };
        bool threw{ false };
        try { w = vmhook::jni::make_unique<factory_wrapper_with_ctor>(std::string{ "vmhook/test/ByName2" }, 42, std::string{ "x" }); }
        catch (...) { threw = true; }
        check("R20_by_name_with_args_no_jvm_null", w == nullptr);
        check("R20_by_name_with_args_does_not_throw", !threw);
    }
    {
        map_state_guard guard{};
        // Even when the type IS registered in both maps, by-name make_unique is
        // null with no JVM (find_class still fails — registration is irrelevant
        // to this entry point, which keys purely off the name argument).
        register_in_maps<registry_gamma>("vmhook/test/ByName3");
        std::unique_ptr<registry_gamma> g{ reinterpret_cast<registry_gamma*>(0) };
        bool threw{ false };
        try { g = vmhook::jni::make_unique<registry_gamma>(std::string{ "vmhook/test/ByName3" }); }
        catch (...) { threw = true; }
        check("R20_by_name_registered_no_jvm_null", g == nullptr);
        check("R20_by_name_registered_does_not_throw", !threw);
    }

    // =====================================================================
    // R21. make_unique<W>() (by-type) no-JVM nullptr across MORE arg arities and
    // types — the ensure_current_java_thread() guard fires before the type-map
    // lookup for every arity, so registration state and arg shape never change
    // the contract.  Complements the earlier R14 / make_unique checks.
    // =====================================================================
    {
        map_state_guard guard{};
        register_in_maps<registry_delta>("vmhook/test/MUDelta");
        check("R21_precondition_registered", type_is_registered<registry_delta>());

        // No args.
        {
            std::unique_ptr<registry_delta> d{ reinterpret_cast<registry_delta*>(0) };
            bool threw{ false };
            try { d = vmhook::make_unique<registry_delta>(); }
            catch (...) { threw = true; }
            check("R21_no_args_null", d == nullptr);
            check("R21_no_args_no_throw", !threw);
        }
        // Single int arg.
        {
            std::unique_ptr<registry_delta> d{ reinterpret_cast<registry_delta*>(0) };
            bool threw{ false };
            try { d = vmhook::make_unique<registry_delta>(1); }
            catch (...) { threw = true; }
            check("R21_one_arg_null", d == nullptr);
            check("R21_one_arg_no_throw", !threw);
        }
        // Several heterogeneous args (int, bool, double, long).
        {
            std::unique_ptr<registry_delta> d{ reinterpret_cast<registry_delta*>(0) };
            bool threw{ false };
            try { d = vmhook::make_unique<registry_delta>(1, true, 2.0, std::int64_t{ 3 }); }
            catch (...) { threw = true; }
            check("R21_many_args_null", d == nullptr);
            check("R21_many_args_no_throw", !threw);
        }
    }

    // =====================================================================
    // R22. type_index distinctness + factory-pointer distinctness across ALL
    // wrapper types.  Different C++ types have distinct std::type_index values
    // (so the per-type map keying never collides), and factory_for<T>() yields a
    // distinct function pointer per T (so a distinct name binds a distinct
    // factory).  These identities are platform-invariant and need no JVM.
    // =====================================================================
    {
        // Distinct type_index per wrapper (the type map's key space).
        const std::array<std::type_index, 5> idx{ {
            std::type_index{ typeid(registry_alpha) },
            std::type_index{ typeid(registry_beta) },
            std::type_index{ typeid(registry_gamma) },
            std::type_index{ typeid(registry_delta) },
            std::type_index{ typeid(registry_unmapped) },
        } };
        bool all_distinct{ true };
        for (std::size_t i{ 0 }; i < idx.size(); ++i)
        {
            for (std::size_t j{ i + 1 }; j < idx.size(); ++j)
            {
                if (idx[i] == idx[j]) { all_distinct = false; }
            }
        }
        check("R22_all_type_indices_distinct", all_distinct);

        // Distinct factory function pointer per wrapper type.
        const std::array<vmhook::type_factory_function_t, 5> fac{ {
            factory_for<registry_alpha>(),
            factory_for<registry_beta>(),
            factory_for<registry_gamma>(),
            factory_for<registry_delta>(),
            factory_for<registry_unmapped>(),
        } };
        bool factories_distinct{ true };
        for (std::size_t i{ 0 }; i < fac.size(); ++i)
        {
            for (std::size_t j{ i + 1 }; j < fac.size(); ++j)
            {
                if (fac[i] == fac[j]) { factories_distinct = false; }
            }
        }
        check("R22_all_factory_pointers_distinct", factories_distinct);
        // factory_for<T>() is a stable identity: the same T yields the same
        // pointer on every call (it decays the SAME lambda each time).
        check("R22_factory_for_is_stable_per_type",
              factory_for<registry_alpha>() == factory_for<registry_alpha>());
        check("R22_factory_for_non_null", factory_for<registry_alpha>() != nullptr);
    }

    // =====================================================================
    // R23. for_each_instance<T>() early-out is robust across visitor signatures
    // and max_visits values when T is unregistered: it returns 0, never calls
    // the visitor, and never throws — BEFORE touching any heap VMStruct.  We
    // vary the cap (0, 1, and unlimited via the default) to prove the early-out
    // precedes any cap handling.
    // =====================================================================
    {
        map_state_guard guard{};
        vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_gamma) });

        // Default (unlimited) cap, by-value unique_ptr visitor.
        {
            std::size_t calls{ 0 };
            std::size_t visited{ std::numeric_limits<std::size_t>::max() };
            bool threw{ false };
            try
            {
                visited = vmhook::for_each_instance<registry_gamma>(
                    [&calls](std::unique_ptr<registry_gamma>) { ++calls; });
            }
            catch (...) { threw = true; }
            check("R23_default_cap_returns_zero", visited == 0u);
            check("R23_default_cap_no_visitor_call", calls == 0u);
            check("R23_default_cap_no_throw", !threw);
        }
        // Explicit cap of 0.
        {
            std::size_t calls{ 0 };
            std::size_t visited{ std::numeric_limits<std::size_t>::max() };
            bool threw{ false };
            try
            {
                visited = vmhook::for_each_instance<registry_gamma>(
                    [&calls](std::unique_ptr<registry_gamma>) { ++calls; }, std::size_t{ 0 });
            }
            catch (...) { threw = true; }
            check("R23_cap0_returns_zero", visited == 0u);
            check("R23_cap0_no_visitor_call", calls == 0u);
            check("R23_cap0_no_throw", !threw);
        }
        // Explicit cap of 1.
        {
            std::size_t calls{ 0 };
            std::size_t visited{ std::numeric_limits<std::size_t>::max() };
            bool threw{ false };
            try
            {
                visited = vmhook::for_each_instance<registry_gamma>(
                    [&calls](std::unique_ptr<registry_gamma>) { ++calls; }, std::size_t{ 1 });
            }
            catch (...) { threw = true; }
            check("R23_cap1_returns_zero", visited == 0u);
            check("R23_cap1_no_visitor_call", calls == 0u);
            check("R23_cap1_no_throw", !threw);
        }
        // A second distinct unregistered type behaves identically (no bleed).
        {
            vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_delta) });
            std::size_t calls{ 0 };
            std::size_t visited{ std::numeric_limits<std::size_t>::max() };
            bool threw{ false };
            try
            {
                visited = vmhook::for_each_instance<registry_delta>(
                    [&calls](std::unique_ptr<registry_delta>) { ++calls; });
            }
            catch (...) { threw = true; }
            check("R23_second_type_returns_zero", visited == 0u);
            check("R23_second_type_no_visitor_call", calls == 0u);
            check("R23_second_type_no_throw", !threw);
        }
    }

    // =====================================================================
    // R24. get_class_methods<W>() / find_methods_by_signature<W>() are empty for
    // an unregistered W regardless of the descriptor queried — the type-map miss
    // short-circuits before any find_class, so NO descriptor string can make the
    // result non-empty.  Sweep a spread of descriptor shapes (valid, empty,
    // garbage) and several unregistered types; all stay empty and never throw.
    // =====================================================================
    {
        map_state_guard guard{};
        vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_gamma) });
        vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_delta) });
        vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_unmapped) });

        check("R24_methods_gamma_empty", vmhook::get_class_methods<registry_gamma>().empty());
        check("R24_methods_delta_empty", vmhook::get_class_methods<registry_delta>().empty());
        check("R24_methods_unmapped_empty", vmhook::get_class_methods<registry_unmapped>().empty());

        const std::array<std::string_view, 6> descriptors{ {
            std::string_view{ "(I)I" },
            std::string_view{ "()V" },
            std::string_view{ "(Ljava/lang/String;)Z" },
            std::string_view{ "" },
            std::string_view{ "not-a-descriptor" },
            std::string_view{ "(((" },
        } };
        for (std::size_t i{ 0 }; i < descriptors.size(); ++i)
        {
            const std::string tag{ "R24_find_gamma_desc_" + std::to_string(i) + "_empty" };
            check(tag.c_str(), vmhook::find_methods_by_signature<registry_gamma>(descriptors[i]).empty());
        }
        // A different unregistered type, same sweep result.
        check("R24_find_delta_valid_desc_empty",
              vmhook::find_methods_by_signature<registry_delta>("(I)I").empty());
        check("R24_find_unmapped_empty_desc_empty",
              vmhook::find_methods_by_signature<registry_unmapped>("").empty());
    }

    // =====================================================================
    // R25. Multi-rebind chain semantics on BOTH maps (no JVM).  Re-pointing one
    // type across a sequence of names (insert_or_assign, last wins) keeps exactly
    // one type-map entry that always resolves to the most-recent name, while each
    // name visited along the way keeps its own factory (emplace, first wins, no
    // erase) — so the factory map only ever GROWS.  This pins both the last-wins
    // type binding and the monotonic factory accumulation (bug #2 at scale).
    // =====================================================================
    {
        map_state_guard guard{};
        const std::size_t factory_before{ vmhook::g_type_factory_map.size() };

        register_in_maps<registry_alpha>("vmhook/test/Chain/N0");
        register_in_maps<registry_alpha>("vmhook/test/Chain/N1");
        register_in_maps<registry_alpha>("vmhook/test/Chain/N2");
        register_in_maps<registry_alpha>("vmhook/test/Chain/N3");

        // Exactly one type-map entry, resolving to the LAST name.
        check("R25_single_type_entry_after_chain",
              vmhook::type_to_class_map.count(std::type_index{ typeid(registry_alpha) }) == 1u);
        check("R25_resolves_to_last_name", registered_name<registry_alpha>() == "vmhook/test/Chain/N3");

        // Every name along the chain kept its factory (4 distinct names added).
        check("R25_n0_factory_present", factory_for_name("vmhook/test/Chain/N0") != nullptr);
        check("R25_n1_factory_present", factory_for_name("vmhook/test/Chain/N1") != nullptr);
        check("R25_n2_factory_present", factory_for_name("vmhook/test/Chain/N2") != nullptr);
        check("R25_n3_factory_present", factory_for_name("vmhook/test/Chain/N3") != nullptr);
        check("R25_factory_map_grew_by_four",
              vmhook::g_type_factory_map.size() == factory_before + 4u);
        // All four name-keyed factories are the SAME pointer (same type alpha),
        // since factory_for<alpha>() decays the same lambda — name keying does
        // not change the underlying function, only the slot.
        check("R25_all_chain_factories_are_alpha",
              factory_for_name("vmhook/test/Chain/N0") == factory_for<registry_alpha>()
              && factory_for_name("vmhook/test/Chain/N3") == factory_for<registry_alpha>());
    }

    // =====================================================================
    // R26. Many distinct registered types resolve their OWN descriptor with no
    // cross-talk, at scale.  We register four distinct wrapper types to four
    // distinct names simultaneously and assert jni_signature_for_arg<W> returns
    // each type's own L<name>; for both the by-value and unique_ptr<W> spelling,
    // while a fifth unregistered type still falls back.  This is the read-side of
    // the per-type type-map keying exercised end-to-end with no JVM.
    // =====================================================================
    {
        map_state_guard guard{};
        vmhook::type_to_class_map.insert_or_assign(std::type_index{ typeid(registry_alpha) }, std::string{ "pkg/A" });
        vmhook::type_to_class_map.insert_or_assign(std::type_index{ typeid(registry_beta) },  std::string{ "pkg/B" });
        vmhook::type_to_class_map.insert_or_assign(std::type_index{ typeid(registry_gamma) }, std::string{ "pkg/G" });
        vmhook::type_to_class_map.insert_or_assign(std::type_index{ typeid(registry_delta) }, std::string{ "pkg/D" });
        vmhook::type_to_class_map.erase(std::type_index{ typeid(registry_unmapped) });

        check("R26_alpha_own_desc", sig<registry_alpha>() == "Lpkg/A;");
        check("R26_beta_own_desc", sig<registry_beta>() == "Lpkg/B;");
        check("R26_gamma_own_desc", sig<registry_gamma>() == "Lpkg/G;");
        check("R26_delta_own_desc", sig<registry_delta>() == "Lpkg/D;");

        check("R26_alpha_uptr_own_desc", sig<std::unique_ptr<registry_alpha>>() == "Lpkg/A;");
        check("R26_beta_uptr_own_desc", sig<std::unique_ptr<registry_beta>>() == "Lpkg/B;");
        check("R26_gamma_uptr_own_desc", sig<std::unique_ptr<registry_gamma>>() == "Lpkg/G;");
        check("R26_delta_uptr_own_desc", sig<std::unique_ptr<registry_delta>>() == "Lpkg/D;");

        // No two distinct registered types share a descriptor here.
        check("R26_descs_pairwise_distinct",
              sig<registry_alpha>() != sig<registry_beta>()
              && sig<registry_beta>() != sig<registry_gamma>()
              && sig<registry_gamma>() != sig<registry_delta>()
              && sig<registry_alpha>() != sig<registry_delta>());

        // The fifth, unregistered type still falls back to Object.
        check("R26_unmapped_falls_back", sig<registry_unmapped>() == "Ljava/lang/Object;");
    }

    return failures == 0 ? 0 : 1;
}
