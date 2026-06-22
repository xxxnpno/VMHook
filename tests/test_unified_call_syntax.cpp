// =============================================================================
// EXHAUSTIVE compile-time (no-JVM) coverage for the vmhook *unified call syntax*
// — the overload-resolution / value-conversion surface that lets a single
// spelling drive both the instance path and the static path on
// `vmhook::object<derived>`:
//
//     get_field("name")              get_method("name")   get_method("name","sig")
//     static_field("name")           static_method("name")  static_method("name","sig")
//
// plus the dispatch ends those resolve to:
//     field_proxy::get()  ->  field_proxy::value_t  (constrained operator T())
//     method_proxy::call(args...)  ->  method_proxy::value_t
//     field_proxy::set(value)
//
// WHY THIS FILE IS PURE static_assert / type-trait PROBES
// -------------------------------------------------------
// There is no JVM in this translation unit, so we cannot (and must not) invoke
// any of the above — they resolve klasses, read mirrors, and dispatch into the
// interpreter.  Instead every check here is a ZERO-RUNTIME compile-time probe of
// the *overload-resolution contract*: which overload a given argument shape
// selects, which conversion targets a value_t offers, and which ill-formed
// shapes the surface rejects.  This runs on every OS/compiler in CI and can
// never flake.  Runtime variant→C++ value conversion is owned by
// test_field_proxy_value_conversions.cpp / test_method_proxy_value_t.cpp and is
// deliberately NOT duplicated here.
//
// ROBUST ACROSS BOTH VMHOOK_HAS_DEDUCING_THIS STATES
// --------------------------------------------------
// The C++23 deducing-this overloads are gated by VMHOOK_HAS_DEDUCING_THIS
// (vmhook.hpp): true only on MSVC and non-NDK Clang < 20, false on GCC / Android
// NDK Clang / Clang >= 20.  The two states differ in HOW the same spelling
// resolves:
//
//   gate ON  : instance get_field/get_method take `char const*` (deducing-this
//              explicit-object members); a SEPARATE same-name `std::string_view`
//              STATIC overload exists and is selected in a static-call context.
//   gate OFF : instance get_field/get_method are the inherited `std::string_view`
//              members (brought in via using-declarations); NO same-name static
//              overload exists — static context must use static_field /
//              static_method.
//
// Historically this file tripped the clang-20 regression where a static-context
// `get_field("x")` bound the deducing-this explicit-object parameter instead of
// the string_view static.  The checks below are written so they assert the
// CORRECT contract in BOTH states and would have caught that regression: the
// instance idiom compiles unconditionally, the portable static accessors compile
// unconditionally, and the gate-specific static-call behaviour is asserted with
// `#if VMHOOK_HAS_DEDUCING_THIS`-aware detectors rather than assumed.
// =============================================================================

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// -----------------------------------------------------------------------------
// 0. Detector toolkit
// -----------------------------------------------------------------------------
// We need to ask, at compile time and SFINAE-friendly, "is expression E
// well-formed?" for each call shape.  std::is_invocable only works for the
// `call(...)` member (a real callable object after a member-access); the
// get_field / static_field family are member functions whose viability we probe
// with bespoke detector concepts.

namespace probe
{
    using vmhook::field_proxy;
    using vmhook::method_proxy;
    using field_opt  = std::optional<field_proxy>;
    using method_opt = std::optional<method_proxy>;

    // ---- INSTANCE-context detectors (need a live object expression) ---------
    // `obj.get_field(arg)` well-formed?
    template<typename obj_t, typename arg_t>
    concept has_instance_get_field =
        requires(obj_t& obj, arg_t arg) { { obj.get_field(arg) } -> std::same_as<field_opt>; };

    template<typename obj_t, typename arg_t>
    concept has_instance_get_method =
        requires(obj_t& obj, arg_t arg) { { obj.get_method(arg) } -> std::same_as<method_opt>; };

    template<typename obj_t, typename name_t, typename sig_t>
    concept has_instance_get_method2 =
        requires(obj_t& obj, name_t n, sig_t s) { { obj.get_method(n, s) } -> std::same_as<method_opt>; };

    // ---- STATIC-context detectors (NO object — name the member on the type) -
    // `wrapper_t::get_field(arg)` well-formed *as an unqualified-by-object call*?
    // This is exactly the static-call context that distinguishes the two gate
    // states: gate ON exposes a string_view STATIC get_field here; gate OFF does
    // not (the inherited member is non-static, so the type-qualified call without
    // an object is ill-formed).
    template<typename wrapper_t, typename arg_t>
    concept has_static_get_field =
        requires(arg_t arg) { { wrapper_t::get_field(arg) } -> std::same_as<field_opt>; };

    template<typename wrapper_t, typename arg_t>
    concept has_static_get_method =
        requires(arg_t arg) { { wrapper_t::get_method(arg) } -> std::same_as<method_opt>; };

    template<typename wrapper_t, typename name_t, typename sig_t>
    concept has_static_get_method2 =
        requires(name_t n, sig_t s) { { wrapper_t::get_method(n, s) } -> std::same_as<method_opt>; };

    // ---- Portable static accessors (must exist on EVERY toolchain) ----------
    template<typename wrapper_t, typename arg_t>
    concept has_static_field_accessor =
        requires(arg_t arg) { { wrapper_t::static_field(arg) } -> std::same_as<field_opt>; };

    template<typename wrapper_t, typename arg_t>
    concept has_static_method_accessor =
        requires(arg_t arg) { { wrapper_t::static_method(arg) } -> std::same_as<method_opt>; };

    template<typename wrapper_t, typename name_t, typename sig_t>
    concept has_static_method_accessor2 =
        requires(name_t n, sig_t s) { { wrapper_t::static_method(n, s) } -> std::same_as<method_opt>; };

    // ---- value_t conversion-target detector ---------------------------------
    // True iff `static_cast<target_t>(some value_t)` is well-formed, i.e. the
    // CONSTRAINED operator target_t() is in the overload set.  Because the
    // operator is gated by a `requires` clause (value_t_convertible_target_v),
    // an excised target makes this FALSE via SFINAE — no hard error.
    template<typename value_t, typename target_t>
    concept value_convertible_to = std::is_convertible_v<value_t, target_t>;
}

// -----------------------------------------------------------------------------
// 1. Wrapper types under test
// -----------------------------------------------------------------------------
// The canonical CRTP shape every vmhook user writes.
class wrapper_class : public vmhook::object<wrapper_class>
{
public:
    explicit wrapper_class(vmhook::oop_t oop) noexcept
        : vmhook::object<wrapper_class>{ oop }
    {
    }

    // ---- INSTANCE call sites — MUST compile on every toolchain --------------
    // The clean `get_field("x")->get()` idiom the library documents as the
    // user-facing spelling (no defensive has_value()).
    auto inst_get()            -> int { return get_field("a")->get(); }
    auto inst_get_method()     -> int { return get_method("m")->call(); }
    auto inst_get_method_sig() -> int { return get_method("m", "()I")->call(); }

    // Same instance idiom exercised through a non-int target to make sure the
    // value_t conversion participates (operator bool / operator std::string).
    auto inst_get_bool()       -> bool        { return get_field("flag")->get(); }
    // value_t::as_string() is the unambiguous String extraction (called on the
    // value_t that get() returns, NOT on the field_proxy itself).
    auto inst_get_string()     -> std::string { return get_field("name")->get().as_string(); }

    // ---- Instance call(...) argument forwarding from a member context -------
    // Drives method_proxy::call across a representative set of argument shapes
    // so the forwarding compiles from inside a wrapper (the documented usage).
    auto inst_call_void()      -> void { get_method("v")->call(); }
    auto inst_call_int()       -> void { get_method("i")->call(42); }
    auto inst_call_long()      -> void { get_method("j")->call(std::int64_t{ 42 }); }
    auto inst_call_double()    -> void { get_method("d")->call(3.14); }
    auto inst_call_bool()      -> void { get_method("z")->call(true); }
    // c-string arg is passed as a `const char*` value (not a bare string-literal
    // array): a string literal forwarded into call() instantiates the library's
    // `arg ? NewStringUTF : nullptr` ternary on a `const char(&)[N]` whose address
    // is never null, which the header reports as -Waddress.  The documented
    // c-string spelling is `const char*`; using it keeps the warning-clean path
    // while still exercising the c-string -> Java String branch.  (The pointer
    // value category is also probed in Section 4d via call_ok<const char*>.)
    auto inst_call_cstr()      -> void { const char* p{ "hello" }; get_method("s")->call(p); }
    auto inst_call_string()    -> void { const std::string s{ "hi" }; get_method("s")->call(s); }
    auto inst_call_multi()     -> void { const char* p{ "x" }; get_method("m")->call(1, 2.0, true, p); }

    // ---- Instance field write ----------------------------------------------
    auto inst_set_int()        -> void { get_field("a")->set(99); }
    auto inst_set_string()     -> void { get_field("name")->set(std::string{ "x" }); }

#if VMHOOK_HAS_DEDUCING_THIS
    // ---- STATIC call sites via the deducing-this fallback -------------------
    // Only required to compile where the C++23 deducing-this feature is on; the
    // overload set in a static-call context falls through to the string_view
    // static get_field/get_method.  Portable equivalents are below.
    static auto stat_get_field()      -> int { return get_field("a")->get(); }
    static auto stat_get_method()     -> int { return get_method("m")->call(); }
    static auto stat_get_method_sig() -> int { return get_method("m", "()I")->call(); }
#endif

    // ---- Portable static call sites — MUST compile on every toolchain -------
    static auto portable_get_field()      -> int { return static_field("a")->get(); }
    static auto portable_get_method()     -> int { return static_method("m")->call(); }
    static auto portable_get_method_sig() -> int { return static_method("m", "()I")->call(); }
};

// A second wrapper to exercise the value_t -> unique_ptr<W> argument/return arm
// (convert_jni_arg requires the wrapped type derive from object_base).
class other_wrapper : public vmhook::object<other_wrapper>
{
public:
    using vmhook::object<other_wrapper>::object;
};

// The DELIBERATELY-misused default-argument instantiation: vmhook::object<>
// defaults `derived` to void (vmhook.hpp forward decl).  CRTP is meant to be
// object<Self>; the `= void` default makes object<> compile, which means the
// static accessors compute typeid(void) and resolve nothing at runtime.  We
// only assert the STRUCTURAL surface here (the type is usable / the accessors
// are still present), characterising that the misuse is not a compile error —
// see Section 9.
class void_defaulted : public vmhook::object<>
{
public:
    using vmhook::object<>::object;
};

// -----------------------------------------------------------------------------
// 2. INSTANCE call surface — exhaustive, gate-INDEPENDENT
// -----------------------------------------------------------------------------
// On BOTH gate states the instance `get_field` / `get_method` family must be
// callable on a live object with each argument shape that names a field/method.
// gate ON  -> deducing-this `char const*` member;
// gate OFF -> inherited `std::string_view` member (using-decl).
// Either way, all of these must be well-formed and yield the optional proxy.

// String-LITERAL argument (the canonical spelling).  char const* literal:
static_assert(probe::has_instance_get_field<wrapper_class, const char*>,
              "instance get_field(const char*) must be callable on every toolchain "
              "(deducing-this char const* member, or inherited string_view member)");
static_assert(probe::has_instance_get_method<wrapper_class, const char*>,
              "instance get_method(const char*) must be callable on every toolchain");
static_assert(probe::has_instance_get_method2<wrapper_class, const char*, const char*>,
              "instance get_method(const char*, const char*) must be callable everywhere");

// A char[] (array, decays to char const*) — the type of an actual string literal
// before decay — must also bind the instance overload.
static_assert(probe::has_instance_get_field<wrapper_class, char[2]>,
              "instance get_field must accept a char array (literal) argument");
static_assert(probe::has_instance_get_method2<wrapper_class, char[2], char[4]>,
              "instance get_method(name, sig) must accept char-array arguments");

// std::string_view argument: well-formed in BOTH gates, but via a DIFFERENT
// overload — instance member when gate OFF, the static string_view fallback when
// gate ON (a static member is still reachable through `obj.member`).  Either
// way the call expression is valid and returns the optional proxy.
static_assert(probe::has_instance_get_field<wrapper_class, std::string_view>,
              "get_field(string_view) must be a valid expression on a live object "
              "in both gate states (instance member OR static fallback)");
