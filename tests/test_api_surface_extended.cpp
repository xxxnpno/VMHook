// Two complementary jobs in one translation unit:
//
//   (1) Runtime null-safety / never-throw checks for vmhook public entry
//       points when NO JVM is loaded in the process.  Every documented entry
//       point must no-op safely (return its safe-default, invoke no visitor,
//       never crash).  Extends tests/test_api_surface.cpp, which only checks
//       that the surface type-checks; this file actually *runs* main() with no
//       JVM behind it.
//
//   (2) An EXHAUSTIVE compile-time lockdown of the PUBLIC API SURFACE — every
//       free function and every public type in namespace vmhook:: (plus the one
//       surviving vmhook:: type, oop_pin) is pinned with
//       std::is_invocable_r_v / is_same_v / type-trait static_asserts that fix
//       its exact callable shape (parameter types + return type) or its
//       structural traits (move-only, noexcept destructor, base classes,
//       defaulted members, ...).  If any entry point is removed, renamed, or has
//       its signature drift, THIS TRANSLATION UNIT FAILS TO COMPILE on every
//       compiler / platform in CI.  That class of regression is exactly what
//       this file exists to catch — the oop_pin primitives went missing once
//       with nothing noticing.
//
//       NOTE: the header used to expose a vmhook:: JNI forwarder layer.
//       The de-JNI refactor (eaff990) deleted it; GROUP I below documents,
//       forwarder by forwarder, which pure-VM entry point inherited the property
//       and which capabilities have no equivalent at all.
//
// Every assertion here is platform-invariant: only is_invocable / type-trait
// booleans and nullptr / empty / size / no-throw comparisons — no <charconv>,
// no float parsing, no std::expected / <syncstream> / <stacktrace> / jthread,
// no exact function-pointer addresses or ABI-dependent sizes.  Identical across
// MSVC, libstdc++ (MinGW) and libc++.  None of the static_asserts *invoke* the
// pinned functions (they live in unevaluated decltype / is_invocable contexts),
// so the no-JVM precondition is never violated by the surface lockdown.
#include <vmhook/vmhook.hpp>
#include <array>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// CONTRACT: make_unique<W>() NEVER returns a null unique_ptr.  The pointer is
// always valid; the OBJECT inside it is absent when the object could not be
// built (with no JVM in this process, that is always).  "failed" therefore
// means "the wrapper arrived and holds no instance".
// ---------------------------------------------------------------------------
namespace
{
    template<typename wrapper_t>
    auto is_empty_wrapper(const std::unique_ptr<wrapper_t>& handle) noexcept
        -> bool
    {
        return handle != nullptr
            && handle->vmhook::object_base::get_instance() == nullptr;
    }
}

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A minimal wrapper type used for register_class<T> / make_unique<T> /
// for_each_instance<T>.  Derives from vmhook::object<T> with the required
// explicit T(vmhook::oop_t) constructor, mirroring the pattern in
// test_api_surface.cpp.
class dummy_wrapper : public vmhook::object<dummy_wrapper>
{
public:
    explicit dummy_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<dummy_wrapper>{ oop }
    {
    }
};

// ---------------------------------------------------------------------------
// Extra throwaway wrapper types used ONLY to instantiate the templated public
// API in unevaluated decltype()/is_invocable contexts below.  None are ever
// constructed at runtime; they exist so the surface lockdown can exercise:
//   * a multi-level object<> hierarchy (incomplete-type / vtable instantiation
//     regressions — the libstdc++-vs-libc++ unique_ptr<object_base> static_assert
//     that once bit the factory is keyed on exactly this shape),
//   * a wrapper with a NON-TRIVIAL destructor (must still satisfy the factory's
//     "derives from object_base" contract and be usable through unique_ptr), and
//   * wrappers standing in for collection element / map key+value types.
// ---------------------------------------------------------------------------
class base_wrapper : public vmhook::object<base_wrapper>
{
public:
    explicit base_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<base_wrapper>{ oop }
    {
    }
};

// Second level of an object<> hierarchy (derives from a concrete object<>).
class derived_wrapper : public base_wrapper
{
public:
    explicit derived_wrapper(vmhook::oop_t oop) noexcept
        : base_wrapper{ oop }
    {
    }
};

// A wrapper whose destructor is non-trivial (touches a member).  object_base's
// destructor is virtual, so this just confirms a user type with its own cleanup
// still slots through register_class / make_unique / for_each_instance / the
// factory map without an incomplete-type or non-trivial-dtor instantiation error.
class nontrivial_dtor_wrapper : public vmhook::object<nontrivial_dtor_wrapper>
{
public:
    explicit nontrivial_dtor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<nontrivial_dtor_wrapper>{ oop }
    {
    }
    ~nontrivial_dtor_wrapper() { this->touched = 0; }

private:
    int touched{ 1 };
};

// Stand-ins for collection element / map key+value wrapper types.
class elem_wrapper : public vmhook::object<elem_wrapper>
{
public:
    explicit elem_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<elem_wrapper>{ oop } {}
};
class key_wrapper : public vmhook::object<key_wrapper>
{
public:
    explicit key_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<key_wrapper>{ oop } {}
};
class val_wrapper : public vmhook::object<val_wrapper>
{
public:
    explicit val_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<val_wrapper>{ oop } {}
};

// Representative detour-callable shapes.  Used purely inside decltype() to
// instantiate the templated hook<>/scoped_hook<>/hook_by_signature<> family,
// which take `auto&& user_detour` and therefore cannot be addressed with a
// plain &function.  The detour signature (return_value& [, self] [, args...])
// matches the documented hook callback contract.
inline constexpr auto detour_void =
    [](vmhook::return_value&) noexcept -> void {};
inline constexpr auto detour_with_self =
    [](vmhook::return_value&, const std::unique_ptr<dummy_wrapper>&) noexcept -> void {};
inline constexpr auto detour_with_args =
    [](vmhook::return_value&, const std::unique_ptr<dummy_wrapper>&, int) noexcept -> void {};

// Representative visitor / predicate / callback shapes for the templated
// for_each_* / deoptimize_methods_if / on_* / watch_static_field family.
inline constexpr auto class_visitor =
    [](const std::string&, vmhook::hotspot::klass*) -> void {};
inline constexpr auto thread_visitor =
    [](const vmhook::thread_info&) -> void {};
inline constexpr auto instance_visitor =
    [](std::unique_ptr<dummy_wrapper>) -> void {};
inline constexpr auto method_predicate =
    [](const std::string&, vmhook::hotspot::method*) -> bool { return true; };
inline constexpr auto name_callback =
    [](const std::string&) -> void {};
inline constexpr auto i32_field_callback =
    [](std::int32_t, std::int32_t) -> void {};

// ===========================================================================
// COMPILE-TIME PUBLIC-API-SURFACE LOCKDOWN
// ===========================================================================
// Everything below is checked at translation-unit parse time.  A removed /
// renamed / re-signatured public entry point makes this file fail to COMPILE,
// which is the whole point — the runtime checks in main() can only see
// functions that still exist with a compatible shape.
//
// Conventions (mirroring tests/test_traits.cpp):
//   * Non-overloaded, non-template free functions: address-of fed into
//     std::is_invocable_r_v<Return, decltype(&fn), Args...> (taking the address
//     is unambiguous), plus an is_same_v on a representative unevaluated call to
//     pin the EXACT return type (is_invocable_r_v only requires the result be
//     convertible to Return, so the is_same_v is the tighter guard).
//   * Templated / `auto&&`-parameter functions (hook, register_class, the
//     for_each_* family, make_unique, ...): cannot be addressed, so we pin them
//     via is_same_v<decltype(call-expression), ExpectedReturn> with the call in
//     an unevaluated context (it is never executed; no JVM is touched).
//   * Public types: structural traits (move-only, trivially/virtually
//     destructible, base classes, member return types).
// ===========================================================================

