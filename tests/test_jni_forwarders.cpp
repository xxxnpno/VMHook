// Lockdown of the public vmhook::jni:: forwarder layer — BOTH halves:
//
//   PART 1 (compile-time): vmhook::jni::* is the supported public spelling of
//   the JNI helpers that used to live under vmhook::detail::jni_*.  Several of
//   these forwarders currently have ZERO call sites anywhere in the header or
//   test-suite, so nothing else would notice if one were deleted, renamed, or
//   had its signature drift — new_global_ref / delete_global_ref already went
//   missing once.  Every forwarder is named in a static_assert that pins its
//   exact callable signature (parameter types + return type) AND, where a public
//   forwarder simply re-exports an internal primitive, that the two have the
//   SAME callable signature so the public spelling can never silently diverge
//   from the one it forwards to.  If any forwarder is removed, renamed, or its
//   signature changes, THIS TRANSLATION UNIT FAILS TO COMPILE on every compiler
//   / platform in CI.
//
//   PART 2 (runtime, no-JVM): every forwarder also carries a documented
//   "no live JVM / no JNIEnv" contract — it must return a safe sentinel
//   (nullptr / empty string / empty global_ref) and NEVER fault.  This test
//   binary loads no jvm.dll, so vmhook::hotspot::current_jni_env is null and
//   ensure_current_java_thread() fails; that is exactly the cold no-JVM state
//   the contract describes.  We drive every JNI call category over the full
//   input matrix it accepts and assert the sentinel/no-op result.  The few
//   forwarders that are PURE (signature_for_arg, oop_handle, decode_object, the
//   is_valid_pointer gate they rely on, function<> table indexing) are pinned
//   value-by-value over an exhaustive input set — including the JNI argument
//   value union and every argument/return descriptor type the table supports.
//
// Convention mirrors test_traits.cpp / test_signature_parsing.cpp: address-of-a-
// function fed into std::is_invocable_r_v for the non-overloaded free forwarders
// (taking the address is unambiguous because none of these are overloaded), and
// decltype()-based std::is_same_v for the templated ones, instantiated with
// representative types; the runtime half uses a check(name, ok) tally and
// returns non-zero on any failure.
//
// DETERMINISM / CROSS-PLATFORM: no JVM fixtures, no heap growth, no
// platform/compiler/JDK-variant hard asserts.  All numeric expectations are
// computed from the live header's own constants (vmhook::os::user_address_floor
// / user_address_ceiling) and from sizeof, never hard-coded per platform; the
// is_valid_pointer ceiling cases are sizeof(void*)-branched so a 32-bit build
// (where user_address_ceiling truncates) does not assert a 64-bit-only fact.
#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// A throwaway wrapper type so the object-constructing templated forwarders
// (jni::make_unique<T>) can be instantiated with something that actually
// satisfies their "derives from object_base" contract.
namespace
{
    struct test_wrapper : public vmhook::object<test_wrapper>
    {
        using vmhook::object<test_wrapper>::object;
    };
}

// =============================================================================
// PART 1 — COMPILE-TIME SIGNATURE LOCKDOWN
// =============================================================================

// -----------------------------------------------------------------------------
// jni::value — the jvalue union alias.
//
// Not a function: it is a type alias for the underlying union used to build the
// argument arrays handed to CallXMethodA.  Pin that it still names exactly the
// detail union (and that it is, in fact, that union type) so the public spelling
// can never silently diverge from the internal one.
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<vmhook::jni::value, vmhook::detail::jni_value>,
              "jni::value must alias the underlying vmhook::detail::jni_value union");
static_assert(std::is_union_v<vmhook::jni::value>,
              "jni::value must be a union (the jvalue layout HotSpot expects)");

// Pin the EXACT member set + per-member type of the jvalue union.  HotSpot reads
// these fields off the stack by JNI primitive type (z=jboolean, b=jbyte,
// c=jchar, s=jshort, i=jint, j=jlong, f=jfloat, d=jdouble, l=jobject); a drift
// in any member's type silently mis-encodes that argument slot.
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.z), bool>,          "jni::value::z must be bool (jboolean)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.b), std::int8_t>,   "jni::value::b must be int8_t (jbyte)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.c), std::uint16_t>, "jni::value::c must be uint16_t (jchar)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.s), std::int16_t>,  "jni::value::s must be int16_t (jshort)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.i), std::int32_t>,  "jni::value::i must be int32_t (jint)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.j), std::int64_t>,  "jni::value::j must be int64_t (jlong)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.f), float>,         "jni::value::f must be float (jfloat)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.d), double>,        "jni::value::d must be double (jdouble)");
static_assert(std::is_same_v<decltype(vmhook::jni::value{}.l), void*>,         "jni::value::l must be void* (jobject)");

// -----------------------------------------------------------------------------
// jni::function<index, function_t>(void* env) -> function_t   [TEMPLATE]
//
// Looks up a JNI function-table slot by compile-time index and reinterpret-casts
// it to the requested function-pointer type.  Instantiate with a representative
// (index, function-pointer) pair and pin both that it is invocable on a void*
// env and that the return type is exactly the requested function-pointer type.
// -----------------------------------------------------------------------------
namespace
{
    using sample_jni_fn_t = void* (*)(void*, void*); // e.g. NewGlobalRef-shaped slot
}
static_assert(
    std::is_same_v<decltype(vmhook::jni::function<21, sample_jni_fn_t>(std::declval<void*>())),
                   sample_jni_fn_t>,
    "jni::function<index, fn_t>(void* env) must return the requested fn_t pointer type");
static_assert(
    std::is_invocable_r_v<sample_jni_fn_t,
                          decltype(vmhook::jni::function<21, sample_jni_fn_t>), void*>,
    "jni::function<index, fn_t> must be invocable with a single void* (JNIEnv*) and "
    "yield fn_t");
// The function_t parameter is honoured verbatim: a DIFFERENT pointer shape at a
// different index must yield exactly that shape (catches a hard-wired return type).
namespace
{
    using sample_jni_fn2_t = int (*)(void*, int); // e.g. PushLocalFrame-shaped slot
}
static_assert(
    std::is_same_v<decltype(vmhook::jni::function<19, sample_jni_fn2_t>(std::declval<void*>())),
                   sample_jni_fn2_t>,
    "jni::function<index, fn2_t> must return fn2_t verbatim for any (index, fn_t)");