static_assert(probe::has_instance_get_method<wrapper_class, std::string_view>,
              "get_method(string_view) must be a valid expression on a live object");
static_assert(probe::has_instance_get_method2<wrapper_class, std::string_view, std::string_view>,
              "get_method(string_view, string_view) must be valid on a live object");

// std::string lvalue argument (implicitly converts to string_view): valid too.
static_assert(probe::has_instance_get_field<wrapper_class, std::string&>,
              "get_field(std::string) must be a valid expression on a live object");
static_assert(probe::has_instance_get_method2<wrapper_class, std::string&, std::string&>,
              "get_method(std::string, std::string) must be valid on a live object");

// Return types of the instance overloads are EXACTLY the proxy optionals.
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_field("a")),
                  std::optional<vmhook::field_proxy>>,
              "instance get_field must return std::optional<field_proxy>");
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method("m")),
                  std::optional<vmhook::method_proxy>>,
              "instance get_method must return std::optional<method_proxy>");
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method("m", "()I")),
                  std::optional<vmhook::method_proxy>>,
              "instance get_method(name, sig) must return std::optional<method_proxy>");

// The clean idiom `get_field("x")->get()` must be well-formed end to end: the
// optional is dereferenced to a field_proxy and get() yields a value_t that
// converts to the documented scalar targets.  (No JVM call happens in an
// unevaluated context.)
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_field("a")->get()),
                  vmhook::field_proxy::value_t>,
              "get_field(\"x\")->get() must yield field_proxy::value_t");
static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, int>,
              "the value_t from get()->... must convert to int (the documented "
              "get_field(\"x\")->get() one-liner returning int)");
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method("m")->call()),
                  vmhook::method_proxy::value_t>,
              "get_method(\"m\")->call() must yield method_proxy::value_t");

// -----------------------------------------------------------------------------
// 3. STATIC-context surface — gate-AWARE
// -----------------------------------------------------------------------------
// 3a. PORTABLE accessors — present on EVERY toolchain, every argument spelling.
static_assert(probe::has_static_field_accessor<wrapper_class, const char*>,
              "static_field(const char*) must exist on every toolchain");
static_assert(probe::has_static_field_accessor<wrapper_class, std::string_view>,
              "static_field(string_view) must exist on every toolchain");
static_assert(probe::has_static_field_accessor<wrapper_class, std::string&>,
              "static_field(std::string) must exist on every toolchain");
static_assert(probe::has_static_method_accessor<wrapper_class, const char*>,
              "static_method(const char*) must exist on every toolchain");
static_assert(probe::has_static_method_accessor<wrapper_class, std::string_view>,
              "static_method(string_view) must exist on every toolchain");
static_assert(probe::has_static_method_accessor2<wrapper_class, const char*, const char*>,
              "static_method(name, sig) must exist on every toolchain");
static_assert(probe::has_static_method_accessor2<wrapper_class, std::string_view, std::string_view>,
              "static_method(string_view, string_view) must exist on every toolchain");

// Portable accessor return types.
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_field("a")),
                  std::optional<vmhook::field_proxy>>,
              "static_field must return std::optional<field_proxy>");
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_method("m", "()I")),
                  std::optional<vmhook::method_proxy>>,
              "static_method(name, sig) must return std::optional<method_proxy>");

// The portable static idiom end to end (mirrors the instance idiom).
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_field("a")->get()),
                  vmhook::field_proxy::value_t>,
              "static_field(\"x\")->get() must yield field_proxy::value_t");
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_method("m")->call()),
                  vmhook::method_proxy::value_t>,
              "static_method(\"m\")->call() must yield method_proxy::value_t");

// 3b. The same-name STATIC get_field/get_method fallbacks: their EXISTENCE is
// the precise structural difference between the two gate states.  Asserting it
// both ways is what would have caught the clang-20 regression (where a static
// `get_field("x")` stopped selecting the string_view static and the contract
// silently changed).
#if VMHOOK_HAS_DEDUCING_THIS
    // gate ON: a string_view static get_field/get_method exists and is callable
    // WITHOUT an object.  A const char* literal there also resolves (it converts
    // to string_view) — that is the uniform-static-call property the feature
    // promises on MSVC / clang<20.
    static_assert(probe::has_static_get_field<wrapper_class, std::string_view>,
                  "[gate ON] string_view static get_field must be callable without an object");
    static_assert(probe::has_static_get_field<wrapper_class, const char*>,
                  "[gate ON] static get_field(const char* literal) must resolve to the "
                  "string_view static fallback (the unified static-call spelling)");
    static_assert(probe::has_static_get_method<wrapper_class, std::string_view>,
                  "[gate ON] string_view static get_method must be callable without an object");
    static_assert(probe::has_static_get_method2<wrapper_class, std::string_view, std::string_view>,
                  "[gate ON] string_view static get_method(name, sig) must be callable without an object");
    static_assert(probe::has_static_get_method2<wrapper_class, const char*, const char*>,
                  "[gate ON] static get_method(name, sig) literals must resolve to the static fallback");

    // The gated static fallback and the portable accessor have byte-identical
    // bodies (both call object_base::get_field(typeid(derived), name)); assert
    // they return the same type so the two spellings stay interchangeable.
    static_assert(std::is_same_v<
                      decltype(wrapper_class::get_field(std::string_view{ "a" })),
                      decltype(wrapper_class::static_field(std::string_view{ "a" }))>,
                  "[gate ON] gated static get_field and portable static_field must "
                  "return the same type (their bodies are identical)");
#else
    // gate OFF (GCC / Android NDK Clang / Clang>=20): there is NO same-name
    // static get_field/get_method.  `wrapper_class::get_field(sv)` without an
    // object is ILL-FORMED (the inherited member is non-static).  Authors there
    // must use static_field / static_method — which we asserted exist in 3a.
    static_assert(!probe::has_static_get_field<wrapper_class, std::string_view>,
                  "[gate OFF] there must be NO static get_field — static context "
                  "must go through static_field()");
    static_assert(!probe::has_static_get_method<wrapper_class, std::string_view>,
                  "[gate OFF] there must be NO static get_method — static context "
                  "must go through static_method()");
    static_assert(!probe::has_static_get_method2<wrapper_class, std::string_view, std::string_view>,
                  "[gate OFF] there must be NO static get_method(name, sig)");
#endif

// -----------------------------------------------------------------------------
// 4. method_proxy::call(args...) — exhaustive ARGUMENT-SHAPE sweep
// -----------------------------------------------------------------------------
// call() is `template<class... Args> value_t call(Args&&...) const noexcept`:
// an unconstrained perfect-forwarding variadic.  std::is_invocable confirms a
// given argument tuple yields a viable call expression (it does NOT instantiate
// the body, so the inner convert_jni_arg static_assert for *unsupported* types
// is not a usable negative oracle — see the note at the bottom of this section).
// Every supported shape below MUST be invocable and yield value_t.

namespace call_probe
{
    using method_proxy = vmhook::method_proxy;
    using value_t      = vmhook::method_proxy::value_t;

    // A requires-expression on the ACTUAL call site `m.call(args...)` — the most
    // faithful probe (and immune to pointer-to-member-template quirks).  It
    // checks overload resolution + return type only; it does NOT instantiate
    // call()'s body, so a viable-but-unsupported arg type would still satisfy it
    // (see the negative-oracle note at the end of this section).  std::declval
    // yields each arg in the value category named by args_t.
    template<typename... args_t>
    concept callable_with =
        requires(const method_proxy& m) {
            { m.call(std::declval<args_t>()...) } -> std::same_as<value_t>;
        };

    template<typename... args_t>
    inline constexpr bool call_ok = callable_with<args_t...>;
}

// 4a. ZERO arguments (void Java method).
static_assert(call_probe::call_ok<>,
              "call() with no arguments must be a viable value_t-returning call");

// 4b. Each JVM primitive, by its C++ representative type.
static_assert(call_probe::call_ok<bool>,           "call(bool) -> Java boolean (.z slot)");
static_assert(call_probe::call_ok<std::int8_t>,    "call(int8_t) -> Java byte (.i slot)");
static_assert(call_probe::call_ok<std::uint8_t>,   "call(uint8_t) must be viable (integral <=4 bytes)");
static_assert(call_probe::call_ok<std::int16_t>,   "call(int16_t) -> Java short");
static_assert(call_probe::call_ok<std::uint16_t>,  "call(uint16_t) -> Java char width");
static_assert(call_probe::call_ok<char>,           "call(char) must be viable (1-byte integral)");
static_assert(call_probe::call_ok<char16_t>,       "call(char16_t) must be viable (2-byte integral)");
static_assert(call_probe::call_ok<std::int32_t>,   "call(int32_t) -> Java int (.i slot)");
static_assert(call_probe::call_ok<std::uint32_t>,  "call(uint32_t) must be viable (integral <=4 bytes)");
static_assert(call_probe::call_ok<std::int64_t>,   "call(int64_t) -> Java long (.j slot, two-slot)");
static_assert(call_probe::call_ok<std::uint64_t>,  "call(uint64_t) -> Java long width (two-slot)");
static_assert(call_probe::call_ok<float>,          "call(float) -> Java float (.f slot)");
static_assert(call_probe::call_ok<double>,         "call(double) -> Java double (.d slot, two-slot)");

// 4c. REFERENCE / CONST-qualified argument categories must all forward (the
// signature is Args&&..., and convert_jni_arg decays via std::decay_t).
static_assert(call_probe::call_ok<int&>,           "call(int&) lvalue must forward");
static_assert(call_probe::call_ok<const int&>,     "call(const int&) must forward");
static_assert(call_probe::call_ok<int&&>,          "call(int&&) rvalue must forward");
static_assert(call_probe::call_ok<const double&>,  "call(const double&) two-slot must forward");
static_assert(call_probe::call_ok<volatile int&>,  "call(volatile int&) must forward (decays away)");

// 4d. STRING shapes: std::string, std::string_view, const char*, char* — all
// route to the object/.l slot via NewStringUTF.
static_assert(call_probe::call_ok<std::string>,        "call(std::string) -> Java String");
static_assert(call_probe::call_ok<const std::string&>, "call(const std::string&) -> Java String");
static_assert(call_probe::call_ok<std::string_view>,   "call(std::string_view) -> Java String");
static_assert(call_probe::call_ok<const char*>,        "call(const char*) -> Java String (or null)");
static_assert(call_probe::call_ok<char*>,              "call(char*) -> Java String (or null)");

// 4e. nullptr passed as a C-STRING: the documented way to pass Java null is a
// TYPED null pointer (const char*)nullptr, which binds convert_jni_arg's
// const char* branch and yields .l = nullptr.  Assert that typed-null form is
// viable.  (A BARE `nullptr` argument is std::nullptr_t, for which
// convert_jni_arg has no branch and fires its internal static_assert when the
// body instantiates.  That rejection is NOT SFINAE-observable — call_ok is a
// requires-expression that does not instantiate call()'s body, so it would
// still report nullptr_t as "callable".  We therefore deliberately do NOT
// assert call_ok<std::nullptr_t> either way; see the negative-oracle note at
// the end of this section.)
static_assert(call_probe::call_ok<decltype((const char*)nullptr)>,
              "call((const char*)nullptr) — a TYPED null c-string — must be viable "
              "and maps to Java null");

// 4f. OBJECT-wrapper arguments: by value and via unique_ptr<W> (W : object_base).
static_assert(call_probe::call_ok<other_wrapper>,
              "call(object_base-derived by value) must forward (raw OOP handle into .l slot)");
static_assert(call_probe::call_ok<const other_wrapper&>,
              "call(const wrapper&) must forward");
static_assert(call_probe::call_ok<std::unique_ptr<other_wrapper>>,
              "call(unique_ptr<W>) must forward (W derives from object_base)");
static_assert(call_probe::call_ok<std::unique_ptr<wrapper_class>>,
              "call(unique_ptr<wrapper_class>) must forward");

// 4g. MULTI-argument tuples, including two-slot args interleaved with one-slot
// args (the long/double slot-width interplay) and mixed object/string/primitive.
static_assert(call_probe::call_ok<int, int>,
              "call(int, int) two-arg must be viable");
static_assert(call_probe::call_ok<int, std::int64_t, int>,
              "call(int, long, int) — long is two-slot between one-slot ints — must be viable");
static_assert(call_probe::call_ok<std::int64_t, std::int64_t, int>,
              "call(long, long, int) — back-to-back two-slot args — must be viable");