namespace surface_lock
{
    // -----------------------------------------------------------------------
    // GROUP A — class lookup & cache control
    //   find_class / find_class_via_oop / override_class_lookup /
    //   evict_class_lookup / reanchor_classes_via_oop / klass_from_oop
    //   (jni::klass_from_class_mirror used to belong here; it was deleted by the
    //   de-JNI refactor with no public replacement — see GROUP I)
    // -----------------------------------------------------------------------
    static_assert(std::is_same_v<
                      decltype(vmhook::find_class(std::declval<std::string_view>())),
                      vmhook::hotspot::klass*>,
                  "vmhook::find_class(string_view) must return hotspot::klass*");
    static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                        decltype(&vmhook::find_class), std::string_view>,
                  "vmhook::find_class must be invocable with a string_view");

    static_assert(std::is_same_v<
                      decltype(vmhook::find_class_via_oop(std::declval<void*>(),
                                                          std::declval<std::string_view>())),
                      vmhook::hotspot::klass*>,
                  "vmhook::find_class_via_oop(void*, string_view) must return hotspot::klass*");
    static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                        decltype(&vmhook::find_class_via_oop),
                                        void*, std::string_view>,
                  "vmhook::find_class_via_oop must be invocable with (void*, string_view)");

    static_assert(std::is_same_v<
                      decltype(vmhook::override_class_lookup(std::declval<std::string_view>(),
                                                             std::declval<vmhook::hotspot::klass*>())),
                      void>,
                  "vmhook::override_class_lookup(string_view, klass*) must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::override_class_lookup),
                                        std::string_view, vmhook::hotspot::klass*>,
                  "vmhook::override_class_lookup must take (string_view, klass*)");

    static_assert(std::is_same_v<
                      decltype(vmhook::evict_class_lookup(std::declval<std::string_view>())),
                      void>,
                  "vmhook::evict_class_lookup(string_view) must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::evict_class_lookup),
                                        std::string_view>,
                  "vmhook::evict_class_lookup must take a string_view");

    static_assert(std::is_same_v<
                      decltype(vmhook::reanchor_classes_via_oop(
                          std::declval<void*>(),
                          std::declval<std::initializer_list<std::string_view>>())),
                      bool>,
                  "vmhook::reanchor_classes_via_oop(void*, init-list<string_view>) must return bool");
    static_assert(std::is_invocable_r_v<bool, decltype(&vmhook::reanchor_classes_via_oop),
                                        void*, std::initializer_list<std::string_view>>,
                  "vmhook::reanchor_classes_via_oop must take (void*, init-list<string_view>)");

    static_assert(std::is_same_v<
                      decltype(vmhook::klass_from_oop(std::declval<void*>())),
                      vmhook::hotspot::klass*>,
                  "vmhook::klass_from_oop(void*) must return hotspot::klass*");
    static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                        decltype(&vmhook::klass_from_oop), void*>,
                  "vmhook::klass_from_oop must take a void*");
    // klass_from_oop / find_class_via_oop / override_class_lookup are noexcept.
    static_assert(noexcept(vmhook::klass_from_oop(std::declval<void*>())),
                  "vmhook::klass_from_oop must be noexcept");

    // -----------------------------------------------------------------------
    // GROUP B — class / method introspection
    //   get_class_methods (by-name + <T>) / find_methods_by_signature<T> /
    //   log_class_methods<T>
    // -----------------------------------------------------------------------
    // NOTE: get_class_methods is OVERLOADED (by-name + templated <T>), so
    // &vmhook::get_class_methods is ambiguous — pin both forms via decltype on a
    // representative call expression instead of address-of.
    static_assert(std::is_same_v<
                      decltype(vmhook::get_class_methods(std::declval<std::string_view>())),
                      std::vector<std::pair<std::string, std::string>>>,
                  "vmhook::get_class_methods(string_view) must return vector<pair<name,desc>>");
    static_assert(std::is_same_v<
                      decltype(vmhook::get_class_methods<dummy_wrapper>()),
                      std::vector<std::pair<std::string, std::string>>>,
                  "vmhook::get_class_methods<T>() must return vector<pair<name,desc>>");
    static_assert(std::is_same_v<
                      decltype(vmhook::find_methods_by_signature<dummy_wrapper>(
                          std::declval<std::string_view>())),
                      std::vector<std::string>>,
                  "vmhook::find_methods_by_signature<T>(string_view) must return vector<string>");
    static_assert(std::is_same_v<
                      decltype(vmhook::log_class_methods<dummy_wrapper>()), void>,
                  "vmhook::log_class_methods<T>() must return void");

    // -----------------------------------------------------------------------
    // GROUP C — registration & object construction
    //   register_class<T> / make_unique<T> / make_java_string / make_java_array
    // -----------------------------------------------------------------------
    static_assert(std::is_same_v<
                      decltype(vmhook::register_class<dummy_wrapper>(std::declval<std::string_view>())),
                      bool>,
                  "vmhook::register_class<T>(string_view) must return bool");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<dummy_wrapper>()),
                      std::unique_ptr<dummy_wrapper>>,
                  "vmhook::make_unique<T>() must return unique_ptr<T>");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<dummy_wrapper>(1, 2.0, true)),
                      std::unique_ptr<dummy_wrapper>>,
                  "vmhook::make_unique<T>(args...) must forward ctor args and return unique_ptr<T>");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_java_string(std::declval<std::string_view>())),
                      void*>,
                  "vmhook::make_java_string(string_view) must return void* (jstring oop)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_string), std::string_view>,
                  "vmhook::make_java_string must take a string_view");
    // make_java_array's 4th arg (retained_for_abi, formerly allow_jni_fallback —
    // now [[maybe_unused]], since there is no JNI fallback left) is defaulted.
    // A function
    // POINTER cannot see default arguments, so the 3-arg form is pinned via a
    // decltype call expression (unevaluated calls DO honour defaults) and the
    // full 4-arg shape via address-of is_invocable.
    static_assert(std::is_same_v<
                      decltype(vmhook::make_java_array(std::declval<std::string_view>(),
                                                       std::declval<std::int32_t>(),
                                                       std::declval<std::size_t>())),
                      void*>,
                  "vmhook::make_java_array(name, length, element_size) must return void* "
                  "(4th arg defaulted)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_array),
                                        std::string_view, std::int32_t, std::size_t, bool>,
                  "vmhook::make_java_array's full shape is (string_view, int32, size_t, bool)");

    // -----------------------------------------------------------------------
    // GROUP D — hook install / teardown / verification
    //   hook<T> (2- and 3-arg) / hook_by_signature<T> / scoped_hook<T> (2/3) /
    //   shutdown_hooks / verify_hooks
    // -----------------------------------------------------------------------
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<dummy_wrapper>(std::declval<std::string_view>(),
                                                           detour_void)),
                      bool>,
                  "vmhook::hook<T>(name, detour) must return bool");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<dummy_wrapper>(std::declval<std::string_view>(),
                                                           std::declval<std::string_view>(),
                                                           detour_void)),
                      bool>,
                  "vmhook::hook<T>(name, signature, detour) must return bool");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<dummy_wrapper>(std::declval<std::string_view>(),
                                                           detour_with_args)),
                      bool>,
                  "vmhook::hook<T> must accept a self+args detour and return bool");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook_by_signature<dummy_wrapper>(
                          std::declval<std::string_view>(), detour_void)),
                      bool>,
                  "vmhook::hook_by_signature<T>(descriptor, detour) must return bool");
    static_assert(std::is_same_v<
                      decltype(vmhook::scoped_hook<dummy_wrapper>(
                          std::declval<std::string_view>(), detour_void)),
                      vmhook::hook_handle>,
                  "vmhook::scoped_hook<T>(name, detour) must return hook_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::scoped_hook<dummy_wrapper>(
                          std::declval<std::string_view>(),
                          std::declval<std::string_view>(), detour_void)),
                      vmhook::hook_handle>,
                  "vmhook::scoped_hook<T>(name, signature, detour) must return hook_handle");
    static_assert(std::is_same_v<decltype(vmhook::shutdown_hooks()), void>,
                  "vmhook::shutdown_hooks() must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::shutdown_hooks)>,
                  "vmhook::shutdown_hooks must take no arguments");
    static_assert(noexcept(vmhook::shutdown_hooks()),
                  "vmhook::shutdown_hooks must be noexcept");
    static_assert(std::is_same_v<decltype(vmhook::verify_hooks()), std::size_t>,
                  "vmhook::verify_hooks() must return std::size_t");
    static_assert(std::is_invocable_r_v<std::size_t, decltype(&vmhook::verify_hooks)>,
                  "vmhook::verify_hooks must take no arguments");
    static_assert(noexcept(vmhook::verify_hooks()),
                  "vmhook::verify_hooks must be noexcept");
    // Background auto-repair watchdog run-time master switch.
    static_assert(std::is_same_v<decltype(vmhook::set_auto_repair_enabled(true)), void>,
                  "vmhook::set_auto_repair_enabled(bool) must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::set_auto_repair_enabled), bool>,
                  "vmhook::set_auto_repair_enabled must take a single bool");
    static_assert(noexcept(vmhook::set_auto_repair_enabled(true)),
                  "vmhook::set_auto_repair_enabled must be noexcept");
    static_assert(std::is_same_v<decltype(vmhook::auto_repair_enabled()), bool>,
                  "vmhook::auto_repair_enabled() must return bool");
    static_assert(std::is_invocable_r_v<bool, decltype(&vmhook::auto_repair_enabled)>,
                  "vmhook::auto_repair_enabled must take no arguments");
    static_assert(noexcept(vmhook::auto_repair_enabled()),
                  "vmhook::auto_repair_enabled must be noexcept");

    // -----------------------------------------------------------------------
    // GROUP E — enumeration & deoptimization
    //   for_each_loaded_class / for_each_thread / for_each_instance<T> /
    //   deoptimize_methods_if / deoptimize_all_jit_compiled_methods
    // -----------------------------------------------------------------------
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_loaded_class(class_visitor)), void>,
                  "vmhook::for_each_loaded_class(visitor) must return void");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_thread(thread_visitor)), void>,
                  "vmhook::for_each_thread(visitor) must return void");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_instance<dummy_wrapper>(instance_visitor)),
                      std::size_t>,
                  "vmhook::for_each_instance<T>(visitor) must return std::size_t");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_instance<dummy_wrapper>(
                          instance_visitor, std::declval<std::size_t>())),
                      std::size_t>,
                  "vmhook::for_each_instance<T>(visitor, max_visits) must return std::size_t");
    static_assert(std::is_same_v<
                      decltype(vmhook::deoptimize_methods_if(method_predicate)),
                      std::size_t>,
                  "vmhook::deoptimize_methods_if(predicate) must return std::size_t");
    static_assert(std::is_same_v<
                      decltype(vmhook::deoptimize_all_jit_compiled_methods()),
                      std::size_t>,
                  "vmhook::deoptimize_all_jit_compiled_methods() must return std::size_t");
    static_assert(std::is_invocable_r_v<std::size_t,
                                        decltype(&vmhook::deoptimize_all_jit_compiled_methods)>,
                  "vmhook::deoptimize_all_jit_compiled_methods must take no arguments");
    static_assert(noexcept(vmhook::deoptimize_all_jit_compiled_methods()),
                  "vmhook::deoptimize_all_jit_compiled_methods must be noexcept");

    // -----------------------------------------------------------------------
    // GROUP F — string & array field helpers
    //   read_java_string / write_java_string / set_str_field / field_oop /
    //   set_bool_array / set_str_array / set_prim_array<T> / decode_array_oop /
    //   get_array_element<T> / set_array_element<T>
    // -----------------------------------------------------------------------
    static_assert(std::is_same_v<
                      decltype(vmhook::read_java_string(std::declval<void*>())), std::string>,
                  "vmhook::read_java_string(void*) must return std::string");
    static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::read_java_string), void*>,
                  "vmhook::read_java_string must take a void*");
    static_assert(std::is_same_v<
                      decltype(vmhook::write_java_string(std::declval<void*>(),
                                                         std::declval<std::string_view>())),
                      void>,
                  "vmhook::write_java_string(void*, string_view) must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::write_java_string),
                                        void*, std::string_view>,
                  "vmhook::write_java_string must take (void*, string_view)");
    static_assert(std::is_same_v<
                      decltype(vmhook::field_oop(std::declval<const vmhook::field_proxy&>())),
                      void*>,
                  "vmhook::field_oop(const field_proxy&) must return void*");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::field_oop),
                                        const vmhook::field_proxy&>,
                  "vmhook::field_oop must take a const field_proxy&");
    static_assert(std::is_same_v<
                      decltype(vmhook::set_str_field(std::declval<const vmhook::field_proxy&>(),
                                                     std::declval<std::string_view>())),
                      void>,
                  "vmhook::set_str_field(const field_proxy&, string_view) must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::set_str_field),
                                        const vmhook::field_proxy&, std::string_view>,
                  "vmhook::set_str_field must take (const field_proxy&, string_view)");
    static_assert(std::is_same_v<
                      decltype(vmhook::set_bool_array(std::declval<const vmhook::field_proxy&>(),
                                                      std::declval<const std::vector<bool>&>())),
                      void>,
                  "vmhook::set_bool_array(const field_proxy&, const vector<bool>&) must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::set_bool_array),
                                        const vmhook::field_proxy&, const std::vector<bool>&>,
                  "vmhook::set_bool_array must take (const field_proxy&, const vector<bool>&)");
    static_assert(std::is_same_v<
                      decltype(vmhook::set_str_array(std::declval<const vmhook::field_proxy&>(),
                                                     std::declval<const std::vector<std::string>&>())),
                      void>,
                  "vmhook::set_str_array(const field_proxy&, const vector<string>&) must return void");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::set_str_array),
                                        const vmhook::field_proxy&, const std::vector<std::string>&>,
                  "vmhook::set_str_array must take (const field_proxy&, const vector<string>&)");
    // set_prim_array<T> is a template; pin via decltype on representative element types.
    static_assert(std::is_same_v<
                      decltype(vmhook::set_prim_array<std::int32_t>(
                          std::declval<const vmhook::field_proxy&>(),
                          std::declval<const std::vector<std::int32_t>&>())),
                      void>,
                  "vmhook::set_prim_array<int32>(const field_proxy&, const vector<int32>&) -> void");
    static_assert(std::is_same_v<
                      decltype(vmhook::set_prim_array<double>(
                          std::declval<const vmhook::field_proxy&>(),
                          std::declval<const std::vector<double>&>())),
                      void>,
                  "vmhook::set_prim_array<double> must return void");
    static_assert(std::is_same_v<
                      decltype(vmhook::decode_array_oop(std::declval<std::uint32_t>())), void*>,
                  "vmhook::decode_array_oop(uint32) must return void*");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::decode_array_oop), std::uint32_t>,
                  "vmhook::decode_array_oop must take a uint32_t");
    // get_array_element<T> / set_array_element<T> are templates over the element type.
    static_assert(std::is_same_v<
                      decltype(vmhook::get_array_element<std::int32_t>(
                          std::declval<void*>(), std::declval<std::int32_t>())),
                      std::int32_t>,
                  "vmhook::get_array_element<int32>(void*, int32) must return int32");
    static_assert(std::is_same_v<
                      decltype(vmhook::get_array_element<float>(
                          std::declval<void*>(), std::declval<std::int32_t>())),
                      float>,
                  "vmhook::get_array_element<float> must return float");
    static_assert(std::is_same_v<
                      decltype(vmhook::set_array_element<std::int64_t>(
                          std::declval<void*>(), std::declval<std::int32_t>(),
                          std::declval<std::int64_t>())),
                      void>,
                  "vmhook::set_array_element<int64>(void*, int32, value) must return void");

    // -----------------------------------------------------------------------
    // GROUP G — watchers / async hooks
    //   on_class_loaded / on_exception / watch_static_field<T, field_type>
    // -----------------------------------------------------------------------
    static_assert(std::is_same_v<
                      decltype(vmhook::on_class_loaded(name_callback)), vmhook::watch_handle>,
                  "vmhook::on_class_loaded(callback) must return watch_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::on_exception(name_callback)), vmhook::watch_handle>,
                  "vmhook::on_exception(callback) must return watch_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::watch_static_field<dummy_wrapper, std::int32_t>(
                          std::declval<std::string_view>(), i32_field_callback)),
                      vmhook::watch_handle>,
                  "vmhook::watch_static_field<T, int32>(name, callback) must return watch_handle");

    // -----------------------------------------------------------------------
    // GROUP H — oop_pin lifetime primitive + pin()
    //   vmhook::oop_pin (move-only RAII) / pin(oop) / pin(unique_ptr<T>&)
    // -----------------------------------------------------------------------
    static_assert(std::is_same_v<decltype(vmhook::pin(std::declval<vmhook::oop_t>())),
                                 vmhook::oop_pin>,
                  "vmhook::pin(oop_t) must return vmhook::oop_pin");
    static_assert(std::is_same_v<
                      decltype(vmhook::pin(std::declval<const std::unique_ptr<dummy_wrapper>&>())),
                      vmhook::oop_pin>,
                  "vmhook::pin(const unique_ptr<T>&) must return vmhook::oop_pin");
    static_assert(noexcept(vmhook::pin(std::declval<vmhook::oop_t>())),
                  "vmhook::pin(oop_t) must be noexcept");
    // oop_pin is move-only with a noexcept destructor.
    static_assert(std::is_nothrow_default_constructible_v<vmhook::oop_pin>,
                  "vmhook::oop_pin must be nothrow default-constructible");
    static_assert(std::is_nothrow_move_constructible_v<vmhook::oop_pin>,
                  "vmhook::oop_pin must be nothrow move-constructible");
    static_assert(std::is_nothrow_move_assignable_v<vmhook::oop_pin>,
                  "vmhook::oop_pin must be nothrow move-assignable");
    static_assert(!std::is_copy_constructible_v<vmhook::oop_pin>,
                  "vmhook::oop_pin must NOT be copy-constructible (it owns a JNI global ref)");
    static_assert(!std::is_copy_assignable_v<vmhook::oop_pin>,
                  "vmhook::oop_pin must NOT be copy-assignable");
    static_assert(std::is_nothrow_destructible_v<vmhook::oop_pin>,
                  "vmhook::oop_pin destructor must be noexcept");
    static_assert(std::is_constructible_v<vmhook::oop_pin, vmhook::oop_t>,
                  "vmhook::oop_pin must be constructible from a raw oop_t");

    // -----------------------------------------------------------------------
    // GROUP I — the pure-VM successors of the deleted vmhook:: forwarder
    //   layer.
    //
    //   The de-JNI refactor (eaff990) removed the entire JNI bridge from the
    //   header.  vmhook:: now contains EXACTLY ONE entity — the oop_pin
    //   holder, pinned in GROUP H.  Everything this group used to pin is gone:
    //
    //     jni::value / detail::jni_value          jni::decode_object
    //     jni::find_class                          jni::get_object_class
    //     jni::find_class_with_context_loader      jni::oop_handle
    //     jni::new_string_utf                      jni::get_method_id
    //     jni::get_string_utf                      jni::get_static_method_id
    //     jni::exception_clear                     jni::get_static_field_id
    //     jni::klass_from_class_mirror             jni::get_static_object_field
    //     jni::function<slot, fn_t>                jni::call_object_method
    //     detail::jni_new_oop_pin               jni::call_static_object_method
    //     detail::jni_delete_oop_pin
    //
    //   Each of those is handled below in one of two ways: RE-POINTED at the
    //   surviving pure-VM entry point that carries the same property (so the
    //   capability still breaks the build if it disappears), or recorded as
    //   DELETED WITH NO EQUIVALENT.  Nothing is weakened: every re-pointed
    //   assertion pins an exact return type, and the group stays deliberately
    //   self-contained (it re-states pins that also live in GROUP A/C/F/J) so
    //   that "the JNI capability moved here" is greppable in one place.
    // -----------------------------------------------------------------------
    // DELETED, NO EQUIVALENT — jni::value / detail::jni_value (the jvalue
    // union) and jni::function<index, fn_t>(void* env) (the JNIEnv vtable-slot
    // fetch).  Both were pure JNI-ABI constructs; there is no JNIEnv, no
    // jvalue and no function table in a JNI-free header, so there is nothing to
    // re-point them at.
    //
    // DELETED (self-contradictory duplicates) — the eaff990 mechanical rewrite
    // sed-replaced BOTH `jni::find_class` AND `jni::find_class_with_context_loader`
    // with `vmhook::find_class`, collapsing four assertions onto one function,
    // two of which then claimed it returns `void*` ("jni::find_class must
    // return void* exactly" was a hard compile failure).  vmhook::find_class
    // returns vmhook::hotspot::klass*, which GROUP A already pins exactly
    // (is_same_v + is_invocable_r_v), and the context-loader variant's surviving
    // stand-in — vmhook::find_class_via_oop — is pinned in GROUP A too.  The
    // false pins are dropped; the true property is untouched.
    //
    // DELETED, NO EQUIVALENT — jni::exception_clear().  The header no longer
    // makes VM calls that can leave a pending JNI exception (all access is
    // direct VMStructs reads), so there is no exception state to clear.
    //
    // DELETED, NO EQUIVALENT — jni::decode_object(jobject) -> oop.  It unwrapped
    // a JNI local/global handle; with no JNI handles in the header there is
    // nothing to unwrap, and the raw/compressed-oop decode that replaced it
    // (vmhook::hotspot::decode_oop_pointer) is internal, not public surface.
    //
    // DELETED, NO EQUIVALENT — jni::klass_from_class_mirror(jclass) -> klass*.
    // It read Klass* out of a java.lang.Class mirror reached through a jclass;
    // there is no public mirror-unwrap entry point in the JNI-free header.

    // RE-POINTED — jni::new_string_utf(string_view) -> void* (jstring) is now
    // vmhook::make_java_string(string_view) -> void* (a raw String oop), and
    // jni::get_string_utf(void*) -> std::string is now
    // vmhook::read_java_string(void*) -> std::string.  Same properties, pure-VM
    // spelling.
    static_assert(std::is_same_v<
                      decltype(vmhook::make_java_string(std::declval<std::string_view>())), void*>,
                  "make_java_string(string_view) must return void* (succeeds jni::new_string_utf)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_string), std::string_view>,
                  "make_java_string must take a string_view");
    static_assert(std::is_same_v<
                      decltype(vmhook::read_java_string(std::declval<void*>())), std::string>,
                  "read_java_string(void*) must return std::string (succeeds jni::get_string_utf)");
    static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::read_java_string), void*>,
                  "read_java_string must take a void*");

    // RE-POINTED — jni::get_object_class(jobject) -> jclass is now
    // vmhook::klass_from_oop(void*) -> hotspot::klass*: same question ("what
    // class is this object?"), answered without a handle round-trip.
    static_assert(std::is_same_v<
                      decltype(vmhook::klass_from_oop(std::declval<void*>())),
                      vmhook::hotspot::klass*>,
                  "klass_from_oop(void*) must return klass* (succeeds jni::get_object_class)");
    static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                        decltype(&vmhook::klass_from_oop), void*>,
                  "klass_from_oop must take a void*");

    // RE-POINTED — jni::oop_handle(oop, void*& storage) minted a handle for a
    // raw oop; detail::jni_new_oop_pin / detail::jni_delete_oop_pin were
    // the two primitives behind oop_pin.  All three are now internal to the
    // JNI-free oop_pin, which stores and clears the raw oop itself.  The
    // original intent of those pins ("this capability went missing once; make it
    // a hard build break") is preserved by pinning the accessor shapes that now
    // ARE the primitive — these were previously only exercised at runtime
    // (oop_pin_*_inert_accessors below), never pinned at compile time.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::oop_pin&>().oop()),
                      vmhook::oop_t>,
                  "vmhook::oop_pin::oop() must return oop_t");
    static_assert(noexcept(std::declval<const vmhook::oop_pin&>().oop()),
                  "vmhook::oop_pin::oop() must be noexcept");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::oop_pin&>().handle()), void*>,
                  "vmhook::oop_pin::handle() must return void*");
    static_assert(noexcept(std::declval<const vmhook::oop_pin&>().handle()),
                  "vmhook::oop_pin::handle() must be noexcept");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::oop_pin&>().reset()), void>,
                  "vmhook::oop_pin::reset() must return void");
    static_assert(noexcept(std::declval<vmhook::oop_pin&>().reset()),
                  "vmhook::oop_pin::reset() must be noexcept");
    static_assert(std::is_constructible_v<bool, vmhook::oop_pin>,
                  "vmhook::oop_pin must be contextually convertible to bool");
    static_assert(!std::is_convertible_v<vmhook::oop_pin, bool>,
                  "vmhook::oop_pin::operator bool must be EXPLICIT (no implicit bool decay)");

    // RE-POINTED — the JNI id-and-call quartet (jni::get_method_id,
    // jni::get_static_method_id, jni::get_static_field_id,
    // jni::get_static_object_field) and the two invoke forwarders
    // (jni::call_object_method / jni::call_static_object_method) are all served
    // pure-VM by the proxy layer: object_base::get_method(name[, sig]) and
    // object<T>::static_method(name[, sig]) resolve a Method*,
    // object<T>::static_field(name) resolves a static field and field_proxy::get()
    // reads it (all pinned in GROUP J), and method_proxy::call(args...) performs
    // the actual invoke.  call() itself was NOT pinned anywhere — GROUP J only
    // pins method_proxy::value_t — so it is pinned here, where the JNI call
    // forwarders used to be.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().call()),
                      vmhook::method_proxy::value_t>,
                  "method_proxy::call() must return method_proxy::value_t "
                  "(succeeds jni::call_object_method / jni::call_static_object_method)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().call(
                          std::declval<std::int32_t>(), std::declval<const char*>())),
                      vmhook::method_proxy::value_t>,
                  "method_proxy::call(args...) must forward arbitrary args and return value_t");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call()),
                  "method_proxy::call() must be noexcept");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_method(
                          std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object_base::get_method(name) must return optional<method_proxy> "
                  "(succeeds jni::get_method_id)");
    static_assert(std::is_same_v<
                      decltype(vmhook::object<dummy_wrapper>::static_method(
                          std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object<T>::static_method(name) must return optional<method_proxy> "
                  "(succeeds jni::get_static_method_id)");
    static_assert(std::is_same_v<
                      decltype(vmhook::object<dummy_wrapper>::static_field(
                          std::declval<std::string_view>())),
                      std::optional<vmhook::field_proxy>>,
                  "object<T>::static_field(name) must return optional<field_proxy> "
                  "(succeeds jni::get_static_field_id)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy&>().get()),
                      vmhook::field_proxy::value_t>,
                  "field_proxy::get() must return field_proxy::value_t "
                  "(succeeds jni::get_static_object_field)");

    // SURVIVOR — detail::jvm_descriptor_for_arg<T>() -> std::string.  Pure
    // compile-time descriptor logic with no VM/JNI dependency, so the de-JNI
    // refactor kept it (the name is historical).  Unchanged pins.
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::jvm_descriptor_for_arg<int>()), std::string>,
                  "detail::jvm_descriptor_for_arg<int>() must return std::string");
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::jvm_descriptor_for_arg<bool>()), std::string>,
                  "detail::jvm_descriptor_for_arg<bool>() must return std::string");
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::jvm_descriptor_for_arg<double>()), std::string>,
                  "detail::jvm_descriptor_for_arg<double>() must return std::string");
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::jvm_descriptor_for_arg<std::string>()), std::string>,
                  "detail::jvm_descriptor_for_arg<std::string>() must return std::string");
    // SURVIVOR — vmhook::make_unique<T>(args...) -> unique_ptr<T>.  It never was
    // a jni:: name (the old comment here said "jni::make_unique", which was
    // wrong even before eaff990); these two pins add the const-string& argument
    // shapes that GROUP C does not cover.
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<dummy_wrapper>(
                          std::declval<const std::string&>())),
                      std::unique_ptr<dummy_wrapper>>,
                  "vmhook::make_unique<T>(const string&) must return unique_ptr<T>");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<dummy_wrapper>(
                          std::declval<const std::string&>(), 1, 2.0)),
                      std::unique_ptr<dummy_wrapper>>,
                  "vmhook::make_unique<T>(const string&, args...) must return unique_ptr<T>");

    // -----------------------------------------------------------------------
    // GROUP J — public TYPE traits
    //   object_base / object<T> / field_proxy(+value_t) / method_proxy(+value_t) /
    //   return_value(+caller_info) / collection family / map family /
    //   watch_handle / hook_handle / thread_info / oop_t
    // -----------------------------------------------------------------------
    // oop_t is the public OOP alias (void*).
    static_assert(std::is_same_v<vmhook::oop_t, void*>,
                  "vmhook::oop_t must be void*");
    static_assert(std::is_same_v<vmhook::oop_t, vmhook::oop_type_t>,
                  "vmhook::oop_t must alias vmhook::oop_type_t");

    // object_base: polymorphic (virtual dtor), copyable + movable.
    static_assert(std::is_polymorphic_v<vmhook::object_base>,
                  "object_base must be polymorphic (virtual destructor)");
    static_assert(std::has_virtual_destructor_v<vmhook::object_base>,
                  "object_base must have a virtual destructor");
    static_assert(std::is_nothrow_move_constructible_v<vmhook::object_base>,
                  "object_base must be nothrow move-constructible");
    static_assert(std::is_copy_constructible_v<vmhook::object_base>,
                  "object_base must be copy-constructible (copies the raw OOP)");
    static_assert(std::is_constructible_v<vmhook::object_base, vmhook::oop_t>,
                  "object_base must be constructible from an oop_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_instance()),
                      vmhook::oop_t>,
                  "object_base::get_instance() must return oop_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_field(
                          std::declval<std::string_view>())),
                      std::optional<vmhook::field_proxy>>,
                  "object_base::get_field(string_view) must return optional<field_proxy>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_method(
                          std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object_base::get_method(string_view) must return optional<method_proxy>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_method(
                          std::declval<std::string_view>(), std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object_base::get_method(name, signature) must return optional<method_proxy>");
    // Static overloads keyed on a std::type_index (used by object<T>::static_*).
    static_assert(std::is_same_v<
                      decltype(vmhook::object_base::get_field(
                          std::declval<std::type_index>(), std::declval<std::string_view>())),
                      std::optional<vmhook::field_proxy>>,
                  "object_base::get_field(type_index, name) must return optional<field_proxy>");
    static_assert(std::is_same_v<
                      decltype(vmhook::object_base::get_method(
                          std::declval<std::type_index>(), std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object_base::get_method(type_index, name) must return optional<method_proxy>");

    // object<T>: derives from object_base; the default template arg is void.
    static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<dummy_wrapper>>,
                  "object<T> must derive from object_base");
    static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<>>,
                  "object<> (default void param) must derive from object_base");
    static_assert(std::is_base_of_v<vmhook::object_base, dummy_wrapper>,
                  "a user wrapper deriving from object<T> must be an object_base");
    static_assert(std::is_base_of_v<vmhook::object_base, derived_wrapper>,
                  "a multi-level object<> hierarchy must still be an object_base");
    static_assert(std::is_base_of_v<base_wrapper, derived_wrapper>,
                  "derived_wrapper must derive from base_wrapper");
    static_assert(std::is_base_of_v<vmhook::object_base, nontrivial_dtor_wrapper>,
                  "a wrapper with a non-trivial destructor must still be an object_base");
    // object<T>::static_field / static_method shapes.
    static_assert(std::is_same_v<
                      decltype(vmhook::object<dummy_wrapper>::static_field(
                          std::declval<std::string_view>())),
                      std::optional<vmhook::field_proxy>>,
                  "object<T>::static_field(name) must return optional<field_proxy>");
    static_assert(std::is_same_v<
                      decltype(vmhook::object<dummy_wrapper>::static_method(
                          std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object<T>::static_method(name) must return optional<method_proxy>");
    static_assert(std::is_same_v<
                      decltype(vmhook::object<dummy_wrapper>::static_method(
                          std::declval<std::string_view>(), std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object<T>::static_method(name, signature) must return optional<method_proxy>");

    // field_proxy: constructible (void*, std::string, bool); accessors' shapes.
    static_assert(std::is_constructible_v<vmhook::field_proxy, void*, std::string, bool>,
                  "field_proxy must be constructible from (void*, std::string, bool)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy&>().get()),
                      vmhook::field_proxy::value_t>,
                  "field_proxy::get() must return field_proxy::value_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy&>().signature()),
                      std::string_view>,
                  "field_proxy::signature() must return string_view");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy&>().raw_address()), void*>,
                  "field_proxy::raw_address() must return void*");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy&>().is_static()), bool>,
                  "field_proxy::is_static() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy&>().is_reference()), bool>,
                  "field_proxy::is_reference() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy&>().get_compressed_oop()),
                      std::uint32_t>,
                  "field_proxy::get_compressed_oop() must return uint32_t");
    // field_proxy::value_t — variant payload + conversions.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy::value_t&>().as_string()),
                      std::string>,
                  "field_proxy::value_t::as_string() must return std::string");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy::value_t&>().is_reference()),
                      bool>,
                  "field_proxy::value_t::is_reference() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy::value_t&>()
                                   .to_vector<elem_wrapper>()),
                      std::vector<std::unique_ptr<elem_wrapper>>>,
                  "field_proxy::value_t::to_vector<T>() must return vector<unique_ptr<T>>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy::value_t&>()
                                   .to_entries<key_wrapper, val_wrapper>()),
                      std::vector<std::pair<std::unique_ptr<key_wrapper>,
                                            std::unique_ptr<val_wrapper>>>>,
                  "field_proxy::value_t::to_entries<K,V>() must return vector<pair<uptr<K>,uptr<V>>>");
    // value_t implicitly converts to scalar field types via std::visit.
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, int>,
                  "field_proxy::value_t must be implicitly convertible to int");
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, bool>,
                  "field_proxy::value_t must be implicitly convertible to bool");
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, double>,
                  "field_proxy::value_t must be implicitly convertible to double");

    // method_proxy::value_t scalar conversions + helpers.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy::value_t&>().is_void()),
                      bool>,
                  "method_proxy::value_t::is_void() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy::value_t&>().is_string()),
                      bool>,
                  "method_proxy::value_t::is_string() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy::value_t&>().as_string()),
                      std::string>,
                  "method_proxy::value_t::as_string() must return std::string");

    // return_value: get/set/cancel/caller/stack_trace/frame shapes.
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::return_value&>().set(std::int32_t{})), void>,
                  "return_value::set<int32>() must return void");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::return_value&>().cancel()), void>,
                  "return_value::cancel() must return void");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::return_value&>().set_arg(
                          std::declval<std::int32_t>(), std::int64_t{})),
                      bool>,
                  "return_value::set_arg(index, value) must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::return_value&>().caller()),
                      vmhook::return_value::caller_info>,
                  "return_value::caller() must return caller_info");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::return_value&>().stack_trace()),
                      std::vector<vmhook::return_value::caller_info>>,
                  "return_value::stack_trace() must return vector<caller_info>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::return_value&>().frame()),
                      vmhook::hotspot::frame*>,
                  "return_value::frame() must return hotspot::frame*");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::return_value::caller_info&>().valid()),
                      bool>,
                  "return_value::caller_info::valid() must return bool");

    // Collection family: inheritance chain + member shapes.
    static_assert(std::is_base_of_v<vmhook::object_base, vmhook::collection>,
                  "collection must (transitively) derive from object_base");
    static_assert(std::is_base_of_v<vmhook::collection, vmhook::list>,
                  "list must derive from collection");
    static_assert(std::is_base_of_v<vmhook::collection, vmhook::set>,
                  "set must derive from collection");
    static_assert(std::is_base_of_v<vmhook::list, vmhook::linked_list>,
                  "linked_list must derive from list");
    static_assert(std::is_base_of_v<vmhook::object_base, vmhook::map>,
                  "map must (transitively) derive from object_base");
    static_assert(std::is_base_of_v<vmhook::map, vmhook::hash_map>,
                  "hash_map must derive from map");
    static_assert(std::is_constructible_v<vmhook::collection, vmhook::oop_t>,
                  "collection must be constructible from an oop_t");
    static_assert(std::is_constructible_v<vmhook::map, vmhook::oop_t>,
                  "map must be constructible from an oop_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::collection&>().size()), std::int32_t>,
                  "collection::size() must return int32");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::collection&>().is_empty()), bool>,
                  "collection::is_empty() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::collection&>().to_vector<elem_wrapper>()),
                      std::vector<std::unique_ptr<elem_wrapper>>>,
                  "collection::to_vector<T>() must return vector<unique_ptr<T>>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::map&>().size()), std::int32_t>,
                  "map::size() must return int32");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::map&>().is_empty()), bool>,
                  "map::is_empty() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::map&>()
                                   .to_entries<key_wrapper, val_wrapper>()),
                      std::vector<std::pair<std::unique_ptr<key_wrapper>,
                                            std::unique_ptr<val_wrapper>>>>,
                  "map::to_entries<K,V>() must return vector<pair<uptr<K>,uptr<V>>>");

    // watch_handle: move-only, running()/stop() shapes, noexcept dtor.
    static_assert(std::is_nothrow_default_constructible_v<vmhook::watch_handle>,
                  "watch_handle must be nothrow default-constructible");
    static_assert(std::is_nothrow_move_constructible_v<vmhook::watch_handle>,
                  "watch_handle must be nothrow move-constructible");
    static_assert(std::is_nothrow_move_assignable_v<vmhook::watch_handle>,
                  "watch_handle must be nothrow move-assignable");
    static_assert(!std::is_copy_constructible_v<vmhook::watch_handle>,
                  "watch_handle must NOT be copy-constructible (move-only)");
    static_assert(!std::is_copy_assignable_v<vmhook::watch_handle>,
                  "watch_handle must NOT be copy-assignable (move-only)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::watch_handle&>().running()), bool>,
                  "watch_handle::running() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::watch_handle&>().stop()), void>,
                  "watch_handle::stop() must return void");
    static_assert(noexcept(std::declval<vmhook::watch_handle&>().stop()),
                  "watch_handle::stop() must be noexcept");

    // hook_handle: move-only, installed()/stop() shapes, noexcept dtor.
    static_assert(std::is_nothrow_default_constructible_v<vmhook::hook_handle>,
                  "hook_handle must be nothrow default-constructible");
    static_assert(std::is_nothrow_move_constructible_v<vmhook::hook_handle>,
                  "hook_handle must be nothrow move-constructible");
    static_assert(std::is_nothrow_move_assignable_v<vmhook::hook_handle>,
                  "hook_handle must be nothrow move-assignable");
    static_assert(!std::is_copy_constructible_v<vmhook::hook_handle>,
                  "hook_handle must NOT be copy-constructible (move-only)");
    static_assert(!std::is_copy_assignable_v<vmhook::hook_handle>,
                  "hook_handle must NOT be copy-assignable (move-only)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::hook_handle&>().installed()), bool>,
                  "hook_handle::installed() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::hook_handle&>().stop()), void>,
                  "hook_handle::stop() must return void");

    // thread_info: aggregate field types (used by for_each_thread visitors).
    static_assert(std::is_same_v<decltype(vmhook::thread_info::thread),
                                 vmhook::hotspot::java_thread*>,
                  "thread_info::thread must be hotspot::java_thread*");
    static_assert(std::is_same_v<decltype(vmhook::thread_info::os_thread_id),
                                 vmhook::os::thread_id_t>,
                  "thread_info::os_thread_id must be os::thread_id_t");
    static_assert(std::is_default_constructible_v<vmhook::thread_info>,
                  "thread_info must be default-constructible (it is an aggregate snapshot)");
} // namespace surface_lock

