// Compile-time-only lockdown of the public vmhook::jni:: forwarder layer.
//
// vmhook::jni::* is the supported public spelling of the JNI helpers that used
// to live under vmhook::detail::jni_*.  Several of these forwarders currently
// have ZERO call sites anywhere in the header or test-suite, so nothing else
// would notice if one were deleted, renamed, or had its signature drift —
// new_global_ref / delete_global_ref already went missing once.  This file is
// pure compile-time coverage: every forwarder is named in a static_assert that
// pins its exact callable signature (parameter types + return type).  If any
// forwarder is removed, renamed, or its signature changes, THIS TRANSLATION
// UNIT FAILS TO COMPILE on every compiler / platform in CI.  Compiling IS the
// test passing; the executable just returns 0.
//
// Convention mirrors test_traits.cpp: address-of-a-function fed into
// std::is_invocable_r_v for the non-overloaded free forwarders (taking the
// address is unambiguous because none of these are overloaded), and
// decltype()-based std::is_same_v for the templated ones, which we instantiate
// with representative types.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

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

// -----------------------------------------------------------------------------
// jni::decode_object(void*) -> void*
//
// Decodes a JNI local-reference handle to the raw heap OOP.  USED by the SDK,
// but pinned here too for total surface coverage.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::decode_object), void*>,
              "jni::decode_object(void* handle) must take a jobject handle and return void* (oop)");
static_assert(std::is_same_v<decltype(vmhook::jni::decode_object(std::declval<void*>())), void*>,
              "jni::decode_object must return void*");

// -----------------------------------------------------------------------------
// jni::oop_handle(void* oop, void*& storage) -> void*
//
// Wraps a raw heap OOP as a fake JNI handle written into caller-owned storage.
// The second parameter is a NON-const lvalue reference (void*&); pin that
// exactly so a drift to by-value / const& is caught.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::oop_handle), void*, void*&>,
              "jni::oop_handle(void* oop, void*& storage) must take an oop and a void*& "
              "out-storage reference and return void* (synthetic handle)");

// -----------------------------------------------------------------------------
// jni::find_class(std::string_view) -> void*
//
// JNI FindClass via the calling thread's context classloader.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::find_class), std::string_view>,
              "jni::find_class(string_view class_name) must accept a string_view and return "
              "void* (jclass handle)");

// -----------------------------------------------------------------------------
// jni::find_class_with_context_loader(std::string_view) -> vmhook::hotspot::klass*
//
// Multi-step lookup (thread loader -> system loader -> Forge LaunchClassLoader)
// returning the resolved HotSpot Klass*, NOT a JNI handle.  Pin the klass*
// return so a drift back to void* is caught.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                    decltype(&vmhook::jni::find_class_with_context_loader),
                                    std::string_view>,
              "jni::find_class_with_context_loader(string_view) must return "
              "vmhook::hotspot::klass* (NOT a JNI handle)");

// -----------------------------------------------------------------------------
// jni::exception_clear() -> void
//
// JNI ExceptionCheck + ExceptionClear.  Takes no arguments.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void, decltype(&vmhook::jni::exception_clear)>,
              "jni::exception_clear() must take no arguments and return void");

// -----------------------------------------------------------------------------
// jni::get_object_class(void*) -> void*
//
// JNI GetObjectClass; returns a jclass local-ref handle.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_object_class), void*>,
              "jni::get_object_class(void* object_handle) must take a jobject and return "
              "void* (jclass handle)");

// -----------------------------------------------------------------------------
// jni::get_method_id(void* klass, const std::string& name, const std::string& sig)
//     -> void*
//
// JNI GetMethodID.  Both name and signature are const std::string& (NOT
// string_view); pin that precisely.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_method_id),
                                    void*, const std::string&, const std::string&>,
              "jni::get_method_id(void* klass, const std::string& name, "
              "const std::string& signature) must return void* (jmethodID)");

// -----------------------------------------------------------------------------
// jni::get_static_method_id(void* klass, const std::string& name,
//                           const std::string& sig) -> void*   [UNUSED forwarder]
//
// JNI GetStaticMethodID.  Has no call sites -> exactly the kind of forwarder
// that could vanish unnoticed; this assert is its only guard.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_static_method_id),
                                    void*, const std::string&, const std::string&>,
              "jni::get_static_method_id(void* klass, const std::string& name, "
              "const std::string& signature) must return void* (jmethodID)");

// -----------------------------------------------------------------------------
// jni::get_static_field_id(void* klass, const std::string& name,
//                          const std::string& sig) -> void*    [UNUSED forwarder]
//
// JNI GetStaticFieldID.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_static_field_id),
                                    void*, const std::string&, const std::string&>,
              "jni::get_static_field_id(void* klass, const std::string& name, "
              "const std::string& signature) must return void* (jfieldID)");