static_assert(call_probe::call_ok<double, int, double>,
              "call(double, int, double) — double two-slot interplay — must be viable");
static_assert(call_probe::call_ok<bool, std::int8_t, std::int16_t, std::int32_t,
                                  std::int64_t, float, double, std::string, const char*>,
              "call(every primitive + string + c-string together) must be viable");
static_assert(call_probe::call_ok<other_wrapper, std::string, int, double>,
              "call(object, string, int, double) mixed shapes must be viable");
static_assert(call_probe::call_ok<std::unique_ptr<other_wrapper>, const char*, std::int64_t>,
              "call(unique_ptr<W>, c-string, long) mixed shapes must be viable");

// 4h. call() return type is ALWAYS method_proxy::value_t, for any arity.
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::method_proxy&>().call()),
                  vmhook::method_proxy::value_t>,
              "call() must return method_proxy::value_t");
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::method_proxy&>().call(1, "x", 2.0)),
                  vmhook::method_proxy::value_t>,
              "call(args...) must return method_proxy::value_t regardless of arity");

// NOTE on the negative side: an UNSUPPORTED call argument (e.g. a bare
// std::nullptr_t, or a std::vector<int>, or a user POD) is rejected by a
// static_assert INSIDE convert_jni_arg, which only fires when the body is
// instantiated.  std::is_invocable does not instantiate the body, so it reports
// such a call as "invocable" (a viable signature exists).  We therefore cannot
// use is_invocable as a negative oracle for those; instantiating the body to
// prove rejection would be a hard compile error and break the build.  The
// rejection contract that IS SFINAE-observable lives on the value_t conversion
// targets — see Section 5.

// -----------------------------------------------------------------------------
// 5. value_t conversion-target SELECTION — positive AND negative
// -----------------------------------------------------------------------------
// Both field_proxy::value_t and method_proxy::value_t expose a CONSTRAINED
// `template<class T> operator T()  requires value_t_convertible_target_v<T>`.
// The constraint is the value-conversion overload-SELECTION gate: it admits the
// legitimate targets and SFINAE-excludes the spurious ones (nullptr_t and any
// non-void pointer).  is_convertible is a faithful, SFINAE-friendly probe of
// exactly that selection.

namespace conv_probe
{
    using fv = vmhook::field_proxy::value_t;
    using mv = vmhook::method_proxy::value_t;
    using probe::value_convertible_to;  // reuse the SFINAE-friendly detector
}

// 5a. LEGITIMATE targets — convertible from BOTH value_t flavours.
static_assert(conv_probe::value_convertible_to<conv_probe::fv, bool>,        "field value_t -> bool");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::int8_t>, "field value_t -> int8_t");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::int16_t>,"field value_t -> int16_t");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::int32_t>,"field value_t -> int32_t");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::int64_t>,"field value_t -> int64_t");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, float>,       "field value_t -> float");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, double>,      "field value_t -> double");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::uint16_t>,"field value_t -> uint16_t (char)");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::uint32_t>,"field value_t -> uint32_t (compressed OOP)");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::string>, "field value_t -> std::string");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, void*>,       "field value_t -> void* (the sole permitted pointer)");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::vector<int>>,    "field value_t -> std::vector<int>");
static_assert(conv_probe::value_convertible_to<conv_probe::fv, std::vector<std::string>>, "field value_t -> std::vector<string>");

static_assert(conv_probe::value_convertible_to<conv_probe::mv, bool>,        "method value_t -> bool");
static_assert(conv_probe::value_convertible_to<conv_probe::mv, std::int32_t>,"method value_t -> int32_t");
static_assert(conv_probe::value_convertible_to<conv_probe::mv, std::int64_t>,"method value_t -> int64_t");
static_assert(conv_probe::value_convertible_to<conv_probe::mv, float>,       "method value_t -> float");
static_assert(conv_probe::value_convertible_to<conv_probe::mv, double>,      "method value_t -> double");
static_assert(conv_probe::value_convertible_to<conv_probe::mv, std::string>, "method value_t -> std::string");
static_assert(conv_probe::value_convertible_to<conv_probe::mv, void*>,       "method value_t -> void*");
static_assert(conv_probe::value_convertible_to<conv_probe::mv, std::unique_ptr<other_wrapper>>,
              "method value_t -> unique_ptr<W> (Object-returning method into a wrapper)");

// 5b. EXCISED targets — the `requires` clause must REJECT these via SFINAE so a
// class target with competing constructors (std::string, unique_ptr<W>) is not
// ambiguous on MSVC /permissive-.  These are the documented ill-formed
// conversions; they must be non-convertible from value_t.
static_assert(!conv_probe::value_convertible_to<conv_probe::fv, std::nullptr_t>,
              "field value_t must NOT be convertible to std::nullptr_t (excised target)");
static_assert(!conv_probe::value_convertible_to<conv_probe::fv, const char*>,
              "field value_t must NOT be convertible to const char* (excised — would "
              "make static_cast<std::string> ambiguous)");
static_assert(!conv_probe::value_convertible_to<conv_probe::fv, char*>,
              "field value_t must NOT be convertible to char* (excised target)");
static_assert(!conv_probe::value_convertible_to<conv_probe::fv, other_wrapper*>,
              "field value_t must NOT be convertible to a raw wrapper pointer W* (excised)");
static_assert(!conv_probe::value_convertible_to<conv_probe::fv, int*>,
              "field value_t must NOT be convertible to a non-void pointer int*");

static_assert(!conv_probe::value_convertible_to<conv_probe::mv, std::nullptr_t>,
              "method value_t must NOT be convertible to std::nullptr_t (excised)");
static_assert(!conv_probe::value_convertible_to<conv_probe::mv, const char*>,
              "method value_t must NOT be convertible to const char* (excised)");
static_assert(!conv_probe::value_convertible_to<conv_probe::mv, char*>,
              "method value_t must NOT be convertible to char* (excised)");
static_assert(!conv_probe::value_convertible_to<conv_probe::mv, other_wrapper*>,
              "method value_t must NOT be convertible to a raw wrapper pointer W* (excised)");

// 5c. The underlying selection trait directly (the gate the operator uses).
static_assert(vmhook::detail::value_t_convertible_target_v<int>,            "trait: int is a legitimate target");
static_assert(vmhook::detail::value_t_convertible_target_v<std::string>,    "trait: std::string is legitimate");
static_assert(vmhook::detail::value_t_convertible_target_v<void*>,          "trait: void* is the one allowed pointer");
static_assert(vmhook::detail::value_t_convertible_target_v<const void*>,    "trait: const void* (cv void*) is legitimate");
static_assert(vmhook::detail::value_t_convertible_target_v<std::unique_ptr<other_wrapper>>,
              "trait: unique_ptr<W> is legitimate");
static_assert(!vmhook::detail::value_t_convertible_target_v<std::nullptr_t>, "trait: nullptr_t excised");
static_assert(!vmhook::detail::value_t_convertible_target_v<const char*>,    "trait: const char* excised");
static_assert(!vmhook::detail::value_t_convertible_target_v<other_wrapper*>, "trait: W* excised");
// cv-ref qualifiers must be stripped before classification.
static_assert(vmhook::detail::value_t_convertible_target_v<const std::string&>,
              "trait: cv-ref must be stripped — const std::string& is legitimate");
static_assert(!vmhook::detail::value_t_convertible_target_v<const char* const&>,
              "trait: cv-ref must be stripped — const char* const& is still excised");

// -----------------------------------------------------------------------------
// 6. field_proxy::set(value) — argument acceptance
// -----------------------------------------------------------------------------
// set() is `template<class V> void set(const V&) const noexcept` — the write
// side of the value-conversion surface.  Confirm each documented value shape
// yields a viable set() expression (it dispatches internally on V; like call(),
// only viability is probed here, not the runtime write).
namespace set_probe
{
    using field_proxy = vmhook::field_proxy;
    template<typename v_t>
    concept settable_with =
        requires(const field_proxy& f, const v_t& v) { { f.set(v) } -> std::same_as<void>; };

    template<typename v_t>
    inline constexpr bool set_ok = settable_with<v_t>;
}
static_assert(set_probe::set_ok<bool>,         "set(bool) must be viable");
static_assert(set_probe::set_ok<std::int8_t>,  "set(int8_t) must be viable");
static_assert(set_probe::set_ok<std::int16_t>, "set(int16_t) must be viable");
static_assert(set_probe::set_ok<std::int32_t>, "set(int32_t) must be viable");
static_assert(set_probe::set_ok<std::int64_t>, "set(int64_t) must be viable");
static_assert(set_probe::set_ok<float>,        "set(float) must be viable");
static_assert(set_probe::set_ok<double>,       "set(double) must be viable");
static_assert(set_probe::set_ok<std::string>,  "set(std::string) must be viable (String field write)");
static_assert(set_probe::set_ok<std::vector<int>>,
              "set(std::vector<int>) must be viable (array field write)");
static_assert(set_probe::set_ok<std::vector<std::string>>,
              "set(std::vector<std::string>) must be viable");

// -----------------------------------------------------------------------------
// 7. Gate-state structural invariants
// -----------------------------------------------------------------------------
// VMHOOK_HAS_DEDUCING_THIS is always defined and strictly boolean (0 or 1).
#if !defined(VMHOOK_HAS_DEDUCING_THIS)
#  error "VMHOOK_HAS_DEDUCING_THIS must always be defined by vmhook.hpp"
#endif
static_assert(VMHOOK_HAS_DEDUCING_THIS == 0 || VMHOOK_HAS_DEDUCING_THIS == 1,
              "VMHOOK_HAS_DEDUCING_THIS must be exactly 0 or 1");

// The CRTP inheritance contract every assertion above relies on.
static_assert(std::is_base_of_v<vmhook::object_base, wrapper_class>,
              "wrapper_class must derive from object_base (CRTP via object<wrapper_class>)");
static_assert(std::is_base_of_v<vmhook::object_base, other_wrapper>,
              "other_wrapper must derive from object_base");
static_assert(std::is_base_of_v<vmhook::object<wrapper_class>, wrapper_class>,
              "wrapper_class must derive from its CRTP base object<wrapper_class>");

// The two gate states are mutually exclusive in their static-context contract:
// the same-name static get_field exists IFF the gate is on.  This couples the
// macro to the observable surface so a future macro/skew can't pass silently.
#if VMHOOK_HAS_DEDUCING_THIS
static_assert(probe::has_static_get_field<wrapper_class, std::string_view>,
              "macro/surface coupling: gate ON implies a static get_field exists");
#else
static_assert(!probe::has_static_get_field<wrapper_class, std::string_view>,
              "macro/surface coupling: gate OFF implies NO static get_field");
#endif
// The portable accessors are the gate-INVARIANT: they exist in either state, so
// user code that always uses static_field/static_method is gate-agnostic.
static_assert(probe::has_static_field_accessor<wrapper_class, std::string_view>
              && probe::has_static_method_accessor<wrapper_class, std::string_view>,
              "static_field/static_method must exist irrespective of the gate state "
              "(the portable, gate-agnostic spelling)");

// -----------------------------------------------------------------------------
// 8. CHARACTERIZATION — the std::string-lvalue instance-context routing split
// -----------------------------------------------------------------------------
// This pins the most subtle real trap in the feature: from an INSTANCE context,
// a string LITERAL and a std::string/std::string_view LVALUE may resolve through
// DIFFERENT paths depending on the gate state.
//
//   gate ON  : the instance deducing-this overload takes `char const*`.  A
//              std::string_view/std::string lvalue does NOT match it, so the
//              call binds the STATIC string_view fallback instead — i.e. an
//              instance-context get_field(non_literal) routes through the
//              typeid(derived) static-mirror lookup, NOT the live OOP.  The
//              observable proxy of that is: the SAME call is ALSO valid as a
//              static (object-less) call.
//   gate OFF : the instance overload itself takes string_view, so a literal and
//              a string_view lvalue bind the SAME instance overload (live OOP);
//              there is no static get_field at all, so the object-less form is
//              ill-formed.
//
// We assert the observable consequence in each state rather than asserting which
// is "correct" — this is a characterization test so the behaviour cannot drift
// silently and any future fix is a deliberate, visible change here.
#if VMHOOK_HAS_DEDUCING_THIS
static_assert(probe::has_static_get_field<wrapper_class, std::string_view>,
              "[characterization, gate ON] a std::string_view name is ALSO accepted in "
              "static (object-less) context — i.e. an instance-context get_field(string_view) "
              "binds the STATIC string_view fallback (typeid(derived) path), not the "
              "char const* deducing-this instance overload.  Documented trap.");