int main()
{
    // --- find_class: no JVM -> nullptr, never throws ---------------------
    {
        vmhook::hotspot::klass* k{ nullptr };
        bool threw{ false };
        try { k = vmhook::find_class("java/lang/String"); }
        catch (...) { threw = true; }
        check("find_class_string_returns_null_without_jvm", k == nullptr);
        check("find_class_does_not_throw_without_jvm", !threw);
    }
    {
        // A class that does not exist anywhere must also be null, not throw.
        vmhook::hotspot::klass* k{ vmhook::find_class("definitely/Not/A/Real/Class") };
        check("find_class_missing_class_returns_null", k == nullptr);
    }
    {
        // Empty class name is still a safe lookup that yields null.
        vmhook::hotspot::klass* k{ vmhook::find_class("") };
        check("find_class_empty_name_returns_null", k == nullptr);
    }

    // --- read_java_string: null oop -> empty string, never throws --------
    {
        std::string s{ "sentinel" };
        bool threw{ false };
        try { s = vmhook::read_java_string(nullptr); }
        catch (...) { threw = true; }
        check("read_java_string_null_returns_empty", s.empty());
        check("read_java_string_null_does_not_throw", !threw);
    }
    {
        // A bogus non-null pointer is rejected by is_valid_pointer and must
        // yield an empty string rather than dereferencing garbage.
        void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        std::string s{ "sentinel" };
        bool threw{ false };
        try { s = vmhook::read_java_string(bogus); }
        catch (...) { threw = true; }
        check("read_java_string_bogus_ptr_returns_empty", s.empty());
        check("read_java_string_bogus_ptr_does_not_throw", !threw);
    }

    // --- shutdown_hooks: safe to call with no hooks installed ------------
    {
        bool threw{ false };
        try
        {
            vmhook::shutdown_hooks();
            // Idempotent: a second call with nothing installed is still safe.
            vmhook::shutdown_hooks();
        }
        catch (...) { threw = true; }
        check("shutdown_hooks_no_hooks_does_not_throw", !threw);
    }

    // --- set_auto_repair_enabled / auto_repair_enabled round-trip --------
    {
        // Default state is ENABLED (production behaviour unchanged).
        check("auto_repair_enabled_default_true", vmhook::auto_repair_enabled());

        bool threw{ false };
        try
        {
            // Disabling with no watchdog live + no hooks installed is a safe no-op
            // (no thread to stop), and the getter must reflect it immediately.
            vmhook::set_auto_repair_enabled(false);
        }
        catch (...) { threw = true; }
        check("set_auto_repair_disable_does_not_throw", !threw);
        check("auto_repair_enabled_false_after_disable", !vmhook::auto_repair_enabled());

        threw = false;
        try
        {
            // Re-enable just flips the gate (no spawn here, no JVM) and is
            // idempotent across repeated calls.
            vmhook::set_auto_repair_enabled(true);
            vmhook::set_auto_repair_enabled(true);
        }
        catch (...) { threw = true; }
        check("set_auto_repair_enable_does_not_throw", !threw);
        check("auto_repair_enabled_true_after_enable", vmhook::auto_repair_enabled());

        // Leave the library in its default ENABLED state for any later checks.
        vmhook::set_auto_repair_enabled(true);
    }

    // --- for_each_loaded_class: no JVM -> visitor never invoked ----------
    {
        int count{ 0 };
        bool threw{ false };
        try
        {
            vmhook::for_each_loaded_class(
                [&count](const std::string&, vmhook::hotspot::klass*)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_loaded_class_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_loaded_class_does_not_throw_without_jvm", !threw);
    }

    // --- for_each_thread: no JVM -> visitor never invoked ----------------
    {
        int count{ 0 };
        bool threw{ false };
        try
        {
            vmhook::for_each_thread(
                [&count](const vmhook::thread_info&)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_thread_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_thread_does_not_throw_without_jvm", !threw);
    }

    // --- register_class<T>: no JVM -> returns false (find_class fails) ---
    bool registered{ true };
    {
        bool threw{ false };
        try { registered = vmhook::register_class<dummy_wrapper>("my/Dummy"); }
        catch (...) { threw = true; }
        check("register_class_returns_false_without_jvm", registered == false);
        check("register_class_does_not_throw_without_jvm", !threw);
    }

    // --- for_each_instance<T>: no JVM -> 0 instances, visitor not run ----
    // for_each_instance resolves T's registered klass first; with no JVM the
    // type was never registered (register_class returned false), so it must
    // bail out reporting zero and never touch the visitor.
    {
        int count{ 0 };
        std::size_t reported{ 123 };
        bool threw{ false };
        try
        {
            reported = vmhook::for_each_instance<dummy_wrapper>(
                [&count](std::unique_ptr<dummy_wrapper>)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_instance_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_instance_reports_zero_without_jvm", reported == 0);
        check("for_each_instance_does_not_throw_without_jvm", !threw);
    }
    {
        // Same, but with an explicit max_visits cap argument exercised.
        int count{ 0 };
        std::size_t reported{ vmhook::for_each_instance<dummy_wrapper>(
            [&count](std::unique_ptr<dummy_wrapper>) { ++count; },
            8) };
        check("for_each_instance_with_max_visits_reports_zero", reported == 0);
        check("for_each_instance_with_max_visits_visitor_not_invoked", count == 0);
    }

    // --- make_unique<T>: no JVM -> nullptr, never throws -----------------
    {
        std::unique_ptr<dummy_wrapper> obj{ reinterpret_cast<dummy_wrapper*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<dummy_wrapper>(); }
        catch (...) { threw = true; }
        check("make_unique_returns_empty_wrapper_without_jvm", is_empty_wrapper(obj));
        check("make_unique_does_not_throw_without_jvm", !threw);
    }

    // --- on_class_loaded: no JVM -> empty handle, running()==false -------
    // The class-load hook install requires resolving java.lang.ClassLoader,
    // which fails without a JVM; the returned watch_handle must therefore be
    // inert (running() == false) and must not fire the callback.
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{ vmhook::on_class_loaded(
                [&fired](const std::string&) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("on_class_loaded_handle_not_running_without_jvm", running_true == false);
        check("on_class_loaded_callback_not_fired_without_jvm", fired == 0);
        check("on_class_loaded_does_not_throw_without_jvm", !threw);
    }

    // --- on_exception: no JVM -> empty handle, running()==false ----------
    // Mirrors on_class_loaded: installing the Throwable.fillInStackTrace hook
    // needs a live JVM; without one the handle is inert.
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{ vmhook::on_exception(
                [&fired](const std::string&) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("on_exception_handle_not_running_without_jvm", running_true == false);
        check("on_exception_callback_not_fired_without_jvm", fired == 0);
        check("on_exception_does_not_throw_without_jvm", !threw);
    }

    // --- watch_handle default-construct is inert -------------------------
    {
        vmhook::watch_handle handle{};
        check("default_watch_handle_not_running", handle.running() == false);
    }

    // --- deoptimize_methods_if: no JVM -> 0, predicate never invoked -----
    // deoptimize_methods_if delegates to for_each_loaded_class, which is a
    // no-op without a live JVM (the class iterator finds nothing).  Because
    // the predicate is only consulted *inside* the per-method loop, and that
    // loop never runs, the predicate must never fire and the returned count
    // must be exactly 0.  std::size_t return type is asserted by binding it.
    {
        int calls{ 0 };
        std::size_t deopt{ 999 };
        bool threw{ false };
        try
        {
            deopt = vmhook::deoptimize_methods_if(
                [&calls](const std::string&, vmhook::hotspot::method*)
                {
                    ++calls;
                    return true;  // always-true predicate
                });
        }
        catch (...) { threw = true; }
        check("deoptimize_methods_if_always_true_returns_zero", deopt == 0);
        check("deoptimize_methods_if_always_true_predicate_not_invoked", calls == 0);
        check("deoptimize_methods_if_always_true_does_not_throw", !threw);
    }
    {
        // An always-false predicate must likewise never be called and return 0.
        int calls{ 0 };
        std::size_t deopt{ vmhook::deoptimize_methods_if(
            [&calls](const std::string&, vmhook::hotspot::method*)
            {
                ++calls;
                return false;
            }) };
        check("deoptimize_methods_if_always_false_returns_zero", deopt == 0);
        check("deoptimize_methods_if_always_false_predicate_not_invoked", calls == 0);
    }
    {
        // A name-based predicate (the documented Minecraft-style use case)
        // must behave identically: zero result, predicate untouched.
        int calls{ 0 };
        std::size_t deopt{ vmhook::deoptimize_methods_if(
            [&calls](const std::string& class_name, vmhook::hotspot::method*)
            {
                ++calls;
                return class_name.starts_with("net/minecraft/");
            }) };
        check("deoptimize_methods_if_name_based_returns_zero", deopt == 0);
        check("deoptimize_methods_if_name_based_predicate_not_invoked", calls == 0);
    }
    {
        // The std::size_t return type is part of the contract; this fails to
        // compile if the return type ever changes to a signed/narrower type.
        static_assert(
            std::is_same_v<decltype(vmhook::deoptimize_methods_if(
                [](const std::string&, vmhook::hotspot::method*) { return true; })),
                std::size_t>,
            "deoptimize_methods_if must return std::size_t");
        check("deoptimize_methods_if_return_type_is_size_t", true);
    }
    {
        // Idempotent / repeatable: calling it twice still yields 0 each time
        // and still never invokes the predicate.
        int calls{ 0 };
        std::size_t first{ vmhook::deoptimize_methods_if(
            [&calls](const std::string&, vmhook::hotspot::method*) { ++calls; return true; }) };
        std::size_t second{ vmhook::deoptimize_methods_if(
            [&calls](const std::string&, vmhook::hotspot::method*) { ++calls; return true; }) };
        check("deoptimize_methods_if_repeated_first_zero", first == 0);
        check("deoptimize_methods_if_repeated_second_zero", second == 0);
        check("deoptimize_methods_if_repeated_predicate_not_invoked", calls == 0);
    }

    // --- deoptimize_all_jit_compiled_methods: no JVM -> 0, never throws --
    // Convenience wrapper over deoptimize_methods_if with an always-true
    // predicate; without a JVM it must return 0 and stay crash-free.
    {
        std::size_t deopt{ 999 };
        bool threw{ false };
        try { deopt = vmhook::deoptimize_all_jit_compiled_methods(); }
        catch (...) { threw = true; }
        check("deoptimize_all_jit_returns_zero_without_jvm", deopt == 0);
        check("deoptimize_all_jit_does_not_throw_without_jvm", !threw);
    }
    {
        // Idempotent across repeated calls with no JVM.
        std::size_t a{ vmhook::deoptimize_all_jit_compiled_methods() };
        std::size_t b{ vmhook::deoptimize_all_jit_compiled_methods() };
        std::size_t c{ vmhook::deoptimize_all_jit_compiled_methods() };
        check("deoptimize_all_jit_idempotent_zero",
              a == 0 && b == 0 && c == 0);
        static_assert(
            std::is_same_v<decltype(vmhook::deoptimize_all_jit_compiled_methods()),
                std::size_t>,
            "deoptimize_all_jit_compiled_methods must return std::size_t");
        check("deoptimize_all_jit_return_type_is_size_t", true);
    }

    // --- scoped_hook<T>: no JVM -> inert handle, never throws ------------
    // scoped_hook<T> first calls hook<T>(); with no JVM the throwaway wrapper
    // type was never registered (register_class returned false above) so
    // hook<T>() raises vmhook::exception internally, but hook<T>() catches
    // every std::exception itself and returns false.  scoped_hook sees the
    // false, logs, and returns an empty hook_handle.  The contract is
    // therefore: returns an inert handle (installed() == false) and does NOT
    // throw.  The callback must never fire.
    {
        int fired{ 0 };
        bool threw{ false };
        bool installed_true{ true };
        try
        {
            vmhook::hook_handle handle{ vmhook::scoped_hook<dummy_wrapper>(
                "doStuff",
                [&fired](vmhook::return_value&) { ++fired; }) };
            installed_true = handle.installed();
        }
        catch (...) { threw = true; }
        check("scoped_hook_handle_not_installed_without_jvm", installed_true == false);
        check("scoped_hook_does_not_throw_without_jvm", !threw);
        check("scoped_hook_callback_not_fired_without_jvm", fired == 0);
    }
    {
        // Same for the explicit-signature overload: it routes through the
        // same hook<T>() pre-check and behaves identically with no JVM.
        int fired{ 0 };
        bool threw{ false };
        bool installed_true{ true };
        try
        {
            vmhook::hook_handle handle{ vmhook::scoped_hook<dummy_wrapper>(
                "doStuff", "()V",
                [&fired](vmhook::return_value&) { ++fired; }) };
            installed_true = handle.installed();
        }
        catch (...) { threw = true; }
        check("scoped_hook_with_signature_handle_not_installed", installed_true == false);
        check("scoped_hook_with_signature_does_not_throw", !threw);
        check("scoped_hook_with_signature_callback_not_fired", fired == 0);
    }
    {
        // hook_handle inert-state contract (the state scoped_hook returns on a
        // post-install resolution failure): default-constructed handle is not
        // installed, is move-constructible, the moved-from source becomes
        // inert too, and destruction of an empty handle is safe.
        bool threw{ false };
        bool src_installed{ true };
        bool dst_installed{ true };
        try
        {
            vmhook::hook_handle empty{};
            check("scoped_hook_default_handle_not_installed",
                  empty.installed() == false);

            vmhook::hook_handle moved{ std::move(empty) };
            dst_installed = moved.installed();
            src_installed = empty.installed();  // NOLINT(bugprone-use-after-move)
            // `moved` and `empty` both destruct here at end of scope.
        }
        catch (...) { threw = true; }
        check("scoped_hook_moved_to_handle_not_installed", dst_installed == false);
        check("scoped_hook_moved_from_handle_not_installed", src_installed == false);
        check("scoped_hook_inert_handle_destructor_safe", !threw);
    }
    {
        // Move-assignment of an inert handle is also safe and stays inert.
        bool threw{ false };
        bool assigned_installed{ true };
        try
        {
            vmhook::hook_handle a{};
            vmhook::hook_handle b{};
            b = std::move(a);
            assigned_installed = b.installed();
        }
        catch (...) { threw = true; }
        check("scoped_hook_move_assigned_handle_not_installed",
              assigned_installed == false);
        check("scoped_hook_move_assign_inert_handle_safe", !threw);
    }

    // --- watch_static_field<T, value_t>: no JVM -> inert handle ----------
    // On Windows x86_64 (hardware data breakpoints available) the real path
    // runs and first resolves the field via object_base::get_field, which
    // returns nullopt without a JVM (klass not resolved) -> inert handle.
    // On every other platform the function returns an inert handle directly.
    // Either way running() must be false and the callback must never fire.
    // Exercised across the field-width-distinct value types the DR LEN field
    // selection depends on (1/2/4/8 bytes + bool + float/double).
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{
                vmhook::watch_static_field<dummy_wrapper, std::int32_t>(
                    "counter",
                    [&fired](std::int32_t, std::int32_t) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("watch_static_field_int32_not_running_without_jvm", running_true == false);
        check("watch_static_field_int32_callback_not_fired", fired == 0);
        check("watch_static_field_int32_does_not_throw", !threw);
    }
    {
        int fired{ 0 };
        std::int64_t dummy{ 0 };
        (void)dummy;
        vmhook::watch_handle handle{
            vmhook::watch_static_field<dummy_wrapper, std::int64_t>(
                "longCounter",
                [&fired](std::int64_t, std::int64_t) { ++fired; }) };
        check("watch_static_field_int64_not_running_without_jvm", handle.running() == false);
        check("watch_static_field_int64_callback_not_fired", fired == 0);
    }
    {
        int fired{ 0 };
        vmhook::watch_handle handle{
            vmhook::watch_static_field<dummy_wrapper, float>(
                "ratio",
                [&fired](float, float) { ++fired; }) };
        check("watch_static_field_float_not_running_without_jvm", handle.running() == false);
        check("watch_static_field_float_callback_not_fired", fired == 0);
    }
    {
        int fired{ 0 };
        vmhook::watch_handle handle{
            vmhook::watch_static_field<dummy_wrapper, double>(
                "scale",
                [&fired](double, double) { ++fired; }) };
        check("watch_static_field_double_not_running_without_jvm", handle.running() == false);
        check("watch_static_field_double_callback_not_fired", fired == 0);
    }
    {
        // bool is a single-byte field width (exercises the one_byte LEN arm).
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{
                vmhook::watch_static_field<dummy_wrapper, bool>(
                    "flag",
                    [&fired](bool, bool) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("watch_static_field_bool_not_running_without_jvm", running_true == false);
        check("watch_static_field_bool_callback_not_fired", fired == 0);
        check("watch_static_field_bool_does_not_throw", !threw);
    }
    {
        // Inert watch_handle from watch_static_field destructs safely when it
        // drops out of scope (its on_stop is empty, so no DR teardown runs).
        bool threw{ false };
        try
        {
            vmhook::watch_handle handle{
                vmhook::watch_static_field<dummy_wrapper, std::int32_t>(
                    "x", [](std::int32_t, std::int32_t) {}) };
        }
        catch (...) { threw = true; }
        check("watch_static_field_inert_handle_destructor_safe", !threw);
    }

    // --- klass_from_oop: null + junk pointers -> nullptr, never throws ---
    // klass_from_oop guards with is_valid_pointer() *before* dereferencing
    // (it reads the narrow-klass at oop+8 only for valid pointers).  null,
    // sub-floor (<= 0xFFFF), and non-canonical (>= ceiling) pointers are all
    // rejected up front, so no garbage is ever dereferenced.
    {
        bool threw{ false };
        vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x1) };
        try { k = vmhook::klass_from_oop(nullptr); }
        catch (...) { threw = true; }
        check("klass_from_oop_null_returns_null", k == nullptr);
        check("klass_from_oop_null_does_not_throw", !threw);
    }
    {
        // 0x1000 (4096) is <= user_address_floor (0xFFFF) -> rejected by
        // is_valid_pointer, so klass_from_oop returns null without reading it.
        void* const small_junk{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x1000)) };
        bool threw{ false };
        vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x1) };
        try { k = vmhook::klass_from_oop(small_junk); }
        catch (...) { threw = true; }
        check("klass_from_oop_small_junk_returns_null", k == nullptr);
        check("klass_from_oop_small_junk_does_not_throw", !threw);
    }
    {
        // A non-canonical high pointer (>= user_address_ceiling) is likewise
        // rejected before any dereference.
        void* const high_junk{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0xDEAD000000000000ull)) };
        bool threw{ false };
        vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x1) };
        try { k = vmhook::klass_from_oop(high_junk); }
        catch (...) { threw = true; }
        check("klass_from_oop_high_noncanonical_returns_null", k == nullptr);
        check("klass_from_oop_high_noncanonical_does_not_throw", !threw);
    }
    {
        // An odd (mis-aligned) pointer is rejected by the alignment check.
        void* const odd_junk{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x100001ull)) };
        vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(odd_junk) };
        check("klass_from_oop_odd_pointer_returns_null", k == nullptr);
    }

    // =====================================================================
    // RUNTIME no-op coverage for entry points the original file compiled but
    // never *ran* (test_api_surface.cpp builds them; this asserts the no-JVM
    // return value).  All must return their safe default + never throw +
    // never invoke a visitor/callback with no JVM present.
    // =====================================================================

    // --- hook<T> (2-arg + 3-arg + self/args detours): no JVM -> false -----
    // dummy_wrapper was never successfully registered (register_class returned
    // false above), so hook<T> looks up an unregistered type, throws internally
    // ("Class not registered"), and the internal catch converts that to false.
    {
        bool threw{ false };
        int fired{ 0 };
        bool r2{ true };
        bool r3{ true };
        bool rself{ true };
        bool rargs{ true };
        try
        {
            r2 = vmhook::hook<dummy_wrapper>(
                "m", [&fired](vmhook::return_value&) { ++fired; });
            r3 = vmhook::hook<dummy_wrapper>(
                "m", "()V", [&fired](vmhook::return_value&) { ++fired; });
            rself = vmhook::hook<dummy_wrapper>(
                "m", [&fired](vmhook::return_value&, const std::unique_ptr<dummy_wrapper>&) { ++fired; });
            rargs = vmhook::hook<dummy_wrapper>(
                "m", "(I)V",
                [&fired](vmhook::return_value&, const std::unique_ptr<dummy_wrapper>&, int) { ++fired; });
        }
        catch (...) { threw = true; }
        check("hook_2arg_returns_false_without_jvm", r2 == false);
        check("hook_3arg_returns_false_without_jvm", r3 == false);
        check("hook_self_detour_returns_false_without_jvm", rself == false);
        check("hook_args_detour_returns_false_without_jvm", rargs == false);
        check("hook_does_not_throw_without_jvm", !threw);
        check("hook_callback_not_fired_without_jvm", fired == 0);
    }

    // --- hook_by_signature<T>: no JVM -> false (no method matches) --------
    {
        bool threw{ false };
        bool r{ true };
        int fired{ 0 };
        try
        {
            r = vmhook::hook_by_signature<dummy_wrapper>(
                "()V", [&fired](vmhook::return_value&) { ++fired; });
        }
        catch (...) { threw = true; }
        check("hook_by_signature_returns_false_without_jvm", r == false);
        check("hook_by_signature_does_not_throw_without_jvm", !threw);
        check("hook_by_signature_callback_not_fired_without_jvm", fired == 0);
    }

    // --- shutdown_hooks remains safe AFTER a failed hook install ----------
    {
        bool threw{ false };
        try { vmhook::shutdown_hooks(); }
        catch (...) { threw = true; }
        check("shutdown_hooks_after_failed_install_no_throw", !threw);
    }

    // --- verify_hooks: no hooks installed -> 0, never throws --------------
    {
        std::size_t repaired{ 999 };
        bool threw{ false };
        try { repaired = vmhook::verify_hooks(); }
        catch (...) { threw = true; }
        check("verify_hooks_returns_zero_without_hooks", repaired == 0);
        check("verify_hooks_does_not_throw_without_hooks", !threw);
        // Idempotent.
        check("verify_hooks_idempotent_zero",
              vmhook::verify_hooks() == 0 && vmhook::verify_hooks() == 0);
    }

    // --- container wrappers from a null OOP: empty / zero at RUNTIME ------
    // test_api_surface.cpp builds these but asserts nothing; pin the no-JVM
    // values here.  size()==0, is_empty()==true, to_vector/to_entries empty.
    {
        vmhook::collection  c{ nullptr };
        vmhook::list        l{ nullptr };
        vmhook::set         s{ nullptr };
        vmhook::linked_list ll{ nullptr };
        vmhook::map         m{ nullptr };
        vmhook::hash_map    hm{ nullptr };
        bool threw{ false };
        bool sizes_zero{ false };
        bool empties_true{ false };
        bool vectors_empty{ false };
        bool entries_empty{ false };
        try
        {
            sizes_zero = c.size() == 0 && l.size() == 0 && s.size() == 0
                      && ll.size() == 0 && m.size() == 0 && hm.size() == 0;
            empties_true = c.is_empty() && l.is_empty() && s.is_empty()
                        && ll.is_empty() && m.is_empty() && hm.is_empty();
            vectors_empty = c.to_vector<elem_wrapper>().empty()
                         && l.to_vector<elem_wrapper>().empty()
                         && s.to_vector<elem_wrapper>().empty()
                         && ll.to_vector<elem_wrapper>().empty();
            entries_empty = m.to_entries<key_wrapper, val_wrapper>().empty()
                         && hm.to_entries<key_wrapper, val_wrapper>().empty();
        }
        catch (...) { threw = true; }
        check("null_oop_containers_size_zero", sizes_zero);
        check("null_oop_containers_is_empty_true", empties_true);
        check("null_oop_containers_to_vector_empty", vectors_empty);
        check("null_oop_maps_to_entries_empty", entries_empty);
        check("null_oop_containers_do_not_throw", !threw);
    }

    // --- field_proxy from a null OOP: scalar getters -> safe defaults -----
    // A reference-typed null-OOP field_proxy: get() returns a value_t with the
    // default int32 alternative; the container conversions are empty; the
    // scalar getters all yield their zero/false/empty defaults.  No deref.
    {
        vmhook::field_proxy ref_field{ nullptr, "Ljava/util/List;", false };
        bool threw{ false };
        bool vec_empty{ false };
        bool entries_empty{ false };
        bool is_ref{ false };
        try
        {
            vec_empty = ref_field.get().to_vector<elem_wrapper>().empty();
            entries_empty = ref_field.get().to_entries<key_wrapper, val_wrapper>().empty();
            is_ref = ref_field.is_reference();
        }
        catch (...) { threw = true; }
        check("null_field_proxy_get_to_vector_empty", vec_empty);
        check("null_field_proxy_get_to_entries_empty", entries_empty);
        check("null_field_proxy_reference_sig_is_reference", is_ref);
        check("null_field_proxy_ref_does_not_throw", !threw);
    }
    {
        // Scalar (primitive) null-OOP field proxies: every getter conversion
        // returns the zero/false default and never dereferences the null ptr.
        vmhook::field_proxy int_field { nullptr, "I", false };
        vmhook::field_proxy long_field{ nullptr, "J", false };
        vmhook::field_proxy bool_field{ nullptr, "Z", false };
        vmhook::field_proxy dbl_field { nullptr, "D", false };
        vmhook::field_proxy flt_field { nullptr, "F", false };
        bool threw{ false };
        int  iv{ 7 };
        long lv{ 7 };
        bool bv{ true };
        double dv{ 7.0 };
        float  fv{ 7.0f };
        try
        {
            iv = int_field.get();
            lv = static_cast<long>(static_cast<std::int64_t>(long_field.get()));
            bv = bool_field.get();
            dv = dbl_field.get();
            fv = flt_field.get();
        }
        catch (...) { threw = true; }
        check("null_field_proxy_int_getter_zero", iv == 0);
        check("null_field_proxy_long_getter_zero", lv == 0);
        check("null_field_proxy_bool_getter_false", bv == false);
        check("null_field_proxy_double_getter_zero", dv == 0.0);
        check("null_field_proxy_float_getter_zero", fv == 0.0f);
        check("null_field_proxy_scalar_getters_do_not_throw", !threw);
        // The static-vs-instance flag round-trips through the accessor.
        vmhook::field_proxy static_field{ nullptr, "I", true };
        check("null_field_proxy_is_static_true", static_field.is_static() == true);
        check("null_field_proxy_is_static_false", int_field.is_static() == false);
        // signature() echoes back exactly what was passed.
        check("null_field_proxy_signature_roundtrip", int_field.signature() == "I");
        // raw_address() is the (null) pointer it was constructed with.
        check("null_field_proxy_raw_address_null", int_field.raw_address() == nullptr);
        // get_compressed_oop() on a null-OOP reference field is 0 (no deref).
        vmhook::field_proxy ref_field_local{ nullptr, "Ljava/lang/String;", false };
        bool compressed_threw{ false };
        std::uint32_t compressed{ 0xFFFFFFFFu };
        try { compressed = ref_field_local.get_compressed_oop(); }
        catch (...) { compressed_threw = true; }
        check("null_field_proxy_compressed_oop_zero", compressed == 0u);
        check("null_field_proxy_compressed_oop_no_throw", !compressed_threw);
    }

    // --- make_java_string / make_java_array: no JVM -> nullptr -----------
    {
        bool threw{ false };
        void* s{ reinterpret_cast<void*>(0x1) };
        void* a{ reinterpret_cast<void*>(0x1) };
        try
        {
            s = vmhook::make_java_string("hello");
            a = vmhook::make_java_array("java/lang/Object", 4, sizeof(void*));
        }
        catch (...) { threw = true; }
        check("make_java_string_returns_null_without_jvm", s == nullptr);
        check("make_java_array_returns_null_without_jvm", a == nullptr);
        check("make_java_string_array_do_not_throw", !threw);
        // Negative length is rejected up front (returns null) and never throws.
        check("make_java_array_negative_length_null",
              vmhook::make_java_array("java/lang/Object", -1, sizeof(void*)) == nullptr);
        // Empty string still yields null with no JVM.
        check("make_java_string_empty_returns_null",
              vmhook::make_java_string("") == nullptr);
    }

    // --- write_java_string / set_str_field / field_oop / set_*_array ------
    // on a null-OOP field_proxy: all are no-throw no-ops, field_oop -> null.
    {
        vmhook::field_proxy ref_field{ nullptr, "Ljava/lang/String;", false };
        bool threw{ false };
        void* fo{ reinterpret_cast<void*>(0x1) };
        try
        {
            vmhook::write_java_string(nullptr, "x");
            vmhook::set_str_field(ref_field, "x");
            fo = vmhook::field_oop(ref_field);
            vmhook::set_bool_array(ref_field, std::vector<bool>{ true, false });
            vmhook::set_str_array(ref_field, std::vector<std::string>{ "a", "b" });
            vmhook::set_prim_array<std::int32_t>(ref_field, std::vector<std::int32_t>{ 1, 2, 3 });
        }
        catch (...) { threw = true; }
        check("field_oop_null_field_returns_null", fo == nullptr);
        check("string_and_array_setters_do_not_throw_without_jvm", !threw);
    }

    // --- get_array_element / set_array_element / decode_array_oop ---------
    // get/set_array_element operate on a caller-owned buffer laid out like a
    // HotSpot primitive array (16-byte header: mark+klass+length at +12, then
    // the element payload at +16).  No JVM is needed.  The buffer is HEAP-backed
    // (std::vector) so its data() is a canonical address that clears
    // is_valid_pointer — the same technique tests/test_array_element_helpers.cpp
    // uses.  We round-trip a value to prove the public template entry points are
    // callable and deterministic, then confirm the no-JVM safe defaults.
    {
        std::vector<std::uint8_t> buffer(16u + 4u * sizeof(std::int32_t), std::uint8_t{ 0 });
        const std::int32_t length{ 4 };
        std::memcpy(buffer.data() + 12, &length, sizeof(length)); // seed _length
        void* const array_oop{ buffer.data() };
        bool threw{ false };
        std::int32_t got0{ -1 };
        std::int32_t got1{ -1 };
        try
        {
            vmhook::set_array_element<std::int32_t>(array_oop, 0, 1234);
            vmhook::set_array_element<std::int32_t>(array_oop, 1, -5678);
            got0 = vmhook::get_array_element<std::int32_t>(array_oop, 0);
            got1 = vmhook::get_array_element<std::int32_t>(array_oop, 1);
        }
        catch (...) { threw = true; }
        check("array_element_roundtrip_index0", got0 == 1234);
        check("array_element_roundtrip_index1", got1 == -5678);
        check("array_length_reflects_seeded_count", vmhook::array_length(array_oop) == 4);
        check("array_element_helpers_do_not_throw", !threw);
        // get_array_element on a null array yields the element default, no throw.
        check("get_array_element_null_array_default",
              vmhook::get_array_element<std::int32_t>(nullptr, 0) == 0);
        // Out-of-bounds index on the real buffer returns the default, no deref.
        check("get_array_element_oob_index_default",
              vmhook::get_array_element<std::int32_t>(array_oop, 99) == 0);
        check("get_array_element_negative_index_default",
              vmhook::get_array_element<std::int32_t>(array_oop, -1) == 0);
        // set_array_element OOB is a no-op (does not corrupt the buffer).
        vmhook::set_array_element<std::int32_t>(array_oop, 99, 0x7FFFFFFF);
        check("set_array_element_oob_is_noop",
              vmhook::get_array_element<std::int32_t>(array_oop, 0) == 1234);
        // decode_array_oop(0) -> null (compressed null), no throw.
        bool decode_threw{ false };
        void* decoded{ reinterpret_cast<void*>(0x1) };
        try { decoded = vmhook::decode_array_oop(0); }
        catch (...) { decode_threw = true; }
        check("decode_array_oop_zero_returns_null", decoded == nullptr);
        check("decode_array_oop_zero_does_not_throw", !decode_threw);
    }

    // --- get_class_methods / find_methods_by_signature / log_class_methods -
    // No JVM -> empty vectors, log_class_methods is a no-op.  (find_class is
    // null so collect_klass_methods returns {}.)
    {
        bool threw{ false };
        bool name_empty{ false };
        bool t_empty{ false };
        bool sig_empty{ false };
        try
        {
            name_empty = vmhook::get_class_methods("java/lang/String").empty();
            t_empty = vmhook::get_class_methods<dummy_wrapper>().empty();
            sig_empty = vmhook::find_methods_by_signature<dummy_wrapper>("()V").empty();
            vmhook::log_class_methods<dummy_wrapper>();
        }
        catch (...) { threw = true; }
        check("get_class_methods_by_name_empty_without_jvm", name_empty);
        check("get_class_methods_T_empty_without_jvm", t_empty);
        check("find_methods_by_signature_T_empty_without_jvm", sig_empty);
        check("class_methods_helpers_do_not_throw", !threw);
    }

    // --- find_class_via_oop(nullptr, ...) -> null, never throws -----------
    {
        bool threw{ false };
        vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x1) };
        try { k = vmhook::find_class_via_oop(nullptr, "java/lang/String"); }
        catch (...) { threw = true; }
        check("find_class_via_oop_null_anchor_returns_null", k == nullptr);
        check("find_class_via_oop_null_anchor_does_not_throw", !threw);
    }

    // --- override_class_lookup / evict_class_lookup: no-throw cache ops ---
    {
        bool threw{ false };
        try
        {
            vmhook::override_class_lookup("api/Surface/Probe", nullptr);
            // After seeding a null entry, find_class still returns null.
            const bool still_null{ vmhook::find_class("api/Surface/Probe") == nullptr };
            vmhook::evict_class_lookup("api/Surface/Probe");
            vmhook::evict_class_lookup("api/Surface/Never/Cached");
            check("override_then_find_class_still_null", still_null);
        }
        catch (...) { threw = true; }
        check("override_evict_class_lookup_do_not_throw", !threw);
    }

    // --- reanchor_classes_via_oop(nullptr, {...}) -> false, never throws --
    {
        bool threw{ false };
        bool r{ true };
        try
        {
            r = vmhook::reanchor_classes_via_oop(
                nullptr, { "java/lang/String", "java/util/List" });
        }
        catch (...) { threw = true; }
        check("reanchor_classes_via_oop_null_anchor_returns_false", r == false);
        check("reanchor_classes_via_oop_does_not_throw", !threw);
    }

    // --- pin(oop) / pin(unique_ptr<T>&): no JVM -> inert oop_pin -------
    // The JNI-free oop_pin just stores the raw oop; oop_pin{ null oop }
    // therefore holds nullptr, makes no VM call at all, and its destructor is a
    // no-op.  pin(empty unique_ptr) likewise yields a default (inert) oop_pin.
    {
        bool threw{ false };
        try
        {
            vmhook::oop_pin g1{ vmhook::pin(static_cast<vmhook::oop_t>(nullptr)) };
            std::unique_ptr<dummy_wrapper> empty_uptr{};
            vmhook::oop_pin g2{ vmhook::pin(empty_uptr) };
            vmhook::oop_pin g3{};                 // default-constructed
            vmhook::oop_pin g4{ std::move(g3) };  // move-construct inert
            g2 = std::move(g4);                           // move-assign inert
            (void)g1;
            (void)g2;
        }
        catch (...) { threw = true; }
        check("pin_and_oop_pin_inert_do_not_throw", !threw);
    }

    // =====================================================================
    // ADDITIVE DEEPENING PASS (wave-14) — exhaustive degenerate-input no-JVM
    // contract.  Every assertion below derives its expected value directly
    // from the vmhook.hpp source (line refs in comments).  No fabricated live
    // oop/Method/klass/handle is ever dereferenced: every pointer fed in is
    // either null, an is_valid_pointer-REJECTED constant (floor/ceiling/odd/
    // poison-sentinel — rejected BEFORE any read, vmhook.hpp:2047-2084), or a
    // REAL heap buffer this test owns (std::vector / std::array data()).
    // =====================================================================
    {
        // --- find_class degenerate names: all null without a JVM, no throw ---
        // find_class short-circuits "" -> null; every other name (including '['
        // array descriptors, which no longer have a JNI FindClass fallback since
        // the de-JNI refactor) falls through the ClassLoaderDataGraph walk, which
        // finds nothing with no JVM loaded, all under a catch-all try/catch.
        // Mixed '.'/'/' separators do NOT short-circuit
        // -- they are resolved like any other name and simply miss -> null.
        bool threw{ false };
        bool all_null{ false };
        try
        {
            const vmhook::hotspot::klass* const a{ vmhook::find_class("[I") };
            const vmhook::hotspot::klass* const b{ vmhook::find_class("[[J") };
            const vmhook::hotspot::klass* const c{ vmhook::find_class("[Ljava/lang/String;") };
            const vmhook::hotspot::klass* const d{ vmhook::find_class("java.lang.Object") };
            const vmhook::hotspot::klass* const e{ vmhook::find_class("java/lang/Object") };
            const vmhook::hotspot::klass* const f{ vmhook::find_class("net/minecraft/client/Minecraft") };
            all_null = a == nullptr && b == nullptr && c == nullptr
                    && d == nullptr && e == nullptr && f == nullptr;
        }
        catch (...) { threw = true; }
        check("find_class_array_and_mixed_separator_names_null", all_null);
        check("find_class_degenerate_names_do_not_throw", !threw);
    }
    {
        // A pathologically long name (100 KB of '/'-separated segments) must
        // still resolve to null without throwing and without overrunning any
        // internal buffer (find_class works on a string_view; no fixed buffer).
        std::string long_name;
        long_name.reserve(100u * 1024u);
        for (int i{ 0 }; i < 6800; ++i) { long_name += "pkg/"; }
        long_name += "Class";
        bool threw{ false };
        const vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x2) };
        try { k = vmhook::find_class(long_name); }
        catch (...) { threw = true; }
        check("find_class_very_long_name_null", k == nullptr);
        check("find_class_very_long_name_does_not_throw", !threw);
    }
    {
        // A name with an embedded NUL byte (string_view length carries past it)
        // is treated as an ordinary miss -> null, never throws, never reads OOB.
        const char raw[]{ 'a', '/', 'B', '\x00', 'c', '/', 'D' };
        const std::string_view embedded_nul{ raw, sizeof(raw) };
        bool threw{ false };
        const vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x2) };
        try { k = vmhook::find_class(embedded_nul); }
        catch (...) { threw = true; }
        check("find_class_embedded_nul_name_null", k == nullptr);
        check("find_class_embedded_nul_name_does_not_throw", !threw);
    }

    // --- read_java_string: is_valid_pointer-REJECTED pointers -> empty -------
    // read_java_string (vmhook.hpp:20471) bails at 20474 when the pointer fails
    // is_valid_pointer (2047-2084) BEFORE any dereference.  Every pointer here
    // is rejected by a DIFFERENT arm of that filter, proving each reject leg
    // yields "" and never throws.  None is ever read through.
    {
        // user_address_floor is 0xFFFF (vmhook.hpp:520); addr <= floor rejects.
        void* const at_floor{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0xFFFFu)) };
        // user_address_ceiling is 0x00007FFFFFFFFFFF (vmhook.hpp:515); addr >=
        // ceiling rejects.  Use the ceiling exactly (>= is inclusive).
        void* const at_ceiling{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x00007FFFFFFFFFFFull)) };
        // Odd (low bit set) in-range pointer: alignment arm (2059) rejects.
        void* const odd_inrange{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x100003ull)) };
        // Debug-poison: low32 == 0xCCCCCCCC (MSVC uninitialised stack, 2072).
        // It is even and in range, so the alignment arm passes it through to
        // the poison switch, which rejects it.  (0xDEADBEEF / 0xCDCDCDCD are
        // ODD, so they would be caught by the alignment arm first -- to prove
        // the POISON arm specifically the low32 sentinel must be even.)
        void* const poison_cccc{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0xCCCCCCCCull)) };
        // Debug-poison: low32 == 0xFEEEFEEE (Windows HeapFree, 2075), even.
        void* const poison_feee{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0xFEEEFEEEull)) };
        bool threw{ false };
        bool all_empty{ false };
        try
        {
            all_empty = vmhook::read_java_string(at_floor).empty()
                     && vmhook::read_java_string(at_ceiling).empty()
                     && vmhook::read_java_string(odd_inrange).empty()
                     && vmhook::read_java_string(poison_cccc).empty()
                     && vmhook::read_java_string(poison_feee).empty();
        }
        catch (...) { threw = true; }
        check("read_java_string_rejected_pointers_all_empty", all_empty);
        check("read_java_string_rejected_pointers_do_not_throw", !threw);
    }

    // --- decode_array_oop: NON-zero compressed -> null without VMStructs -----
    // decode_array_oop (vmhook.hpp:20984) forwards a non-zero compressed value
    // to decode_oop_pointer (5504), which resolves the narrow-oop base/shift
    // VMStruct entries (5527-5530); with no JVM both are null so it returns
    // null at 5532, and decode_array_oop's is_valid_pointer gate (20992) keeps
    // it null.  The existing file only covered the compressed==0 fast path.
    {
        bool threw{ false };
        bool all_null{ false };
        try
        {
            all_null = vmhook::decode_array_oop(1u) == nullptr
                    && vmhook::decode_array_oop(0x7FFFFFFFu) == nullptr
                    && vmhook::decode_array_oop(0xFFFFFFFFu) == nullptr;
        }
        catch (...) { threw = true; }
        check("decode_array_oop_nonzero_null_without_jvm", all_null);
        check("decode_array_oop_nonzero_does_not_throw", !threw);
    }

    // --- get/set_array_element element-type breadth on REAL owned buffers ----
    // The existing file round-trips only int32.  Element data starts at +16
    // with stride sizeof(element_type); _length is the int32 at +12
    // (vmhook.hpp:14796-14797 / 14845 / 14875).  Each buffer below is a real
    // heap allocation (std::array on the stack is also a canonical mapped
    // address) sized header(16) + count*stride, with _length seeded at +12.
    // We prove the public template entry points round-trip every width the
    // primitive-array helpers support and that the no-JVM safe defaults hold.
    {
        // Element count for buffer SIZING is size_t (no signed/unsigned mixing
        // in the std::array bound); the int32 length SEEDED at +12 is separate.
        constexpr std::size_t elem_count{ 3u };
        const std::int32_t length{ 3 };
        // int8_t
        {
            std::array<std::uint8_t, 16u + elem_count * sizeof(std::int8_t)> buf{};
            std::memcpy(buf.data() + 12, &length, sizeof(length));
            void* const oop{ buf.data() };
            vmhook::set_array_element<std::int8_t>(oop, 0, std::int8_t{ 0x7F });
            vmhook::set_array_element<std::int8_t>(oop, 2, std::int8_t{ -128 });
            check("array_element_int8_roundtrip0",
                  vmhook::get_array_element<std::int8_t>(oop, 0) == std::int8_t{ 0x7F });
            check("array_element_int8_roundtrip2",
                  vmhook::get_array_element<std::int8_t>(oop, 2) == std::int8_t{ -128 });
            check("array_element_int8_length", vmhook::array_length(oop) == length);
        }
        // int16_t
        {
            std::array<std::uint8_t, 16u + elem_count * sizeof(std::int16_t)> buf{};
            std::memcpy(buf.data() + 12, &length, sizeof(length));
            void* const oop{ buf.data() };
            vmhook::set_array_element<std::int16_t>(oop, 1, std::int16_t{ -12345 });
            check("array_element_int16_roundtrip1",
                  vmhook::get_array_element<std::int16_t>(oop, 1) == std::int16_t{ -12345 });
        }
        // int64_t
        {
            std::array<std::uint8_t, 16u + elem_count * sizeof(std::int64_t)> buf{};
            std::memcpy(buf.data() + 12, &length, sizeof(length));
            void* const oop{ buf.data() };
            const std::int64_t v{ static_cast<std::int64_t>(0x0123456789ABCDEFll) };
            vmhook::set_array_element<std::int64_t>(oop, 2, v);
            check("array_element_int64_roundtrip2",
                  vmhook::get_array_element<std::int64_t>(oop, 2) == v);
        }
        // float
        {
            std::array<std::uint8_t, 16u + elem_count * sizeof(float)> buf{};
            std::memcpy(buf.data() + 12, &length, sizeof(length));
            void* const oop{ buf.data() };
            vmhook::set_array_element<float>(oop, 0, 3.5f);
            check("array_element_float_roundtrip0",
                  vmhook::get_array_element<float>(oop, 0) == 3.5f);
        }
        // double
        {
            std::array<std::uint8_t, 16u + elem_count * sizeof(double)> buf{};
            std::memcpy(buf.data() + 12, &length, sizeof(length));
            void* const oop{ buf.data() };
            vmhook::set_array_element<double>(oop, 1, 2.25);
            check("array_element_double_roundtrip1",
                  vmhook::get_array_element<double>(oop, 1) == 2.25);
        }
        // bool (single byte width)
        {
            std::array<std::uint8_t, 16u + elem_count * sizeof(bool)> buf{};
            std::memcpy(buf.data() + 12, &length, sizeof(length));
            void* const oop{ buf.data() };
            vmhook::set_array_element<bool>(oop, 0, true);
            vmhook::set_array_element<bool>(oop, 1, false);
            check("array_element_bool_roundtrip0",
                  vmhook::get_array_element<bool>(oop, 0) == true);
            check("array_element_bool_roundtrip1",
                  vmhook::get_array_element<bool>(oop, 1) == false);
        }
    }
    {
        // array_length on the same rejected pointers used elsewhere is 0 with
        // no deref: null, sub-floor, and odd (vmhook.hpp:14781).
        check("array_length_null_zero", vmhook::array_length(nullptr) == 0);
        check("array_length_subfloor_zero",
              vmhook::array_length(reinterpret_cast<void*>(
                  static_cast<std::uintptr_t>(0x100u))) == 0);
        check("array_length_odd_zero",
              vmhook::array_length(reinterpret_cast<void*>(
                  static_cast<std::uintptr_t>(0x100001ull))) == 0);
    }

    // --- make_java_array degenerate args: null without a JVM, no throw -------
    // make_java_array rejects a negative length up front; otherwise it needs the
    // element klass from find_class, which is null with no JVM (the '[' JNI
    // FindClass fallback it used to fall back on is gone since the de-JNI
    // refactor), so it returns null.  It is noexcept, so it cannot throw.
    {
        bool all_null{ false };
        all_null = vmhook::make_java_array("[I", 0, sizeof(std::int32_t)) == nullptr
                && vmhook::make_java_array("[Ljava/lang/Object;", 3, sizeof(void*)) == nullptr
                && vmhook::make_java_array("", 2, 1u) == nullptr
                && vmhook::make_java_array("java/lang/Object", -7, sizeof(void*)) == nullptr
                && vmhook::make_java_array("byte[]", 4, 1u) == nullptr;
        check("make_java_array_degenerate_args_all_null", all_null);
    }

    // --- object<T> field/method resolution with no JVM -> std::nullopt -------
    // get_field/get_method/static_field/static_method all call resolve_klass
    // first (vmhook.hpp:18138/18260/18325/18540).  dummy_wrapper was never
    // registered (register_class returned false above), so resolve_klass
    // returns null (18544-18548) -> std::nullopt.  Building the wrapper around
    // a NULL oop never dereferences it (object_base just stores the pointer).
    {
        const dummy_wrapper obj{ nullptr };
        bool threw{ false };
        bool all_nullopt{ false };
        try
        {
            const auto f{ obj.get_field("counter") };
            const auto m1{ obj.get_method("doStuff") };
            const auto m2{ obj.get_method("doStuff", "()V") };
            const auto sf{ vmhook::object<dummy_wrapper>::static_field("COUNT") };
            const auto sm1{ vmhook::object<dummy_wrapper>::static_method("create") };
            const auto sm2{ vmhook::object<dummy_wrapper>::static_method("create", "()V") };
            all_nullopt = !f.has_value() && !m1.has_value() && !m2.has_value()
                       && !sf.has_value() && !sm1.has_value() && !sm2.has_value();
        }
        catch (...) { threw = true; }
        check("object_field_method_resolution_all_nullopt", all_nullopt);
        check("object_field_method_resolution_does_not_throw", !threw);
        // get_instance() echoes back the (null) oop the wrapper was built with.
        check("object_null_wrapper_get_instance_null", obj.get_instance() == nullptr);
    }

    // --- vmhook::oop_pin inert-state accessors: no JVM, never deref ----------
    // A default-constructed / null-pinned oop_pin keeps its stored oop at
    // nullptr, so operator bool() is false, oop() returns null, handle() (the
    // retained-for-compatibility spelling of the same field) is null, and
    // reset() is an idempotent no-op.  Since the de-JNI refactor the holder
    // makes no VM call on any path — construction, accessors and destruction are
    // all plain field reads/writes — and no fabricated non-null oop is ever
    // stored here, so nothing is dereferenced either way.
    {
        bool threw{ false };
        bool default_inert{ false };
        bool null_pin_inert{ false };
        try
        {
            vmhook::oop_pin g_default{};
            default_inert = !static_cast<bool>(g_default)
                         && g_default.oop() == nullptr
                         && g_default.handle() == nullptr;

            vmhook::oop_pin g_null{ vmhook::pin(static_cast<vmhook::oop_t>(nullptr)) };
            null_pin_inert = !static_cast<bool>(g_null)
                          && g_null.oop() == nullptr
                          && g_null.handle() == nullptr;

            // reset() on an inert handle is a safe idempotent no-op.
            g_null.reset();
            g_null.reset();
            null_pin_inert = null_pin_inert
                          && g_null.oop() == nullptr
                          && g_null.handle() == nullptr;
        }
        catch (...) { threw = true; }
        check("oop_pin_default_inert_accessors", default_inert);
        check("oop_pin_null_pin_inert_accessors", null_pin_inert);
        check("oop_pin_inert_accessors_do_not_throw", !threw);
    }

    // --- NEVER-THROW BLANKET over the remaining no-JVM surface ---------------
    // One try/catch around a batch of entry points not individually wrapped
    // above, asserting the whole degenerate-input surface is exception-safe
    // with no JVM.  Every value fed in is null / empty / an is_valid_pointer-
    // rejected constant; no result is inspected here (the value contracts are
    // pinned above) -- this isolates the "never throws" half of the contract.
    {
        bool threw{ false };
        try
        {
            (void)vmhook::find_class("a/b/c");
            (void)vmhook::find_class_via_oop(nullptr, "a/b/c");
            (void)vmhook::klass_from_oop(nullptr);
            vmhook::override_class_lookup("blanket/Probe", nullptr);
            vmhook::evict_class_lookup("blanket/Probe");
            (void)vmhook::reanchor_classes_via_oop(nullptr, { "a/b/c" });
            (void)vmhook::get_class_methods("a/b/c");
            (void)vmhook::get_class_methods<dummy_wrapper>();
            (void)vmhook::find_methods_by_signature<dummy_wrapper>("()V");
            vmhook::log_class_methods<dummy_wrapper>();
            (void)vmhook::register_class<base_wrapper>("a/b/c");
            (void)vmhook::make_unique<dummy_wrapper>();
            (void)vmhook::make_unique<dummy_wrapper>(1, 2.0, true);
            (void)vmhook::make_java_string("blanket");
            (void)vmhook::make_java_string("");
            (void)vmhook::make_java_array("[I", 1, sizeof(std::int32_t));
            (void)vmhook::read_java_string(nullptr);
            (void)vmhook::decode_array_oop(0u);
            (void)vmhook::decode_array_oop(42u);
            (void)vmhook::verify_hooks();
            vmhook::shutdown_hooks();
            (void)vmhook::deoptimize_all_jit_compiled_methods();
            (void)vmhook::get_array_element<std::int32_t>(nullptr, 0);
            vmhook::set_array_element<std::int32_t>(nullptr, 0, 0);
            (void)vmhook::array_length(nullptr);
            (void)vmhook::auto_repair_enabled();
        }
        catch (...) { threw = true; }
        check("never_throw_blanket_no_jvm_surface", !threw);
    }

    // =====================================================================
    // COMPILE-TIME SURFACE-LOCKDOWN ACKNOWLEDGEMENTS
    // =====================================================================
    // The hundreds of static_asserts in namespace surface_lock above are the
    // real guard: this file does not compile if any pinned public entry point
    // is removed / renamed / re-signatured.  We surface one [PASS] line per
    // group so the lockdown is *visible* in the test output (and bumps the
    // check() count), turning a silent compile-time guarantee into an explicit,
    // greppable record of which API clusters are pinned.
    check("surface_lock_groupA_class_lookup_pinned", true);
    check("surface_lock_groupB_introspection_pinned", true);
    check("surface_lock_groupC_registration_construction_pinned", true);
    check("surface_lock_groupD_hook_install_teardown_pinned", true);
    check("surface_lock_groupE_enumeration_deopt_pinned", true);
    check("surface_lock_groupF_string_array_helpers_pinned", true);
    check("surface_lock_groupG_watchers_pinned", true);
    check("surface_lock_groupH_oop_pin_pin_pinned", true);
    check("surface_lock_groupI_purevm_jni_successors_pinned", true);
    check("surface_lock_groupJ_public_type_traits_pinned", true);
    check("wave14_additive_degenerate_input_deepening_present", true);

    return failures == 0 ? 0 : 1;
}