// -----------------------------------------------------------------------------
// Forwarder-fidelity: each public jni::F must have the SAME callable signature
// as the detail::jni_F primitive it re-exports.  decltype on the function names
// compares the full type (parameters + return) in one shot, so a drift in
// either layer that the per-forwarder asserts below somehow missed is still
// caught here.  (jni::function / jni::make_unique / jni::signature_for_arg are
// templates and are pinned by instantiation instead, further down.)
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<decltype(vmhook::jni::decode_object), decltype(vmhook::detail::jni_decode_object)>,
              "jni::decode_object must forward detail::jni_decode_object verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::oop_handle), decltype(vmhook::detail::jni_oop_handle)>,
              "jni::oop_handle must forward detail::jni_oop_handle verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::find_class), decltype(vmhook::detail::jni_find_class)>,
              "jni::find_class must forward detail::jni_find_class verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::find_class_with_context_loader),
                             decltype(vmhook::detail::jni_find_class_with_context_loader)>,
              "jni::find_class_with_context_loader must forward the detail primitive verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::exception_clear), decltype(vmhook::detail::jni_exception_clear)>,
              "jni::exception_clear must forward detail::jni_exception_clear verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::get_object_class), decltype(vmhook::detail::jni_get_object_class)>,
              "jni::get_object_class must forward detail::jni_get_object_class verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::get_method_id), decltype(vmhook::detail::jni_get_method_id)>,
              "jni::get_method_id must forward detail::jni_get_method_id verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::get_static_method_id), decltype(vmhook::detail::jni_get_static_method_id)>,
              "jni::get_static_method_id must forward detail::jni_get_static_method_id verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::get_static_field_id), decltype(vmhook::detail::jni_get_static_field_id)>,
              "jni::get_static_field_id must forward detail::jni_get_static_field_id verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::get_static_object_field), decltype(vmhook::detail::jni_get_static_object_field)>,
              "jni::get_static_object_field must forward detail::jni_get_static_object_field verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::call_object_method), decltype(vmhook::detail::jni_call_object_method)>,
              "jni::call_object_method must forward detail::jni_call_object_method verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::call_static_object_method), decltype(vmhook::detail::jni_call_static_object_method)>,
              "jni::call_static_object_method must forward detail::jni_call_static_object_method verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::klass_from_class_mirror), decltype(vmhook::detail::jni_klass_from_class_mirror)>,
              "jni::klass_from_class_mirror must forward detail::jni_klass_from_class_mirror verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::new_string_utf), decltype(vmhook::detail::jni_new_string_utf)>,
              "jni::new_string_utf must forward detail::jni_new_string_utf verbatim");
static_assert(std::is_same_v<decltype(vmhook::jni::get_string_utf), decltype(vmhook::detail::jni_get_string_utf)>,
              "jni::get_string_utf must forward detail::jni_get_string_utf verbatim");

// -----------------------------------------------------------------------------
// jni::decode_object(void*) -> void*
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::decode_object), void*>,
              "jni::decode_object(void* handle) must take a jobject handle and return void* (oop)");
static_assert(std::is_same_v<decltype(vmhook::jni::decode_object(std::declval<void*>())), void*>,
              "jni::decode_object must return void*");
static_assert(noexcept(vmhook::jni::decode_object(std::declval<void*>())),
              "jni::decode_object must be noexcept (safe in a hook detour)");

// -----------------------------------------------------------------------------
// jni::oop_handle(void* oop, void*& storage) -> void*
//
// The second parameter is a NON-const lvalue reference (void*&); pin that
// exactly so a drift to by-value / const& is caught.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::oop_handle), void*, void*&>,
              "jni::oop_handle(void* oop, void*& storage) must take an oop and a void*& "
              "out-storage reference and return void* (synthetic handle)");
static_assert(!std::is_invocable_v<decltype(&vmhook::jni::oop_handle), void*, void* const&>,
              "jni::oop_handle's storage parameter must be a NON-const void*& (a const& must NOT bind)");
static_assert(noexcept(vmhook::jni::oop_handle(std::declval<void*>(), std::declval<void*&>())),
              "jni::oop_handle must be noexcept");

// -----------------------------------------------------------------------------
// jni::find_class(std::string_view) -> void*
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::find_class), std::string_view>,
              "jni::find_class(string_view class_name) must accept a string_view and return "
              "void* (jclass handle)");

// -----------------------------------------------------------------------------
// jni::find_class_with_context_loader(std::string_view) -> vmhook::hotspot::klass*
// Pin the klass* return so a drift back to void* is caught.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                    decltype(&vmhook::jni::find_class_with_context_loader),
                                    std::string_view>,
              "jni::find_class_with_context_loader(string_view) must return "
              "vmhook::hotspot::klass* (NOT a JNI handle)");
static_assert(std::is_same_v<decltype(vmhook::jni::find_class_with_context_loader(std::declval<std::string_view>())),
                             vmhook::hotspot::klass*>,
              "jni::find_class_with_context_loader must return klass* exactly");

// -----------------------------------------------------------------------------
// jni::exception_clear() -> void
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void, decltype(&vmhook::jni::exception_clear)>,
              "jni::exception_clear() must take no arguments and return void");

// -----------------------------------------------------------------------------
// jni::get_object_class(void*) -> void*
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_object_class), void*>,
              "jni::get_object_class(void* object_handle) must take a jobject and return "
              "void* (jclass handle)");

// -----------------------------------------------------------------------------
// jni::get_method_id(void* klass, const std::string& name, const std::string& sig) -> void*
// Both name and signature are const std::string& (NOT string_view); pin precisely.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_method_id),
                                    void*, const std::string&, const std::string&>,
              "jni::get_method_id(void* klass, const std::string& name, "
              "const std::string& signature) must return void* (jmethodID)");

// -----------------------------------------------------------------------------
// jni::get_static_method_id / get_static_field_id (const std::string& args) -> void*
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_static_method_id),
                                    void*, const std::string&, const std::string&>,
              "jni::get_static_method_id(void* klass, const std::string& name, "
              "const std::string& signature) must return void* (jmethodID)");
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_static_field_id),
                                    void*, const std::string&, const std::string&>,
              "jni::get_static_field_id(void* klass, const std::string& name, "
              "const std::string& signature) must return void* (jfieldID)");

// -----------------------------------------------------------------------------
// jni::get_static_object_field(void* klass, void* field_id) -> void*
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_static_object_field),
                                    void*, void*>,
              "jni::get_static_object_field(void* klass, void* field_id) must return "
              "void* (jobject handle)");

// -----------------------------------------------------------------------------
// jni::call_object_method / call_static_object_method
//
// Third parameter is a const jni::value* defaulting to nullptr.  is_invocable on
// a function POINTER cannot see default arguments (they belong to the
// declaration, not the pointer type), so we pin the 3-arg signature with
// is_invocable_r_v AND separately confirm the default makes the args pointer
// optional by naming a real 2-arg call inside decltype (unevaluated call
// expressions DO honour default arguments).
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::call_object_method),
                                    void*, void*, const vmhook::jni::value*>,
              "jni::call_object_method(void* object, void* method_id, "
              "const jni::value* args) must return void* (jobject handle)");
