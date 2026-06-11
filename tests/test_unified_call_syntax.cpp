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

// -----------------------------------------------------------------------------
// main(): the executable merely proves the TU compiled; all coverage above is
// in static_asserts, evaluated by the compiler with zero runtime cost.
// -----------------------------------------------------------------------------
int main()
{
    std::printf("vmhook unified call-syntax (exhaustive compile-time): OK "
                "[VMHOOK_HAS_DEDUCING_THIS=%d]\n",
                VMHOOK_HAS_DEDUCING_THIS);
    return 0;
}
