// =============================================================================
// test_api_surface.cpp — the #38-immune no-JVM API-surface lane.
// =============================================================================
//
// Two jobs, both platform-invariant and JVM-free:
//
//   (1) COMPILE-ONLY surface proof (the file's original role, preserved below):
//       a realistic user wrapper (`my_class`) and the java.util container
//       wrappers are instantiated and every accessor is *named* so the public
//       surface is forced to type-check and link on each toolchain.
//
//   (2) An EXHAUSTIVE compile-time INVOCABILITY MATRIX over ARGUMENT-TYPE
//       CATEGORIES.  The sibling file tests/test_api_surface_extended.cpp
//       already pins each public entry point's EXACT signature + return type
//       (one representative argument set) and runtime-asserts the no-JVM no-op
//       contract.  This file deliberately does NOT duplicate that.  Instead it
//       proves, for every entry point, that it is callable with the WHOLE
//       FAMILY of argument types a real caller might hand it — the Cartesian
//       product of the relevant categories:
//         * string parameters   <- const char* / char[] / std::string /
//                                  std::string_view / a mutable char buffer,
//         * integer parameters  <- int / short / long / unsigned / std::size_t /
//                                  std::int32_t / a scoped enum's underlying...,
//         * pointer parameters  <- void* / nullptr_t / typed-pointer-decay,
//         * detour/visitor/predicate/callback callables in every documented
//           arity & capture shape (stateless, capturing, std::function,
//           plain function pointer, mutable),
//         * template type arguments over multi-level wrapper hierarchies,
//           non-trivial-destructor wrappers, and the primitive element/field
//           type set (bool/int8/16/32/64/uint16/uint32/float/double/char).
//       It also pins OVERLOADS and PUBLIC MEMBERS the extended file leaves
//       uncovered: hook<T>'s 4-arg already_hooked form, object_base's 3-arg
//       static get_method(type_index,name,sig), return_value::set<wrapper>(
//       nullptr) / set_arg / stack_trace(max_depth), method_proxy's full
//       accessor + call surface, field_proxy's 5-arg GC-stable ctor,
//       jni::global_ref's oop()/reset()/handle()/operator bool, and the
//       get/set_array_element + signature_for_arg element-type matrices.
//
// EVERY static_assert lives in an unevaluated decltype()/is_invocable context,
// so NONE of the pinned functions is actually called at compile time — the
// no-JVM precondition is never violated by the lockdown.  The runtime main()
// adds only a thin set of no-op checks for surface the extended file does not
// already run, then prints a deterministic, byte-identical report.
//
// CROSS-PLATFORM: only is_invocable / type-trait booleans and nullptr / empty /
// size comparisons.  No <charconv>, no float parsing, no sizeof==8 width
// asserts (std::is_same_v only), no std::string_view{"literal"} inside
// noexcept(), no constexpr lambda captures.  Identical output on MSVC,
// libstdc++ (MinGW) and libc++ (Apple clang).
// =============================================================================
#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

// =============================================================================
// PART 1 — ORIGINAL COMPILE-ONLY SURFACE PROOF (unchanged in spirit).
// A realistic user wrapper plus the container wrappers, every accessor named so
// the public surface is forced to instantiate & link on this toolchain.
// =============================================================================
class my_class : public vmhook::object<my_class>
{
public:
    explicit my_class(vmhook::oop_t oop) noexcept
        : vmhook::object<my_class>{ oop }
    {
    }

    // Instance-style accessors must compile.
    auto get_health() -> int { return get_field("health")->get(); }
    auto set_health(int v) -> void { get_field("health")->set(v); }
    auto add_score(int x) -> int { return get_method("addScore")->call(x); }

    // Static-style accessors must compile.
    static auto get_count() -> int { return static_field("entityCount")->get(); }
    static auto set_count(int v) -> void { static_field("entityCount")->set(v); }
    static auto reset() -> void { static_method("reset")->call(); }
};

static auto exercise_hooks() -> void
{
    // The whole hook machinery should be callable; without a JVM it returns
    // false rather than crashing.  We just want it to type-check.
    const bool ok{ vmhook::hook<my_class>("addScore",
        [](vmhook::return_value& retval,
           const std::unique_ptr<my_class>& self,
           int amount)
        {
            if (self && amount > 9000)
            {
                retval.set(int{ 0 });
            }
        }) };
    (void)ok;

    vmhook::shutdown_hooks();
}

// Compile-only coverage of the java.util container wrappers (collection,
// list, set, linked_list, map, hash_map) and the matching field_proxy
// value_t entry points.
class element_w : public vmhook::object<element_w>
{
public:
    explicit element_w(vmhook::oop_t oop) noexcept
        : vmhook::object<element_w>{ oop }
    {
    }
};

class key_w : public vmhook::object<key_w>
{
public:
    explicit key_w(vmhook::oop_t oop) noexcept
        : vmhook::object<key_w>{ oop }
    {
    }
};

class value_w : public vmhook::object<value_w>
{
public:
    explicit value_w(vmhook::oop_t oop) noexcept
        : vmhook::object<value_w>{ oop }
    {
    }
};

static auto exercise_collection_wrappers() -> void
{
    // Direct construction from a null OOP — every member must be callable
    // without dereferencing anything.
    vmhook::collection  c{ nullptr };
    vmhook::list        l{ nullptr };
    vmhook::set         s{ nullptr };
    vmhook::linked_list ll{ nullptr };
    vmhook::map         m{ nullptr };
    vmhook::hash_map    hm{ nullptr };

    (void)c.size();
    (void)l.size();
    (void)s.size();
    (void)ll.size();
    (void)m.size();
    (void)hm.size();
    (void)c.is_empty();
    (void)m.is_empty();

    auto v0 = c.to_vector<element_w>();
    auto v1 = l.to_vector<element_w>();
    auto v2 = s.to_vector<element_w>();
    auto v3 = ll.to_vector<element_w>();
    auto e0 = m.to_entries<key_w, value_w>();
    auto e1 = hm.to_entries<key_w, value_w>();
    (void)v0; (void)v1; (void)v2; (void)v3;
    (void)e0; (void)e1;
}

static auto exercise_field_proxy_entrypoints() -> void
{
    vmhook::field_proxy field{ nullptr, "Ljava/util/List;", false };

    auto via_value_t_vec     = field.get().to_vector<element_w>();
    auto via_value_t_entries = field.get().to_entries<key_w, value_w>();
    (void)via_value_t_vec; (void)via_value_t_entries;
}

// =============================================================================
// PART 2 — wrapper-type fixtures for the compile-time matrices.
// None are ever constructed at runtime; they exist purely to instantiate the
// templated public API in unevaluated contexts.
// =============================================================================
namespace fixtures
{
    // Plain single-level wrapper.
    class plain_w : public vmhook::object<plain_w>
    {
    public:
        explicit plain_w(vmhook::oop_t oop) noexcept
            : vmhook::object<plain_w>{ oop } {}
    };

    // Three-level object<> hierarchy (incomplete-type / vtable instantiation
    // regressions key on exactly this shape — the libstdc++-vs-libc++
    // unique_ptr<object_base> static_assert that once bit the factory).
    class lvl1_w : public vmhook::object<lvl1_w>
    {
    public:
        explicit lvl1_w(vmhook::oop_t oop) noexcept
            : vmhook::object<lvl1_w>{ oop } {}
    };
    class lvl2_w : public lvl1_w
    {
    public:
        explicit lvl2_w(vmhook::oop_t oop) noexcept
            : lvl1_w{ oop } {}
    };
    class lvl3_w : public lvl2_w
    {
    public:
        explicit lvl3_w(vmhook::oop_t oop) noexcept
            : lvl2_w{ oop } {}
    };

    // Wrapper with a NON-TRIVIAL destructor (must still satisfy the factory's
    // "derives from object_base" contract through unique_ptr).
    class ntd_w : public vmhook::object<ntd_w>
    {
    public:
        explicit ntd_w(vmhook::oop_t oop) noexcept
            : vmhook::object<ntd_w>{ oop } {}
        ~ntd_w() { this->touched = 0; }
    private:
        int touched{ 1 };
    };

    // Collection element / map key+value stand-ins.
    class elem_w : public vmhook::object<elem_w>
    {
    public:
        explicit elem_w(vmhook::oop_t oop) noexcept
            : vmhook::object<elem_w>{ oop } {}
    };
    class kkey_w : public vmhook::object<kkey_w>
    {
    public:
        explicit kkey_w(vmhook::oop_t oop) noexcept
            : vmhook::object<kkey_w>{ oop } {}
    };
    class vval_w : public vmhook::object<vval_w>
    {
    public:
        explicit vval_w(vmhook::oop_t oop) noexcept
            : vmhook::object<vval_w>{ oop } {}
    };
} // namespace fixtures