#else
static_assert(!probe::has_static_get_field<wrapper_class, std::string_view>,
              "[characterization, gate OFF] there is NO static get_field; a string_view "
              "name from an instance context binds the inherited string_view instance "
              "overload (live OOP), and the object-less form is ill-formed.");
#endif
// In BOTH states the instance-context call expression itself is well-formed for
// every name spelling (literal / string_view / std::string) — only the SELECTED
// overload differs.  This is the half of the contract that is gate-invariant.
static_assert(probe::has_instance_get_field<wrapper_class, const char*>
              && probe::has_instance_get_field<wrapper_class, std::string_view>
              && probe::has_instance_get_field<wrapper_class, std::string&>,
              "instance-context get_field must be a valid expression for literal, "
              "string_view, and std::string names in BOTH gate states");

// -----------------------------------------------------------------------------
// 9. CHARACTERIZATION — object<> (derived == void) default
// -----------------------------------------------------------------------------
// The forward declaration defaults derived to void, so object<> / object<void>
// COMPILES (CRTP misuse is not a hard error).  The static accessors then compute
// typeid(void) and resolve nothing at runtime (a logged no-op).  We characterize
// the COMPILE-time surface: object<void> is usable and still exposes the portable
// accessors — there is no static_assert(!is_same_v<derived,void>) guarding it.
static_assert(std::is_base_of_v<vmhook::object_base, void_defaulted>,
              "object<> (derived==void) still derives from object_base — the void "
              "default makes the CRTP misuse COMPILE (no guard rejects it)");
static_assert(std::is_same_v<vmhook::object<>, vmhook::object<void>>,
              "object<> must be the same type as object<void> (the forward-decl default)");
static_assert(probe::has_static_field_accessor<void_defaulted, std::string_view>,
              "[characterization] object<void>-derived still exposes static_field — the "
              "typeid(void) lookup is a runtime no-op, NOT a compile error");
static_assert(probe::has_static_method_accessor<void_defaulted, std::string_view>,
              "[characterization] object<void>-derived still exposes static_method "
              "(runtime no-op via typeid(void))");
// object<void> exposes the gate-specific same-name static get_field/get_method
// exactly as the gate dictates (the typeid is computed at the *body*, which the
// detector never instantiates — so the SURFACE is identical to any other
// derived type).  Couples the void-default misuse to the gate just like a
// well-formed wrapper.
#if VMHOOK_HAS_DEDUCING_THIS
static_assert(probe::has_static_get_field<void_defaulted, std::string_view>,
              "[gate ON] object<void>-derived still exposes the same-name static "
              "get_field surface (typeid(void) only bites at the never-instantiated body)");
#else
static_assert(!probe::has_static_get_field<void_defaulted, std::string_view>,
              "[gate OFF] object<void>-derived has NO same-name static get_field, same as "
              "any other derived type");
#endif

// =============================================================================
// 10. INSTANCE get_method / get_method(name, sig) — full argument-shape matrix
// =============================================================================
// Section 2 exhausted the instance get_field argument shapes and gave get_method
// a representative few.  Here we complete the get_method side to the SAME breadth
// get_field already has: every name-spelling category for the 1-arg form, and the
// full NAME x SIG cross-product for the 2-arg form.  In BOTH gate states each of
// these is a well-formed call expression on a live object yielding the optional
// proxy (only the SELECTED overload differs — deducing-this char const* member vs
// the inherited / static string_view path).

// 1-arg instance get_method: every name-spelling category.
static_assert(probe::has_instance_get_method<wrapper_class, char[2]>,
              "instance get_method must accept a char array (literal) name");
static_assert(probe::has_instance_get_method<wrapper_class, const char[6]>,
              "instance get_method must accept a const char array name");
static_assert(probe::has_instance_get_method<wrapper_class, std::string&>,
              "instance get_method(std::string lvalue) must be a valid expression");
static_assert(probe::has_instance_get_method<wrapper_class, const std::string&>,
              "instance get_method(const std::string&) must be a valid expression");
static_assert(probe::has_instance_get_method<wrapper_class, const std::string_view&>,
              "instance get_method(const string_view&) must be a valid expression");

// 2-arg instance get_method: the full NAME x SIG category cross-product.  Each
// pairing must be a viable call on a live object in both gate states.
static_assert(probe::has_instance_get_method2<wrapper_class, const char*, const char*>,
              "instance get_method(const char*, const char*) must be valid");
static_assert(probe::has_instance_get_method2<wrapper_class, const char*, std::string_view>,
              "instance get_method(const char*, string_view) must be valid");
static_assert(probe::has_instance_get_method2<wrapper_class, std::string_view, const char*>,
              "instance get_method(string_view, const char*) must be valid");
static_assert(probe::has_instance_get_method2<wrapper_class, std::string_view, std::string_view>,
              "instance get_method(string_view, string_view) must be valid");
static_assert(probe::has_instance_get_method2<wrapper_class, std::string&, std::string&>,
              "instance get_method(std::string, std::string) must be valid");
static_assert(probe::has_instance_get_method2<wrapper_class, const std::string&, const std::string&>,
              "instance get_method(const std::string&, const std::string&) must be valid");
static_assert(probe::has_instance_get_method2<wrapper_class, char[2], char[4]>,
              "instance get_method(char[], char[]) must be valid (array names)");
static_assert(probe::has_instance_get_method2<wrapper_class, const char*, char[4]>,
              "instance get_method(const char*, char[]) mixed categories must be valid");

// Return type of the 1-arg instance get_method for a string_view name is still
// the method optional (whatever overload resolution picks).
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method(std::string_view{ "m" })),
                  std::optional<vmhook::method_proxy>>,
              "instance get_method(string_view) must return std::optional<method_proxy>");
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method(
                      std::string_view{ "m" }, std::string_view{ "()I" })),
                  std::optional<vmhook::method_proxy>>,
              "instance get_method(string_view, string_view) must return std::optional<method_proxy>");

// =============================================================================
// 11. PORTABLE static accessor surface — complete argument & return matrix
// =============================================================================
// Section 3a covered the common static_field / static_method spellings.  Here we
// finish the matrix: the 2-arg static_method across every name x sig category,
// array-typed names, and the RETURN TYPE of the single-arg static_method (3a only
// pinned the 2-arg return type).  These hold on EVERY toolchain — they are the
// gate-agnostic portable spelling.
static_assert(probe::has_static_field_accessor<wrapper_class, char[2]>,
              "static_field must accept a char-array (literal) name on every toolchain");
static_assert(probe::has_static_field_accessor<wrapper_class, const std::string&>,
              "static_field(const std::string&) must exist on every toolchain");
static_assert(probe::has_static_field_accessor<wrapper_class, const std::string_view&>,
              "static_field(const string_view&) must exist on every toolchain");
static_assert(probe::has_static_method_accessor<wrapper_class, char[2]>,
              "static_method must accept a char-array name on every toolchain");
static_assert(probe::has_static_method_accessor<wrapper_class, std::string&>,
              "static_method(std::string) must exist on every toolchain");
static_assert(probe::has_static_method_accessor2<wrapper_class, std::string&, std::string&>,
              "static_method(std::string, std::string) must exist on every toolchain");
static_assert(probe::has_static_method_accessor2<wrapper_class, const char*, std::string_view>,
              "static_method(const char*, string_view) mixed names must exist everywhere");
static_assert(probe::has_static_method_accessor2<wrapper_class, std::string_view, const char*>,
              "static_method(string_view, const char*) mixed names must exist everywhere");
static_assert(probe::has_static_method_accessor2<wrapper_class, char[2], char[4]>,
              "static_method(char[], char[]) array names must exist everywhere");

// The single-arg static_method return type (3a only asserted the 2-arg form).
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_method("m")),
                  std::optional<vmhook::method_proxy>>,
              "static_method(name) must return std::optional<method_proxy>");
// static_field with a string_view argument return type (3a only used a literal).
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_field(std::string_view{ "a" })),
                  std::optional<vmhook::field_proxy>>,
              "static_field(string_view) must return std::optional<field_proxy>");

// The portable static idiom with name+signature, end to end.
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_method("m", "()I")->call()),
                  vmhook::method_proxy::value_t>,
              "static_method(\"m\", \"()I\")->call() must yield method_proxy::value_t");

// =============================================================================
// 12. field_proxy::get() -> value_t : chained conversion to EVERY target
// =============================================================================
// Section 2 proved get_field("x")->get() yields value_t and converts to int.
// Here we prove the chained one-liner `get_field(name)->get()` is convertible to
// the FULL set of legitimate value_t targets, through BOTH the instance idiom AND
// the portable static idiom — i.e. both call sites land on the same value_t whose
// constrained operator admits the same target set.  (Unevaluated; no JVM call.)

namespace chain_probe
{
    // The exact value_t each idiom's get() yields (instance + portable static).
    using inst_field_value =
        decltype(std::declval<wrapper_class&>().get_field("a")->get());
    using stat_field_value =
        decltype(wrapper_class::static_field("a")->get());

    // Both idioms must land on field_proxy::value_t (one value-conversion surface).
    static_assert(std::is_same_v<inst_field_value, vmhook::field_proxy::value_t>);
    static_assert(std::is_same_v<stat_field_value, vmhook::field_proxy::value_t>);

    // A compile-time check that BOTH idioms' value_t convert to target_t.
    template<typename target_t>
    inline constexpr bool both_field_idioms_convert_to =
        std::is_convertible_v<inst_field_value, target_t>
        && std::is_convertible_v<stat_field_value, target_t>;
}

static_assert(chain_probe::both_field_idioms_convert_to<bool>,
              "get_field(name)->get() must convert to bool from instance AND static idioms");