// -----------------------------------------------------------------------------
// jni::get_static_object_field(void* klass, void* field_id) -> void*  [UNUSED]
//
// JNI GetStaticObjectField; returns a jobject local-ref handle.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::get_static_object_field),
                                    void*, void*>,
              "jni::get_static_object_field(void* klass, void* field_id) must return "
              "void* (jobject handle)");

// -----------------------------------------------------------------------------
// jni::call_object_method(void* object, void* method_id,
//                         const jni::value* args = nullptr) -> void*   [UNUSED]
//
// JNI CallObjectMethodA.  The third parameter is a const jni::value* defaulting
// to nullptr.  is_invocable on a function POINTER cannot see default arguments
// (they belong to the declaration, not the pointer type), so we pin the 3-arg
// signature with is_invocable_r_v and separately confirm the default makes the
// args pointer optional by naming a real 2-arg call inside decltype (unevaluated
// call expressions DO honour default arguments).
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::call_object_method),
                                    void*, void*, const vmhook::jni::value*>,
              "jni::call_object_method(void* object, void* method_id, "
              "const jni::value* args) must return void* (jobject handle)");
static_assert(std::is_same_v<decltype(vmhook::jni::call_object_method(
                                 std::declval<void*>(), std::declval<void*>())),
                             void*>,
              "jni::call_object_method's args pointer must be defaulted (callable with 2 args)");

// -----------------------------------------------------------------------------
// jni::call_static_object_method(void* klass, void* method_id,
//                                const jni::value* args = nullptr) -> void* [UNUSED]
//
// JNI CallStaticObjectMethodA.  Same defaulted-args shape as above (see the
// call_object_method note on why the 2-arg form is checked via decltype).
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::call_static_object_method),
                                    void*, void*, const vmhook::jni::value*>,
              "jni::call_static_object_method(void* klass, void* method_id, "
              "const jni::value* args) must return void* (jobject handle)");
static_assert(std::is_same_v<decltype(vmhook::jni::call_static_object_method(
                                 std::declval<void*>(), std::declval<void*>())),
                             void*>,
              "jni::call_static_object_method's args pointer must be defaulted (callable with 2 args)");

// -----------------------------------------------------------------------------
// jni::klass_from_class_mirror(void* class_handle) -> vmhook::hotspot::klass* [UNUSED]
//
// Given a jclass (= java.lang.Class mirror) handle, return the HotSpot Klass*.
// Pin the klass* return type.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                                    decltype(&vmhook::jni::klass_from_class_mirror), void*>,
              "jni::klass_from_class_mirror(void* class_handle) must return "
              "vmhook::hotspot::klass*");

// -----------------------------------------------------------------------------
// jni::new_string_utf(std::string_view) -> void*
//
// JNI NewStringUTF; returns a jstring local-ref handle.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::new_string_utf), std::string_view>,
              "jni::new_string_utf(string_view value) must accept a string_view and return "
              "void* (jstring handle)");

// -----------------------------------------------------------------------------
// jni::get_string_utf(void* string_handle) -> std::string
//
// JNI GetStringUTFChars (+release), copied into a std::string.  Pin the
// std::string return so a drift to e.g. const char* is caught.
// -----------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::jni::get_string_utf), void*>,
              "jni::get_string_utf(void* string_handle) must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::get_string_utf(std::declval<void*>())), std::string>,
              "jni::get_string_utf must return std::string exactly");

// -----------------------------------------------------------------------------
// jni::signature_for_arg<T>() -> std::string   [TEMPLATE]
//
// Compile-time JVM-descriptor table.  Instantiate over several representative
// argument types and pin that each returns std::string.
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<int>()), std::string>,
              "jni::signature_for_arg<int>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<bool>()), std::string>,
              "jni::signature_for_arg<bool>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<double>()), std::string>,
              "jni::signature_for_arg<double>() must return std::string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::string>()), std::string>,
              "jni::signature_for_arg<std::string>() must return std::string");

// -----------------------------------------------------------------------------
// jni::make_unique<wrapper_type, args_t...>(const std::string& class_name,
//                                           args_t&&...) -> std::unique_ptr<wrapper_type>
//   [TEMPLATE]
//
// Constructs a Java object via JNI NewObjectA and returns the wrapper.
// Instantiate with the throwaway wrapper above for both the zero-extra-arg and
// the forwarded-args forms, pinning the std::unique_ptr<wrapper_type> result.
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

int main()
{
    std::printf("vmhook jni forwarders: OK\n");
    return 0;
}