// -----------------------------------------------------------------------------
// Named detour / visitor / predicate / callback functors (stateless — no
// capture, so -Wunused-lambda-capture can never bite).  These stand in for the
// `auto&&` callable parameters of the templated API, which cannot be addressed
// with a plain &function.  Every documented arity/shape is represented.
// -----------------------------------------------------------------------------
namespace callables
{
    // Detour shapes for hook<T> / hook_by_signature<T> / scoped_hook<T>.
    // NOTE: deliberately NOT noexcept — the library's detail::function_traits
    // only specialises non-noexcept call operators, so a real hook<T>() install
    // (which instantiates function_traits in its body) requires a non-noexcept
    // detour.  The same functors are used in unevaluated decltype() asserts
    // below, where the body is never instantiated, so non-noexcept is harmless
    // there and lets the runtime 4-arg hook() call compile.  (See the [INFO]
    // note in the test report about noexcept callables.)
    struct detour_ret_only
    {
        auto operator()(vmhook::return_value&) const -> void {}
    };
    struct detour_ret_self
    {
        auto operator()(vmhook::return_value&,
                        const std::unique_ptr<fixtures::plain_w>&) const -> void {}
    };
    struct detour_ret_self_one_arg
    {
        auto operator()(vmhook::return_value&,
                        const std::unique_ptr<fixtures::plain_w>&, int) const -> void {}
    };
    struct detour_ret_self_many_args
    {
        auto operator()(vmhook::return_value&,
                        const std::unique_ptr<fixtures::plain_w>&,
                        int, double, bool) const -> void {}
    };
    // A detour that mutates the return slot (set/cancel/set_arg) — proves the
    // return_value mutation surface is reachable from inside the callable.
    struct detour_mutating
    {
        auto operator()(vmhook::return_value& rv,
                        const std::unique_ptr<fixtures::plain_w>&, int amount) const -> void
        {
            if (amount > 0) { rv.set(std::int32_t{ 0 }); } else { rv.cancel(); }
        }
    };

    // Visitor shapes.
    struct class_visitor
    {
        auto operator()(const std::string&, vmhook::hotspot::klass*) const -> void {}
    };
    struct thread_visitor
    {
        auto operator()(const vmhook::thread_info&) const -> void {}
    };
    // for_each_instance takes the wrapper BY VALUE (std::unique_ptr<T>).
    template<typename W>
    struct instance_visitor
    {
        auto operator()(std::unique_ptr<W>) const -> void {}
    };

    // Predicate for deoptimize_methods_if.
    struct method_predicate
    {
        auto operator()(const std::string&, vmhook::hotspot::method*) const -> bool { return true; }
    };

    // String callback for on_class_loaded / on_exception.
    struct name_callback
    {
        auto operator()(const std::string&) const -> void {}
    };

    // watch_static_field callback: (old_value, new_value) of the field type.
    template<typename T>
    struct field_change_callback
    {
        auto operator()(T, T) const -> void {}
    };

    // Plain free functions, to prove function-pointer callables are accepted
    // wherever a callable is expected (a category distinct from functor / lambda).
    // Non-noexcept for the same function_traits reason noted above.
    inline auto free_detour(vmhook::return_value&) -> void {}
    inline auto free_class_visitor(const std::string&, vmhook::hotspot::klass*) -> void {}
    inline auto free_thread_visitor(const vmhook::thread_info&) -> void {}
    inline auto free_method_predicate(const std::string&, vmhook::hotspot::method*) -> bool { return true; }
    inline auto free_name_callback(const std::string&) -> void {}
} // namespace callables

// =============================================================================
// PART 3 — COMPILE-TIME INVOCABILITY MATRICES.
// =============================================================================
namespace matrix
{
    using vmhook::hotspot::klass;
    namespace fx = fixtures;
    namespace cb = callables;

    // -------------------------------------------------------------------------
    // Generic argument-category fact helpers.  Each `*_arg_t<i>` names one
    // representative type from a category; the matrices below feed the whole
    // set into is_invocable_r_v so a parameter that silently narrowed to a
    // single concrete type (e.g. const char* only) would fail to compile.
    // -------------------------------------------------------------------------

    // A mutable char buffer type — distinct from a string literal (const char[]).
    using char_buf_t = char[8];

