// Exhaustive no-JVM unit tests for vmhook::make_unique<T>(args...) — the
// templated Java-object factory (vmhook.hpp, free function at vmhook namespace
// scope; the JNI backbone is detail::jni_make_unique / detail::make_unique).
//
// WHAT make_unique<T> ACTUALLY IS (and therefore what "maximum input coverage"
// means here).  Unlike a generic std::make_unique-style allocator, this factory
// NEVER constructs the C++ payload from the forwarded pack: it allocates a *Java*
// object (NewObjectA, or the TLAB fallback) and wraps the decoded OOP via
// std::make_unique<wrapper_type>(oop) — a SINGLE-oop construction every time.
// The forwarded args (args...) drive TWO things that are observable WITHOUT a
// live JVM, and they are the real perfect-forwarding surface of this function:
//
//   (1) The Java "<init>" descriptor the JNI path assembles:
//           "(" + (jni_signature_for_arg<remove_cvref_t<args_t>>() + ...) + ")V"
//       (vmhook.hpp jni_make_unique).  remove_cvref_t means the descriptor is
//       value-category INVARIANT — lvalue / const-lvalue / rvalue / mixed all
//       yield the SAME "<init>" descriptor for the same decayed types.  That
//       invariance IS the perfect-forwarding contract, and it is a pure
//       compile-time string we can assert.
//
//   (2) The construct(args...) fallback dispatch:
//           if constexpr (requires(wrapper_type& w, args_t&&... a)
//                         { w.construct(std::forward<args_t>(a)...); })
//       (vmhook.hpp make_unique).  This requires-probe is the EXACT predicate
//       that selects the construct() branch vs the "no matching construct()"
//       warning branch on the TLAB fallback.  Whether it is satisfied is a
//       function of the forwarded value categories (lvalue / rvalue / const)
//       and overload set — so replicating the predicate lets us assert perfect
//       forwarding of arbitrary ctor-arg shapes at compile time, including the
//       arity/convertibility footgun (an unintended construct(long) capturing
//       an int call by promotion).
//
// WHY construct() / the constructed Java fields are NOT runtime-checked here:
// make_unique<T> returns BEFORE either path runs when there is no JVM — its very
// first statement is ensure_current_java_thread(), which fails with no attached
// JavaThread and returns a null unique_ptr.  And construct() is reachable ONLY on
// the TLAB fallback, which needs a live klass/TLAB.  So the actual <init> call,
// field write-back, and construct() invocation are JVM-only and live in the JVM
// integration module tests/jvm/modules/make_unique.cpp + example.cpp.  Here we
// assert ONLY the safe, deterministic, flake-free surface:
//   * the COMPILE-TIME return-type / forwarding / descriptor / construct-detect
//     contracts (static_assert), and
//   * the no-JVM RUNTIME null contract over an exhaustive arg matrix (every
//     value category, 0..N args, every primitive + string + object/unique_ptr
//     arg, move-only args), proving no arg shape throws and all return null AND
//     that every forwarding instantiation of the template actually compiles.
//
// All checks are pure logic — no JVM, no threads, no timing — so the suite is
// fully deterministic on every OS/compiler in the CI matrix.
//
// NOTE on the -Werror string-literal footgun (documented library flaw, NOT
// worked around by touching library code): make_unique<T>("raw literal") routes
// the literal (decayed to const char*) into append_jni_arg's
//   value.l = jni_new_string_utf(arg ? std::string_view{arg} : std::string_view{})
// const-char* branch.  Because the argument originates from a string LITERAL,
// GCC knows its address can never be null and fires -Werror=address /
// -Werror=nonnull-compare; the construct() requires-forwarding instantiation for
// const char[N] trips the same diagnostic.  This standalone test target is built
// with -Werror (vmhook_apply_warnings, no NO_WERROR), so to exercise the SAME
// const char* code path without breaking the warnings-as-errors job we always
// pass c-strings through a named `const char*` LVALUE — never a bare literal at
// the make_unique call site.  The behaviour exercised is identical; only the
// literal-address diagnostic is side-stepped.
#include <vmhook/vmhook.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
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

// ---------------------------------------------------------------------------
// Wrapper types.  Each derives from vmhook::object<T> with the required
// explicit T(vmhook::oop_t) constructor, exactly like the canonical wrapper
// pattern (test_object_factory.cpp / test_api_surface.cpp).  make_unique<T>
// wraps a decoded oop into one of these via std::make_unique<T>(oop).
// ---------------------------------------------------------------------------

// Plain wrapper: NO construct(...) overload at all.  Used to prove make_unique
// with args still compiles + returns null (no JVM), routing through the
// "no matching construct()" branch on the would-be fallback.
class plain_wrapper : public vmhook::object<plain_wrapper>
{
public:
    explicit plain_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<plain_wrapper>{ oop }
    {
    }
};

// Wrapper exposing a multi-arg construct(int, const std::string&) — routes
// make_unique<T>(int, string) through the construct() branch of the
// if-constexpr at compile time.
class ctor_wrapper : public vmhook::object<ctor_wrapper>
{
public:
    explicit ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<ctor_wrapper>{ oop }
    {
    }
    auto construct(int, const std::string&) -> void {}
};

// Wrapper with a SINGLE construct(bool) — mirrors the JVM fixture whose only
// fallback-selecting overload is (Z)V.  Lets us assert the requires-probe is
// satisfied for bool and (by promotion) for other arithmetic-to-bool calls.
class bool_ctor_wrapper : public vmhook::object<bool_ctor_wrapper>
{
public:
    explicit bool_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<bool_ctor_wrapper>{ oop }
    {
    }
    auto construct(bool) -> void {}
};

// Wrapper whose construct() takes a NON-const lvalue reference.  The requires
// probe forwards args_t&&; an rvalue cannot bind to int&, an lvalue can.  This
// is the perfect-forwarding value-category discriminator.
class lref_ctor_wrapper : public vmhook::object<lref_ctor_wrapper>
{
public:
    explicit lref_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<lref_ctor_wrapper>{ oop }
    {
    }
    auto construct(int&) -> void {}
};

// Wrapper whose construct() takes an RVALUE reference.  Symmetric to the above:
// only an rvalue (or xvalue) forwarded arg can bind to int&&.
class rref_ctor_wrapper : public vmhook::object<rref_ctor_wrapper>
{
public:
    explicit rref_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<rref_ctor_wrapper>{ oop }
    {
    }
    auto construct(int&&) -> void {}
};

// Wrapper whose construct() takes a const lvalue reference — binds to BOTH
// lvalues and rvalues (the permissive category).
class cref_ctor_wrapper : public vmhook::object<cref_ctor_wrapper>
{
public:
    explicit cref_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<cref_ctor_wrapper>{ oop }
    {
    }
    auto construct(const int&) -> void {}
};

// Wrapper whose only construct() takes a wider arithmetic type (long).  An int
// call satisfies the probe via integral promotion — the flaw-#4 footgun where
// an unintended overload silently captures the fallback.  Pinned by inspection.
class wide_ctor_wrapper : public vmhook::object<wide_ctor_wrapper>
{
public:
    explicit wide_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<wide_ctor_wrapper>{ oop }
    {
    }
    auto construct(long) -> void {}
};

// A move-only argument type (non-copyable, movable).  Forwarded as an rvalue it
// must thread through make_unique's std::forward pack without a copy; this
// proves the arg pipeline never silently copies a move-only ctor arg.
struct move_only
{
    int tag{ 0 };
    move_only() = default;
    explicit move_only(int t) : tag{ t } {}
    move_only(const move_only&)            = delete;
    move_only& operator=(const move_only&) = delete;
    move_only(move_only&&) noexcept            = default;
    move_only& operator=(move_only&&) noexcept = default;
};

// Wrapper whose construct() consumes a move_only by value (sink) — the probe is
// satisfied only when the forwarded arg is an rvalue (a copy is deleted).
class move_sink_wrapper : public vmhook::object<move_sink_wrapper>
{
public:
    explicit move_sink_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<move_sink_wrapper>{ oop }
    {
    }
    auto construct(move_only) -> void {}
};

// Wrapper exposing a NO-ARG construct() — proves the requires-probe is satisfied
// for the empty pack (make_unique<W>() would invoke construct() on the fallback)
// and is NOT satisfied once any arg is forwarded.  The mirror image of
// plain_wrapper (which has no construct() at all).
class noarg_ctor_wrapper : public vmhook::object<noarg_ctor_wrapper>
{
public:
    explicit noarg_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<noarg_ctor_wrapper>{ oop }
    {
    }
    auto construct() -> void {}
};

// Wrapper exposing construct(int, std::int64_t, double) — mirrors the JVM
// fixture's (IJD)V constructor.  Lets the multi-arg construct() branch be pinned
// for the exact heterogeneous pack make_unique would forward.
class ijd_ctor_wrapper : public vmhook::object<ijd_ctor_wrapper>
{
public:
    explicit ijd_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<ijd_ctor_wrapper>{ oop }
    {
    }
    auto construct(int, std::int64_t, double) -> void {}
};

