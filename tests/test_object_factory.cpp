// Runtime no-JVM contract checks for vmhook's Java-object *factory* entry
// points: vmhook::make_java_array, vmhook::make_java_object,
// vmhook::make_java_string, and the templated vmhook::make_unique<T>.
//
// These four allocate live Java heap objects when a HotSpot JVM is present, but
// the audit (audit/AUDIT_FINDINGS.md, logic-tests cluster) flagged that their
// *guard* / *no-JVM* contracts had ZERO pure-logic coverage even though they
// ARE no-JVM-testable: each early-returns its safe default (nullptr / null
// unique_ptr) without a live JVM and never dereferences uninitialised VM state.
//
// This file actually *runs* main() with no JVM behind it and asserts the exact
// guarded behaviour proven from the header source (vmhook/vmhook.hpp):
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
//       (for every input shape: empty / ascii / embedded-nul /
//        unicode / astral / over-the-4096-cap / long)
//   * make_unique<T>(args...)         [NOT noexcept, -> unique_ptr<T>]
//       - ensure_current_java_thread() fails with no JVM, so it
//         returns BEFORE registration / allocation              -> nullptr
//
// Anything that needs a live oop / TLAB / interpreter (the actual allocation,
// header stamping, UTF-16 encode into a backing array, constructor dispatch) is
// OUT OF SCOPE here and is covered by JVM integration in example.cpp.  We assert
// ONLY the safe guarded paths; no call below can reach a path that dereferences
// VM state, because every factory bails at its first guard with no JVM.
#include <vmhook/vmhook.hpp>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

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
        // Over the documented 4096-char cap: the cap logic lives AFTER the
        // find_class guard, so with no JVM a long input is still a clean
        // nullptr (and, importantly, no oversized allocation is attempted).
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

    return failures == 0 ? 0 : 1;
}