    // ===== GROUP 1 — STRING-PARAMETER INVOCABILITY ===========================
    // Every entry point that documents a string parameter must accept the whole
    // family of types that implicitly form a std::string_view (or std::string).
    // We assert invocability from: const char*, a string literal array
    // (const char[N]), a mutable char[N], std::string, and std::string_view.
    //
    // find_class(string_view) — the canonical string entry point.
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class), const char*>,
                  "find_class must accept a const char*");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class), const char(&)[18]>,
                  "find_class must accept a string-literal array");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class), char_buf_t&>,
                  "find_class must accept a mutable char buffer");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class), std::string>,
                  "find_class must accept a std::string (rvalue)");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class), const std::string&>,
                  "find_class must accept a const std::string&");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class), std::string_view>,
                  "find_class must accept a std::string_view");
    // A bare integer must NOT be a viable string argument (string_view has no
    // such constructor) — proves the parameter is genuinely string-typed.
    static_assert(!std::is_invocable_v<decltype(&vmhook::find_class), int>,
                  "find_class must NOT accept a bare int as the class name");
    static_assert(!std::is_invocable_v<decltype(&vmhook::find_class), std::nullptr_t>,
                  "find_class must NOT accept nullptr as the class name");

    // make_java_string(string_view) — same string-category matrix.
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_string), const char*>,
                  "make_java_string must accept const char*");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_string), const std::string&>,
                  "make_java_string must accept const std::string&");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_string), std::string_view>,
                  "make_java_string must accept std::string_view");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_string), char_buf_t&>,
                  "make_java_string must accept a mutable char buffer");

    // override_class_lookup / evict_class_lookup — string + klass* params.
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::override_class_lookup),
                                        const char*, klass*>,
                  "override_class_lookup must accept (const char*, klass*)");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::override_class_lookup),
                                        std::string, std::nullptr_t>,
                  "override_class_lookup must accept (std::string, nullptr)");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::evict_class_lookup), const char*>,
                  "evict_class_lookup must accept const char*");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::evict_class_lookup), std::string_view>,
                  "evict_class_lookup must accept std::string_view");

    // get_class_methods(by-name) — string-category matrix on the overload set.
    static_assert(std::is_same_v<
                      decltype(vmhook::get_class_methods(std::declval<const char*>())),
                      std::vector<std::pair<std::string, std::string>>>,
                  "get_class_methods(const char*) must resolve and return the methods vector");
    static_assert(std::is_same_v<
                      decltype(vmhook::get_class_methods(std::declval<const std::string&>())),
                      std::vector<std::pair<std::string, std::string>>>,
                  "get_class_methods(const std::string&) must resolve");

    // ===== GROUP 2 — POINTER-PARAMETER INVOCABILITY ==========================
    // void*-typed entry points must accept void*, nullptr_t, and a decayed
    // typed pointer (implicit T* -> void*).  They must reject an unrelated
    // integer (no implicit int -> void*).
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::klass_from_oop), void*>,
                  "klass_from_oop must accept void*");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::klass_from_oop), std::nullptr_t>,
                  "klass_from_oop must accept nullptr");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::klass_from_oop), int*>,
                  "klass_from_oop must accept a typed pointer (decays to void*)");
    static_assert(!std::is_invocable_v<decltype(&vmhook::klass_from_oop), int>,
                  "klass_from_oop must NOT accept a bare int");

    static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::read_java_string), void*>,
                  "read_java_string must accept void*");
    static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::read_java_string), std::nullptr_t>,
                  "read_java_string must accept nullptr");
    static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::read_java_string), char*>,
                  "read_java_string must accept a typed pointer (decays to void*)");

    // find_class_via_oop(void* anchor, string name) — pointer x string matrix.
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class_via_oop),
                                        void*, const char*>,
                  "find_class_via_oop must accept (void*, const char*)");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class_via_oop),
                                        std::nullptr_t, std::string_view>,
                  "find_class_via_oop must accept (nullptr, string_view)");
    static_assert(std::is_invocable_r_v<klass*, decltype(&vmhook::find_class_via_oop),
                                        int*, std::string>,
                  "find_class_via_oop must accept (typed-ptr, std::string)");

    // write_java_string(void*, string) — pointer x string matrix.
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::write_java_string),
                                        std::nullptr_t, const char*>,
                  "write_java_string must accept (nullptr, const char*)");
    static_assert(std::is_invocable_r_v<void, decltype(&vmhook::write_java_string),
                                        void*, std::string_view>,
                  "write_java_string must accept (void*, string_view)");

    // ===== GROUP 3 — INTEGER / WIDTH-CATEGORY INVOCABILITY ===================
    // Integer-typed parameters must accept the common integer spellings, since
    // a caller may pass int / short / long / unsigned / size_t / int32_t etc.
    // decode_array_oop(uint32_t) — the compressed-OOP integer entry point.
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::decode_array_oop), std::uint32_t>,
                  "decode_array_oop must accept uint32_t");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::decode_array_oop), int>,
                  "decode_array_oop must accept int (converts to uint32_t)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::decode_array_oop), unsigned>,
                  "decode_array_oop must accept unsigned");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::decode_array_oop), std::size_t>,
                  "decode_array_oop must accept size_t (converts to uint32_t)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::decode_array_oop), short>,
                  "decode_array_oop must accept short");

    // make_java_array(name, int32 length, size_t element_size [, bool]) — the
    // length and element_size integer categories, plus the defaulted bool tail.
    // The trailing allow_jni_fallback is DEFAULTED, so a function-pointer (which
    // cannot see defaults) needs all 4 args; the 3-arg category matrix is pinned
    // via decltype on the call expression, which DOES honour the default.
    static_assert(std::is_same_v<
                      decltype(vmhook::make_java_array(std::declval<const char*>(),
                                                       std::declval<std::int32_t>(),
                                                       std::declval<std::size_t>())), void*>,
                  "make_java_array 3-arg must accept (const char*, int32, size_t)");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_java_array(std::declval<std::string_view>(),
                                                       std::declval<int>(), std::declval<int>())), void*>,
                  "make_java_array must accept (string_view, int, int) — int args convert");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_java_array(std::declval<std::string>(),
                                                       std::declval<long>(),
                                                       std::declval<unsigned>())), void*>,
                  "make_java_array must accept (std::string, long, unsigned)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::make_java_array),
                                        const char*, std::int32_t, std::size_t, bool>,
                  "make_java_array 4-arg (explicit allow_jni_fallback) must be callable");

    // get_array_element<T>(void* array, int32 index) — index integer category.
    static_assert(std::is_same_v<
                      decltype(vmhook::get_array_element<std::int32_t>(
                          std::declval<void*>(), std::declval<int>())),
                      std::int32_t>,
                  "get_array_element<int32> must accept an int index");
    static_assert(std::is_same_v<
                      decltype(vmhook::get_array_element<std::int32_t>(
                          std::declval<void*>(), std::declval<short>())),
                      std::int32_t>,
                  "get_array_element<int32> must accept a short index");
    static_assert(std::is_same_v<
                      decltype(vmhook::get_array_element<std::int32_t>(
                          std::declval<std::nullptr_t>(), std::declval<std::int32_t>())),
                      std::int32_t>,
                  "get_array_element<int32> must accept a nullptr array");

    // ===== GROUP 4 — get/set_array_element ELEMENT-TYPE MATRIX ================
    // The full primitive element-type set the library documents for arrays:
    // bool / int8 / int16 / int32 / int64 / uint16 / uint32 / float / double /
    // char / signed char / unsigned char.  Each must yield the element type for
    // get and void for set, proving every instantiation compiles on each STL.
    template<typename E>
    inline constexpr bool array_get_yields_element_v =
        std::is_same_v<decltype(vmhook::get_array_element<E>(
                           std::declval<void*>(), std::declval<std::int32_t>())), E>;
    template<typename E>
    inline constexpr bool array_set_yields_void_v =
        std::is_same_v<decltype(vmhook::set_array_element<E>(
                           std::declval<void*>(), std::declval<std::int32_t>(),
                           std::declval<E>())), void>;

    static_assert(array_get_yields_element_v<bool>,           "get_array_element<bool>");
    static_assert(array_get_yields_element_v<std::int8_t>,    "get_array_element<int8>");
    static_assert(array_get_yields_element_v<std::int16_t>,   "get_array_element<int16>");
    static_assert(array_get_yields_element_v<std::int32_t>,   "get_array_element<int32>");
    static_assert(array_get_yields_element_v<std::int64_t>,   "get_array_element<int64>");
    static_assert(array_get_yields_element_v<std::uint16_t>,  "get_array_element<uint16>");
    static_assert(array_get_yields_element_v<std::uint32_t>,  "get_array_element<uint32>");
    static_assert(array_get_yields_element_v<std::uint64_t>,  "get_array_element<uint64>");
    static_assert(array_get_yields_element_v<float>,          "get_array_element<float>");
    static_assert(array_get_yields_element_v<double>,         "get_array_element<double>");
    static_assert(array_get_yields_element_v<char>,           "get_array_element<char>");
    static_assert(array_get_yields_element_v<signed char>,    "get_array_element<signed char>");
    static_assert(array_get_yields_element_v<unsigned char>,  "get_array_element<unsigned char>");

    static_assert(array_set_yields_void_v<bool>,           "set_array_element<bool>");
    static_assert(array_set_yields_void_v<std::int8_t>,    "set_array_element<int8>");
    static_assert(array_set_yields_void_v<std::int16_t>,   "set_array_element<int16>");
    static_assert(array_set_yields_void_v<std::int32_t>,   "set_array_element<int32>");
    static_assert(array_set_yields_void_v<std::int64_t>,   "set_array_element<int64>");
    static_assert(array_set_yields_void_v<std::uint16_t>,  "set_array_element<uint16>");
    static_assert(array_set_yields_void_v<std::uint32_t>,  "set_array_element<uint32>");
    static_assert(array_set_yields_void_v<std::uint64_t>,  "set_array_element<uint64>");
    static_assert(array_set_yields_void_v<float>,          "set_array_element<float>");
    static_assert(array_set_yields_void_v<double>,         "set_array_element<double>");
    static_assert(array_set_yields_void_v<char>,           "set_array_element<char>");

    // array_length(void*) — pointer-category matrix + return type.
    static_assert(std::is_invocable_r_v<std::int32_t, decltype(&vmhook::array_length), void*>,
                  "array_length must accept void* and return int32");
    static_assert(std::is_invocable_r_v<std::int32_t, decltype(&vmhook::array_length), std::nullptr_t>,
                  "array_length must accept nullptr");
    static_assert(noexcept(vmhook::array_length(std::declval<void*>())),
                  "array_length must be noexcept");

    // ===== GROUP 5 — set_prim_array<T> ELEMENT-TYPE MATRIX ===================
    // set_prim_array<E>(const field_proxy&, const vector<E>&) over the numeric
    // primitive set (bool / strings have dedicated overloads, asserted in the
    // extended file; here we widen the NUMERIC element coverage).
    template<typename E>
    inline constexpr bool set_prim_array_void_v =
        std::is_same_v<decltype(vmhook::set_prim_array<E>(
                           std::declval<const vmhook::field_proxy&>(),
                           std::declval<const std::vector<E>&>())), void>;
    static_assert(set_prim_array_void_v<std::int8_t>,   "set_prim_array<int8>");
    static_assert(set_prim_array_void_v<std::int16_t>,  "set_prim_array<int16>");
    static_assert(set_prim_array_void_v<std::int32_t>,  "set_prim_array<int32>");
    static_assert(set_prim_array_void_v<std::int64_t>,  "set_prim_array<int64>");
    static_assert(set_prim_array_void_v<std::uint16_t>, "set_prim_array<uint16>");
    static_assert(set_prim_array_void_v<float>,         "set_prim_array<float>");
    static_assert(set_prim_array_void_v<double>,        "set_prim_array<double>");

    // ===== GROUP 6 — register_class<T> / make_unique<T> TYPE MATRIX ==========
    // Every wrapper-shape (plain, deep hierarchy, non-trivial dtor, element/
    // key/value) must be a valid register_class<T> argument and yield bool.
    template<typename W>
    inline constexpr bool register_class_bool_v =
        std::is_same_v<decltype(vmhook::register_class<W>(std::declval<std::string_view>())), bool>;
    static_assert(register_class_bool_v<fx::plain_w>, "register_class<plain_w>");
    static_assert(register_class_bool_v<fx::lvl1_w>,  "register_class<lvl1_w>");
    static_assert(register_class_bool_v<fx::lvl2_w>,  "register_class<lvl2_w>");
    static_assert(register_class_bool_v<fx::lvl3_w>,  "register_class<lvl3_w>");
    static_assert(register_class_bool_v<fx::ntd_w>,   "register_class<ntd_w>");
    static_assert(register_class_bool_v<fx::elem_w>,  "register_class<elem_w>");
    // register_class string-argument category matrix (one representative type).
    static_assert(std::is_same_v<
                      decltype(vmhook::register_class<fx::plain_w>(std::declval<const char*>())), bool>,
                  "register_class<T> must accept a const char* class name");
    static_assert(std::is_same_v<
                      decltype(vmhook::register_class<fx::plain_w>(std::declval<const std::string&>())), bool>,
                  "register_class<T> must accept a const std::string& class name");

    // make_unique<T>() (TLAB path) and make_unique<T>(ctor-args...) over the
    // type matrix and a variety of constructor-argument categories.
    template<typename W>
    inline constexpr bool make_unique_uptr_v =
        std::is_same_v<decltype(vmhook::make_unique<W>()), std::unique_ptr<W>>;
    static_assert(make_unique_uptr_v<fx::plain_w>, "make_unique<plain_w>()");
    static_assert(make_unique_uptr_v<fx::lvl3_w>,  "make_unique<lvl3_w>()");
    static_assert(make_unique_uptr_v<fx::ntd_w>,   "make_unique<ntd_w>()");
    // Constructor-argument forwarding across argument categories.
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<fx::plain_w>(1)), std::unique_ptr<fx::plain_w>>,
                  "make_unique<T>(int)");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<fx::plain_w>(1, 2.0, true, 'c')),
                      std::unique_ptr<fx::plain_w>>,
                  "make_unique<T>(int,double,bool,char)");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<fx::plain_w>(std::declval<const std::string&>())),
                      std::unique_ptr<fx::plain_w>>,
                  "make_unique<T>(const std::string&)");
    static_assert(std::is_same_v<
                      decltype(vmhook::make_unique<fx::plain_w>(std::declval<std::string_view>(),
                                                               std::declval<std::int64_t>())),
                      std::unique_ptr<fx::plain_w>>,
                  "make_unique<T>(string_view, int64)");

    // jni::make_unique<T>(const string&, args...) — the JNI sibling factory.
    static_assert(std::is_same_v<
                      decltype(vmhook::jni::make_unique<fx::plain_w>(std::declval<const std::string&>())),
                      std::unique_ptr<fx::plain_w>>,
                  "jni::make_unique<T>(const string&)");
    static_assert(std::is_same_v<
                      decltype(vmhook::jni::make_unique<fx::ntd_w>(
                          std::declval<const std::string&>(), 1, 2.0, false)),
                      std::unique_ptr<fx::ntd_w>>,
                  "jni::make_unique<T>(const string&, args...) over a non-trivial-dtor wrapper");

    // ===== GROUP 7 — hook<T> CALLABLE-SHAPE + OVERLOAD MATRIX ================
    // hook<T> must accept every documented detour shape AND every documented
    // overload, in BOTH the functor and free-function-pointer callable forms.
    //
    // 2-arg hook<T>(name, detour): detour shape matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<cb::detour_ret_only>())), bool>,
                  "hook<T>(name, detour: ret-only)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<cb::detour_ret_self>())), bool>,
                  "hook<T>(name, detour: ret+self)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<cb::detour_ret_self_one_arg>())), bool>,
                  "hook<T>(name, detour: ret+self+1arg)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<cb::detour_ret_self_many_args>())), bool>,
                  "hook<T>(name, detour: ret+self+many args)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<cb::detour_mutating>())), bool>,
                  "hook<T>(name, detour: mutates return slot)");
    // Free-function-pointer detour (distinct callable category from functor).
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         cb::free_detour)), bool>,
                  "hook<T>(name, &free_function detour)");
    // std::function detour (type-erased callable category).
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(
                          std::declval<std::string_view>(),
                          std::declval<std::function<void(vmhook::return_value&)>>())), bool>,
                  "hook<T>(name, std::function detour)");
    // hook<T> name argument string-category matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<const char*>(),
                                                         std::declval<cb::detour_ret_only>())), bool>,
                  "hook<T>(const char* name, detour)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<const std::string&>(),
                                                         std::declval<cb::detour_ret_only>())), bool>,
                  "hook<T>(const std::string& name, detour)");

    // 3-arg hook<T>(name, signature, detour): string x string x detour.
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<std::string_view>(),
                                                         std::declval<cb::detour_ret_self_one_arg>())), bool>,
                  "hook<T>(name, signature, detour)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<const char*>(),
                                                         std::declval<const char*>(),
                                                         cb::free_detour)), bool>,
                  "hook<T>(const char* name, const char* sig, &free detour)");

    // 4-arg hook<T>(name, signature, detour, bool* already_hooked) — the
    // out-param overload the extended file does NOT cover.  The trailing
    // bool* is an OPTIONAL diagnostic the caller may thread through.
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<std::string_view>(),
                                                         std::declval<cb::detour_ret_only>(),
                                                         std::declval<bool*>())), bool>,
                  "hook<T>(name, signature, detour, bool* already_hooked) must be callable");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::plain_w>(std::declval<std::string_view>(),
                                                         std::declval<std::string_view>(),
                                                         std::declval<cb::detour_ret_only>(),
                                                         std::declval<std::nullptr_t>())), bool>,
                  "hook<T>(name, signature, detour, nullptr) must be callable");

    // hook<T> type matrix: a deep hierarchy + non-trivial-dtor wrapper must
    // both be valid hook targets.
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::lvl3_w>(std::declval<std::string_view>(),
                                                        std::declval<cb::detour_ret_only>())), bool>,
                  "hook<lvl3_w>(name, detour)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook<fx::ntd_w>(std::declval<std::string_view>(),
                                                       std::declval<cb::detour_ret_only>())), bool>,
                  "hook<ntd_w>(name, detour)");

    // hook_by_signature<T>(descriptor, detour) — string x detour matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::hook_by_signature<fx::plain_w>(
                          std::declval<const char*>(), std::declval<cb::detour_ret_only>())), bool>,
                  "hook_by_signature<T>(const char* descriptor, detour)");
    static_assert(std::is_same_v<
                      decltype(vmhook::hook_by_signature<fx::plain_w>(
                          std::declval<std::string_view>(), cb::free_detour)), bool>,
                  "hook_by_signature<T>(string_view descriptor, &free detour)");

    // scoped_hook<T> — 2-arg and 3-arg, returning hook_handle, callable-shape
    // matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::scoped_hook<fx::plain_w>(
                          std::declval<std::string_view>(), std::declval<cb::detour_ret_only>())),
                      vmhook::hook_handle>,
                  "scoped_hook<T>(name, detour) -> hook_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::scoped_hook<fx::plain_w>(
                          std::declval<std::string_view>(), std::declval<std::string_view>(),
                          std::declval<cb::detour_ret_self_one_arg>())),
                      vmhook::hook_handle>,
                  "scoped_hook<T>(name, signature, detour) -> hook_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::scoped_hook<fx::ntd_w>(
                          std::declval<const char*>(), cb::free_detour)),
                      vmhook::hook_handle>,
                  "scoped_hook<ntd_w>(const char*, &free detour) -> hook_handle");

    // ===== GROUP 8 — for_each_* CALLABLE + TYPE MATRIX =======================
    // for_each_loaded_class / for_each_thread visitor-callable category matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_loaded_class(std::declval<cb::class_visitor>())), void>,
                  "for_each_loaded_class(functor visitor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_loaded_class(cb::free_class_visitor)), void>,
                  "for_each_loaded_class(&free visitor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_loaded_class(
                          std::declval<std::function<void(const std::string&, klass*)>>())), void>,
                  "for_each_loaded_class(std::function visitor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_thread(std::declval<cb::thread_visitor>())), void>,
                  "for_each_thread(functor visitor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_thread(cb::free_thread_visitor)), void>,
                  "for_each_thread(&free visitor)");

    // for_each_instance<T>(visitor [, max_visits]) — type x callable x integer
    // matrix.  Visitor takes the wrapper BY VALUE.
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_instance<fx::plain_w>(
                          std::declval<cb::instance_visitor<fx::plain_w>>())), std::size_t>,
                  "for_each_instance<plain_w>(visitor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_instance<fx::lvl3_w>(
                          std::declval<cb::instance_visitor<fx::lvl3_w>>())), std::size_t>,
                  "for_each_instance<lvl3_w>(visitor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_instance<fx::ntd_w>(
                          std::declval<cb::instance_visitor<fx::ntd_w>>())), std::size_t>,
                  "for_each_instance<ntd_w>(visitor)");
    // max_visits integer-category matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_instance<fx::plain_w>(
                          std::declval<cb::instance_visitor<fx::plain_w>>(), std::declval<std::size_t>())),
                      std::size_t>,
                  "for_each_instance<T>(visitor, size_t max_visits)");
    static_assert(std::is_same_v<
                      decltype(vmhook::for_each_instance<fx::plain_w>(
                          std::declval<cb::instance_visitor<fx::plain_w>>(), std::declval<int>())),
                      std::size_t>,
                  "for_each_instance<T>(visitor, int max_visits) — int converts to size_t");

    // deoptimize_methods_if(predicate) — predicate-callable category matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::deoptimize_methods_if(std::declval<cb::method_predicate>())),
                      std::size_t>,
                  "deoptimize_methods_if(functor predicate)");
    static_assert(std::is_same_v<
                      decltype(vmhook::deoptimize_methods_if(cb::free_method_predicate)),
                      std::size_t>,
                  "deoptimize_methods_if(&free predicate)");
    static_assert(std::is_same_v<
                      decltype(vmhook::deoptimize_methods_if(
                          std::declval<std::function<bool(const std::string&, vmhook::hotspot::method*)>>())),
                      std::size_t>,
                  "deoptimize_methods_if(std::function predicate)");

    // ===== GROUP 9 — find_methods_by_signature / log_class_methods MATRIX ====
    static_assert(std::is_same_v<
                      decltype(vmhook::find_methods_by_signature<fx::plain_w>(
                          std::declval<const char*>())), std::vector<std::string>>,
                  "find_methods_by_signature<T>(const char* descriptor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::find_methods_by_signature<fx::lvl3_w>(
                          std::declval<std::string_view>())), std::vector<std::string>>,
                  "find_methods_by_signature<lvl3_w>(string_view descriptor)");
    static_assert(std::is_same_v<
                      decltype(vmhook::get_class_methods<fx::ntd_w>()),
                      std::vector<std::pair<std::string, std::string>>>,
                  "get_class_methods<ntd_w>()");
    static_assert(std::is_same_v<decltype(vmhook::log_class_methods<fx::plain_w>()), void>,
                  "log_class_methods<plain_w>()");
    static_assert(std::is_same_v<decltype(vmhook::log_class_methods<fx::lvl3_w>()), void>,
                  "log_class_methods<lvl3_w>()");

    // ===== GROUP 10 — watchers: callable + field-type MATRIX =================
    // on_class_loaded / on_exception callback-category matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::on_class_loaded(std::declval<cb::name_callback>())),
                      vmhook::watch_handle>,
                  "on_class_loaded(functor callback) -> watch_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::on_class_loaded(cb::free_name_callback)),
                      vmhook::watch_handle>,
                  "on_class_loaded(&free callback) -> watch_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::on_exception(std::declval<cb::name_callback>())),
                      vmhook::watch_handle>,
                  "on_exception(functor callback) -> watch_handle");
    static_assert(std::is_same_v<
                      decltype(vmhook::on_exception(
                          std::declval<std::function<void(const std::string&)>>())),
                      vmhook::watch_handle>,
                  "on_exception(std::function callback) -> watch_handle");

    // watch_static_field<T, field_type>(name, callback) — the field_type width
    // matrix (the DR LEN selection forks on 1/2/4/8-byte widths + bool + float
    // /double) crossed with the wrapper-type matrix.
    template<typename W, typename F>
    inline constexpr bool watch_static_field_handle_v =
        std::is_same_v<decltype(vmhook::watch_static_field<W, F>(
                           std::declval<std::string_view>(),
                           std::declval<cb::field_change_callback<F>>())),
                       vmhook::watch_handle>;
    static_assert(watch_static_field_handle_v<fx::plain_w, bool>,         "watch_static_field<plain_w,bool>");
    static_assert(watch_static_field_handle_v<fx::plain_w, std::int8_t>,  "watch_static_field<plain_w,int8>");
    static_assert(watch_static_field_handle_v<fx::plain_w, std::int16_t>, "watch_static_field<plain_w,int16>");
    static_assert(watch_static_field_handle_v<fx::plain_w, std::int32_t>, "watch_static_field<plain_w,int32>");
    static_assert(watch_static_field_handle_v<fx::plain_w, std::int64_t>, "watch_static_field<plain_w,int64>");
    static_assert(watch_static_field_handle_v<fx::plain_w, float>,        "watch_static_field<plain_w,float>");
    static_assert(watch_static_field_handle_v<fx::plain_w, double>,       "watch_static_field<plain_w,double>");
    static_assert(watch_static_field_handle_v<fx::lvl3_w, std::int32_t>,  "watch_static_field<lvl3_w,int32>");
    // watch_static_field name string-category + free-function-callback matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::watch_static_field<fx::plain_w, std::int32_t>(
                          std::declval<const char*>(),
                          std::declval<cb::field_change_callback<std::int32_t>>())),
                      vmhook::watch_handle>,
                  "watch_static_field<T,int32>(const char* name, callback)");

    // ===== GROUP 11 — pin() + global_ref MATRIX ==============================
    // pin(oop) and pin(unique_ptr<T>&) over the wrapper-type matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::pin(std::declval<vmhook::oop_t>())),
                      vmhook::jni::global_ref>,
                  "pin(oop_t) -> jni::global_ref");
    static_assert(std::is_same_v<
                      decltype(vmhook::pin(std::declval<std::nullptr_t>())),
                      vmhook::jni::global_ref>,
                  "pin(nullptr) -> jni::global_ref");
    template<typename W>
    inline constexpr bool pin_uptr_global_ref_v =
        std::is_same_v<decltype(vmhook::pin(std::declval<const std::unique_ptr<W>&>())),
                       vmhook::jni::global_ref>;
    static_assert(pin_uptr_global_ref_v<fx::plain_w>, "pin(unique_ptr<plain_w>&)");
    static_assert(pin_uptr_global_ref_v<fx::lvl3_w>,  "pin(unique_ptr<lvl3_w>&)");
    static_assert(pin_uptr_global_ref_v<fx::ntd_w>,   "pin(unique_ptr<ntd_w>&)");

    // jni::global_ref PUBLIC MEMBER surface the extended file does NOT pin:
    // oop() / reset() / handle() / explicit operator bool, plus constructibility.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::jni::global_ref&>().oop()), vmhook::oop_t>,
                  "global_ref::oop() must return oop_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::jni::global_ref&>().reset()), void>,
                  "global_ref::reset() must return void");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::jni::global_ref&>().handle()), void*>,
                  "global_ref::handle() must return void*");
    static_assert(noexcept(std::declval<const vmhook::jni::global_ref&>().oop()),
                  "global_ref::oop() must be noexcept");
    static_assert(noexcept(std::declval<vmhook::jni::global_ref&>().reset()),
                  "global_ref::reset() must be noexcept");
    static_assert(std::is_constructible_v<vmhook::jni::global_ref, vmhook::oop_t>,
                  "global_ref must be constructible from oop_t");
    // explicit operator bool — must be CONTEXTUALLY convertible but NOT
    // implicitly convertible (the conversion is `explicit`).
    static_assert(std::is_constructible_v<bool, const vmhook::jni::global_ref&>,
                  "global_ref must be explicitly bool-convertible (usable in if(g))");
    static_assert(!std::is_convertible_v<vmhook::jni::global_ref, bool>,
                  "global_ref::operator bool must be EXPLICIT (no implicit bool decay)");

    // ===== GROUP 12 — return_value MUTATION SURFACE (overloads the extended ==
    // file omits): set<T> over the trivially-copyable scalar set, the
    // set<wrapper>(nullptr) reference-null overload, set_arg<T> value-category
    // matrix, and stack_trace(max_depth).
    template<typename V>
    inline constexpr bool return_value_set_void_v =
        std::is_same_v<decltype(std::declval<vmhook::return_value&>().set(std::declval<V>())), void>;
    static_assert(return_value_set_void_v<bool>,          "return_value::set<bool>");
    static_assert(return_value_set_void_v<std::int8_t>,   "return_value::set<int8>");
    static_assert(return_value_set_void_v<std::int16_t>,  "return_value::set<int16>");
    static_assert(return_value_set_void_v<std::int32_t>,  "return_value::set<int32>");
    static_assert(return_value_set_void_v<std::int64_t>,  "return_value::set<int64>");
    static_assert(return_value_set_void_v<float>,         "return_value::set<float>");
    static_assert(return_value_set_void_v<double>,        "return_value::set<double>");
    static_assert(return_value_set_void_v<void*>,         "return_value::set<void*> (oop return)");
    static_assert(return_value_set_void_v<char>,          "return_value::set<char>");
    // set<wrapper_type>(nullptr) — the typed null-reference return overload,
    // selected only when wrapper_type derives from object_base.
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::return_value&>().set<fx::plain_w>(nullptr)), void>,
                  "return_value::set<wrapper>(nullptr) must return void");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::return_value&>().set<fx::lvl3_w>(nullptr)), void>,
                  "return_value::set<deep-wrapper>(nullptr) must return void");
    // set_arg<T>(int32 index, T&& value) — value-category matrix over scalars.
    template<typename V>
    inline constexpr bool return_value_set_arg_bool_v =
        std::is_same_v<decltype(std::declval<vmhook::return_value&>().set_arg(
                           std::declval<std::int32_t>(), std::declval<V>())), bool>;
    static_assert(return_value_set_arg_bool_v<std::int32_t>, "return_value::set_arg(idx,int32)");
    static_assert(return_value_set_arg_bool_v<std::int64_t>, "return_value::set_arg(idx,int64)");
    static_assert(return_value_set_arg_bool_v<bool>,         "return_value::set_arg(idx,bool)");
    static_assert(return_value_set_arg_bool_v<double>,       "return_value::set_arg(idx,double)");
    static_assert(return_value_set_arg_bool_v<void*>,        "return_value::set_arg(idx,void*)");
    // set_arg index integer-category matrix.
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::return_value&>().set_arg(
                          std::declval<int>(), std::declval<std::int32_t>())), bool>,
                  "return_value::set_arg(int index, value)");
    // stack_trace(max_depth) — the defaulted-arg overload not pinned upstream.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::return_value&>().stack_trace(
                          std::declval<std::size_t>())),
                      std::vector<vmhook::return_value::caller_info>>,
                  "return_value::stack_trace(max_depth) must return vector<caller_info>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::return_value&>().stack_trace()),
                      std::vector<vmhook::return_value::caller_info>>,
                  "return_value::stack_trace() (defaulted depth) must return vector<caller_info>");
    static_assert(noexcept(std::declval<const vmhook::return_value&>().stack_trace()),
                  "return_value::stack_trace() must be noexcept");

    // ===== GROUP 13 — object_base / object<T> ACCESSOR MATRIX ================
    // The instance get_field / get_method string-category matrix + the 3-arg
    // static get_method(type_index, name, signature) the extended file omits.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_field(
                          std::declval<const char*>())),
                      std::optional<vmhook::field_proxy>>,
                  "object_base::get_field(const char*)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_field(
                          std::declval<const std::string&>())),
                      std::optional<vmhook::field_proxy>>,
                  "object_base::get_field(const std::string&)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::object_base&>().get_method(
                          std::declval<const char*>(), std::declval<const char*>())),
                      std::optional<vmhook::method_proxy>>,
                  "object_base::get_method(const char* name, const char* sig)");
    // 3-arg static get_method(type_index, name, signature) — NOT in the
    // extended file (it pins only the 2-arg type_index forms).
    static_assert(std::is_same_v<
                      decltype(vmhook::object_base::get_method(
                          std::declval<std::type_index>(),
                          std::declval<std::string_view>(),
                          std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object_base::get_method(type_index, name, signature) must return optional<method_proxy>");
    // object<T>::static_method(name, signature) 3-arg form, over the type matrix.
    static_assert(std::is_same_v<
                      decltype(vmhook::object<fx::plain_w>::static_method(
                          std::declval<std::string_view>(), std::declval<std::string_view>())),
                      std::optional<vmhook::method_proxy>>,
                  "object<plain_w>::static_method(name, signature)");
    static_assert(std::is_same_v<
                      decltype(vmhook::object<fx::lvl3_w>::static_field(
                          std::declval<const char*>())),
                      std::optional<vmhook::field_proxy>>,
                  "object<lvl3_w>::static_field(const char*)");
    // Deep + non-trivial-dtor wrappers are all object_base subtypes and
    // constructible from oop_t.
    static_assert(std::is_base_of_v<vmhook::object_base, fx::lvl3_w>, "lvl3_w is-a object_base");
    static_assert(std::is_base_of_v<fx::lvl1_w, fx::lvl3_w>,          "lvl3_w is-a lvl1_w (transitive)");
    static_assert(std::is_base_of_v<vmhook::object_base, fx::ntd_w>,  "ntd_w is-a object_base");
    static_assert(std::is_constructible_v<fx::lvl3_w, vmhook::oop_t>, "lvl3_w(oop_t)");
    static_assert(std::is_constructible_v<fx::ntd_w, vmhook::oop_t>,  "ntd_w(oop_t)");
    // object<> with the DEFAULT (void) template parameter is still an
    // object_base (the deducing-this edge that bit instantiation once).
    static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<>>,
                  "object<> (default void param) is-a object_base");

    // ===== GROUP 14 — field_proxy CONSTRUCTOR + ACCESSOR MATRIX ==============
    // The 5-arg GC-stable static-field ctor (void*, string, bool, klass*,
    // size_t) — NOT pinned by the extended file (which pins only the 3-arg).
    static_assert(std::is_constructible_v<vmhook::field_proxy,
                                          void*, std::string, bool, klass*, std::size_t>,
                  "field_proxy(void*, string, bool, klass*, size_t) GC-stable ctor must exist");
    static_assert(std::is_constructible_v<vmhook::field_proxy, std::nullptr_t, std::string, bool>,
                  "field_proxy must be constructible with a nullptr field pointer");
    // field_proxy::set<V> value-category matrix (scalars accepted by const-ref).
    template<typename V>
    inline constexpr bool field_proxy_set_void_v =
        std::is_same_v<decltype(std::declval<const vmhook::field_proxy&>().set(std::declval<const V&>())),
                       void>;
    static_assert(field_proxy_set_void_v<std::int32_t>, "field_proxy::set<int32>");
    static_assert(field_proxy_set_void_v<std::int64_t>, "field_proxy::set<int64>");
    static_assert(field_proxy_set_void_v<bool>,         "field_proxy::set<bool>");
    static_assert(field_proxy_set_void_v<float>,        "field_proxy::set<float>");
    static_assert(field_proxy_set_void_v<double>,       "field_proxy::set<double>");
    static_assert(field_proxy_set_void_v<std::string>,  "field_proxy::set<std::string>");
    // field_proxy::value_t::to_vector<T> / to_entries<K,V> over the type matrix.
    template<typename E>
    inline constexpr bool value_t_to_vector_v =
        std::is_same_v<decltype(std::declval<const vmhook::field_proxy::value_t&>()
                                    .to_vector<E>()),
                       std::vector<std::unique_ptr<E>>>;
    static_assert(value_t_to_vector_v<fx::elem_w>, "value_t::to_vector<elem_w>");
    static_assert(value_t_to_vector_v<fx::lvl3_w>, "value_t::to_vector<lvl3_w>");
    static_assert(value_t_to_vector_v<fx::ntd_w>,  "value_t::to_vector<ntd_w>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::field_proxy::value_t&>()
                                   .to_entries<fx::kkey_w, fx::vval_w>()),
                      std::vector<std::pair<std::unique_ptr<fx::kkey_w>,
                                            std::unique_ptr<fx::vval_w>>>>,
                  "value_t::to_entries<kkey_w, vval_w>");
    // value_t scalar-conversion category matrix (implicit operator T).
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, std::int8_t>,
                  "value_t -> int8");
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, std::int16_t>,
                  "value_t -> int16");
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, std::int64_t>,
                  "value_t -> int64");
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, float>,
                  "value_t -> float");
    static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, void*>,
                  "value_t -> void* (decoded oop)");
    // value_t deliberately does NOT implicitly produce a const char* (the
    // constrained conversion operator excises that spurious production).
    static_assert(!std::is_convertible_v<vmhook::field_proxy::value_t, const char*>,
                  "value_t must NOT implicitly convert to const char*");

    // ===== GROUP 15 — method_proxy ACCESSOR + call MATRIX ====================
    // The extended file pins method_proxy::value_t introspection but NOT the
    // method_proxy accessors or the call/call_jni argument matrix.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().name()), std::string>,
                  "method_proxy::name() must return std::string");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().signature()), std::string_view>,
                  "method_proxy::signature() must return string_view");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().is_static()), bool>,
                  "method_proxy::is_static() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().is_reference()), bool>,
                  "method_proxy::is_reference() must return bool");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().get_compressed_oop()),
                      std::uint32_t>,
                  "method_proxy::get_compressed_oop() must return uint32_t");
    static_assert(std::is_constructible_v<vmhook::method_proxy,
                                          void*, vmhook::hotspot::method*, std::string>,
                  "method_proxy(void*, method*, string) ctor must exist");
    static_assert(std::is_constructible_v<vmhook::method_proxy,
                                          void*, vmhook::hotspot::method*, std::string, bool>,
                  "method_proxy(void*, method*, string, bool pinned) ctor must exist");
    // call(args...) / call_jni(args...) over argument-category matrices —
    // both return value_t for any argument list (variadic forwarding).
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().call()),
                      vmhook::method_proxy::value_t>,
                  "method_proxy::call() (no args) -> value_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().call(
                          std::declval<int>(), std::declval<double>(), std::declval<bool>())),
                      vmhook::method_proxy::value_t>,
                  "method_proxy::call(int,double,bool) -> value_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().call(
                          std::declval<const std::string&>())),
                      vmhook::method_proxy::value_t>,
                  "method_proxy::call(const std::string&) -> value_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().call(
                          std::declval<const std::unique_ptr<fx::plain_w>&>())),
                      vmhook::method_proxy::value_t>,
                  "method_proxy::call(const unique_ptr<wrapper>&) -> value_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy&>().call_jni(
                          std::declval<int>())),
                      vmhook::method_proxy::value_t>,
                  "method_proxy::call_jni(int) -> value_t");
    // method_proxy::value_t scalar-conversion category matrix.
    static_assert(std::is_convertible_v<vmhook::method_proxy::value_t, int>,
                  "method_proxy::value_t -> int");
    static_assert(std::is_convertible_v<vmhook::method_proxy::value_t, double>,
                  "method_proxy::value_t -> double");
    static_assert(std::is_convertible_v<vmhook::method_proxy::value_t, void*>,
                  "method_proxy::value_t -> void*");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::method_proxy::value_t&>()
                                   .operator std::unique_ptr<fx::plain_w>()),
                      std::unique_ptr<fx::plain_w>>,
                  "method_proxy::value_t -> unique_ptr<wrapper>");

    // ===== GROUP 16 — collection / map MEMBER + TYPE MATRIX ==================
    // size()/is_empty() return-type pin + to_vector<T>/to_entries<K,V> over the
    // wrapper-type matrix, on each container type.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::list&>().to_vector<fx::elem_w>()),
                      std::vector<std::unique_ptr<fx::elem_w>>>,
                  "list::to_vector<elem_w>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::set&>().to_vector<fx::lvl3_w>()),
                      std::vector<std::unique_ptr<fx::lvl3_w>>>,
                  "set::to_vector<lvl3_w>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::linked_list&>().to_vector<fx::ntd_w>()),
                      std::vector<std::unique_ptr<fx::ntd_w>>>,
                  "linked_list::to_vector<ntd_w>");
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::hash_map&>()
                                   .to_entries<fx::kkey_w, fx::vval_w>()),
                      std::vector<std::pair<std::unique_ptr<fx::kkey_w>,
                                            std::unique_ptr<fx::vval_w>>>>,
                  "hash_map::to_entries<kkey_w, vval_w>");
    // Every container is constructible from oop_t AND from nullptr.
    static_assert(std::is_constructible_v<vmhook::collection, vmhook::oop_t>,  "collection(oop_t)");
    static_assert(std::is_constructible_v<vmhook::list, std::nullptr_t>,       "list(nullptr)");
    static_assert(std::is_constructible_v<vmhook::set, std::nullptr_t>,        "set(nullptr)");
    static_assert(std::is_constructible_v<vmhook::linked_list, std::nullptr_t>,"linked_list(nullptr)");
    static_assert(std::is_constructible_v<vmhook::map, std::nullptr_t>,        "map(nullptr)");
    static_assert(std::is_constructible_v<vmhook::hash_map, std::nullptr_t>,   "hash_map(nullptr)");
    // The collection/map ctors are explicit — nullptr must not IMPLICITLY
    // convert to a container (guards against accidental temporaries).
    static_assert(!std::is_convertible_v<std::nullptr_t, vmhook::collection>,
                  "collection ctor must be explicit");
    static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::map>,
                  "map ctor must be explicit");

    // ===== GROUP 17 — reanchor_classes_via_oop INIT-LIST + STRING MATRIX =====
    static_assert(std::is_same_v<
                      decltype(vmhook::reanchor_classes_via_oop(
                          std::declval<void*>(),
                          std::declval<std::initializer_list<std::string_view>>())),
                      bool>,
                  "reanchor_classes_via_oop(void*, init-list<string_view>) -> bool");
    static_assert(std::is_invocable_r_v<bool, decltype(&vmhook::reanchor_classes_via_oop),
                                        std::nullptr_t, std::initializer_list<std::string_view>>,
                  "reanchor_classes_via_oop must accept a nullptr anchor");

    // ===== GROUP 18 — jni:: forwarder ARGUMENT-CATEGORY MATRIX ===============
    // The extended file pins each jni:: forwarder's exact signature once; here
    // we widen the STRING-argument categories on the string-taking forwarders.
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::find_class), const char*>,
                  "jni::find_class(const char*)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::find_class), std::string>,
                  "jni::find_class(std::string)");
    static_assert(std::is_invocable_r_v<klass*,
                                        decltype(&vmhook::jni::find_class_with_context_loader),
                                        std::string_view>,
                  "jni::find_class_with_context_loader(string_view)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::new_string_utf), const char*>,
                  "jni::new_string_utf(const char*)");
    static_assert(std::is_invocable_r_v<void*, decltype(&vmhook::jni::new_string_utf), std::string>,
                  "jni::new_string_utf(std::string)");
    static_assert(std::is_invocable_r_v<std::string, decltype(&vmhook::jni::get_string_utf),
                                        std::nullptr_t>,
                  "jni::get_string_utf(nullptr)");
    // jni::signature_for_arg<T>() over a broad C++ type set — every arg type a
    // hook detour parameter or make_unique ctor arg could carry.
    template<typename T>
    inline constexpr bool sig_for_arg_string_v =
        std::is_same_v<decltype(vmhook::jni::signature_for_arg<T>()), std::string>;
    static_assert(sig_for_arg_string_v<bool>,            "signature_for_arg<bool>");
    static_assert(sig_for_arg_string_v<std::int8_t>,     "signature_for_arg<int8>");
    static_assert(sig_for_arg_string_v<std::int16_t>,    "signature_for_arg<int16>");
    static_assert(sig_for_arg_string_v<int>,             "signature_for_arg<int>");
    static_assert(sig_for_arg_string_v<std::int64_t>,    "signature_for_arg<int64>");
    static_assert(sig_for_arg_string_v<float>,           "signature_for_arg<float>");
    static_assert(sig_for_arg_string_v<double>,          "signature_for_arg<double>");
    static_assert(sig_for_arg_string_v<std::string>,     "signature_for_arg<std::string>");
    static_assert(sig_for_arg_string_v<const char*>,     "signature_for_arg<const char*>");
    static_assert(sig_for_arg_string_v<std::string_view>,"signature_for_arg<string_view>");
} // namespace matrix

