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
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
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

    return failures == 0 ? 0 : 1;
}