static_assert(chain_probe::both_field_idioms_convert_to<std::int8_t>,   "...->get() -> int8_t (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<std::int16_t>,  "...->get() -> int16_t (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<std::int32_t>,  "...->get() -> int32_t (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<std::int64_t>,  "...->get() -> int64_t (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<float>,         "...->get() -> float (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<double>,        "...->get() -> double (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<std::uint16_t>, "...->get() -> uint16_t/char (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<std::uint32_t>, "...->get() -> uint32_t/OOP (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<std::string>,   "...->get() -> std::string (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<void*>,         "...->get() -> void* (both idioms)");
static_assert(chain_probe::both_field_idioms_convert_to<std::vector<int>>,
              "...->get() -> std::vector<int> (array field, both idioms)");

// The excised targets must be NON-convertible from the chained one-liner too
// (the constraint travels with the value_t regardless of call site).
static_assert(!std::is_convertible_v<chain_probe::inst_field_value, const char*>,
              "get_field(name)->get() must NOT convert to const char* (excised)");
static_assert(!std::is_convertible_v<chain_probe::stat_field_value, std::nullptr_t>,
              "static_field(name)->get() must NOT convert to nullptr_t (excised)");
static_assert(!std::is_convertible_v<chain_probe::inst_field_value, int*>,
              "get_field(name)->get() must NOT convert to a non-void pointer (excised)");

// as_string() is the unambiguous String extraction on the value_t get() returns
// (NOT on the field_proxy); its return type is std::string from both idioms.
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_field("name")->get().as_string()),
                  std::string>,
              "get_field(name)->get().as_string() must return std::string (instance idiom)");
static_assert(std::is_same_v<
                  decltype(wrapper_class::static_field("name")->get().as_string()),
                  std::string>,
              "static_field(name)->get().as_string() must return std::string (static idiom)");

// =============================================================================
// 13. method_proxy reachable surface — introspection + value_t after a call
// =============================================================================
// Everything reachable from `get_method(...)->` and from `...->call()` — the
// proxy introspectors and the method value_t's own surface.  All compile-time
// return-type / convertibility probes; no method is ever invoked.

namespace mproxy_probe
{
    using mp = vmhook::method_proxy;
    using mv = vmhook::method_proxy::value_t;
}

// method_proxy introspection return types (reachable via get_method(...)->).
static_assert(std::is_same_v<decltype(std::declval<const mproxy_probe::mp&>().name()), std::string>,
              "method_proxy::name() must return std::string");
static_assert(std::is_same_v<decltype(std::declval<const mproxy_probe::mp&>().signature()), std::string_view>,
              "method_proxy::signature() must return std::string_view");
static_assert(std::is_same_v<decltype(std::declval<const mproxy_probe::mp&>().is_static()), bool>,
              "method_proxy::is_static() must return bool");
static_assert(std::is_same_v<decltype(std::declval<const mproxy_probe::mp&>().is_reference()), bool>,
              "method_proxy::is_reference() must return bool");

// method value_t introspection + extraction return types.
static_assert(std::is_same_v<decltype(std::declval<const mproxy_probe::mv&>().is_void()), bool>,
              "method value_t::is_void() must return bool");
static_assert(std::is_same_v<decltype(std::declval<const mproxy_probe::mv&>().is_string()), bool>,
              "method value_t::is_string() must return bool");
static_assert(std::is_same_v<decltype(std::declval<const mproxy_probe::mv&>().as_string()), std::string>,
              "method value_t::as_string() must return std::string");

// The full chained one-liner get_method(name)->call() -> value_t convertibility,
// from instance AND portable static idioms.
namespace mchain_probe
{
    using inst_mv = decltype(std::declval<wrapper_class&>().get_method("m")->call());
    using stat_mv = decltype(wrapper_class::static_method("m")->call());
    static_assert(std::is_same_v<inst_mv, vmhook::method_proxy::value_t>);
    static_assert(std::is_same_v<stat_mv, vmhook::method_proxy::value_t>);

    template<typename target_t>
    inline constexpr bool both_method_idioms_convert_to =
        std::is_convertible_v<inst_mv, target_t>
        && std::is_convertible_v<stat_mv, target_t>;
}
static_assert(mchain_probe::both_method_idioms_convert_to<bool>,         "call() -> bool (both idioms)");
static_assert(mchain_probe::both_method_idioms_convert_to<std::int32_t>, "call() -> int32_t (both idioms)");
static_assert(mchain_probe::both_method_idioms_convert_to<std::int64_t>, "call() -> int64_t (both idioms)");
static_assert(mchain_probe::both_method_idioms_convert_to<float>,        "call() -> float (both idioms)");
static_assert(mchain_probe::both_method_idioms_convert_to<double>,       "call() -> double (both idioms)");
static_assert(mchain_probe::both_method_idioms_convert_to<std::string>,  "call() -> std::string (both idioms)");
static_assert(mchain_probe::both_method_idioms_convert_to<void*>,        "call() -> void* (both idioms)");
static_assert(mchain_probe::both_method_idioms_convert_to<std::unique_ptr<other_wrapper>>,
              "call() -> unique_ptr<W> (Object-returning method, both idioms)");
static_assert(!mchain_probe::both_method_idioms_convert_to<const char*>,
              "call() must NOT convert to const char* (excised, both idioms)");
static_assert(!mchain_probe::both_method_idioms_convert_to<other_wrapper*>,
              "call() must NOT convert to a raw wrapper pointer (excised, both idioms)");

// =============================================================================
// 14. value_t_convertible_target_v — exhaustive classification sweep
// =============================================================================
// The selection gate the constrained operator T() uses.  Section 5c sampled it;
// here we sweep it to closure: EVERY variant alternative type is a legitimate
// target; EVERY pointer-to-non-void (with every cv-qualifier combination) is
// excised; void* with every cv combination is admitted; nullptr_t is excised;
// container / wrapper targets are admitted; references are stripped first.
namespace vt
{
    using vmhook::detail::value_t_convertible_target_v;
}

// 14a. Every alternative type carried by the variants is a legitimate target.
static_assert(vt::value_t_convertible_target_v<bool>,          "trait: bool legitimate");
static_assert(vt::value_t_convertible_target_v<std::int8_t>,   "trait: int8_t legitimate");
static_assert(vt::value_t_convertible_target_v<std::int16_t>,  "trait: int16_t legitimate");
static_assert(vt::value_t_convertible_target_v<std::int32_t>,  "trait: int32_t legitimate");
static_assert(vt::value_t_convertible_target_v<std::int64_t>,  "trait: int64_t legitimate");
static_assert(vt::value_t_convertible_target_v<float>,         "trait: float legitimate");
static_assert(vt::value_t_convertible_target_v<double>,        "trait: double legitimate");
static_assert(vt::value_t_convertible_target_v<std::uint16_t>, "trait: uint16_t legitimate");
static_assert(vt::value_t_convertible_target_v<std::uint32_t>, "trait: uint32_t legitimate");

// 14b. Other arithmetic / character targets pass through (non-pointer scalars).
static_assert(vt::value_t_convertible_target_v<char>,          "trait: char legitimate");
static_assert(vt::value_t_convertible_target_v<char16_t>,      "trait: char16_t legitimate");
static_assert(vt::value_t_convertible_target_v<unsigned long long>, "trait: unsigned long long legitimate");
static_assert(vt::value_t_convertible_target_v<long double>,   "trait: long double legitimate");

// 14c. void* (the SOLE permitted pointer) under every cv-qualifier combination.
static_assert(vt::value_t_convertible_target_v<void*>,               "trait: void* legitimate");
static_assert(vt::value_t_convertible_target_v<const void*>,         "trait: const void* legitimate");
static_assert(vt::value_t_convertible_target_v<volatile void*>,      "trait: volatile void* legitimate");
static_assert(vt::value_t_convertible_target_v<const volatile void*>,"trait: const volatile void* legitimate");
static_assert(vt::value_t_convertible_target_v<void* const>,         "trait: void* const (top-level cv stripped) legitimate");

// 14d. Every pointer-to-non-void is excised, across cv-qualifiers and element types.
static_assert(!vt::value_t_convertible_target_v<char*>,           "trait: char* excised");
static_assert(!vt::value_t_convertible_target_v<const char*>,     "trait: const char* excised");
static_assert(!vt::value_t_convertible_target_v<volatile char*>,  "trait: volatile char* excised");
static_assert(!vt::value_t_convertible_target_v<int*>,            "trait: int* excised");
static_assert(!vt::value_t_convertible_target_v<const int*>,      "trait: const int* excised");
static_assert(!vt::value_t_convertible_target_v<double*>,         "trait: double* excised");
static_assert(!vt::value_t_convertible_target_v<void**>,          "trait: void** (ptr-to-ptr, element is void*) excised");
static_assert(!vt::value_t_convertible_target_v<other_wrapper*>,  "trait: W* excised");
static_assert(!vt::value_t_convertible_target_v<const other_wrapper*>, "trait: const W* excised");

// 14e. nullptr_t is excised under every cv-ref qualifier (stripped, then matched).
static_assert(!vt::value_t_convertible_target_v<std::nullptr_t>,         "trait: nullptr_t excised");
static_assert(!vt::value_t_convertible_target_v<const std::nullptr_t>,   "trait: const nullptr_t excised");
static_assert(!vt::value_t_convertible_target_v<std::nullptr_t&>,        "trait: nullptr_t& excised (ref stripped)");
static_assert(!vt::value_t_convertible_target_v<const std::nullptr_t&>,  "trait: const nullptr_t& excised");

// 14f. Class / container / wrapper targets pass through.
static_assert(vt::value_t_convertible_target_v<std::string>,                       "trait: std::string legitimate");
static_assert(vt::value_t_convertible_target_v<std::vector<int>>,                  "trait: vector<int> legitimate");
static_assert(vt::value_t_convertible_target_v<std::vector<std::string>>,          "trait: vector<string> legitimate");
static_assert(vt::value_t_convertible_target_v<std::unique_ptr<other_wrapper>>,    "trait: unique_ptr<W> legitimate");
static_assert(vt::value_t_convertible_target_v<std::unique_ptr<wrapper_class>>,    "trait: unique_ptr<wrapper_class> legitimate");

// 14g. cv-ref stripping: a pointer-to-non-void stays excised through references;
// void* stays admitted through references; a legitimate class target through ref.
static_assert(!vt::value_t_convertible_target_v<char* const&>,     "trait: char* const& excised (ref+cv stripped)");
static_assert(!vt::value_t_convertible_target_v<const char* const&>,"trait: const char* const& excised");
static_assert(!vt::value_t_convertible_target_v<int* &&>,          "trait: int*&& excised");
static_assert(vt::value_t_convertible_target_v<void* &>,           "trait: void*& legitimate (ref stripped to void*)");
static_assert(vt::value_t_convertible_target_v<const void* const&>, "trait: const void* const& legitimate");
static_assert(vt::value_t_convertible_target_v<std::string&&>,     "trait: std::string&& legitimate (ref stripped)");
static_assert(vt::value_t_convertible_target_v<std::unique_ptr<other_wrapper>&&>,
              "trait: unique_ptr<W>&& legitimate (ref stripped)");

// =============================================================================
// 15. field_proxy::set(value) — complete acceptance matrix
// =============================================================================
// Section 6 sampled set().  Complete it: every JVM-primitive C++ representative,
// reference / cv-qualified categories (set takes `const value_type&`, so it must
// accept every value category), wider vector element types, and the value_t
// round-trip (writing back a value_t read from get()).
static_assert(set_probe::set_ok<std::uint8_t>,   "set(uint8_t) must be viable");
static_assert(set_probe::set_ok<std::uint16_t>,  "set(uint16_t) must be viable");
static_assert(set_probe::set_ok<std::uint32_t>,  "set(uint32_t) must be viable (compressed OOP / char)");
static_assert(set_probe::set_ok<std::uint64_t>,  "set(uint64_t) must be viable");
static_assert(set_probe::set_ok<char>,           "set(char) must be viable");
static_assert(set_probe::set_ok<char16_t>,       "set(char16_t) must be viable");
// Reference / cv categories (set's parameter is `const value_type&`).
static_assert(set_probe::set_ok<int&>,           "set(int& lvalue) must be viable");
static_assert(set_probe::set_ok<const int&>,     "set(const int&) must be viable");
static_assert(set_probe::set_ok<int&&>,          "set(int&& rvalue) must be viable");
static_assert(set_probe::set_ok<const std::string&>, "set(const std::string&) must be viable");
static_assert(set_probe::set_ok<std::string&&>,  "set(std::string&& rvalue) must be viable");
// const char* / string_view string writes.
static_assert(set_probe::set_ok<const char*>,    "set(const char*) must be viable (String field write)");
static_assert(set_probe::set_ok<std::string_view>, "set(std::string_view) must be viable");
// Wider vector element types (array field writes).
static_assert(set_probe::set_ok<std::vector<bool>>,        "set(vector<bool>) must be viable");
static_assert(set_probe::set_ok<std::vector<std::int8_t>>, "set(vector<int8_t>) must be viable");
static_assert(set_probe::set_ok<std::vector<std::int64_t>>,"set(vector<int64_t>) must be viable");
static_assert(set_probe::set_ok<std::vector<float>>,       "set(vector<float>) must be viable");
static_assert(set_probe::set_ok<std::vector<double>>,      "set(vector<double>) must be viable");
// The value_t round-trip: set() must accept the exact value_t get() returns, so
// `proxy->set(other_proxy->get())` compiles (read-then-write idiom).
static_assert(set_probe::set_ok<vmhook::field_proxy::value_t>,
              "set(field_proxy::value_t) must be viable (read-then-write round-trip)");

// =============================================================================
// 16. noexcept contracts of the reachable surface
// =============================================================================
// Every entry point the unified call syntax routes to is documented noexcept.
// These are compile-time noexcept(expr) probes (unevaluated operands).
static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call()),
              "method_proxy::call() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(1, 2.0, true)),
              "method_proxy::call(args...) must be noexcept");
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().get()),
              "field_proxy::get() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().set(42)),
              "field_proxy::set() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::field_proxy::value_t&>().as_string()),
              "field value_t::as_string() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::method_proxy::value_t&>().as_string()),
              "method value_t::as_string() must be noexcept");
static_assert(noexcept(static_cast<int>(std::declval<const vmhook::field_proxy::value_t&>())),
              "field value_t::operator T() must be noexcept");
static_assert(noexcept(static_cast<int>(std::declval<const vmhook::method_proxy::value_t&>())),
              "method value_t::operator T() must be noexcept");
// The accessors themselves are NOT marked noexcept in the header (they allocate a
// std::string for the cache key and may throw std::bad_alloc); pin that contract
// so a future noexcept addition is a deliberate, visible change.
static_assert(!noexcept(std::declval<wrapper_class&>().get_field("a")),
              "instance get_field is NOT noexcept (documents the current contract)");
static_assert(!noexcept(wrapper_class::static_field("a")),
              "static_field is NOT noexcept (documents the current contract)");