static_assert(std::is_same_v<decltype(vmhook::jni::call_object_method(
                                 std::declval<void*>(), std::declval<void*>())),
                             void*>,
              "jni::call_object_method's args pointer must be defaulted (callable with 2 args)");
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::call_static_object_method),
                                    void*, void*, const vmhook::jni::value*>,
              "jni::call_static_object_method(void* klass, void* method_id, "
              "const jni::value* args) must return void* (jobject handle)");
static_assert(std::is_same_v<decltype(vmhook::jni::call_static_object_method(
                                 std::declval<void*>(), std::declval<void*>())),
                             void*>,
              "jni::call_static_object_method's args pointer must be defaulted (callable with 2 args)");

// -----------------------------------------------------------------------------
// jni::klass_from_class_mirror(void* class_handle) -> vmhook::hotspot::klass*
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                    decltype(&vmhook::jni::klass_from_class_mirror), void*>,
              "jni::klass_from_class_mirror(void* class_handle) must return "
              "vmhook::hotspot::klass*");

// -----------------------------------------------------------------------------
// jni::new_string_utf(std::string_view) -> void*
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::new_string_utf), std::string_view>,
              "jni::new_string_utf(string_view value) must accept a string_view and return "
              "void* (jstring handle)");

// -----------------------------------------------------------------------------
// jni::get_string_utf(void* string_handle) -> std::string
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::jni::get_string_utf), void*>,
              "jni::get_string_utf(void* string_handle) must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::get_string_utf(std::declval<void*>())), std::string>,
              "jni::get_string_utf must return std::string exactly");

// -----------------------------------------------------------------------------
// jni::signature_for_arg<T>() -> std::string   [TEMPLATE]
//
// Compile-time JVM-descriptor table.  Instantiate over EVERY type category the
// table recognises (the runtime half below pins the exact string each returns)
// and pin that each returns std::string.
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<int>()), std::string>,
              "jni::signature_for_arg<int>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<bool>()), std::string>,
              "jni::signature_for_arg<bool>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<double>()), std::string>,
              "jni::signature_for_arg<double>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<float>()), std::string>,
              "jni::signature_for_arg<float>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::string>()), std::string>,
              "jni::signature_for_arg<std::string>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::string_view>()), std::string>,
              "jni::signature_for_arg<std::string_view>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<const char*>()), std::string>,
              "jni::signature_for_arg<const char*>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::int8_t>()), std::string>,
              "jni::signature_for_arg<int8_t>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::uint8_t>()), std::string>,
              "jni::signature_for_arg<uint8_t>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::int16_t>()), std::string>,
              "jni::signature_for_arg<int16_t>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::uint16_t>()), std::string>,
              "jni::signature_for_arg<uint16_t>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::int64_t>()), std::string>,
              "jni::signature_for_arg<int64_t>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::uint64_t>()), std::string>,
              "jni::signature_for_arg<uint64_t>() must return std::string");

// -----------------------------------------------------------------------------
// jni::make_unique<wrapper_type, args_t...>(const std::string& class_name, args_t&&...)
//     -> std::unique_ptr<wrapper_type>   [TEMPLATE]
//
// Constructs a Java object via JNI NewObjectA and returns the wrapper.
// Instantiate with the throwaway wrapper for the zero-extra-arg form and a
// spread of forwarded-arg shapes (every descriptor-table category), pinning the
// std::unique_ptr<wrapper_type> result in each case.
// -----------------------------------------------------------------------------
static_assert(
    std::is_same_v<decltype(vmhook::jni::make_unique<test_wrapper>(std::declval<const std::string&>())),
                   std::unique_ptr<test_wrapper>>,
    "jni::make_unique<T>(const std::string& class_name) must return std::unique_ptr<T>");
static_assert(
    std::is_same_v<decltype(vmhook::jni::make_unique<test_wrapper>(
                       std::declval<const std::string&>(), 1, 2.0)),
                   std::unique_ptr<test_wrapper>>,
    "jni::make_unique<T>(const std::string&, args...) must forward ctor args and return "
    "std::unique_ptr<T>");
static_assert(
    std::is_same_v<decltype(vmhook::jni::make_unique<test_wrapper>(
                       std::declval<const std::string&>(),
                       true, std::int8_t{ 1 }, std::int16_t{ 2 }, std::uint16_t{ 3 },
                       std::int64_t{ 4 }, 5.0f, 6.0, std::string_view{ "x" })),
                   std::unique_ptr<test_wrapper>>,
    "jni::make_unique<T>(...) must forward the full descriptor-type spread and return "
    "std::unique_ptr<T>");

// -----------------------------------------------------------------------------
// PRE-EXISTING GAP GUARD: jni::new_global_ref / jni::delete_global_ref
//
// The audit listed these among the jni:: forwarders, and they DID go missing
// once.  As of this writing they do NOT exist as jni:: forwarders — only the
// internal vmhook::detail::jni_new_global_ref / jni_delete_global_ref do, used
// directly by vmhook::jni::global_ref.  We therefore CANNOT static_assert
// jni::new_global_ref / jni::delete_global_ref without referencing a phantom
// (which would fail to compile for the wrong reason).  Instead we lock down the
// underlying detail functions they would forward to, so the capability itself
// can never silently disappear.  If a public jni::new_global_ref / delete pair
// is ever (re)introduced, add the corresponding &vmhook::jni::* asserts here.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::detail::jni_new_global_ref), void*>,
              "detail::jni_new_global_ref(void* handle) must return void* (global handle) — "
              "the primitive behind any jni::new_global_ref forwarder / global_ref ctor");
static_assert(std::is_invocable_r_v<void, decltype(&vmhook::detail::jni_delete_global_ref), void*>,
              "detail::jni_delete_global_ref(void* handle) must return void — "
              "the primitive behind any jni::delete_global_ref forwarder / global_ref dtor");

// =============================================================================
// PART 2 — RUNTIME NO-JVM CONTRACT (every input)
// =============================================================================

namespace
{
    int failures{ 0 };

    auto check(const char* const name, const bool ok) -> void
    {
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
        if (!ok)
        {
            ++failures;
        }
    }

    // ----- The input matrix shared by the pointer-gated forwarders -----------
    //
    // is_valid_pointer() is the gate inside jni_decode_object()/jni_oop_handle()
    // consumers; the set below spans every branch of its decision tree so the
    // forwarders that funnel through it are exercised over their whole domain.
    // Values are numeric uintptr_t patterns (endianness-agnostic) reconstituted
    // into void* only at the call site.