// Wrapper exposing construct(std::string, int) — mirrors the (Ljava/lang/String;I)V
// constructor.  Used to confirm the probe is satisfied for the string-family
// arg-shapes (std::string / const char* / std::string_view) the JNI path accepts.
class string_int_ctor_wrapper : public vmhook::object<string_int_ctor_wrapper>
{
public:
    explicit string_int_ctor_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<string_int_ctor_wrapper>{ oop }
    {
    }
    auto construct(std::string, int) -> void {}
};

// A second move-only sink whose construct() takes the move_only by RVALUE
// reference — the requires-probe must accept a forwarded rvalue and reject a
// forwarded lvalue, the strictest value-category discriminator (an rvalue ref
// cannot bind an lvalue at all, independent of copyability).
class move_rref_sink_wrapper : public vmhook::object<move_rref_sink_wrapper>
{
public:
    explicit move_rref_sink_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<move_rref_sink_wrapper>{ oop }
    {
    }
    auto construct(move_only&&) -> void {}
};

// Wrapper used purely to exercise the object-reference *descriptor* arm of
// jni_signature_for_arg (an object_base-derived arg / unique_ptr<object> arg
// resolves "L<registered-name>;" or the "Ljava/lang/Object;" fallback).  Kept
// separate from plain_wrapper so its registration state can be toggled in an
// isolated, snapshot/restore block without perturbing the other sections.
class object_arg_wrapper : public vmhook::object<object_arg_wrapper>
{
public:
    explicit object_arg_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<object_arg_wrapper>{ oop }
    {
    }
};

// ---------------------------------------------------------------------------
// Replicate make_unique's EXACT construct()-detection predicate so we can
// static_assert on it for any wrapper + forwarded value categories.  This is
// the same expression the library uses inside make_unique's `if constexpr`:
//     requires(wrapper_type& w, args_t&&... a){ w.construct(forward<args_t>(a)...); }
// Mirroring it (rather than calling make_unique) is the only way to observe the
// branch selection at compile time, because make_unique itself returns at the
// no-JVM thread guard long before the if-constexpr is reached at run time.
// ---------------------------------------------------------------------------
template<typename wrapper_type, typename... args_t>
concept has_matching_construct =
    requires(wrapper_type& w, args_t&&... a) { w.construct(std::forward<args_t>(a)...); };

// The compile-time JNI descriptor builder make_unique uses per arg.  Each arg's
// contribution is jni_signature_for_arg<remove_cvref_t<arg>>(); make_unique
// concatenates them inside "(" ... ")V".  We reproduce that assembly EXACTLY so
// the test's "expected descriptor" is derived from the same builder the library
// calls, then assert value-category invariance against it.
template<typename... args_t>
static auto init_descriptor() -> std::string
{
    std::string sig{ "(" };
    ((sig += vmhook::detail::jni_signature_for_arg<std::remove_cvref_t<args_t>>()), ...);
    sig += ")V";
    return sig;
}

// Run make_unique<W>(forwarded...) with NO JVM and assert it (a) does not throw
// and (b) yields a null unique_ptr.  Returns the conjunction so callers can fold
// it into a single check.  make_unique is NOT noexcept, hence the try/catch
// (mirrors test_object_factory.cpp / test_api_surface_extended.cpp).
template<typename wrapper_type, typename... args_t>
static auto make_unique_is_null_and_safe(args_t&&... args) -> bool
{
    // Sentinel is reinterpret_cast<W*>(0): a null pointer, so the assertion still
    // proves make_unique RETURNED null (it cannot make a null unique_ptr
    // non-null) while the unique_ptr's destructor is provably a no-op.
    std::unique_ptr<wrapper_type> obj{ reinterpret_cast<wrapper_type*>(0) };
    bool threw{ false };
    try { obj = vmhook::make_unique<wrapper_type>(std::forward<args_t>(args)...); }
    catch (...) { threw = true; }
    return obj == nullptr && !threw;
}

// Construct a wrapper DIRECTLY over a given (null or sentinel) oop the way
// make_unique's success path does internally (std::make_unique<W>(oop)), then
// assert the produced unique_ptr is a sane owning handle whose wrapped object
// reports EXACTLY the oop it was built from via object_base::get_instance().
// This is the no-JVM analogue of "the factory returned a usable wrapper": the
// JVM-only paths return std::make_unique<W>(decoded_oop), and here we prove that
// the W(oop) construction + ownership + get_instance() round-trip is fault-free
// for any oop value (no JVM, no deref of the oop — get_instance just returns the
// stored pointer).  `oop` is the bit-pattern get_instance() must report back.
template<typename wrapper_type>
static auto wrap_oop_roundtrips(vmhook::oop_t oop, bool& threw) -> bool
{
    threw = false;
    try
    {
        std::unique_ptr<wrapper_type> p{ std::make_unique<wrapper_type>(oop) };
        if (!p) { return false; }
        // The wrapper must expose the exact oop it was constructed from.
        if (p->get_instance() != oop) { return false; }
        // Owning-handle sanity on a real (possibly non-null) pointer.
        if (p.get() == nullptr) { return false; }
        // Move the ownership; the moved-to handle still reports the same oop and
        // the moved-from handle is emptied (unique_ptr move contract).
        std::unique_ptr<wrapper_type> q{ std::move(p) };
        if (p != nullptr) { return false; }
        if (!q || q->get_instance() != oop) { return false; }
        return true;
    }
    catch (...)
    {
        threw = true;
        return false;
    }
}