// =============================================================================
// 17. Cross-gate equivalence + deducing-this receiver value-category matrix
// =============================================================================
// 17a. The instance idiom and the portable static idiom return the SAME proxy
// optionals — the unified-syntax promise that one spelling family yields one
// result type, independent of call context and gate state.
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_field("a")),
                  decltype(wrapper_class::static_field("a"))>,
              "instance get_field and portable static_field must return the same type");
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method("m")),
                  decltype(wrapper_class::static_method("m"))>,
              "instance get_method and portable static_method must return the same type");
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method("m", "()I")),
                  decltype(wrapper_class::static_method("m", "()I"))>,
              "instance get_method(name,sig) and portable static_method(name,sig) must match");

// 17b. Receiver value-category matrix.  The deducing-this explicit-object
// parameter is `this object_base const& self` — a const lvalue reference, which
// binds a const lvalue, a non-const lvalue, AND an rvalue receiver.  When gate
// OFF the inherited instance member is `const`, which is equally bindable from
// all three categories.  So the instance idiom must be a valid expression on a
// const object, a non-const object, and an rvalue object in BOTH gate states.
namespace recv_probe
{
    template<typename recv_t>
    concept get_field_on =
        requires(recv_t r) { { r.get_field("a") } -> std::same_as<std::optional<vmhook::field_proxy>>; };
    template<typename recv_t>
    concept get_method_on =
        requires(recv_t r) { { r.get_method("m") } -> std::same_as<std::optional<vmhook::method_proxy>>; };
}
// declval<T&> = lvalue, declval<const T&> = const lvalue, declval<T&&> = rvalue.
static_assert(recv_probe::get_field_on<wrapper_class&>,
              "get_field callable on a non-const lvalue receiver");
static_assert(recv_probe::get_field_on<const wrapper_class&>,
              "get_field callable on a const lvalue receiver (deducing-this self is const&)");
static_assert(recv_probe::get_field_on<wrapper_class&&>,
              "get_field callable on an rvalue receiver");
static_assert(recv_probe::get_method_on<wrapper_class&>,
              "get_method callable on a non-const lvalue receiver");
static_assert(recv_probe::get_method_on<const wrapper_class&>,
              "get_method callable on a const lvalue receiver");
static_assert(recv_probe::get_method_on<wrapper_class&&>,
              "get_method callable on an rvalue receiver");

#if VMHOOK_HAS_DEDUCING_THIS
// 17c. gate ON only: the deducing-this instance overload takes char const*.  A
// const char* literal binds it directly; the return type is the field optional.
// (This is the exact-match path that outranks the string_view static from an
// instance context — the half of the unified contract that routes to the live
// OOP.)
static_assert(std::is_same_v<
                  decltype(std::declval<const wrapper_class&>().get_field("a")),
                  std::optional<vmhook::field_proxy>>,
              "[gate ON] deducing-this get_field on a const receiver yields the field optional");
#endif

// =============================================================================
// 18. NEGATIVE / ill-formed spellings — the rejection contract (SFINAE oracle)
// =============================================================================
// Detection-idiom proofs that genuinely ill-formed call shapes are NOT viable.
// These are the SFINAE-observable negatives (overload resolution / arity), as
// opposed to the body-instantiation negatives that call_ok cannot see.
namespace neg_probe
{
    // Wrong ARITY on the accessors.
    template<typename w>
    concept get_field_zero_arg = requires { { w::static_field() }; };
    template<typename w>
    concept get_field_three_arg =
        requires(std::string_view a) { { w::static_field(a, a, a) }; };
    template<typename w>
    concept static_method_three_arg =
        requires(std::string_view a) { { w::static_method(a, a, a) }; };

    // A name argument of a type with NO conversion to string_view / char const*
    // (e.g. int) must not produce a viable call in any context.
    template<typename w>
    concept get_field_int_name =
        requires(int n) { { std::declval<w&>().get_field(n) }; };
    template<typename w>
    concept static_field_int_name =
        requires(int n) { { w::static_field(n) }; };

    // set() with NO argument is ill-formed (the template needs one value).
    template<typename f>
    concept set_zero_arg = requires(const f& p) { { p.set() }; };
}
static_assert(!neg_probe::get_field_zero_arg<wrapper_class>,
              "static_field() with no name must be ill-formed (a name is required)");
static_assert(!neg_probe::get_field_three_arg<wrapper_class>,
              "static_field(a,b,c) — there is no 3-arg field accessor");
static_assert(!neg_probe::static_method_three_arg<wrapper_class>,
              "static_method(a,b,c) — there is no 3-arg method accessor");
static_assert(!neg_probe::get_field_int_name<wrapper_class>,
              "get_field(int) must be ill-formed (int is not a name spelling)");
static_assert(!neg_probe::static_field_int_name<wrapper_class>,
              "static_field(int) must be ill-formed (int is not a name spelling)");
static_assert(!neg_probe::set_zero_arg<vmhook::field_proxy>,
              "field_proxy::set() with no value must be ill-formed");

// The excised value_t conversions stated as a hard rejection oracle through the
// detection idiom (a static_cast to an excised target must be ill-formed — the
// constraint removes the operator from the overload set).
namespace cast_neg
{
    template<typename value_t, typename target_t>
    concept static_castable =
        requires(const value_t& v) { { static_cast<target_t>(v) }; };
}
static_assert(!cast_neg::static_castable<vmhook::field_proxy::value_t, const char*>,
              "static_cast<const char*>(field value_t) must be ill-formed (operator excised)");
static_assert(!cast_neg::static_castable<vmhook::field_proxy::value_t, std::nullptr_t>,
              "static_cast<nullptr_t>(field value_t) must be ill-formed (operator excised)");
static_assert(!cast_neg::static_castable<vmhook::method_proxy::value_t, other_wrapper*>,
              "static_cast<W*>(method value_t) must be ill-formed (operator excised)");
// ...while a legitimate static_cast IS well-formed (positive control).
static_assert(cast_neg::static_castable<vmhook::field_proxy::value_t, std::string>,
              "static_cast<std::string>(field value_t) must be well-formed (operator admitted)");
static_assert(cast_neg::static_castable<vmhook::method_proxy::value_t, void*>,
              "static_cast<void*>(method value_t) must be well-formed (operator admitted)");

// =============================================================================
// 19. CHARACTERIZATION — the name+signature instance routing split (gate-aware)
// =============================================================================
// Section 8 pinned the 1-arg get_field split.  The 2-arg get_method has the SAME
// gate-dependent routing: the deducing-this 2-arg instance overload takes
// (char const*, char const*).  A string_view name+sig from an instance context
// does NOT match it (gate ON) and binds the static (string_view, string_view)
// fallback instead — observable as: the same 2-arg call is ALSO valid object-less.
#if VMHOOK_HAS_DEDUCING_THIS
static_assert(probe::has_static_get_method2<wrapper_class, std::string_view, std::string_view>,
              "[characterization, gate ON] a (string_view, string_view) name+sig is ALSO "
              "accepted object-less — an instance-context get_method(sv, sv) binds the STATIC "
              "(string_view, string_view) fallback (typeid(derived) path), not the "
              "(char const*, char const*) deducing-this instance overload");
#else
static_assert(!probe::has_static_get_method2<wrapper_class, std::string_view, std::string_view>,
              "[characterization, gate OFF] there is NO static get_method(name, sig); a "
              "(string_view, string_view) name+sig from an instance context binds the inherited "
              "instance overload (live OOP), and the object-less form is ill-formed");
#endif
// Gate-invariant half: the instance 2-arg call expression is valid for every
// name/sig spelling in BOTH states (only the selected overload differs).
static_assert(probe::has_instance_get_method2<wrapper_class, const char*, const char*>
              && probe::has_instance_get_method2<wrapper_class, std::string_view, std::string_view>
              && probe::has_instance_get_method2<wrapper_class, std::string&, std::string&>,
              "instance get_method(name, sig) must be valid for literal / string_view / "
              "std::string spellings in BOTH gate states");

// =============================================================================
// 20. other_wrapper / second-wrapper surface parity
// =============================================================================
// The same unified call syntax must hold on a DIFFERENT registered wrapper type
// (one declared with `using object<W>::object;` rather than a bespoke ctor) so
// the contract is per-type, not a quirk of wrapper_class.
static_assert(probe::has_instance_get_field<other_wrapper, const char*>,
              "instance get_field must be callable on other_wrapper too");
static_assert(probe::has_instance_get_method2<other_wrapper, const char*, const char*>,
              "instance get_method(name, sig) must be callable on other_wrapper too");
static_assert(probe::has_static_field_accessor<other_wrapper, std::string_view>,
              "static_field must exist on other_wrapper too");
static_assert(probe::has_static_method_accessor2<other_wrapper, std::string_view, std::string_view>,
              "static_method(name, sig) must exist on other_wrapper too");
static_assert(std::is_same_v<
                  decltype(std::declval<other_wrapper&>().get_field("a")),
                  decltype(other_wrapper::static_field("a"))>,
              "other_wrapper: instance get_field and static_field return the same type");
#if VMHOOK_HAS_DEDUCING_THIS
static_assert(probe::has_static_get_field<other_wrapper, std::string_view>,
              "[gate ON] other_wrapper also exposes the same-name static get_field");
#else
static_assert(!probe::has_static_get_field<other_wrapper, std::string_view>,
              "[gate OFF] other_wrapper also lacks the same-name static get_field");
#endif

// =============================================================================
// 21. proxy / optional value-semantics invariants the idiom relies on
// =============================================================================
// The `get_field(name)->get()` one-liner dereferences a std::optional<proxy>
// without checking has_value() — that compiles iff the optional's value_type is
// exactly the proxy and operator-> yields a proxy.  Pin those structural facts.
static_assert(std::is_same_v<std::optional<vmhook::field_proxy>::value_type, vmhook::field_proxy>,
              "optional<field_proxy>::value_type is field_proxy");
static_assert(std::is_same_v<std::optional<vmhook::method_proxy>::value_type, vmhook::method_proxy>,
              "optional<method_proxy>::value_type is method_proxy");
static_assert(std::is_same_v<
                  decltype(std::declval<std::optional<vmhook::field_proxy>&>().operator->()),
                  vmhook::field_proxy*>,
              "optional<field_proxy>::operator-> yields field_proxy* (the idiom's deref target)");
// The proxies must be copyable/movable so the optional can hold and return them.
static_assert(std::is_copy_constructible_v<vmhook::field_proxy>,  "field_proxy must be copy-constructible");
static_assert(std::is_move_constructible_v<vmhook::field_proxy>,  "field_proxy must be move-constructible");
static_assert(std::is_copy_constructible_v<vmhook::method_proxy>, "method_proxy must be copy-constructible");
static_assert(std::is_move_constructible_v<vmhook::method_proxy>, "method_proxy must be move-constructible");
// value_t must be a value type (held by the proxies' return-by-value get()/call()).
static_assert(std::is_copy_constructible_v<vmhook::field_proxy::value_t>,  "field value_t copy-constructible");
static_assert(std::is_move_constructible_v<vmhook::method_proxy::value_t>, "method value_t move-constructible");

// =============================================================================
// 22. CRTP / type-identity invariants underpinning the static path
// =============================================================================
// The static path keys on typeid(derived); these pin the type relationships that
// make the deducing-this self-slice (to object_base const&) and the static
// fallback (typeid(derived)) both well-formed.
static_assert(std::is_base_of_v<vmhook::object<wrapper_class>, wrapper_class>,
              "wrapper_class : object<wrapper_class> (CRTP)");
static_assert(std::is_base_of_v<vmhook::object<other_wrapper>, other_wrapper>,
              "other_wrapper : object<other_wrapper> (CRTP)");
static_assert(!std::is_same_v<vmhook::object<wrapper_class>, vmhook::object<other_wrapper>>,
              "distinct derived params yield distinct CRTP bases (distinct typeid paths)");
// The deducing-this self parameter slices to object_base const&, so a wrapper&
// must be convertible to object_base const& (the receiver bind the overload uses).
static_assert(std::is_convertible_v<wrapper_class&, const vmhook::object_base&>,
              "wrapper& converts to object_base const& (deducing-this self bind)");
static_assert(std::is_convertible_v<const wrapper_class&, const vmhook::object_base&>,
              "const wrapper& converts to object_base const& (deducing-this self bind)");