    // The nine debug-fill / sentinel low-32-bit patterns is_valid_pointer
    // rejects outright (taken verbatim from the live switch).
    constexpr std::uint32_t sentinel_low32[]{
        0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
        0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
    };

    // A handful of addresses that ARE valid per is_valid_pointer: strictly
    // between floor and ceiling, even, and whose low-32 bits match no sentinel.
    constexpr std::uintptr_t valid_addrs[]{
        std::uintptr_t{ 0x10000u },        // just above the 0xFFFF floor
        std::uintptr_t{ 0x10010u },
        std::uintptr_t{ 0x00400000u },     // a typical image base
        std::uintptr_t{ 0x12340000u },
        std::uintptr_t{ 0x7FFFFFFEu },     // large but still even / non-sentinel
    };

    auto as_ptr(const std::uintptr_t v) noexcept -> void*
    {
        return reinterpret_cast<void*>(v);
    }

    // -------------------------------------------------------------------------
    // is_valid_pointer — the gate every pointer-decoding forwarder relies on.
    // Pinned value-by-value across each branch of its decision tree.
    // -------------------------------------------------------------------------
    auto run_is_valid_pointer_matrix() -> void
    {
        namespace hs = vmhook::hotspot;

        // nullptr and everything at/below the floor are rejected.
        check("is_valid_pointer(nullptr)==false", !hs::is_valid_pointer(nullptr));
        check("is_valid_pointer(floor)==false",
              !hs::is_valid_pointer(as_ptr(vmhook::os::user_address_floor)));
        check("is_valid_pointer(floor-1)==false",
              !hs::is_valid_pointer(as_ptr(vmhook::os::user_address_floor - 1u)));
        check("is_valid_pointer(1)==false", !hs::is_valid_pointer(as_ptr(std::uintptr_t{ 1 })));

        // floor+1 is 0x10000 (even, non-sentinel) -> valid; floor+2 (0x10001) is
        // odd -> rejected by the alignment guard.  Pins floor exclusivity AND the
        // odd-address rule in one neighbourhood.
        check("is_valid_pointer(floor+1)==true",
              hs::is_valid_pointer(as_ptr(vmhook::os::user_address_floor + 1u)));
        check("is_valid_pointer(floor+2 odd)==false",
              !hs::is_valid_pointer(as_ptr(vmhook::os::user_address_floor + 2u)));

        // Every odd valid-base+1 is rejected (alignment guard, exhaustively).
        for (const std::uintptr_t base : valid_addrs)
        {
            check("is_valid_pointer(odd)==false", !hs::is_valid_pointer(as_ptr(base | 1u)));
        }

        // Every sentinel low-32 pattern is rejected (the EXACT pattern: the
        // odd-valued sentinels are also caught by the alignment guard, the
        // even-valued ones by the dedicated debug-fill switch; either way the
        // contract is "this bit pattern is not a valid pointer").  Pass them
        // verbatim — masking the low bit to force evenness would DESTROY the
        // pattern and (correctly) make e.g. 0xBAADF00D->0xBAADF00C a valid
        // address, which is a property of the masked value, not of the sentinel.
        for (const std::uint32_t low : sentinel_low32)
        {
            check("is_valid_pointer(sentinel)==false",
                  !hs::is_valid_pointer(as_ptr(static_cast<std::uintptr_t>(low))));
        }
        // The three EVEN debug-fill sentinels isolate the dedicated switch arm
        // (they are not rejected by the odd-address guard), proving that branch
        // specifically — not just the alignment rule — fires.
        check("is_valid_pointer(0xCAFEBABE even sentinel)==false",
              !hs::is_valid_pointer(as_ptr(std::uintptr_t{ 0xCAFEBABEu })));
        check("is_valid_pointer(0xCCCCCCCC even sentinel)==false",
              !hs::is_valid_pointer(as_ptr(std::uintptr_t{ 0xCCCCCCCCu })));
        check("is_valid_pointer(0xFEEEFEEE even sentinel)==false",
              !hs::is_valid_pointer(as_ptr(std::uintptr_t{ 0xFEEEFEEEu })));

        // Every hand-picked valid address is accepted.
        for (const std::uintptr_t addr : valid_addrs)
        {
            check("is_valid_pointer(valid)==true", hs::is_valid_pointer(as_ptr(addr)));
        }

        // The ceiling is exclusive; a fact that only holds where uintptr_t is
        // wide enough to represent it (64-bit).  On a 32-bit build
        // user_address_ceiling truncates to 0xFFFFFFFF, so guard with sizeof.
        if constexpr (sizeof(void*) >= 8)
        {
            check("is_valid_pointer(ceiling)==false",
                  !hs::is_valid_pointer(as_ptr(vmhook::os::user_address_ceiling)));
            check("is_valid_pointer(ceiling+1)==false",
                  !hs::is_valid_pointer(as_ptr(vmhook::os::user_address_ceiling + 1u)));
            // ceiling-1 is 0x7FFFFFFFFFFFFFFE (even, non-sentinel) -> valid.
            check("is_valid_pointer(ceiling-1)==true",
                  hs::is_valid_pointer(as_ptr(vmhook::os::user_address_ceiling - 1u)));
        }
    }

    // -------------------------------------------------------------------------
    // jni::oop_handle(oop, storage) — PURE: storage<-oop, returns &storage.
    // Exhaustive over null / sentinels / valid OOP bit patterns.  No JVM needed.
    // -------------------------------------------------------------------------
    auto run_oop_handle_matrix() -> void
    {
        // The exact value handed in (whatever its bit pattern) must land in
        // storage verbatim, and the returned handle must point AT that storage.
        const auto one = [](void* const oop) -> bool
        {
            void* storage{ reinterpret_cast<void*>(std::uintptr_t{ 0xA5A5A5A5u }) };
            void* const handle{ vmhook::jni::oop_handle(oop, storage) };
            return handle == &storage && storage == oop
                   && *static_cast<void* const*>(handle) == oop;
        };

        check("oop_handle(nullptr): storage==null, handle==&storage", one(nullptr));

        for (const std::uintptr_t addr : valid_addrs)
        {
            check("oop_handle(valid oop): roundtrip", one(as_ptr(addr)));
        }
        for (const std::uint32_t low : sentinel_low32)
        {
            // oop_handle does NO validity filtering — it stores whatever it is
            // given, sentinel pattern or not.  Pin that it is a dumb conduit.
            check("oop_handle(sentinel oop): stored verbatim (no filtering)",
                  one(as_ptr(static_cast<std::uintptr_t>(low))));
        }

        // Two calls with two different storages yield two different handles, and
        // re-using one storage overwrites it (the handle address is stable).
        void* storage_a{};
        void* storage_b{};
        void* const handle_a{ vmhook::jni::oop_handle(as_ptr(valid_addrs[0]), storage_a) };
        void* const handle_b{ vmhook::jni::oop_handle(as_ptr(valid_addrs[1]), storage_b) };
        check("oop_handle: distinct storages -> distinct handles", handle_a != handle_b);
        void* const handle_a2{ vmhook::jni::oop_handle(as_ptr(valid_addrs[2]), storage_a) };
        check("oop_handle: same storage -> stable handle address", handle_a == handle_a2);
        check("oop_handle: same storage -> value overwritten", storage_a == as_ptr(valid_addrs[2]));
    }