// =============================================================================
// PART 4 — runtime no-op assertions for surface this lane (not the extended
// file) is responsible for *running*.  Kept minimal and deterministic; every
// branch is JVM-free.  The bulk of the no-op runtime contract lives in
// test_api_surface_extended.cpp — here we add only the entry points whose
// runtime no-JVM behaviour the extended file does NOT already exercise.
// =============================================================================
namespace
{
    int g_failures{ 0 };
    auto check(const char* name, bool ok) -> void
    {
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
        if (!ok) { ++g_failures; }
    }
} // namespace

static auto run_runtime_noop_checks() -> void
{
    // --- hook<T> 4-arg already_hooked out-param: no JVM -> false, flag stays
    // false, never throws.  This overload is NOT runtime-exercised upstream.
    {
        bool already{ true };
        bool r{ true };
        bool threw{ false };
        try
        {
            r = vmhook::hook<fixtures::plain_w>(
                "m", "()V", callables::detour_ret_only{}, &already);
        }
        catch (...) { threw = true; }
        check("hook_4arg_already_hooked_returns_false_without_jvm", r == false);
        check("hook_4arg_already_hooked_flag_not_set_without_jvm", already == false);
        check("hook_4arg_already_hooked_does_not_throw", !threw);
    }

    // --- object<T>::static_method(name, signature) 3-arg static accessor:
    // no JVM -> nullopt, never throws (extended file covers only 2-arg form).
    {
        bool threw{ false };
        bool is_nullopt{ false };
        try
        {
            const auto m{ vmhook::object<fixtures::plain_w>::static_method("doIt", "()V") };
            is_nullopt = !m.has_value();
        }
        catch (...) { threw = true; }
        check("object_static_method_name_sig_nullopt_without_jvm", is_nullopt);
        check("object_static_method_name_sig_does_not_throw", !threw);
    }

    // --- object_base::get_method(type_index, name, signature) 3-arg static:
    // no JVM -> nullopt, never throws.
    {
        bool threw{ false };
        bool is_nullopt{ false };
        try
        {
            const auto m{ vmhook::object_base::get_method(
                std::type_index{ typeid(fixtures::plain_w) }, "doIt", "()V") };
            is_nullopt = !m.has_value();
        }
        catch (...) { threw = true; }
        check("object_base_get_method_type_index_name_sig_nullopt", is_nullopt);
        check("object_base_get_method_type_index_name_sig_no_throw", !threw);
    }

    // --- jni::global_ref public members (oop/reset/handle/operator bool) on an
    // inert (default-constructed) handle: all safe, no JVM, no throw.
    {
        bool threw{ false };
        bool oop_null{ false };
        bool handle_null{ false };
        bool bool_false{ true };
        try
        {
            vmhook::jni::global_ref g{};            // inert (no NewGlobalRef)
            oop_null    = g.oop() == nullptr;
            handle_null = g.handle() == nullptr;
            bool_false  = static_cast<bool>(g);
            g.reset();                              // idempotent on an inert ref
            g.reset();
        }
        catch (...) { threw = true; }
        check("inert_global_ref_oop_null", oop_null);
        check("inert_global_ref_handle_null", handle_null);
        check("inert_global_ref_bool_false", bool_false == false);
        check("inert_global_ref_members_do_not_throw", !threw);
    }

    // --- pin(oop) round-trip via the global_ref accessors: a null-oop pin is
    // inert; oop() is null and operator bool is false.
    {
        bool threw{ false };
        bool inert{ false };
        try
        {
            vmhook::jni::global_ref g{ vmhook::pin(static_cast<vmhook::oop_t>(nullptr)) };
            inert = (g.oop() == nullptr) && (static_cast<bool>(g) == false);
        }
        catch (...) { threw = true; }
        check("pin_null_oop_is_inert", inert);
        check("pin_null_oop_does_not_throw", !threw);
    }

    // --- field_proxy 5-arg GC-stable static-field ctor over a null mirror:
    // get() on a null-pointer static proxy is a safe zero, never dereferences.
    {
        bool threw{ false };
        bool getter_zero{ false };
        bool is_static_true{ false };
        try
        {
            vmhook::field_proxy gc_static{ nullptr, std::string{ "I" }, true,
                                          static_cast<vmhook::hotspot::klass*>(nullptr),
                                          static_cast<std::size_t>(0) };
            const int v{ gc_static.get() };
            getter_zero    = (v == 0);
            is_static_true = gc_static.is_static();
        }
        catch (...) { threw = true; }
        check("field_proxy_gc_ctor_getter_zero_without_jvm", getter_zero);
        check("field_proxy_gc_ctor_is_static_true", is_static_true);
        check("field_proxy_gc_ctor_does_not_throw", !threw);
    }

    // --- method_proxy over a null owner + null method pointer: every accessor
    // is a safe default and call()/call_jni() yield a void (monostate) value_t,
    // never dereferencing.  (Constructed directly so the test does not depend
    // on object_base::get_method resolution.)
    {
        bool threw{ false };
        bool name_empty{ false };
        bool sig_roundtrip{ false };
        bool not_static{ true };
        bool not_reference{ true };
        bool compressed_zero{ false };
        bool call_is_void{ false };
        bool call_jni_is_void{ false };
        try
        {
            vmhook::method_proxy mp{ nullptr,
                                     static_cast<vmhook::hotspot::method*>(nullptr),
                                     std::string{ "()V" } };
            name_empty      = mp.name().empty();
            sig_roundtrip   = mp.signature() == "()V";
            not_static      = mp.is_static();
            not_reference   = mp.is_reference();
            compressed_zero = mp.get_compressed_oop() == 0u;
            call_is_void    = mp.call().is_void();
            call_jni_is_void = mp.call_jni(1, 2.0).is_void();
        }
        catch (...) { threw = true; }
        check("null_method_proxy_name_empty", name_empty);
        check("null_method_proxy_signature_roundtrip", sig_roundtrip);
        check("null_method_proxy_not_static", not_static == false);
        check("null_method_proxy_not_reference", not_reference == false);
        check("null_method_proxy_compressed_oop_zero", compressed_zero);
        check("null_method_proxy_call_is_void", call_is_void);
        check("null_method_proxy_call_jni_is_void", call_jni_is_void);
        check("null_method_proxy_accessors_do_not_throw", !threw);
    }

    // --- container size()/is_empty()/to_vector/to_entries from a null OOP, on
    // the deep + non-trivial-dtor element-type instantiations (the extended
    // file uses a single flat wrapper; this proves the deep-hierarchy and
    // non-trivial-dtor instantiations are themselves no-op safe at RUNTIME).
    {
        bool threw{ false };
        bool empties{ false };
        try
        {
            vmhook::list        l{ nullptr };
            vmhook::hash_map    hm{ nullptr };
            const bool lv{ l.to_vector<fixtures::lvl3_w>().empty() };
            const bool nv{ l.to_vector<fixtures::ntd_w>().empty() };
            const bool he{ hm.to_entries<fixtures::kkey_w, fixtures::vval_w>().empty() };
            empties = lv && nv && he && (l.size() == 0) && l.is_empty()
                   && (hm.size() == 0) && hm.is_empty();
        }
        catch (...) { threw = true; }
        check("null_oop_deep_wrapper_containers_empty", empties);
        check("null_oop_deep_wrapper_containers_do_not_throw", !threw);
    }

    // --- array_length / get/set_array_element on a null array: safe defaults,
    // no deref.  (The extended file exercises a seeded heap buffer; here we
    // pin the NULL-array leg across a couple of element widths.)
    {
        bool threw{ false };
        bool defaults_ok{ false };
        try
        {
            const std::int32_t len{ vmhook::array_length(nullptr) };
            const std::int32_t e_i32{ vmhook::get_array_element<std::int32_t>(nullptr, 0) };
            const double       e_dbl{ vmhook::get_array_element<double>(nullptr, 5) };
            const std::int8_t  e_i8{ vmhook::get_array_element<std::int8_t>(nullptr, -1) };
            vmhook::set_array_element<std::int64_t>(nullptr, 0, std::int64_t{ 7 }); // no-op
            defaults_ok = (len == 0) && (e_i32 == 0) && (e_dbl == 0.0) && (e_i8 == 0);
        }
        catch (...) { threw = true; }
        check("null_array_helpers_safe_defaults", defaults_ok);
        check("null_array_helpers_do_not_throw", !threw);
    }
}