// object<wrapper_class> itself derives from object_base (the substrate the
// instance overloads forward into via self.object_base::get_field).
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<wrapper_class>>,
              "object<wrapper_class> derives from object_base");

// =============================================================================
// 24. DEEPEN -- value_t_convertible_target_v : closure of the classifier branches
// =============================================================================
// The trait body (vmhook.hpp) is a 3-way constexpr lambda over remove_cvref_t<T>:
//   (a) clean == std::nullptr_t            -> false
//   (b) is_pointer_v<clean>               -> is_void_v<remove_pointer_t<clean>>
//   (c) everything else                   -> true
// Sections 5c / 14 sampled (a)/(b)/(c) for the obvious cases.  This section drives
// every UNCOVERED corner of those three branches to closure -- function pointers,
// pointer-to-member, function references, arrays, enums, nested templates, and
// every exotic-but-still-class/scalar target -- with the exact value the lambda
// computes for each.  Pure source-derived; no JVM, no runtime.
namespace ucs_deepen_trait
{
    using vmhook::detail::value_t_convertible_target_v;

    enum plain_enum { pe0 };
    enum class scoped_enum : std::int64_t { se0 };
    struct some_pod { int x; };
}

// 24a. BRANCH (b) -- pointer targets.  Only void* (any cv) is admitted; EVERY
// other pointer flavour the trait can see is excised because remove_pointer_t is
// not `void`.  Function pointers and pointer-to-member are the subtle ones:
//   - `void(*)()` IS is_pointer_v, remove_pointer_t = `void()` (a FUNCTION type),
//     and is_void_v<function-type> is FALSE -> excised.
//   - a pointer-to-MEMBER (`int C::*`, `void(C::*)()`) is NOT is_pointer_v, so it
//     falls to branch (c) and is ADMITTED.
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<void(*)()>,
              "trait: function pointer void(*)() is excised (remove_pointer is a function type, not void)");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<int(*)(int)>,
              "trait: function pointer int(*)(int) is excised");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<void(**)()>,
              "trait: pointer-to-(function-pointer) is excised (element is a pointer, not void)");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<int wrapper_class::*>,
              "trait: pointer-to-data-member is NOT is_pointer_v -> branch (c) ADMITTED");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<void (wrapper_class::*)()>,
              "trait: pointer-to-member-function is NOT is_pointer_v -> branch (c) ADMITTED");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<const volatile void*>,
              "trait: const volatile void* -- pointee is void (cv) -> is_void_v true -> ADMITTED");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<wrapper_class*>,
              "trait: wrapper_class* (the CRTP type itself) excised as a non-void pointer");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<std::string*>,
              "trait: std::string* excised (non-void pointer)");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<std::nullptr_t*>,
              "trait: std::nullptr_t* excised (pointer whose element is nullptr_t, not void)");

// 24b. BRANCH (a) -- std::nullptr_t under EVERY cv-ref combination (remove_cvref_t
// strips to nullptr_t first, then matched).  Section 14e did const/&; close it.
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<volatile std::nullptr_t>,
              "trait: volatile nullptr_t excised (cv stripped to nullptr_t)");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<const volatile std::nullptr_t>,
              "trait: const volatile nullptr_t excised");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<std::nullptr_t&&>,
              "trait: nullptr_t&& excised (rvalue ref stripped)");
static_assert(!ucs_deepen_trait::value_t_convertible_target_v<const volatile std::nullptr_t&>,
              "trait: const volatile nullptr_t& excised");

// 24c. BRANCH (c) -- non-pointer, non-nullptr targets are UNIVERSALLY admitted.
// Enums, arrays, references-to-arrays, function references, nested templates,
// PODs -- the trait does not inspect them further, it returns true.
static_assert(ucs_deepen_trait::value_t_convertible_target_v<ucs_deepen_trait::plain_enum>,
              "trait: unscoped enum admitted (branch c)");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<ucs_deepen_trait::scoped_enum>,
              "trait: scoped enum admitted (branch c)");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<ucs_deepen_trait::some_pod>,
              "trait: arbitrary POD admitted (branch c -- the trait gates SELECTION, not real convertibility)");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<int[3]>,
              "trait: array type int[3] is not a pointer -> admitted (branch c)");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<int(&)[3]>,
              "trait: reference-to-array int(&)[3] -- remove_cvref_t leaves int[3] -> admitted");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<void(&)()>,
              "trait: function REFERENCE void(&)() -- remove_cvref_t leaves void() (not a pointer) -> admitted");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<std::vector<std::vector<int>>>,
              "trait: nested vector admitted (branch c)");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<std::optional<int>>,
              "trait: std::optional<int> admitted (branch c)");
static_assert(ucs_deepen_trait::value_t_convertible_target_v<std::unique_ptr<int>>,
              "trait: unique_ptr<int> (class type) admitted -- only RAW non-void pointers are excised");

// =============================================================================
// 25. DEEPEN -- variant ALTERNATIVE-TYPE map, by index, for BOTH value_ts
// =============================================================================
// The runtime lane (Section 23) checks the variant SIZES (9 / 11) and the
// default ACTIVE index (0).  Here we pin every ALTERNATIVE TYPE by index against
// the exact std::variant<...> declarations in vmhook.hpp:
//   field value_t :  0 bool, 1 int8, 2 int16, 3 int32, 4 int64, 5 float,
//                    6 double, 7 uint16, 8 uint32
//   method value_t:  0 monostate, 1 bool, 2 int8, 3 int16, 4 int32, 5 int64,
//                    6 float, 7 double, 8 uint16, 9 uint32, 10 std::string
// A reorder/insertion in either variant (which would silently change the get()
// fast-path indices and the default-state contract) breaks exactly here.
namespace ucs_deepen_variant
{
    using fvar = std::remove_cvref_t<decltype(vmhook::field_proxy::value_t{}.data)>;
    using mvar = std::remove_cvref_t<decltype(vmhook::method_proxy::value_t{}.data)>;

    template<std::size_t I, typename var_t>
    using alt = std::variant_alternative_t<I, var_t>;
}

// 25a. field value_t -- 9 alternatives, exact type per index.
static_assert(std::variant_size_v<ucs_deepen_variant::fvar> == 9u,
              "field value_t variant size is 9 (compile-time)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<0, ucs_deepen_variant::fvar>, bool>,
              "field value_t[0] is bool (the value-initialised / null-proxy default)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<1, ucs_deepen_variant::fvar>, std::int8_t>,
              "field value_t[1] is int8_t (Java byte slot)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<2, ucs_deepen_variant::fvar>, std::int16_t>,
              "field value_t[2] is int16_t (Java short)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<3, ucs_deepen_variant::fvar>, std::int32_t>,
              "field value_t[3] is int32_t (Java int)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<4, ucs_deepen_variant::fvar>, std::int64_t>,
              "field value_t[4] is int64_t (Java long)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<5, ucs_deepen_variant::fvar>, float>,
              "field value_t[5] is float (Java float)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<6, ucs_deepen_variant::fvar>, double>,
              "field value_t[6] is double (Java double)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<7, ucs_deepen_variant::fvar>, std::uint16_t>,
              "field value_t[7] is uint16_t (Java char)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<8, ucs_deepen_variant::fvar>, std::uint32_t>,
              "field value_t[8] is uint32_t (reference / array compressed OOP -- the is_reference() alternative)");

// 25b. method value_t -- 11 alternatives, monostate FIRST, std::string LAST.
static_assert(std::variant_size_v<ucs_deepen_variant::mvar> == 11u,
              "method value_t variant size is 11 (compile-time)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<0, ucs_deepen_variant::mvar>, std::monostate>,
              "method value_t[0] is std::monostate (the is_void() / call-failed default)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<1, ucs_deepen_variant::mvar>, bool>,
              "method value_t[1] is bool");
static_assert(std::is_same_v<ucs_deepen_variant::alt<2, ucs_deepen_variant::mvar>, std::int8_t>,
              "method value_t[2] is int8_t");
static_assert(std::is_same_v<ucs_deepen_variant::alt<3, ucs_deepen_variant::mvar>, std::int16_t>,
              "method value_t[3] is int16_t");
static_assert(std::is_same_v<ucs_deepen_variant::alt<4, ucs_deepen_variant::mvar>, std::int32_t>,
              "method value_t[4] is int32_t");
static_assert(std::is_same_v<ucs_deepen_variant::alt<5, ucs_deepen_variant::mvar>, std::int64_t>,
              "method value_t[5] is int64_t");
static_assert(std::is_same_v<ucs_deepen_variant::alt<6, ucs_deepen_variant::mvar>, float>,
              "method value_t[6] is float");
static_assert(std::is_same_v<ucs_deepen_variant::alt<7, ucs_deepen_variant::mvar>, double>,
              "method value_t[7] is double");
static_assert(std::is_same_v<ucs_deepen_variant::alt<8, ucs_deepen_variant::mvar>, std::uint16_t>,
              "method value_t[8] is uint16_t");
static_assert(std::is_same_v<ucs_deepen_variant::alt<9, ucs_deepen_variant::mvar>, std::uint32_t>,
              "method value_t[9] is uint32_t (reference compressed OOP)");
static_assert(std::is_same_v<ucs_deepen_variant::alt<10, ucs_deepen_variant::mvar>, std::string>,
              "method value_t[10] is std::string (eagerly-decoded String result)");

// 25c. The field value_t carries a trailing `std::string signature{}` member;
// the method value_t does NOT.  Pin that structural difference (the field proxy
// needs the signature to interpret an array/reference field; the method value_t
// gets its String eagerly as alternative 10).  Probed via member-access SFINAE.
namespace ucs_deepen_members
{
    template<typename v_t>
    concept has_signature_member =
        requires(const v_t& v) { { v.signature } -> std::convertible_to<std::string>; };
}
static_assert(ucs_deepen_members::has_signature_member<vmhook::field_proxy::value_t>,
              "field value_t has a `signature` member (used to interpret array/ref fields)");
static_assert(!ucs_deepen_members::has_signature_member<vmhook::method_proxy::value_t>,
              "method value_t has NO `signature` member (String is variant alternative 10)");

// =============================================================================
// 26. DEEPEN -- every value_t alternative type is itself a CONVERSION target
// =============================================================================
// Closing the loop between Section 25 (what the variant HOLDS) and Section 5/14
// (what the constrained operator T() ADMITS): every alternative type a value_t
// can hold must also be a legitimate conversion target, otherwise the value
// could never be read back out as its own type.  uint32_t (the OOP alternative)
// is admitted as a scalar; std::string (method alt 10) is admitted as a class;
// std::monostate (method alt 0) is a class type and is therefore ALSO admitted
// by the trait (branch c) -- characterising that the trait gates on pointer-ness,
// not on "is this a sensible target".
static_assert(vmhook::detail::value_t_convertible_target_v<bool>
              && vmhook::detail::value_t_convertible_target_v<std::int8_t>
              && vmhook::detail::value_t_convertible_target_v<std::int16_t>
              && vmhook::detail::value_t_convertible_target_v<std::int32_t>
              && vmhook::detail::value_t_convertible_target_v<std::int64_t>
              && vmhook::detail::value_t_convertible_target_v<float>
              && vmhook::detail::value_t_convertible_target_v<double>
              && vmhook::detail::value_t_convertible_target_v<std::uint16_t>
              && vmhook::detail::value_t_convertible_target_v<std::uint32_t>,
              "every FIELD value_t alternative type is itself a legitimate conversion target");
static_assert(vmhook::detail::value_t_convertible_target_v<std::monostate>,
              "std::monostate (method value_t alt 0) is a class type -> trait branch (c) ADMITS it "
              "(the trait gates pointer-ness, not target sensibility)");
static_assert(vmhook::detail::value_t_convertible_target_v<std::string>,
              "std::string (method value_t alt 10) is admitted");

// =============================================================================
// 27. DEEPEN -- gate-INVARIANT identity of the static accessors, and the gated
//              same-name statics, return EXACTLY the proxy optionals for every
//              name spelling (not just the literal sampled in Sections 3/11)
// =============================================================================
// static_field / static_method return-type closure across name spellings.
static_assert(std::is_same_v<decltype(wrapper_class::static_field(std::string_view{ "a" })),
                             std::optional<vmhook::field_proxy>>,
              "static_field(string_view) returns optional<field_proxy>");
static_assert(std::is_same_v<decltype(wrapper_class::static_method(std::string_view{ "m" })),
                             std::optional<vmhook::method_proxy>>,
              "static_method(string_view) returns optional<method_proxy>");