    // -------------------------------------------------------------------------
    // jni::decode_object(handle) — null handle -> null; otherwise *handle gated
    // through is_valid_pointer.  Buildable WITHOUT a JVM by making the handle a
    // pointer to a local that holds the candidate OOP (exactly what a real JNI
    // local-ref slot is: a pointer-to-pointer-to-oop).
    // -------------------------------------------------------------------------
    auto run_decode_object_matrix() -> void
    {
        namespace hs = vmhook::hotspot;

        // Null handle is the documented sentinel -> null, no deref.
        check("decode_object(nullptr)==nullptr", vmhook::jni::decode_object(nullptr) == nullptr);

        // For a non-null handle, the result is the slot value IFF it passes
        // is_valid_pointer, else nullptr.  Drive every category of slot value.
        const auto decode_slot = [](void* const slot_value) -> void*
        {
            void* slot{ slot_value };           // the "oop" the handle points at
            return vmhook::jni::decode_object(&slot);
        };

        for (const std::uintptr_t addr : valid_addrs)
        {
            void* const oop{ as_ptr(addr) };
            check("decode_object(handle->valid oop)==oop", decode_slot(oop) == oop);
        }
        for (const std::uint32_t low : sentinel_low32)
        {
            // Slot holds a debug-fill sentinel pattern verbatim -> is_valid_pointer
            // rejects it (odd-guard or switch) -> decode yields null.
            void* const oop{ as_ptr(static_cast<std::uintptr_t>(low)) };
            check("decode_object(handle->sentinel)==null", decode_slot(oop) == nullptr);
        }
        // Slot holding null, an odd address, the floor, and (64-bit) the ceiling
        // all decode to null.
        check("decode_object(handle->null)==null", decode_slot(nullptr) == nullptr);
        check("decode_object(handle->odd)==null",
              decode_slot(as_ptr(valid_addrs[0] | 1u)) == nullptr);
        check("decode_object(handle->floor)==null",
              decode_slot(as_ptr(vmhook::os::user_address_floor)) == nullptr);

        // Cross-check the decode against the gate directly for every valid addr:
        // decode result must equal (is_valid_pointer(oop) ? oop : nullptr).
        for (const std::uintptr_t addr : valid_addrs)
        {
            void* const oop{ as_ptr(addr) };
            void* const expected{ hs::is_valid_pointer(oop) ? oop : nullptr };
            check("decode_object matches is_valid_pointer gate", decode_slot(oop) == expected);
        }
    }

    // -------------------------------------------------------------------------
    // jni::function<index, fn_t>(env) — PURE pointer arithmetic over the env's
    // function table.  Fully exercisable WITHOUT a JVM by handing it a fake env:
    // a JNIEnv is `table** -> table* -> fn_ptr[]`, so we build a real local
    // function-pointer array, point a "table pointer" at it, and an "env" at the
    // table pointer.  Pin: null env -> null; null table -> null; valid env ->
    // table[index] for several indices and pointer shapes.
    // -------------------------------------------------------------------------
    auto run_function_matrix() -> void
    {
        // null env -> null (every index / shape).
        check("function<6>(nullptr)==null",
              vmhook::jni::function<6, sample_jni_fn_t>(nullptr) == nullptr);
        check("function<0>(nullptr)==null",
              vmhook::jni::function<0, sample_jni_fn_t>(nullptr) == nullptr);
        check("function<228>(nullptr)==null",
              vmhook::jni::function<228, sample_jni_fn2_t>(nullptr) == nullptr);

        // env whose table pointer is null -> null.
        void* null_table{ nullptr };
        void* env_with_null_table{ &null_table };
        check("function<6>(env{table=null})==null",
              vmhook::jni::function<6, sample_jni_fn_t>(env_with_null_table) == nullptr);
        check("function<30>(env{table=null})==null",
              vmhook::jni::function<30, sample_jni_fn_t>(env_with_null_table) == nullptr);

        // A fully-formed fake env: an array of distinct function-pointer-sized
        // slots, a table pointer at slot 0, and an env at the table pointer.
        // function<index> must return EXACTLY the slot at `index`, reinterpreted
        // to the requested function-pointer type.  We compare the returned fn_t
        // directly against reinterpret_cast<fn_t>(table_slots[idx]) — the SAME
        // void*->fn_t cast the implementation performs — so no function-pointer
        // is ever cast back to an object pointer (which is only conditionally
        // supported and would draw a -Wpedantic diagnostic under clang).
        constexpr std::size_t table_len{ 256 };
        static void* table_slots[table_len]{};
        for (std::size_t i{ 0 }; i < table_len; ++i)
        {
            // Distinct, even, non-sentinel, in-range marker per slot so equality
            // is meaningful and unambiguous.
            table_slots[i] = as_ptr(std::uintptr_t{ 0x100000u } + (i * 0x10u));
        }
        void* table_ptr{ &table_slots[0] };
        void* fake_env{ &table_ptr };

        // Cover the real JNI slot indices every forwarder in this header uses
        // (FindClass=6, ExceptionClear=17, PushLocalFrame=19, NewGlobalRef=21,
        // DeleteGlobalRef=22, DeleteLocalRef=23, NewObjectA=30, GetObjectClass=31,
        // GetMethodID=33, CallObjectMethodA=36, GetStaticMethodID=113,
        // CallStaticObjectMethodA=116, GetStaticFieldID=144, GetStaticObjectField=145,
        // ExceptionCheck=228) plus the table extremes (0 and 255), with BOTH
        // pointer shapes the template is instantiated with in the header.
        // table_slots is a static local (no capture needed); fake_env is used at
        // the call sites, not inside the comparator — so capture nothing and dodge
        // clang's -Wunused-lambda-capture entirely.
        const auto slot_ok_a = [](const sample_jni_fn_t fn, const std::size_t idx) -> bool
        {
            return fn == reinterpret_cast<sample_jni_fn_t>(table_slots[idx]);
        };
        check("function<0>(env)==table[0]",     slot_ok_a(vmhook::jni::function<0,   sample_jni_fn_t>(fake_env), 0u));
        check("function<6>(env)==table[6]",     slot_ok_a(vmhook::jni::function<6,   sample_jni_fn_t>(fake_env), 6u));
        check("function<17>(env)==table[17]",   slot_ok_a(vmhook::jni::function<17,  sample_jni_fn_t>(fake_env), 17u));
        check("function<21>(env)==table[21]",   slot_ok_a(vmhook::jni::function<21,  sample_jni_fn_t>(fake_env), 21u));
        check("function<22>(env)==table[22]",   slot_ok_a(vmhook::jni::function<22,  sample_jni_fn_t>(fake_env), 22u));
        check("function<23>(env)==table[23]",   slot_ok_a(vmhook::jni::function<23,  sample_jni_fn_t>(fake_env), 23u));
        check("function<30>(env)==table[30]",   slot_ok_a(vmhook::jni::function<30,  sample_jni_fn_t>(fake_env), 30u));
        check("function<31>(env)==table[31]",   slot_ok_a(vmhook::jni::function<31,  sample_jni_fn_t>(fake_env), 31u));
        check("function<33>(env)==table[33]",   slot_ok_a(vmhook::jni::function<33,  sample_jni_fn_t>(fake_env), 33u));
        check("function<36>(env)==table[36]",   slot_ok_a(vmhook::jni::function<36,  sample_jni_fn_t>(fake_env), 36u));
        check("function<113>(env)==table[113]", slot_ok_a(vmhook::jni::function<113, sample_jni_fn_t>(fake_env), 113u));
        check("function<116>(env)==table[116]", slot_ok_a(vmhook::jni::function<116, sample_jni_fn_t>(fake_env), 116u));
        check("function<144>(env)==table[144]", slot_ok_a(vmhook::jni::function<144, sample_jni_fn_t>(fake_env), 144u));
        check("function<145>(env)==table[145]", slot_ok_a(vmhook::jni::function<145, sample_jni_fn_t>(fake_env), 145u));
        check("function<255>(env)==table[255]", slot_ok_a(vmhook::jni::function<255, sample_jni_fn_t>(fake_env), 255u));

        // The OTHER pointer shape (int(*)(void*,int)) at the indices the header
        // uses it for (PushLocalFrame=19, PopLocalFrame=20, ExceptionCheck=228).
        const auto slot_ok_b = [](const sample_jni_fn2_t fn, const std::size_t idx) -> bool
        {
            return fn == reinterpret_cast<sample_jni_fn2_t>(table_slots[idx]);
        };
        check("function<19,fn2>(env)==table[19]",   slot_ok_b(vmhook::jni::function<19,  sample_jni_fn2_t>(fake_env), 19u));
        check("function<20,fn2>(env)==table[20]",   slot_ok_b(vmhook::jni::function<20,  sample_jni_fn2_t>(fake_env), 20u));
        check("function<228,fn2>(env)==table[228]", slot_ok_b(vmhook::jni::function<228, sample_jni_fn2_t>(fake_env), 228u));
    }