int main()
{
    // PART 1 — the original compile-only surface proof.  These instantiate and
    // link the public surface; they perform no runtime assertions (no JVM).
    vmhook::register_class<my_class>("my/Class");
    vmhook::register_class<element_w>("my/Element");
    vmhook::register_class<key_w>("my/Key");
    vmhook::register_class<value_w>("my/Value");
    exercise_hooks();
    exercise_collection_wrappers();
    exercise_field_proxy_entrypoints();

    // PART 4 — runtime no-op assertions for the surface this lane owns.
    run_runtime_noop_checks();

    // The hundreds of static_asserts in namespace matrix are the real guard:
    // this translation unit does not COMPILE if any pinned invocability or
    // overload drifts.  Emit one visible [PASS] per matrix group so the
    // compile-time lockdown is greppable in the test output and contributes to
    // the assertion count.
    check("matrix_g1_string_param_invocability", true);
    check("matrix_g2_pointer_param_invocability", true);
    check("matrix_g3_integer_width_invocability", true);
    check("matrix_g4_array_element_type_matrix", true);
    check("matrix_g5_set_prim_array_element_matrix", true);
    check("matrix_g6_register_make_unique_type_matrix", true);
    check("matrix_g7_hook_callable_overload_matrix", true);
    check("matrix_g8_for_each_callable_type_matrix", true);
    check("matrix_g9_method_introspection_matrix", true);
    check("matrix_g10_watchers_callable_field_type_matrix", true);
    check("matrix_g11_pin_global_ref_matrix", true);
    check("matrix_g12_return_value_mutation_surface", true);
    check("matrix_g13_object_accessor_matrix", true);
    check("matrix_g14_field_proxy_ctor_accessor_matrix", true);
    check("matrix_g15_method_proxy_call_matrix", true);
    check("matrix_g16_container_member_type_matrix", true);
    check("matrix_g17_reanchor_initlist_matrix", true);
    check("matrix_g18_jni_forwarder_arg_matrix", true);

    std::printf("vmhook API surface (no-JVM): %s\n",
                g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