static_assert(std::is_same_v<decltype(wrapper_class::static_method(std::string_view{ "m" },
                                                                   std::string_view{ "()I" })),
                             std::optional<vmhook::method_proxy>>,
              "static_method(string_view, string_view) returns optional<method_proxy>");
// The instance idiom and the portable static idiom land on the SAME value_t for
// the name+signature method form too (Section 17 covered field + name-only).
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_method("m", "()I")->call()),
                  decltype(wrapper_class::static_method("m", "()I")->call())>,
              "instance get_method(name,sig)->call() and static_method(name,sig)->call() yield the same value_t");
static_assert(std::is_same_v<
                  decltype(std::declval<wrapper_class&>().get_field("a")->get()),
                  decltype(wrapper_class::static_field("a")->get())>,
              "instance get_field(name)->get() and static_field(name)->get() yield the same value_t");

#if VMHOOK_HAS_DEDUCING_THIS
// gate ON: the gated same-name static get_method (1-arg and 2-arg) and the
// portable static_method return the SAME type -- their bodies are byte-identical
// (both forward to object_base::get_method(typeid(derived), ...)).  Section 3b
// pinned this for get_field; complete it for both get_method arities.
static_assert(std::is_same_v<
                  decltype(wrapper_class::get_method(std::string_view{ "m" })),
                  decltype(wrapper_class::static_method(std::string_view{ "m" }))>,
              "[gate ON] gated static get_method(name) and portable static_method(name) match");
static_assert(std::is_same_v<
                  decltype(wrapper_class::get_method(std::string_view{ "m" }, std::string_view{ "()I" })),
                  decltype(wrapper_class::static_method(std::string_view{ "m" }, std::string_view{ "()I" }))>,
              "[gate ON] gated static get_method(name,sig) and portable static_method(name,sig) match");
#endif

// =============================================================================
// 28. DEEPEN -- NEGATIVE arity / wrong-arg closure on the accessors & proxies
// =============================================================================
// Section 18 hit the headline ill-formed shapes.  Close the arity/typing matrix:
// a 2-arg static_field, a 4-arg static_method, get_method with zero args, and a
// name argument of a pointer-to-non-void (e.g. int*) that has no conversion to
// string_view / char const* -- all must be SFINAE-non-viable (never a hard error).
namespace ucs_deepen_neg
{
    template<typename w>
    concept static_field_two_arg =
        requires(std::string_view a) { { w::static_field(a, a) }; };
    template<typename w>
    concept static_method_four_arg =
        requires(std::string_view a) { { w::static_method(a, a, a, a) }; };
    template<typename w>
    concept static_method_zero_arg =
        requires { { w::static_method() }; };
    template<typename w>
    concept inst_get_method_zero_arg =
        requires { { std::declval<w&>().get_method() }; };
    template<typename w>
    concept static_field_ptr_name =
        requires(int* p) { { w::static_field(p) }; };
    template<typename w>
    concept inst_get_field_double_name =
        requires(double d) { { std::declval<w&>().get_field(d) }; };
}
static_assert(!ucs_deepen_neg::static_field_two_arg<wrapper_class>,
              "static_field(name, sig) is ill-formed (fields have no signature accessor -- only static_method does)");
static_assert(!ucs_deepen_neg::static_method_four_arg<wrapper_class>,
              "static_method(a,b,c,d) is ill-formed (no 4-arg accessor)");
static_assert(!ucs_deepen_neg::static_method_zero_arg<wrapper_class>,
              "static_method() with no name is ill-formed");
static_assert(!ucs_deepen_neg::inst_get_method_zero_arg<wrapper_class>,
              "instance get_method() with no name is ill-formed");
static_assert(!ucs_deepen_neg::static_field_ptr_name<wrapper_class>,
              "static_field(int*) is ill-formed (a pointer is not a name spelling)");
static_assert(!ucs_deepen_neg::inst_get_field_double_name<wrapper_class>,
              "instance get_field(double) is ill-formed (a double is not a name spelling)");

// =============================================================================
// 29. DEEPEN -- proxy / value_t value-semantics & noexcept closure
// =============================================================================
// Section 21 pinned copy/move constructibility; close it with assignability,
// destructibility, and the noexcept of the moves the optional<proxy> machinery
// relies on, plus the optional<method_proxy>::operator-> target (Section 21 only
// did field).  All derivable from the proxy/value_t declarations.
static_assert(std::is_same_v<
                  decltype(std::declval<std::optional<vmhook::method_proxy>&>().operator->()),
                  vmhook::method_proxy*>,
              "optional<method_proxy>::operator-> yields method_proxy* (the get_method idiom's deref target)");
static_assert(std::is_copy_assignable_v<vmhook::field_proxy::value_t>,
              "field value_t is copy-assignable (holds copyable variant alternatives)");
static_assert(std::is_move_assignable_v<vmhook::method_proxy::value_t>,
              "method value_t is move-assignable");
static_assert(std::is_copy_constructible_v<vmhook::method_proxy::value_t>,
              "method value_t is copy-constructible (held by value, returned from call())");
static_assert(std::is_move_constructible_v<vmhook::field_proxy::value_t>,
              "field value_t is move-constructible");
static_assert(std::is_destructible_v<vmhook::field_proxy>
              && std::is_destructible_v<vmhook::method_proxy>
              && std::is_destructible_v<vmhook::field_proxy::value_t>
              && std::is_destructible_v<vmhook::method_proxy::value_t>,
              "all proxies and value_ts are destructible (held in std::optional / by value)");
// The two value_ts are DISTINCT types (different variant arity / leading
// alternative) -- the unified syntax keeps field and method results separate.
static_assert(!std::is_same_v<vmhook::field_proxy::value_t, vmhook::method_proxy::value_t>,
              "field value_t and method value_t are distinct types");
static_assert(!std::is_same_v<vmhook::field_proxy, vmhook::method_proxy>,
              "field_proxy and method_proxy are distinct types");

// =============================================================================
// 30. DEEPEN -- call() / set() reachable from BOTH idioms, additional shapes
// =============================================================================
// Section 4 swept call() shapes on a bare method_proxy.  Confirm the SAME shapes
// remain viable when reached through the unified spellings (instance get_method
// and portable static_method), and add a few uncovered argument categories.
namespace ucs_deepen_call
{
    using inst_mp = decltype(*std::declval<wrapper_class&>().get_method("m"));
    using stat_mp = decltype(*wrapper_class::static_method("m"));
    static_assert(std::is_same_v<std::remove_cvref_t<inst_mp>, vmhook::method_proxy>,
                  "deref of instance get_method optional is a method_proxy");
    static_assert(std::is_same_v<std::remove_cvref_t<stat_mp>, vmhook::method_proxy>,
                  "deref of static_method optional is a method_proxy");
}
// Additional call() argument-shape closure (mirrors Section 4 with uncovered
// categories): long double, signed/unsigned char, wchar_t, and an empty pack via
// the static idiom's proxy.
static_assert(call_probe::call_ok<long double>,
              "call(long double) must be viable (arithmetic, decays through convert_jni_arg)");
static_assert(call_probe::call_ok<signed char>,
              "call(signed char) must be viable (1-byte integral)");
static_assert(call_probe::call_ok<unsigned char>,
              "call(unsigned char) must be viable (1-byte integral)");
static_assert(call_probe::call_ok<wchar_t>,
              "call(wchar_t) must be viable (integral)");
static_assert(call_probe::call_ok<std::int64_t&, const std::string&, double&&>,
              "call(long&, const string&, double&&) mixed value categories must forward");
// set() additional reference/cv closure beyond Sections 6/15.
static_assert(set_probe::set_ok<const std::string_view&>,
              "set(const string_view&) must be viable");
static_assert(set_probe::set_ok<std::vector<std::uint8_t>>,
              "set(vector<uint8_t>) must be viable (byte[] write)");
static_assert(set_probe::set_ok<std::vector<std::int16_t>>,
              "set(vector<int16_t>) must be viable (short[] write)");
static_assert(set_probe::set_ok<std::vector<std::int32_t>>,
              "set(vector<int32_t>) must be viable (int[] write)");
static_assert(set_probe::set_ok<vmhook::method_proxy::value_t>,
              "set(method_proxy::value_t) must be viable (set dispatches on the value template arg)");

// =============================================================================
// 23. Tiny DETERMINISTIC runtime lane
// =============================================================================
// Almost every fact above is compile-time.  A handful of facts are genuinely
// runtime values (the variant active-index after a value-initialisation, the
// printed gate value) -- assert those through a deterministic check() harness so
// the executable does more than print "OK", while staying 100% JVM-free and
// byte-identical across runs (no addresses, no time, no platform branches).
namespace rt
{
    inline int g_failures{ 0 };
    inline auto check(const bool cond, const char* what) noexcept -> void
    {
        if (!cond)
        {
            std::printf("  [FAIL] %s\n", what);
            ++g_failures;
        }
    }
}

// -----------------------------------------------------------------------------
// main(): runs the small deterministic runtime lane, then reports.  All the
// heavy coverage above is in static_asserts the compiler already evaluated.
// -----------------------------------------------------------------------------
int main()
{
    // ---- Deterministic, JVM-free runtime checks ----------------------------
    // These touch genuine runtime VALUES (variant active indices after value-
    // initialisation, the gate macro as a runtime int) that complement the
    // compile-time matrix.  None depend on a JVM, an address, time, the OS, or
    // the compiler, so the output is byte-identical on every run / platform.

    // (1) The gate macro is a strict boolean at runtime too.
    rt::check(VMHOOK_HAS_DEDUCING_THIS == 0 || VMHOOK_HAS_DEDUCING_THIS == 1,
              "VMHOOK_HAS_DEDUCING_THIS is 0 or 1 at runtime");

    // (2) A value-initialised field_proxy::value_t holds its FIRST alternative
    // (bool) -- the variant's value-initialised state the get() fast paths and
    // the as_string() "" fallback rely on.
    {
        const vmhook::field_proxy::value_t fv{};
        rt::check(fv.data.index() == 0u,
                  "default field value_t holds alternative 0 (bool)");
        rt::check(fv.is_reference() == false,
                  "default field value_t is not a reference (bool alternative)");
        // The bool alternative converts to int as 0 (the documented null-proxy
        // numeric default behind get_field(x)->get() returning int).
        rt::check(static_cast<int>(fv) == 0,
                  "default field value_t converts to int 0");
        rt::check(fv.as_string().empty(),
                  "default field value_t as_string() is empty (numeric alternative)");
    }

    // (3) A value-initialised method_proxy::value_t holds std::monostate
    // (alternative 0) -- i.e. it reports is_void()/!is_string(), the documented
    // "void return / call failed" state of call() before any JVM dispatch.
    {
        const vmhook::method_proxy::value_t mv{};
        rt::check(mv.data.index() == 0u,
                  "default method value_t holds alternative 0 (monostate)");
        rt::check(mv.is_void() == true,
                  "default method value_t is_void() is true (monostate)");
        rt::check(mv.is_string() == false,
                  "default method value_t is_string() is false (monostate)");
        rt::check(mv.as_string().empty(),
                  "default method value_t as_string() is empty (monostate)");
        rt::check(static_cast<int>(mv) == 0,
                  "default method value_t converts to int 0 (monostate fallback)");
    }

    // (4) The two value_t variants carry the documented alternative counts
    // (field: 9 primitive/OOP alternatives; method: 11 incl. monostate + string).
    rt::check(std::variant_size_v<std::remove_cvref_t<decltype(vmhook::field_proxy::value_t{}.data)>> == 9u,
              "field value_t variant has 9 alternatives");
    rt::check(std::variant_size_v<std::remove_cvref_t<decltype(vmhook::method_proxy::value_t{}.data)>> == 11u,
              "method value_t variant has 11 alternatives");

    if (rt::g_failures != 0)
    {
        std::printf("vmhook unified call-syntax: %d RUNTIME CHECK(S) FAILED "
                    "[VMHOOK_HAS_DEDUCING_THIS=%d]\n",
                    rt::g_failures, VMHOOK_HAS_DEDUCING_THIS);
        return 1;
    }

    std::printf("vmhook unified call-syntax (exhaustive compile-time + runtime): OK "
                "[VMHOOK_HAS_DEDUCING_THIS=%d]\n",
                VMHOOK_HAS_DEDUCING_THIS);
    return 0;
}