    // -------------------------------------------------------------------------
    // signature_for_arg<T>() — value-by-value over EVERY descriptor category.
    // Pure compile-time dispatch returning a runtime std::string; no JVM.
    // Also pins (a) the public forwarder equals the detail primitive, and
    // (b) cv/ref-qualified spellings decay to the same descriptor.
    // -------------------------------------------------------------------------
    template<typename T>
    auto sig() -> std::string
    {
        return vmhook::jni::signature_for_arg<T>();
    }

    auto run_signature_for_arg_matrix() -> void
    {
        // Reference / string-like types -> "Ljava/lang/String;".
        check("sig<std::string>==Ljava/lang/String;", sig<std::string>() == "Ljava/lang/String;");
        check("sig<std::string_view>==Ljava/lang/String;", sig<std::string_view>() == "Ljava/lang/String;");
        check("sig<const char*>==Ljava/lang/String;", sig<const char*>() == "Ljava/lang/String;");
        check("sig<char*>==Ljava/lang/String;", sig<char*>() == "Ljava/lang/String;");

        // Primitive descriptor letters.
        check("sig<bool>==Z", sig<bool>() == "Z");
        check("sig<int8_t>==B", sig<std::int8_t>() == "B");
        check("sig<uint8_t>==B", sig<std::uint8_t>() == "B");
        check("sig<int16_t>==S", sig<std::int16_t>() == "S");
        check("sig<uint16_t>==C", sig<std::uint16_t>() == "C");
        check("sig<int32_t>==I", sig<std::int32_t>() == "I");
        check("sig<uint32_t>==I", sig<std::uint32_t>() == "I");
        check("sig<int>==I", sig<int>() == "I");
        check("sig<unsigned>==I", sig<unsigned>() == "I");
        check("sig<int64_t>==J", sig<std::int64_t>() == "J");
        check("sig<uint64_t>==J", sig<std::uint64_t>() == "J");
        check("sig<float>==F", sig<float>() == "F");
        check("sig<double>==D", sig<double>() == "D");

        // An object_base-derived wrapper that is NOT registered in this no-JVM
        // process falls back to "Ljava/lang/Object;" (the documented non-fatal
        // path — the silently-wrong "I" fallback was removed).
        check("sig<test_wrapper> (unregistered)==Ljava/lang/Object;",
              sig<test_wrapper>() == "Ljava/lang/Object;");
        // unique_ptr<wrapper> (unregistered) likewise -> "Ljava/lang/Object;".
        check("sig<unique_ptr<test_wrapper>> (unregistered)==Ljava/lang/Object;",
              sig<std::unique_ptr<test_wrapper>>() == "Ljava/lang/Object;");

        // Forwarder fidelity: the public jni:: spelling returns the identical
        // string the detail:: primitive produces, for a representative spread.
        check("forward: jni::==detail:: for int",
              vmhook::jni::signature_for_arg<int>() == vmhook::detail::jni_signature_for_arg<int>());
        check("forward: jni::==detail:: for std::string",
              vmhook::jni::signature_for_arg<std::string>() == vmhook::detail::jni_signature_for_arg<std::string>());
        check("forward: jni::==detail:: for int64_t",
              vmhook::jni::signature_for_arg<std::int64_t>() == vmhook::detail::jni_signature_for_arg<std::int64_t>());
        check("forward: jni::==detail:: for double",
              vmhook::jni::signature_for_arg<double>() == vmhook::detail::jni_signature_for_arg<double>());
        check("forward: jni::==detail:: for bool",
              vmhook::jni::signature_for_arg<bool>() == vmhook::detail::jni_signature_for_arg<bool>());

        // std::decay_t invariance: cv- / ref- / array-qualified spellings must
        // map to the SAME descriptor as the bare type (the table decays first).
        check("sig decays: const int == int", sig<const int>() == sig<int>());
        check("sig decays: int& == int", sig<int&>() == sig<int>());
        check("sig decays: const int& == int", sig<const int&>() == sig<int>());
        check("sig decays: int&& == int", sig<int&&>() == sig<int>());
        check("sig decays: volatile double == double", sig<volatile double>() == sig<double>());
        check("sig decays: const std::string& == std::string", sig<const std::string&>() == sig<std::string>());
        check("sig decays: const char[4] -> string",
              sig<const char[4]>() == "Ljava/lang/String;"); // decays to const char*
    }