int main()
{
    // --- Precondition: we really are running with no JVM --------------------
    // make_unique's first statement (ensure_current_java_thread) fails here, so
    // every make_unique below returns null at that guard.  current_jni_env being
    // null is the same no-JVM signal the sibling no-JVM suites assert.
    check("precondition_no_jvm_env_is_null",
          vmhook::hotspot::current_jni_env == nullptr);

    // =====================================================================
    // SECTION A — RETURN-TYPE CONTRACT (compile-time).
    // make_unique<W>(args...) is EXACTLY std::unique_ptr<W> for every arg pack
    // shape: 0 args, 1..N args, and every value category (lvalue / const-lvalue
    // / rvalue / mixed).  The owning pointer's element_type is W and its deleter
    // is the default deleter (so it is the canonical owning pointer, not some
    // aliasing/custom-deleter variant).  decltype on a never-evaluated call
    // expression makes this a pure compile-time assertion.
    // =====================================================================
    {
        // 0 args.
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>()),
            std::unique_ptr<plain_wrapper>>,
            "make_unique<W>() must return std::unique_ptr<W>");

        // 1 arg, each value category.
        int lv{ 7 };
        const int clv{ 7 };
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>(lv)),
            std::unique_ptr<plain_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>(clv)),
            std::unique_ptr<plain_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>(7)),
            std::unique_ptr<plain_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>(std::move(lv))),
            std::unique_ptr<plain_wrapper>>);

        // N args, mixed categories + mixed types.
        std::string s{ "x" };
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>(lv, clv, std::move(s), 3.5, true)),
            std::unique_ptr<plain_wrapper>>);

        // The returned type is the OWNING pointer with the DEFAULT deleter and
        // element_type W (canonical unique_ptr, not a custom-deleter variant).
        using ret_t = decltype(vmhook::make_unique<plain_wrapper>());
        static_assert(std::is_same_v<ret_t::element_type, plain_wrapper>,
                      "element_type must be the wrapper type");
        static_assert(std::is_same_v<ret_t::deleter_type, std::default_delete<plain_wrapper>>,
                      "deleter must be std::default_delete<W>");

        // The return type holds across DIFFERENT wrapper types (it is keyed on
        // the explicit template argument, never on the arg pack).
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<ctor_wrapper>(1, std::string{ "y" })),
            std::unique_ptr<ctor_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<bool_ctor_wrapper>(true)),
            std::unique_ptr<bool_ctor_wrapper>>);

        // The owning pointer is MOVE-ONLY (copy ctor/assign are deleted): the
        // factory hands out unique ownership, never a shared/copyable handle.
        static_assert(!std::is_copy_constructible_v<ret_t>,
                      "unique_ptr return must be non-copyable");
        static_assert(!std::is_copy_assignable_v<ret_t>,
                      "unique_ptr return must be non-copy-assignable");
        static_assert(std::is_move_constructible_v<ret_t>,
                      "unique_ptr return must be movable");
        static_assert(std::is_nothrow_move_constructible_v<ret_t>,
                      "unique_ptr move must be noexcept");
        // Move ASSIGNMENT is also available and noexcept (the factory result can
        // be re-seated into an existing owning slot, e.g. `obj = make_unique...`).
        static_assert(std::is_move_assignable_v<ret_t>,
                      "unique_ptr return must be move-assignable");
        static_assert(std::is_nothrow_move_assignable_v<ret_t>,
                      "unique_ptr move-assign must be noexcept");
        // The return is NOT a raw pointer / reference / array — it is precisely the
        // class template std::unique_ptr (a value-type owning handle).
        static_assert(!std::is_pointer_v<ret_t>);
        static_assert(!std::is_reference_v<ret_t>);
        static_assert(!std::is_array_v<ret_t>);
        static_assert(std::is_class_v<ret_t>);
        // pointer typedef is W* (the managed raw pointer type), confirming the
        // single-object (non-array) unique_ptr specialisation.
        static_assert(std::is_same_v<ret_t::pointer, plain_wrapper*>,
                      "unique_ptr<W>::pointer must be W*");

        // The return type is INVARIANT to the forwarded arg pack: the SAME
        // std::unique_ptr<W> is produced whether 0, 1, or many args of any
        // category/type are passed (it is keyed only on the explicit W).  This is
        // the return-type half of the perfect-forwarding contract.
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>(std::int8_t{ 1 })),
            decltype(vmhook::make_unique<plain_wrapper>())>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<plain_wrapper>(1, 2L, 3.0, true, std::string{})),
            decltype(vmhook::make_unique<plain_wrapper>())>);

        // Return type holds for EVERY wrapper variant defined in this TU — the
        // factory is uniform across the whole wrapper-shape matrix (construct()
        // overload set never bleeds into the return type).
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<noarg_ctor_wrapper>()),
            std::unique_ptr<noarg_ctor_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<ijd_ctor_wrapper>(1, std::int64_t{ 2 }, 3.0)),
            std::unique_ptr<ijd_ctor_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<string_int_ctor_wrapper>(std::string{ "k" }, 1)),
            std::unique_ptr<string_int_ctor_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<lref_ctor_wrapper>(std::declval<int&>())),
            std::unique_ptr<lref_ctor_wrapper>>);
        static_assert(std::is_same_v<
            decltype(vmhook::make_unique<rref_ctor_wrapper>(7)),
            std::unique_ptr<rref_ctor_wrapper>>);

        check("A_return_type_is_unique_ptr_of_wrapper_compile_time", true);
    }

    // =====================================================================
    // SECTION B — PERFECT-FORWARDING construct()-detection (compile-time).
    // make_unique selects its construct() branch via
    //   requires(W& w, args_t&&... a){ w.construct(forward<args_t>(a)...); }
    // The chosen branch is a pure function of (a) the wrapper's construct()
    // overload set and (b) the forwarded VALUE CATEGORIES.  has_matching_construct
    // is that exact predicate; we assert it across the category matrix to prove
    // perfect forwarding (lvalue / const-lvalue / rvalue / mixed) is preserved
    // all the way into the construct() call the fallback would make.
    // =====================================================================
    {
        // (1) Wrapper with NO construct(): the probe is UNSATISFIED for every
        // arg shape — make_unique would take the "no matching construct()"
        // warning branch.  (Zero-arg too: there is no construct() to call.)
        static_assert(!has_matching_construct<plain_wrapper>);
        static_assert(!has_matching_construct<plain_wrapper, int>);
        static_assert(!has_matching_construct<plain_wrapper, int, std::string>);

        // (2) construct(int, const std::string&): satisfied ONLY for a matching
        // 2-arg pack (int-convertible, string-convertible), and NOT for arity
        // mismatches or for a single arg.
        static_assert(has_matching_construct<ctor_wrapper, int, std::string>);
        static_assert(has_matching_construct<ctor_wrapper, int, const std::string&>);
        static_assert(has_matching_construct<ctor_wrapper, int&, std::string&>);     // lvalues OK
        static_assert(has_matching_construct<ctor_wrapper, int&&, std::string&&>);   // rvalues OK
        static_assert(has_matching_construct<ctor_wrapper, short, const char*>);     // convertible
        static_assert(!has_matching_construct<ctor_wrapper>);                        // arity 0
        static_assert(!has_matching_construct<ctor_wrapper, int>);                   // arity 1
        static_assert(!has_matching_construct<ctor_wrapper, int, std::string, int>); // arity 3
        static_assert(!has_matching_construct<ctor_wrapper, std::string, int>);      // wrong order/types

        // (3) construct(bool): satisfied for bool and for arithmetic args that
        // convert to bool, and NOT for the wrong arity.
        static_assert(has_matching_construct<bool_ctor_wrapper, bool>);
        static_assert(has_matching_construct<bool_ctor_wrapper, int>);   // int -> bool conversion
        static_assert(!has_matching_construct<bool_ctor_wrapper>);
        static_assert(!has_matching_construct<bool_ctor_wrapper, int, int>);

        // (4) THE PERFECT-FORWARDING DISCRIMINATOR.  construct(int&) binds ONLY
        // an lvalue; construct(int&&) binds ONLY an rvalue; construct(const int&)
        // binds BOTH.  The probe forwards args_t&&..., so the SATISFACTION of
        // the predicate flips with the forwarded value category — which is
        // exactly what proves make_unique forwards categories faithfully.
        static_assert(has_matching_construct<lref_ctor_wrapper, int&>);   // lvalue binds int&
        static_assert(!has_matching_construct<lref_ctor_wrapper, int>);   // prvalue cannot bind int&
        static_assert(!has_matching_construct<lref_ctor_wrapper, int&&>); // xvalue cannot bind int&

        static_assert(has_matching_construct<rref_ctor_wrapper, int>);    // prvalue binds int&&
        static_assert(has_matching_construct<rref_ctor_wrapper, int&&>);  // xvalue binds int&&
        static_assert(!has_matching_construct<rref_ctor_wrapper, int&>);  // lvalue cannot bind int&&

        static_assert(has_matching_construct<cref_ctor_wrapper, int&>);   // const& binds lvalue
        static_assert(has_matching_construct<cref_ctor_wrapper, int>);    // const& binds prvalue
        static_assert(has_matching_construct<cref_ctor_wrapper, int&&>);  // const& binds xvalue
        static_assert(has_matching_construct<cref_ctor_wrapper, const int&>);

        // (5) MOVE-ONLY arg sink construct(move_only): satisfied ONLY when the
        // forwarded arg is an rvalue (a copy is deleted, so an lvalue makes the
        // by-value parameter ill-formed).  This proves a move-only ctor arg is
        // perfectly forwarded as an rvalue (no hidden copy).
        static_assert(has_matching_construct<move_sink_wrapper, move_only>);    // prvalue
        static_assert(has_matching_construct<move_sink_wrapper, move_only&&>);  // xvalue
        static_assert(!has_matching_construct<move_sink_wrapper, move_only&>);  // lvalue -> needs copy (deleted)
        static_assert(!has_matching_construct<move_sink_wrapper, const move_only&>);

        // (5b) MOVE-ONLY arg sink whose parameter is an RVALUE REFERENCE
        // construct(move_only&&): the binding rule (not copyability) is the
        // discriminator — an rvalue binds, an lvalue / const-lvalue cannot bind to
        // a non-const rvalue reference at all.  Strictest forwarding proof.
        static_assert(has_matching_construct<move_rref_sink_wrapper, move_only>);     // prvalue
        static_assert(has_matching_construct<move_rref_sink_wrapper, move_only&&>);   // xvalue
        static_assert(!has_matching_construct<move_rref_sink_wrapper, move_only&>);   // lvalue
        static_assert(!has_matching_construct<move_rref_sink_wrapper, const move_only&>);

        // (6) NO-ARG construct(): satisfied ONLY by the empty pack — make_unique<W>()
        // on the fallback would invoke construct().  Any forwarded arg makes the
        // probe fail (arity 1+ does not match the zero-parameter construct()).
        static_assert(has_matching_construct<noarg_ctor_wrapper>);
        static_assert(!has_matching_construct<noarg_ctor_wrapper, int>);
        static_assert(!has_matching_construct<noarg_ctor_wrapper, int, int>);
        // ...and a wrapper that has NO construct() at all still fails the empty
        // probe (the negative control for the line above).
        static_assert(!has_matching_construct<plain_wrapper>);

        // (7) MULTI-ARG construct(int, std::int64_t, double): satisfied for the
        // exact 3-arg pack across all value-category spellings (the descriptor
        // twin of the (IJD)V constructor), and arity-checked on both sides.
        static_assert(has_matching_construct<ijd_ctor_wrapper, int, std::int64_t, double>);
        static_assert(has_matching_construct<ijd_ctor_wrapper, int&, std::int64_t&, double&>);
        static_assert(has_matching_construct<ijd_ctor_wrapper, const int&, const std::int64_t&, const double&>);
        static_assert(has_matching_construct<ijd_ctor_wrapper, int&&, std::int64_t&&, double&&>);
        static_assert(has_matching_construct<ijd_ctor_wrapper, short, int, float>);  // all convertible
        static_assert(!has_matching_construct<ijd_ctor_wrapper, int, std::int64_t>);          // arity 2
        static_assert(!has_matching_construct<ijd_ctor_wrapper, int, std::int64_t, double, int>); // arity 4

        // (8) STRING-FAMILY construct(std::string, int).  IMPORTANT asymmetry:
        // make_unique's JNI *descriptor* path accepts std::string, const char*,
        // AND std::string_view for a String parameter (all map to
        // "Ljava/lang/String;", see Section D), but the construct() *fallback*
        // probe is plain C++ overload resolution against `std::string` — which
        // accepts std::string and const char* (the implicit
        // basic_string(const char*) ctor) yet REJECTS std::string_view, because
        // basic_string's string_view-accepting constructor is `explicit`.  Pinned
        // here so a regression that (de)couples the two paths is caught.
        static_assert(has_matching_construct<string_int_ctor_wrapper, std::string, int>);
        static_assert(has_matching_construct<string_int_ctor_wrapper, const char*, int>);
        static_assert(!has_matching_construct<string_int_ctor_wrapper, std::string_view, int>); // explicit ctor
        static_assert(has_matching_construct<string_int_ctor_wrapper, const std::string&, int&>);
        static_assert(has_matching_construct<string_int_ctor_wrapper, std::string&&, int&&>);
        static_assert(!has_matching_construct<string_int_ctor_wrapper, int, std::string>); // wrong order

        check("B_construct_detection_follows_forwarded_value_category", true);
    }

    // =====================================================================
    // SECTION C — construct() ARITY/PROMOTION FOOTGUN (flaw #4, pinned).
    // The requires-probe is convertibility/overload-resolution based, so a
    // wrapper whose ONLY construct() takes a WIDER arithmetic type captures a
    // narrower fallback call via promotion.  construct(long) is selected for an
    // int / short / char fallback even though no construct(int) exists — the
    // silent-wrong-initialiser hazard the brief calls out.  Pinned, not "fixed":
    // wrappers should declare exactly the overloads they intend.
    // =====================================================================
    {
        static_assert(has_matching_construct<wide_ctor_wrapper, long>);
        static_assert(has_matching_construct<wide_ctor_wrapper, int>);    // int   -> long promotion
        static_assert(has_matching_construct<wide_ctor_wrapper, short>);  // short -> long
        static_assert(has_matching_construct<wide_ctor_wrapper, char>);   // char  -> long
        static_assert(has_matching_construct<wide_ctor_wrapper, bool>);   // bool  -> long
        // Still arity-checked: zero / two args do not match the single-param ctor.
        static_assert(!has_matching_construct<wide_ctor_wrapper>);
        static_assert(!has_matching_construct<wide_ctor_wrapper, int, int>);

        check("C_construct_probe_matches_wider_overload_by_promotion", true);
    }

    // =====================================================================
    // SECTION D — "<init>" DESCRIPTOR ASSEMBLY & VALUE-CATEGORY INVARIANCE.
    // make_unique's JNI path builds the constructor descriptor as
    //   "(" + jni_signature_for_arg<remove_cvref_t<args_t>>()... + ")V".
    // init_descriptor<...>() reproduces that EXACT assembly.  We assert:
    //   (a) the empty-pack descriptor is "()V";
    //   (b) each arity/type combination produces the documented descriptor; and
    //   (c) the descriptor is IDENTICAL across lvalue / const-lvalue / rvalue /
    //       const-rvalue spellings of the same underlying types — the
    //       remove_cvref_t decay IS the perfect-forwarding guarantee at the
    //       descriptor layer (categories never leak into the Java signature).
    // (Per-type descriptor correctness in isolation is covered exhaustively in
    // test_jni_arg_packing.cpp Section E; here the focus is the make_unique
    // ASSEMBLY + category invariance of the whole "(...)V" string.)
    // =====================================================================
    {
        // (a) Zero args -> "()V".
        check("D_descriptor_empty_pack_is_void_ctor",
              init_descriptor<>() == "()V");

        // (b) Single-arg descriptors for EVERY type jni_signature_for_arg maps.
        // These mirror the live header's per-type dispatch exactly:
        //   bool->Z  int8/uint8->B  int16->S  uint16->C  int32/uint32->I
        //   int64/uint64->J  float->F  double->D  string family->Ljava/lang/String;
        // Each fixed-width std::intNN_t is endianness/word-size invariant, so the
        // expected descriptor is identical on every platform in the CI matrix.
        check("D_descriptor_bool",    init_descriptor<bool>() == "(Z)V");
        check("D_descriptor_int8",    init_descriptor<std::int8_t>() == "(B)V");
        check("D_descriptor_uint8",   init_descriptor<std::uint8_t>() == "(B)V");
        check("D_descriptor_int16",   init_descriptor<std::int16_t>() == "(S)V");
        // uint16 -> 'C' (Java char), DISTINCT from int16 -> 'S'.  This is the one
        // mapping where the signedness flips the descriptor letter; pin both.
        check("D_descriptor_uint16",  init_descriptor<std::uint16_t>() == "(C)V");
        check("D_descriptor_int32",   init_descriptor<std::int32_t>() == "(I)V");
        check("D_descriptor_uint32",  init_descriptor<std::uint32_t>() == "(I)V");
        check("D_descriptor_int",     init_descriptor<int>() == "(I)V");
        check("D_descriptor_int64",   init_descriptor<std::int64_t>() == "(J)V");
        check("D_descriptor_uint64",  init_descriptor<std::uint64_t>() == "(J)V");
        check("D_descriptor_long",    init_descriptor<std::int64_t>() == "(J)V");
        check("D_descriptor_float",   init_descriptor<float>() == "(F)V");
        check("D_descriptor_double",  init_descriptor<double>() == "(D)V");
        check("D_descriptor_string",  init_descriptor<std::string>() == "(Ljava/lang/String;)V");

        // uint16 'C' vs int16 'S' really differ (guards against a copy-paste that
        // collapses them) — assert the descriptors are NOT equal.
        check("D_descriptor_uint16_differs_from_int16",
              init_descriptor<std::uint16_t>() != init_descriptor<std::int16_t>());
        // int8/uint8 collapse to the SAME 'B' (signedness ignored for byte).
        check("D_descriptor_int8_equals_uint8",
              init_descriptor<std::int8_t>() == init_descriptor<std::uint8_t>());

        // (b') Multi-arg descriptors matching the JVM fixture's constructors
        // ()V (I)V (II)V (IJD)V (Ljava/lang/String;)V (Ljava/lang/String;I)V.
        check("D_descriptor_II",
              init_descriptor<int, int>() == "(II)V");
        check("D_descriptor_IJD",
              init_descriptor<int, std::int64_t, double>() == "(IJD)V");
        check("D_descriptor_StringI",
              init_descriptor<std::string, int>() == "(Ljava/lang/String;I)V");

        // (b'') The WHOLE string family — std::string, const char*, char*
        // (non-const), and std::string_view — yields the SAME String descriptor
        // (jni_signature_for_arg lists all four in one branch).  Proves the
        // assembly is type-, not spelling-, driven.
        check("D_descriptor_cstring",
              init_descriptor<const char*>() == "(Ljava/lang/String;)V");
        check("D_descriptor_mutable_cstring",
              init_descriptor<char*>() == "(Ljava/lang/String;)V");
        check("D_descriptor_string_view",
              init_descriptor<std::string_view>() == "(Ljava/lang/String;)V");
        check("D_descriptor_all_string_spellings_agree",
              init_descriptor<std::string>() == init_descriptor<const char*>()
                  && init_descriptor<char*>() == init_descriptor<std::string_view>()
                  && init_descriptor<std::string>() == init_descriptor<std::string_view>());

        // (b''') Longer descriptors that exercise the NEW per-type letters in a
        // single assembled "(...)V".  These are the descriptors make_unique would
        // hand GetMethodID for ctors taking byte/short/char/float mixes.
        check("D_descriptor_BSCF",
              init_descriptor<std::int8_t, std::int16_t, std::uint16_t, float>() == "(BSCF)V");
        check("D_descriptor_ZBI",
              init_descriptor<bool, std::uint8_t, std::int32_t>() == "(ZBI)V");
        check("D_descriptor_FD",
              init_descriptor<float, double>() == "(FD)V");
        check("D_descriptor_all_primitive_letters",
              init_descriptor<bool, std::int8_t, std::int16_t, std::uint16_t,
                              std::int32_t, std::int64_t, float, double>() == "(ZBSCIJFD)V");
        // String interleaved with the new primitive letters.
        check("D_descriptor_string_then_byte_then_float",
              init_descriptor<std::string, std::int8_t, float>()
                  == "(Ljava/lang/String;BF)V");

        // (c) VALUE-CATEGORY INVARIANCE: every cv/ref spelling of (int, string)
        // decays to the SAME "(ILjava/lang/String;)V".  This is the descriptor-
        // level proof that make_unique's perfect forwarding never changes the
        // Java constructor it targets, regardless of how the caller passes args.
        const std::string canonical{ "(ILjava/lang/String;)V" };
        check("D_invariance_value_value",
              init_descriptor<int, std::string>() == canonical);
        check("D_invariance_lref_lref",
              init_descriptor<int&, std::string&>() == canonical);
        check("D_invariance_clref_clref",
              init_descriptor<const int&, const std::string&>() == canonical);
        check("D_invariance_rref_rref",
              init_descriptor<int&&, std::string&&>() == canonical);
        check("D_invariance_const_rref",
              init_descriptor<const int&&, const std::string&&>() == canonical);
        check("D_invariance_mixed_categories",
              init_descriptor<int&, std::string&&>() == canonical);
        check("D_invariance_const_value",
              init_descriptor<const int, const std::string>() == canonical);

        // The same invariance must hold for a LONGER, multi-type pack.
        const std::string canonical_long{ "(ZIJDLjava/lang/String;)V" };
        check("D_invariance_long_pack_value",
              init_descriptor<bool, int, std::int64_t, double, std::string>() == canonical_long);
        check("D_invariance_long_pack_mixed_categories",
              init_descriptor<bool&, const int&, std::int64_t&&, const double&, std::string&>()
                  == canonical_long);

        // remove_cvref_t<args_t> is EXACTLY what jni_make_unique uses when it
        // assembles `signature`, so init_descriptor here is byte-identical to
        // the descriptor make_unique would hand GetMethodID.  Pin that the
        // decayed-type descriptor equals the bare-type descriptor.
        check("D_decay_matches_bare_for_int",
              init_descriptor<const int&>() == init_descriptor<int>());
        check("D_decay_matches_bare_for_string",
              init_descriptor<std::string&&>() == init_descriptor<std::string>());
        // Same decay invariance for EACH new primitive letter.
        check("D_decay_matches_bare_for_int8",
              init_descriptor<const std::int8_t&>() == init_descriptor<std::int8_t>());
        check("D_decay_matches_bare_for_uint16",
              init_descriptor<std::uint16_t&&>() == init_descriptor<std::uint16_t>());
        check("D_decay_matches_bare_for_float",
              init_descriptor<const float&>() == init_descriptor<float>());
        check("D_decay_matches_bare_for_int64",
              init_descriptor<std::int64_t&>() == init_descriptor<std::int64_t>());

        // (c') VALUE-CATEGORY INVARIANCE over a pack of the NEW letters: every
        // cv/ref spelling of (byte, short, char, float) decays to one descriptor.
        const std::string canonical_bscf{ "(BSCF)V" };
        check("D_invariance_bscf_value",
              init_descriptor<std::int8_t, std::int16_t, std::uint16_t, float>() == canonical_bscf);
        check("D_invariance_bscf_lref",
              init_descriptor<std::int8_t&, std::int16_t&, std::uint16_t&, float&>() == canonical_bscf);
        check("D_invariance_bscf_clref",
              init_descriptor<const std::int8_t&, const std::int16_t&,
                              const std::uint16_t&, const float&>() == canonical_bscf);
        check("D_invariance_bscf_rref",
              init_descriptor<std::int8_t&&, std::int16_t&&, std::uint16_t&&, float&&>() == canonical_bscf);
        check("D_invariance_bscf_mixed",
              init_descriptor<std::int8_t&, const std::int16_t&,
                              std::uint16_t&&, const float>() == canonical_bscf);

        // The c-string family is also category-invariant at the descriptor layer:
        // a const char*& (lvalue ref to pointer) decays to const char* -> String.
        check("D_invariance_cstring_lref",
              init_descriptor<const char*&>() == init_descriptor<const char*>());
        check("D_invariance_cstring_rref",
              init_descriptor<const char*&&>() == init_descriptor<const char*>());
    }

    // =====================================================================
    // SECTION D2 — OBJECT-REFERENCE arg descriptor (the wrapper-as-ctor-arg arm).
    // make_unique can take an object_base-derived arg or a unique_ptr<wrapper>
    // arg; jni_signature_for_arg resolves its descriptor to "L<registered-name>;"
    // by looking the wrapper's typeid up in type_to_class_map, and FALLS BACK to
    // "Ljava/lang/Object;" when the wrapper is unregistered.  Both arms are
    // deterministic with no JVM (pure map lookup + string assembly), so we pin
    // them inside a snapshot/restore block (identical discipline to Section F) to
    // keep the suite order-independent.  This is the descriptor surface for the
    // "constructor that takes another object" case the header comments call out.
    // =====================================================================
    {
        std::unordered_map<std::type_index, std::string> saved_types{};
        {
            std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
            saved_types = vmhook::type_to_class_map;
        }

        // Ensure object_arg_wrapper is UNREGISTERED, then assert the fallback.
        {
            std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
            vmhook::type_to_class_map.erase(std::type_index{ typeid(object_arg_wrapper) });
        }
        check("D2_unregistered_object_by_value_is_jlobject",
              init_descriptor<object_arg_wrapper>() == "(Ljava/lang/Object;)V");
        check("D2_unregistered_unique_ptr_is_jlobject",
              init_descriptor<std::unique_ptr<object_arg_wrapper>>() == "(Ljava/lang/Object;)V");

        // Register it directly in the map (the pure-bookkeeping half of
        // register_class; find_class needs a JVM and is out of scope), then the
        // descriptor must read the registered binary class name.
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(object_arg_wrapper) }, std::string{ "com/example/ObjArg" });
        check("D2_registered_object_by_value_uses_class_name",
              init_descriptor<object_arg_wrapper>() == "(Lcom/example/ObjArg;)V");
        check("D2_registered_unique_ptr_uses_class_name",
              init_descriptor<std::unique_ptr<object_arg_wrapper>>() == "(Lcom/example/ObjArg;)V");
        // The by-value object arm and the unique_ptr<object> arm agree (both
        // resolve the SAME wrapper typeid -> same "L...;" string).
        check("D2_object_value_and_unique_ptr_descriptors_agree",
              init_descriptor<object_arg_wrapper>()
                  == init_descriptor<std::unique_ptr<object_arg_wrapper>>());
        // cv/ref spellings of the object arg decay to the same descriptor.
        check("D2_object_arg_value_category_invariant",
              init_descriptor<const object_arg_wrapper&>() == "(Lcom/example/ObjArg;)V"
                  && init_descriptor<object_arg_wrapper&&>() == "(Lcom/example/ObjArg;)V");

        // Restore the map exactly so later sections see the prior state.
        {
            std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
            vmhook::type_to_class_map = saved_types;
        }
        check("D2_type_map_restored",
              vmhook::type_to_class_map == saved_types);
    }

    // =====================================================================
    // SECTION D3 — TRAIT GATES the factory's arg pipeline depends on.
    // make_unique's per-arg dispatch (jni_signature_for_arg / append_jni_arg)
    // routes object-wrapper args through is_unique_ptr / is_unique_object_ptr and
    // the object_base base test.  These traits are the compile-time gates that
    // decide whether an arg becomes a Java object parameter vs a primitive — pin
    // their exact behaviour (cv-ref stripping, value_type_t resolution, the
    // object_base derivation requirement) so a regression in the gates (which
    // would silently mis-encode every wrapper arg) is caught here, no JVM needed.
    // =====================================================================
    {
        using vmhook::detail::is_unique_ptr;
        using vmhook::detail::is_unique_ptr_v;
        using vmhook::detail::is_unique_object_ptr;

        // is_unique_ptr<T>::value — true only for unique_ptr specialisations.
        static_assert(is_unique_ptr<std::unique_ptr<plain_wrapper>>::value);
        static_assert(!is_unique_ptr<plain_wrapper>::value);
        static_assert(!is_unique_ptr<int>::value);
        static_assert(!is_unique_ptr<plain_wrapper*>::value);

        // value_type_t exposes the pointee (the wrapper) — this is what
        // jni_signature_for_arg uses to resolve the "L...;" descriptor.  It must
        // be the WRAPPER type, NOT bool (the shadowing footgun the header warns
        // about); pin it so that regression cannot return silently.
        static_assert(std::is_same_v<
            is_unique_ptr<std::unique_ptr<plain_wrapper>>::value_type_t, plain_wrapper>,
            "is_unique_ptr<unique_ptr<W>>::value_type_t must be W (not bool)");
        static_assert(std::is_same_v<
            is_unique_ptr<std::unique_ptr<object_arg_wrapper>>::value_type_t, object_arg_wrapper>);

        // is_unique_ptr_v strips cv-ref BEFORE testing (so a forwarded
        // const unique_ptr<W>& arg is still recognised as a unique_ptr).
        static_assert(is_unique_ptr_v<std::unique_ptr<plain_wrapper>>);
        static_assert(is_unique_ptr_v<const std::unique_ptr<plain_wrapper>&>);
        static_assert(is_unique_ptr_v<std::unique_ptr<plain_wrapper>&&>);
        static_assert(is_unique_ptr_v<std::unique_ptr<plain_wrapper>&>);
        static_assert(!is_unique_ptr_v<plain_wrapper>);
        static_assert(!is_unique_ptr_v<int>);

        // is_unique_object_ptr — true ONLY for unique_ptr<T> where T derives from
        // object_base (the exact predicate that classifies an arg as a managed
        // Java-object wrapper).  unique_ptr<int> is a unique_ptr but NOT an
        // object-wrapper, so it must be false.
        static_assert(is_unique_object_ptr<std::unique_ptr<plain_wrapper>>::value);
        static_assert(is_unique_object_ptr<std::unique_ptr<ctor_wrapper>>::value);
        static_assert(!is_unique_object_ptr<std::unique_ptr<int>>::value);
        static_assert(!is_unique_object_ptr<plain_wrapper>::value);
        static_assert(!is_unique_object_ptr<int>::value);

        // The wrappers really do derive from object_base (the property both the
        // descriptor arm and is_unique_object_ptr key on).  move_only is a plain
        // user struct and must NOT be mistaken for an object wrapper.
        static_assert(std::is_base_of_v<vmhook::object_base, plain_wrapper>);
        static_assert(std::is_base_of_v<vmhook::object_base, object_arg_wrapper>);
        static_assert(!std::is_base_of_v<vmhook::object_base, move_only>);
        static_assert(!std::is_base_of_v<vmhook::object_base, int>);

        // The wrapper construction contract make_unique relies on:
        // std::make_unique<W>(oop) must be well-formed (W is constructible from a
        // single oop_t) for EVERY wrapper — this is the call the success path
        // makes.  oop_t is void*, so assert constructibility from void*.
        static_assert(std::is_constructible_v<plain_wrapper, vmhook::oop_t>);
        static_assert(std::is_constructible_v<ctor_wrapper, vmhook::oop_t>);
        static_assert(std::is_constructible_v<bool_ctor_wrapper, vmhook::oop_t>);
        static_assert(std::is_constructible_v<object_arg_wrapper, vmhook::oop_t>);
        // ...and that single-oop constructor is noexcept (the wrappers mark it so;
        // make_unique's std::make_unique<W>(oop) therefore cannot throw on the
        // construction step itself).
        static_assert(std::is_nothrow_constructible_v<plain_wrapper, vmhook::oop_t>);
        static_assert(std::is_nothrow_constructible_v<object_arg_wrapper, vmhook::oop_t>);

        check("D3_factory_trait_gates_compile_time", true);
    }

    // =====================================================================
    // SECTION E — NO-JVM RUNTIME NULL CONTRACT over an EXHAUSTIVE arg matrix.
    // Every make_unique<W>(pack) below ACTUALLY INSTANTIATES the template (so
    // the perfect-forwarding std::forward pack, the construct()-detection
    // if-constexpr, AND the descriptor assembly all compile for that pack) and
    // is invoked with no JVM.  The contract is uniform: NOTHING throws and the
    // result is a null unique_ptr (the ensure_current_java_thread guard wins
    // before registration, allocation, <init>, or construct()).  We sweep:
    //   * arity 0..N,
    //   * every value category (lvalue / const-lvalue / rvalue / mixed),
    //   * every primitive arg type + string family + object/unique_ptr args,
    //   * move-only args (forwarded as rvalues),
    //   * wrappers WITH and WITHOUT a matching construct().
    // =====================================================================
    {
        // --- arity 0 ---
        check("E_no_args_plain", make_unique_is_null_and_safe<plain_wrapper>());
        check("E_no_args_ctor",  make_unique_is_null_and_safe<ctor_wrapper>());

        // --- arity 1, every value category, on a no-construct wrapper (routes
        //     the would-be fallback through the "no matching construct" branch) ---
        {
            int lv{ 1337 };
            const int clv{ 1337 };
            check("E_one_arg_lvalue",       make_unique_is_null_and_safe<plain_wrapper>(lv));
            check("E_one_arg_const_lvalue", make_unique_is_null_and_safe<plain_wrapper>(clv));
            check("E_one_arg_prvalue",      make_unique_is_null_and_safe<plain_wrapper>(1337));
            check("E_one_arg_xvalue",       make_unique_is_null_and_safe<plain_wrapper>(std::move(lv)));
        }

        // --- arity 1, EVERY primitive arg type (each instantiates a distinct
        //     descriptor + packer path; all must still be null + no-throw) ---
        check("E_arg_bool",   make_unique_is_null_and_safe<plain_wrapper>(true));
        check("E_arg_int8",   make_unique_is_null_and_safe<plain_wrapper>(std::int8_t{ -7 }));
        check("E_arg_uint8",  make_unique_is_null_and_safe<plain_wrapper>(std::uint8_t{ 200 }));
        check("E_arg_int16",  make_unique_is_null_and_safe<plain_wrapper>(std::int16_t{ -12345 }));
        check("E_arg_uint16", make_unique_is_null_and_safe<plain_wrapper>(std::uint16_t{ 0xBEEF }));
        check("E_arg_int32",  make_unique_is_null_and_safe<plain_wrapper>(std::int32_t{ -123456 }));
        check("E_arg_uint32", make_unique_is_null_and_safe<plain_wrapper>(std::uint32_t{ 0xDEADBEEFu }));
        check("E_arg_int64",  make_unique_is_null_and_safe<plain_wrapper>(std::int64_t{ 0x0123456789ABCDEFLL }));
        check("E_arg_uint64", make_unique_is_null_and_safe<plain_wrapper>(std::uint64_t{ 0xFFFFFFFFFFFFFFFFULL }));
        check("E_arg_float",  make_unique_is_null_and_safe<plain_wrapper>(3.5f));
        check("E_arg_double", make_unique_is_null_and_safe<plain_wrapper>(2.718281828));

        // --- arity 1, STRING FAMILY (every spelling). The c-string is passed via
        //     a named const char* LVALUE (NOT a bare literal) to exercise the
        //     const-char* branch without tripping GCC -Werror=nonnull-compare on
        //     a literal address — see the file header note. ---
        {
            const char* cstr{ "c-string-lvalue" };
            const char* empty_cstr{ "" };
            const char* null_cstr{ nullptr };
            check("E_arg_std_string",        make_unique_is_null_and_safe<plain_wrapper>(std::string{ "hello" }));
            check("E_arg_std_string_empty",  make_unique_is_null_and_safe<plain_wrapper>(std::string{}));
            check("E_arg_string_view",       make_unique_is_null_and_safe<plain_wrapper>(std::string_view{ "view-arg" }));
            check("E_arg_string_view_empty", make_unique_is_null_and_safe<plain_wrapper>(std::string_view{}));
            check("E_arg_cstring_lvalue",    make_unique_is_null_and_safe<plain_wrapper>(cstr));
            check("E_arg_cstring_empty",     make_unique_is_null_and_safe<plain_wrapper>(empty_cstr));
            check("E_arg_cstring_null",      make_unique_is_null_and_safe<plain_wrapper>(null_cstr));
            // UTF-8 multibyte content (still null w/o JVM; proves the byte string
            // shape does not throw on the way to the guard).
            check("E_arg_std_string_utf8",   make_unique_is_null_and_safe<plain_wrapper>(std::string{ "caf\xC3\xA9-\xE2\x9C\x93" }));
        }

        // --- arity 1, OBJECT / unique_ptr<object> args.  These route through the
        //     object-reference arm (synthetic OOP handle).  A null unique_ptr and
        //     a by-value wrapper both compile + return null with no JVM. ---
        {
            // A by-value object_base-derived wrapper argument.
            plain_wrapper obj_arg{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0xABCD0000)) };
            check("E_arg_object_by_value_lvalue", make_unique_is_null_and_safe<plain_wrapper>(obj_arg));

            // A unique_ptr<wrapper> argument (rvalue + null).
            std::unique_ptr<plain_wrapper> up_null{ reinterpret_cast<plain_wrapper*>(0) };
            check("E_arg_unique_ptr_null_rvalue",
                  make_unique_is_null_and_safe<plain_wrapper>(std::move(up_null)));
        }

        // --- arity 2..N, mixed types AND mixed value categories, exercising the
        //     full forwarding pack on a wrapper WITH a matching construct() and
        //     on one WITHOUT.  All null + no-throw. ---
        {
            int a{ 42 };
            const int b{ 7 };
            std::string s{ "mix" };
            check("E_two_args_ii_mixed_cat",
                  make_unique_is_null_and_safe<plain_wrapper>(a, b));
            check("E_three_args_ijd",
                  make_unique_is_null_and_safe<plain_wrapper>(7, std::int64_t{ 0x0123456789ABCDEFLL }, 3.5));
            check("E_two_args_string_int_with_ctor",
                  make_unique_is_null_and_safe<ctor_wrapper>(55, std::move(s)));
            check("E_two_args_int_string_ctor_lvalue",
                  make_unique_is_null_and_safe<ctor_wrapper>(a, std::string{ "y" }));
            // Long heterogeneous pack with every category present.
            std::string s2{ "z" };
            check("E_five_args_mixed_categories",
                  make_unique_is_null_and_safe<plain_wrapper>(true, a, std::int64_t{ 9 }, 1.5, std::move(s2)));
        }

        // --- construct()-selecting wrappers, called WITH the matching pack: the
        //     construct() branch instantiates (the requires-probe is satisfied)
        //     yet make_unique still returns null at the no-JVM guard BEFORE
        //     construct() runs. Proves the construct() instantiation is harmless
        //     without a JVM. ---
        check("E_bool_ctor_with_bool",   make_unique_is_null_and_safe<bool_ctor_wrapper>(true));
        check("E_wide_ctor_with_int",    make_unique_is_null_and_safe<wide_ctor_wrapper>(5));
        {
            int lv{ 9 };
            check("E_lref_ctor_with_lvalue", make_unique_is_null_and_safe<lref_ctor_wrapper>(lv));
            check("E_rref_ctor_with_rvalue", make_unique_is_null_and_safe<rref_ctor_wrapper>(9));
            check("E_cref_ctor_with_lvalue", make_unique_is_null_and_safe<cref_ctor_wrapper>(lv));
            check("E_cref_ctor_with_rvalue", make_unique_is_null_and_safe<cref_ctor_wrapper>(9));
        }

        // NOTE on move-only / arbitrary user types as make_unique ARGS: a
        // move_only (or any non-JNI-convertible user type) is REJECTED at
        // compile time by make_unique — every forwarded arg flows through
        // jni_make_unique -> make_jni_args -> jni_signature_for_arg /
        // convert_jni_arg, which static_assert (dependent_false_v) unless the
        // arg is one of {string, c-string, unique_ptr<object>, object_base,
        // bool, integral, float, double} (it must become a Java <init>
        // parameter).  So `make_unique<W>(move_only{...})` is INTENTIONALLY
        // ill-formed and is NOT a valid call.  The move-only FORWARDING property
        // is therefore exercised at the construct()-detection layer instead —
        // see Section B (move_sink_wrapper), where the requires-probe proves a
        // move-only arg is forwarded as an rvalue with no hidden copy.

        // --- a wrapper WITHOUT construct(), called WITH args: routes the
        //     would-be fallback through the "no matching construct()" branch
        //     (sizeof...(args)>0). Still null + no-throw. ---
        check("E_plain_wrapper_with_args_no_construct_branch",
              make_unique_is_null_and_safe<plain_wrapper>(1, 2, 3));

        // --- NO-ARG construct() wrapper called with NO args: the construct()
        //     branch instantiates (requires-probe satisfied for the empty pack)
        //     yet make_unique still returns null at the thread guard. ---
        check("E_noarg_ctor_no_args",
              make_unique_is_null_and_safe<noarg_ctor_wrapper>());

        // --- every primitive arg type ALSO on a wrapper WITH a matching
        //     construct() (wide_ctor_wrapper::construct(long) accepts each via
        //     promotion): exercises the construct()-branch instantiation for each
        //     packer path, all null + no-throw. ---
        check("E_wide_ctor_bool",   make_unique_is_null_and_safe<wide_ctor_wrapper>(true));
        check("E_wide_ctor_int8",   make_unique_is_null_and_safe<wide_ctor_wrapper>(std::int8_t{ -1 }));
        check("E_wide_ctor_uint8",  make_unique_is_null_and_safe<wide_ctor_wrapper>(std::uint8_t{ 1 }));
        check("E_wide_ctor_int16",  make_unique_is_null_and_safe<wide_ctor_wrapper>(std::int16_t{ -2 }));
        check("E_wide_ctor_uint16", make_unique_is_null_and_safe<wide_ctor_wrapper>(std::uint16_t{ 2 }));
        check("E_wide_ctor_int32",  make_unique_is_null_and_safe<wide_ctor_wrapper>(std::int32_t{ -3 }));

        // --- float arg specifically (the descriptor 'F' packer path), on the
        //     no-construct wrapper. Distinct from the double path. ---
        check("E_arg_float_negzero", make_unique_is_null_and_safe<plain_wrapper>(-0.0f));
        check("E_arg_double_negzero", make_unique_is_null_and_safe<plain_wrapper>(-0.0));

        // --- the (IJD) multi-arg shape on a wrapper whose construct(int,
        //     std::int64_t, double) matches: the multi-arg construct() branch
        //     instantiates, every category present, still null + no-throw. ---
        {
            int i{ 7 };
            std::int64_t j{ 0x0123456789ABCDEFLL };
            double d{ 3.5 };
            check("E_ijd_ctor_lvalues",
                  make_unique_is_null_and_safe<ijd_ctor_wrapper>(i, j, d));
            check("E_ijd_ctor_rvalues",
                  make_unique_is_null_and_safe<ijd_ctor_wrapper>(7, std::int64_t{ 1 }, 3.5));
            check("E_ijd_ctor_mixed",
                  make_unique_is_null_and_safe<ijd_ctor_wrapper>(i, std::int64_t{ 9 }, d));
        }

        // --- (String,int) shape on the matching construct() wrapper, across the
        //     accepted string spellings (std::string + const char* lvalue). The
        //     string_view spelling is NOT used here: construct(std::string,...)
        //     rejects string_view (explicit ctor — Section B(8)), and routing it
        //     through make_unique would still be a valid CALL (the JNI descriptor
        //     path accepts string_view) but would take the "no matching
        //     construct()" branch; we cover that distinct routing below. ---
        {
            const char* cstr{ "c-mix" };
            check("E_string_int_ctor_stdstring",
                  make_unique_is_null_and_safe<string_int_ctor_wrapper>(std::string{ "mix" }, 55));
            check("E_string_int_ctor_cstring_lvalue",
                  make_unique_is_null_and_safe<string_int_ctor_wrapper>(cstr, 55));
            // string_view arg: VALID make_unique call (descriptor path accepts it)
            // but selects the no-construct branch on the fallback. Still null.
            check("E_string_int_ctor_string_view_no_construct_branch",
                  make_unique_is_null_and_safe<string_int_ctor_wrapper>(std::string_view{ "sv" }, 55));
        }

        // --- OBJECT-reference args with NON-NULL sentinel handles.  A by-value
        //     object_base wrapper and a non-null unique_ptr<wrapper> both route
        //     through the object-handle arm (append_jni_arg re-homes the OOP into
        //     object_handles).  With no JVM the thread guard still wins -> null,
        //     no-throw, and crucially nothing dereferences the sentinel OOP. ---
        {
            object_arg_wrapper obj_sentinel{
                reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x1234ABCD)) };
            check("E_arg_object_by_value_nonnull_sentinel",
                  make_unique_is_null_and_safe<plain_wrapper>(obj_sentinel));

            // A NON-NULL unique_ptr<wrapper> built over a sentinel oop. We use
            // release() after the call to avoid std::default_delete invoking the
            // virtual destructor on a fake (non-heap) pointer.
            std::unique_ptr<object_arg_wrapper> up_sentinel{
                std::make_unique<object_arg_wrapper>(
                    reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x5678BEEF))) };
            check("E_arg_unique_ptr_nonnull_sentinel_rvalue",
                  make_unique_is_null_and_safe<plain_wrapper>(std::move(up_sentinel)));

            // A const unique_ptr<wrapper> LVALUE arg (forwarded as const lvalue):
            // is_unique_ptr_v strips the cv-ref so it is still recognised as a
            // unique_ptr object arg. Built over null so destruction is a no-op.
            const std::unique_ptr<object_arg_wrapper> up_const_null{
                reinterpret_cast<object_arg_wrapper*>(0) };
            check("E_arg_unique_ptr_const_lvalue",
                  make_unique_is_null_and_safe<plain_wrapper>(up_const_null));
        }

        // --- a multi-OBJECT-arg pack (two object handles in one <init>): the
        //     object-handle re-home loop runs twice; still null + no-throw. ---
        {
            object_arg_wrapper o1{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0xA1)) };
            object_arg_wrapper o2{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0xB2)) };
            check("E_two_object_args",
                  make_unique_is_null_and_safe<plain_wrapper>(o1, o2));
            // object arg interleaved with primitives + string.
            check("E_mixed_object_primitive_string",
                  make_unique_is_null_and_safe<plain_wrapper>(o1, 42, std::string{ "s" }, 3.5));
        }

        // --- higher arities (6..8 args) to exercise the fold past the small
        //     cases, every category + the new primitive types present. ---
        {
            int a{ 1 };
            const int b{ 2 };
            std::string s{ "p" };
            check("E_six_args",
                  make_unique_is_null_and_safe<plain_wrapper>(
                      true, std::int8_t{ 3 }, std::int16_t{ 4 }, a, std::int64_t{ 5 }, 6.0f));
            check("E_seven_args",
                  make_unique_is_null_and_safe<plain_wrapper>(
                      a, b, std::uint16_t{ 7 }, std::int64_t{ 8 }, 9.0f, 10.0, std::move(s)));
            std::string s2{ "q" };
            check("E_eight_args_all_categories",
                  make_unique_is_null_and_safe<plain_wrapper>(
                      false, std::uint8_t{ 1 }, std::int16_t{ -2 }, std::uint16_t{ 3 },
                      a, std::int64_t{ 0x7FFFFFFFFFFFFFFFLL }, 4.5, std::move(s2)));
        }
    }

    // =====================================================================
    // SECTION F — REGISTRATION-INDEPENDENCE + OWNED-POINTER SEMANTICS.
    // (1) The no-JVM null contract is independent of whether W is registered in
    //     type_to_class_map (the thread guard precedes the map lookup).  We
    //     register a wrapper by writing the public map directly (register_class
    //     itself needs find_class -> a live JVM) and confirm make_unique is
    //     STILL null; then restore the map so the suite stays order-independent.
    // (2) The returned unique_ptr has full owning-pointer semantics even when
    //     null: it is move-constructible, swappable, reset()-able, release()-able
    //     and bool-convertible — i.e. a bona fide std::unique_ptr<W>, not a
    //     degenerate stand-in.
    // =====================================================================
    {
        // Snapshot + restore the type map around the registration probe.
        std::unordered_map<std::type_index, std::string> saved_types{};
        {
            std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
            saved_types = vmhook::type_to_class_map;
        }
        const bool was_registered_before{
            vmhook::type_to_class_map.find(std::type_index{ typeid(plain_wrapper) })
                != vmhook::type_to_class_map.end() };

        // Unregistered (precondition for this section): null.
        check("F_unregistered_make_unique_null",
              make_unique_is_null_and_safe<plain_wrapper>());

        // Register the type directly in the public map (the part of register_class
        // that is pure bookkeeping; find_class needs a JVM and is out of scope).
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(plain_wrapper) }, std::string{ "vmhook/test/MakeUnique" });
        check("F_precondition_type_registered",
              vmhook::type_to_class_map.find(std::type_index{ typeid(plain_wrapper) })
                  != vmhook::type_to_class_map.end());

        // Registered: STILL null with no JVM (the thread guard wins first), for
        // both the no-arg and the with-args (construct-detect) forms.
        check("F_registered_make_unique_still_null_no_args",
              make_unique_is_null_and_safe<plain_wrapper>());
        check("F_registered_make_unique_still_null_with_args",
              make_unique_is_null_and_safe<ctor_wrapper>(1, std::string{ "y" }));

        // Restore the map exactly.
        {
            std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
            vmhook::type_to_class_map = saved_types;
        }
        const bool registered_after_restore{
            vmhook::type_to_class_map.find(std::type_index{ typeid(plain_wrapper) })
                != vmhook::type_to_class_map.end() };
        check("F_type_map_restored_to_prior_state",
              registered_after_restore == was_registered_before);

        // (2) Owned-pointer semantics on the (null) return value.
        std::unique_ptr<plain_wrapper> p{ vmhook::make_unique<plain_wrapper>() };
        check("F_null_return_is_falsey", !p);
        check("F_null_return_get_is_null", p.get() == nullptr);

        // Move-construct from the null owning pointer (must compile + stay null).
        std::unique_ptr<plain_wrapper> q{ std::move(p) };
        check("F_move_constructed_is_null", q == nullptr);

        // reset() and swap() on owning pointers.
        q.reset();
        check("F_reset_is_null", q == nullptr);

        std::unique_ptr<plain_wrapper> r{ vmhook::make_unique<plain_wrapper>() };
        q.swap(r);
        check("F_swap_both_null", q == nullptr && r == nullptr);

        // release() on a null owning pointer yields null and leaves it null.
        std::unique_ptr<plain_wrapper> rel{ vmhook::make_unique<plain_wrapper>() };
        plain_wrapper* raw{ rel.release() };
        check("F_release_null_pointer", raw == nullptr && rel == nullptr);
    }

    // =====================================================================
    // SECTION G — WRAP-AN-OOP ROUND-TRIP (the no-JVM analogue of the factory's
    // success path).  make_unique's success path ends in std::make_unique<W>(oop)
    // (the JNI path returns exactly that; the TLAB path does the same before
    // construct()).  Without a JVM we can still drive that EXACT construction
    // ourselves over any oop bit-pattern and prove it is fault-free and that the
    // produced wrapper reports the oop verbatim via object_base::get_instance() —
    // i.e. the wrapper is a faithful, sane handle for whatever OOP the factory
    // would have decoded.  get_instance() only RETURNS the stored void*; it never
    // dereferences it, so a sentinel/non-canonical value is safe to use here.
    // =====================================================================
    {
        bool threw{ false };

        // Null oop -> sane default state: get_instance() == nullptr, no fault.
        check("G_wrap_null_oop_roundtrips",
              wrap_oop_roundtrips<plain_wrapper>(nullptr, threw) && !threw);
        check("G_wrap_null_oop_no_throw", !threw);

        // A handful of sentinel (non-null, NON-dereferenced) oop bit-patterns:
        // each must be reported verbatim by get_instance().  Values chosen to
        // cover low, mid, high, and all-ones pointer patterns; all are pure
        // pointer bookkeeping (never read), so this is endianness/word-size safe.
        const std::uintptr_t patterns[]{
            std::uintptr_t{ 0x8 },
            std::uintptr_t{ 0x1000 },
            std::uintptr_t{ 0xABCD1234 },
            static_cast<std::uintptr_t>(0xDEADBEEFu),
            ~std::uintptr_t{ 0 },        // all-ones
            std::uintptr_t{ 0x7FFFFFF8 } // large, 8-aligned
        };
        bool all_roundtrip{ true };
        bool any_threw{ false };
        for (const std::uintptr_t pat : patterns)
        {
            bool t{ false };
            const auto oop{ reinterpret_cast<vmhook::oop_t>(pat) };
            if (!wrap_oop_roundtrips<plain_wrapper>(oop, t)) { all_roundtrip = false; }
            if (t) { any_threw = true; }
        }
        check("G_wrap_sentinel_oops_roundtrip", all_roundtrip);
        check("G_wrap_sentinel_oops_no_throw", !any_threw);

        // The round-trip holds for EVERY wrapper variant (the W(oop) ctor + the
        // ownership + get_instance() contract is uniform across wrapper shapes).
        const auto sentinel{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0xC0DE)) };
        bool t2{ false };
        check("G_wrap_ctor_wrapper",        wrap_oop_roundtrips<ctor_wrapper>(sentinel, t2) && !t2);
        check("G_wrap_bool_ctor_wrapper",   wrap_oop_roundtrips<bool_ctor_wrapper>(sentinel, t2) && !t2);
        check("G_wrap_noarg_ctor_wrapper",  wrap_oop_roundtrips<noarg_ctor_wrapper>(sentinel, t2) && !t2);
        check("G_wrap_object_arg_wrapper",  wrap_oop_roundtrips<object_arg_wrapper>(sentinel, t2) && !t2);
        check("G_wrap_ijd_ctor_wrapper",    wrap_oop_roundtrips<ijd_ctor_wrapper>(sentinel, t2) && !t2);
    }

    // =====================================================================
    // SECTION G2 — WRAPPER VALUE SEMANTICS over a null oop (the object_base the
    // factory's unique_ptr points at).  object_base defines copy (shares the OOP),
    // move (transfers + nulls the source), and the corresponding assignments;
    // make_unique hands out a unique_ptr<W>, but W ITSELF is a value type whose
    // semantics matter when callers deref the pointer.  Pin them over a null/
    // sentinel oop (no JVM, no deref): copy duplicates the OOP, move nulls the
    // source, and every wrapper is the move-friendly handle the factory promises.
    // =====================================================================
    {
        // Compile-time wrapper trait gates (what callers of the factory result
        // can rely on): the wrapper is move-constructible/assignable and copy is
        // available (object_base provides all four), and the move ops are noexcept.
        static_assert(std::is_nothrow_move_constructible_v<plain_wrapper>,
                      "wrapper move-ctor must be noexcept (object_base contract)");
        static_assert(std::is_nothrow_move_assignable_v<plain_wrapper>,
                      "wrapper move-assign must be noexcept (object_base contract)");
        static_assert(std::is_copy_constructible_v<plain_wrapper>);
        static_assert(std::is_copy_assignable_v<plain_wrapper>);
        // The wrapper is constructible from a single oop and that ctor is explicit
        // (guards against an accidental implicit oop_t -> wrapper conversion).
        static_assert(std::is_constructible_v<plain_wrapper, vmhook::oop_t>);
        static_assert(!std::is_convertible_v<vmhook::oop_t, plain_wrapper>,
                      "oop_t -> wrapper must be EXPLICIT only");

        const auto sentinel{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0xFEED)) };
        bool threw{ false };
        try
        {
            // Copy SHARES the OOP (both report the same instance).
            plain_wrapper a{ sentinel };
            plain_wrapper b{ a };
            check("G2_copy_shares_oop",
                  a.get_instance() == sentinel && b.get_instance() == sentinel);

            // Move TRANSFERS the OOP and NULLS the source (object_base move ctor).
            plain_wrapper c{ std::move(a) };
            check("G2_move_transfers_oop_and_nulls_source",
                  c.get_instance() == sentinel && a.get_instance() == nullptr);

            // Move ASSIGNMENT: same transfer-and-null contract.
            plain_wrapper d{ nullptr };
            d = std::move(c);
            check("G2_move_assign_transfers_and_nulls",
                  d.get_instance() == sentinel && c.get_instance() == nullptr);

            // Copy ASSIGNMENT shares the OOP.
            plain_wrapper e{ nullptr };
            e = d;
            check("G2_copy_assign_shares_oop",
                  e.get_instance() == sentinel && d.get_instance() == sentinel);

            // A default/null-constructed wrapper reports a null instance.
            plain_wrapper z{ nullptr };
            check("G2_null_constructed_instance_is_null", z.get_instance() == nullptr);
        }
        catch (...)
        {
            threw = true;
        }
        check("G2_wrapper_value_semantics_no_throw", !threw);
    }

    return failures == 0 ? 0 : 1;
}