    // -------------------------------------------------------------------------
    // The JVM-DEPENDENT forwarders: with no jvm.dll loaded, current_jni_env is
    // null and ensure_current_java_thread() fails, so each must return its
    // documented safe sentinel (nullptr / empty string) WITHOUT faulting.  We
    // pass a spread of inputs (null and crafted non-null handles) to show the
    // sentinel is unconditional in the no-JVM state, not just on a null arg.
    // -------------------------------------------------------------------------
    auto run_no_jvm_sentinel_matrix() -> void
    {
        const std::string name{ "doStuff" };
        const std::string sigv{ "()V" };
        const std::string field_name{ "value" };
        const std::string field_sig{ "I" };

        // A few non-null but synthetic handles to prove the sentinel does not
        // depend on the argument being null.
        void* const fake_handle{ as_ptr(valid_addrs[0]) };
        void* const fake_handle2{ as_ptr(valid_addrs[1]) };

        // find_class: null jclass for any name (empty, valid, junk).
        check("find_class('')==null", vmhook::jni::find_class("") == nullptr);
        check("find_class('java/lang/String')==null",
              vmhook::jni::find_class("java/lang/String") == nullptr);
        check("find_class('does/not/Exist')==null",
              vmhook::jni::find_class("does/not/Exist") == nullptr);

        // find_class_with_context_loader: null klass* for any name.
        check("find_class_with_context_loader('')==null",
              vmhook::jni::find_class_with_context_loader("") == nullptr);
        check("find_class_with_context_loader('java/lang/Object')==null",
              vmhook::jni::find_class_with_context_loader("java/lang/Object") == nullptr);

        // get_object_class: null for null and non-null handles.
        check("get_object_class(nullptr)==null", vmhook::jni::get_object_class(nullptr) == nullptr);
        check("get_object_class(fake)==null", vmhook::jni::get_object_class(fake_handle) == nullptr);

        // get_method_id / get_static_method_id / get_static_field_id: null id.
        check("get_method_id(null,...)==null",
              vmhook::jni::get_method_id(nullptr, name, sigv) == nullptr);
        check("get_method_id(fake,...)==null",
              vmhook::jni::get_method_id(fake_handle, name, sigv) == nullptr);
        check("get_static_method_id(null,...)==null",
              vmhook::jni::get_static_method_id(nullptr, name, sigv) == nullptr);
        check("get_static_method_id(fake,...)==null",
              vmhook::jni::get_static_method_id(fake_handle, name, sigv) == nullptr);
        check("get_static_field_id(null,...)==null",
              vmhook::jni::get_static_field_id(nullptr, field_name, field_sig) == nullptr);
        check("get_static_field_id(fake,...)==null",
              vmhook::jni::get_static_field_id(fake_handle, field_name, field_sig) == nullptr);

        // get_static_object_field: null for any (klass, field_id) combination.
        check("get_static_object_field(null,null)==null",
              vmhook::jni::get_static_object_field(nullptr, nullptr) == nullptr);
        check("get_static_object_field(fake,fake)==null",
              vmhook::jni::get_static_object_field(fake_handle, fake_handle2) == nullptr);

        // call_object_method / call_static_object_method: null result, both with
        // an explicit nullptr args and with the default-argument 2-arg form, and
        // with a real (stack) jni::value argument array.
        vmhook::jni::value one_arg[1]{};
        one_arg[0].i = 42;
        check("call_object_method(null,null)==null (default args)",
              vmhook::jni::call_object_method(nullptr, nullptr) == nullptr);
        check("call_object_method(fake,fake,nullptr)==null",
              vmhook::jni::call_object_method(fake_handle, fake_handle2, nullptr) == nullptr);
        check("call_object_method(fake,fake,args)==null",
              vmhook::jni::call_object_method(fake_handle, fake_handle2, one_arg) == nullptr);
        check("call_static_object_method(null,null)==null (default args)",
              vmhook::jni::call_static_object_method(nullptr, nullptr) == nullptr);
        check("call_static_object_method(fake,fake,args)==null",
              vmhook::jni::call_static_object_method(fake_handle, fake_handle2, one_arg) == nullptr);

        // klass_from_class_mirror: null for null and non-null handles (the
        // jni_exception_pending() fail-safe returns true with no env, so this
        // bails before ever dereferencing the handle — no fault).
        check("klass_from_class_mirror(nullptr)==null",
              vmhook::jni::klass_from_class_mirror(nullptr) == nullptr);
        check("klass_from_class_mirror(fake)==null",
              vmhook::jni::klass_from_class_mirror(fake_handle) == nullptr);

        // new_string_utf: null jstring for any input string.
        check("new_string_utf('')==null", vmhook::jni::new_string_utf("") == nullptr);
        check("new_string_utf('hello')==null", vmhook::jni::new_string_utf("hello") == nullptr);
        check("new_string_utf(unicode)==null", vmhook::jni::new_string_utf("\xC3\xA9\xE2\x82\xAC") == nullptr);

        // get_string_utf: empty std::string for null and non-null handles.
        check("get_string_utf(nullptr).empty()", vmhook::jni::get_string_utf(nullptr).empty());
        check("get_string_utf(fake).empty()", vmhook::jni::get_string_utf(fake_handle).empty());

        // exception_clear: pure no-op with no env — must simply return (call it
        // several times to show idempotency / no state corruption).
        vmhook::jni::exception_clear();
        vmhook::jni::exception_clear();
        check("exception_clear() is a no-op with no JVM (did not fault)", true);

        // make_unique<T>: null unique_ptr for the zero-arg and forwarded-arg
        // forms (type unregistered AND no JVM — both gate to nullptr cleanly).
        check("make_unique<T>(name) -> null (no JVM)",
              vmhook::jni::make_unique<test_wrapper>(std::string{ "com/example/Foo" }) == nullptr);
        check("make_unique<T>(name, args...) -> null (no JVM)",
              vmhook::jni::make_unique<test_wrapper>(std::string{ "com/example/Foo" }, 1, 2.0, true) == nullptr);
    }

    // -------------------------------------------------------------------------
    // detail primitives behind the global-ref forwarders: null is a no-op, a
    // non-null handle with no env returns the sentinel and does NOT fault.
    // -------------------------------------------------------------------------
    auto run_global_ref_primitives_matrix() -> void
    {
        void* const fake_handle{ as_ptr(valid_addrs[0]) };

        // new_global_ref: null in -> null out; non-null with no env -> null out.
        check("jni_new_global_ref(nullptr)==null",
              vmhook::detail::jni_new_global_ref(nullptr) == nullptr);
        check("jni_new_global_ref(fake)==null (no JVM)",
              vmhook::detail::jni_new_global_ref(fake_handle) == nullptr);

        // delete_global_ref / delete_local_ref: void no-op for null and non-null
        // (must not fault with no env).
        vmhook::detail::jni_delete_global_ref(nullptr);
        vmhook::detail::jni_delete_global_ref(fake_handle);
        vmhook::detail::jni_delete_local_ref(nullptr);
        vmhook::detail::jni_delete_local_ref(fake_handle);
        check("jni_delete_global_ref / jni_delete_local_ref no-op with no JVM (did not fault)", true);
    }

    // Performs dst = std::move(src) behind a function boundary.  Passing the
    // SAME object as both arguments exercises global_ref's self-assignment guard
    // (this != &other) without any compiler being able to see a syntactic
    // self-move at the call site — so clang's -Wself-move cannot fire under
    // -Werror.  noinline-ish by virtue of being a separate function.
    auto move_assign(vmhook::jni::global_ref& dst, vmhook::jni::global_ref& src) -> void
    {
        dst = std::move(src);
    }

    // -------------------------------------------------------------------------
    // vmhook::jni::global_ref + pin() — the highest-level forwarder consumer.
    // No-JVM contract: every construction yields an EMPTY ref (handle null,
    // operator bool false, oop()/handle() null), move transfers ownership, and
    // reset() is idempotent.  All exercised over the full OOP input matrix.
    // -------------------------------------------------------------------------
    auto run_global_ref_matrix() -> void
    {
        // Default-constructed is empty.
        {
            vmhook::jni::global_ref g{};
            check("global_ref{}: !bool", !static_cast<bool>(g));
            check("global_ref{}: handle()==null", g.handle() == nullptr);
            check("global_ref{}: oop()==null", g.oop() == nullptr);
        }

        // Constructed from a null oop is empty (early-return path).
        {
            vmhook::jni::global_ref g{ static_cast<vmhook::oop_t>(nullptr) };
            check("global_ref{null oop}: !bool", !static_cast<bool>(g));
            check("global_ref{null oop}: handle()==null", g.handle() == nullptr);
        }

        // Constructed from EVERY valid OOP pattern: with no JVM, NewGlobalRef is
        // unavailable so the handle stays null -> empty.  No fault.
        for (const std::uintptr_t addr : valid_addrs)
        {
            vmhook::jni::global_ref g{ as_ptr(addr) };
            check("global_ref{valid oop}: empty with no JVM", !static_cast<bool>(g) && g.handle() == nullptr);
            check("global_ref{valid oop}: oop()==null", g.oop() == nullptr);
        }

        // Move construction from an empty ref stays empty; source stays empty.
        {
            vmhook::jni::global_ref src{ as_ptr(valid_addrs[0]) };
            vmhook::jni::global_ref dst{ std::move(src) };
            check("global_ref move-ctor: dst empty", !static_cast<bool>(dst));
            // NOLINTNEXTLINE(bugprone-use-after-move) — intentionally inspecting moved-from state.
            check("global_ref move-ctor: src empty (nulled)", !static_cast<bool>(src) && src.handle() == nullptr);
        }

        // Move assignment is safe and leaves both empty in the no-JVM state.
        {
            vmhook::jni::global_ref a{ as_ptr(valid_addrs[1]) };
            vmhook::jni::global_ref b{};
            b = std::move(a);
            check("global_ref move-assign: dst empty", !static_cast<bool>(b));
            // NOLINTNEXTLINE(bugprone-use-after-move)
            check("global_ref move-assign: src empty", !static_cast<bool>(a));
        }

        // Self-move-assignment is a safe no-op (guarded by this!=&other).  Routed
        // through move_assign(dst, src) with the SAME object for both params so
        // the self-move is invisible to clang's syntactic -Wself-move check.
        {
            vmhook::jni::global_ref g{ as_ptr(valid_addrs[2]) };
            move_assign(g, g);
            check("global_ref self-move-assign: still valid (empty), did not fault",
                  !static_cast<bool>(g));
        }

        // reset() is idempotent and leaves the ref empty.
        {
            vmhook::jni::global_ref g{ as_ptr(valid_addrs[3]) };
            g.reset();
            g.reset();
            check("global_ref reset(): idempotent, empty", !static_cast<bool>(g) && g.handle() == nullptr);
        }

        // pin(oop): free-function form. Null -> empty; valid oop -> empty (no JVM).
        {
            vmhook::jni::global_ref p0{ vmhook::pin(static_cast<vmhook::oop_t>(nullptr)) };
            check("pin(null oop): empty", !static_cast<bool>(p0));
            vmhook::jni::global_ref p1{ vmhook::pin(as_ptr(valid_addrs[0])) };
            check("pin(valid oop): empty with no JVM", !static_cast<bool>(p1));
        }

        // pin(unique_ptr<wrapper>): null wrapper -> empty; the non-null-wrapper
        // path requires a constructed wrapper, which needs a JVM, so only the
        // null-wrapper branch is in scope here.
        {
            const std::unique_ptr<test_wrapper> null_wrapper{};
            vmhook::jni::global_ref p{ vmhook::pin(null_wrapper) };
            check("pin(null unique_ptr): empty", !static_cast<bool>(p));
        }
    }
}

int main()
{
    run_is_valid_pointer_matrix();
    run_oop_handle_matrix();
    run_decode_object_matrix();
    run_function_matrix();
    run_signature_for_arg_matrix();
    run_no_jvm_sentinel_matrix();
    run_global_ref_primitives_matrix();
    run_global_ref_matrix();

    if (failures == 0)
    {
        std::printf("vmhook jni forwarders: OK\n");
        return 0;
    }
    std::printf("vmhook jni forwarders: %d FAILED\n", failures);
    return 1;
}
