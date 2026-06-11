// Exhaustive, JVM-free compile-time sweep of vmhook::detail::function_traits —
// the callable-introspection trait that the typed hook<T>() overload uses to
// recover a user detour's parameter list.  This file is the dedicated home for
// the *callable-shape* axis of the trait; the slot-offset / decomposition-chain
// end (function_traits -> tuple_tail -> java_slot_offsets, and the cv/ref +
// convergence contracts) is owned by test_traits_extra.cpp and is NOT repeated
// here.  The two files are complementary, not overlapping.
//
// What function_traits is, exactly (verified against vmhook.hpp):
//   * Primary template `function_traits<F, void>` is DECLARED, left UNDEFINED —
//     it has no args_tuple_t.  Any callable shape that matches none of the five
//     specialisations below therefore has no args_tuple_t (a detectable absence
//     via SFINAE, or — for a present-but-unsupported-qualifier operator() — a
//     hard error; see the detectability note on has_args_tuple).
//   * Five specialisations populate `using args_tuple_t = std::tuple<args...>`:
//       1. free-function POINTER       R(*)(args...)
//       2. std::function               std::function<R(args...)>
//       3. generic functor (void_t probe on &F::operator(), forwards to 4/5)
//       4. const member function ptr   R(C::*)(args...) const   (the lambda case)
//       5. non-const member fn ptr     R(C::*)(args...)         (mutable lambda)
//   * The trait exposes ONLY args_tuple_t.  There is deliberately NO return_type
//     / result_type / arity member (the detour's return value is delivered out
//     of band via vmhook::return_value, never read from the callable's type).
//     A large block below pins that the RETURN type is irrelevant to args_tuple_t
//     across every return shape — the axis test_traits_extra.cpp does not cover.
//
// Every fact here is constexpr / static_assert-checkable: it passes or fails at
// BUILD time on every CI compiler/STL with no live JVM, no interpreter frame,
// and no flakiness.  Each fact is additionally mirrored through check() so a
// regression also surfaces as a visible [FAIL] line at runtime.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // ── The exact chain hook<T>() instantiates ──────────────────────────────
    // hook<T>() forms function_traits<remove_cvref_t<Detour>>::args_tuple_t,
    // then tuple_tail to drop the leading vmhook::return_value&.  Mirror both so
    // the trait is exercised precisely as the library uses it.
    template<typename Callable>
    using args_of = typename vmhook::detail::function_traits<
        std::remove_cvref_t<Callable>>::args_tuple_t;

    template<typename Callable>
    using method_args_of =
        typename vmhook::detail::tuple_tail<args_of<Callable>>::type_t;

    // A representative user wrapper, the shape a caller passes via unique_ptr.
    struct sample_wrapper : public vmhook::object<sample_wrapper>
    {
        using vmhook::object<sample_wrapper>::object;
    };

    // ── SFINAE detector: does function_traits<F>::args_tuple_t EXIST? ────────
    // The primary template is undefined, so for any shape that matches none of
    // the five specialisations args_tuple_t is absent and this reports false.
    //
    // DETECTABILITY BOUNDARY (mirrors the note in test_traits_extra.cpp): this
    // probe is only well-formed for F whose operator() is EITHER (a) a single,
    // non-overloaded, non-template member-pointer the void_t spec forwards to,
    // OR (b) has NO single addressable operator() (overloaded / templated /
    // generic), in which case the void_t spec drops and F falls to the undefined
    // primary -> false.  A functor whose operator() IS addressable but carries a
    // qualifier the member-pointer specs do not cover (noexcept / & / && /
    // volatile) is a HARD COMPILE ERROR, not a detectable false — the void_t
    // spec IS selected, its base is the undefined primary, and reading
    // args_tuple_t off it is a non-SFINAE error.  Such shapes are documented
    // below as build-breaking and are deliberately never fed to this detector.
    template<typename F, typename = void>
    struct has_args_tuple : std::false_type {};
    template<typename F>
    struct has_args_tuple<F,
        std::void_t<typename vmhook::detail::function_traits<F>::args_tuple_t>>
        : std::true_type {};

    // ── Free functions: return-type matrix (args identical regardless of R) ──
    // The trait reads R as a template parameter and then DISCARDS it.  Declare
    // the same parameter list (int, double) behind every interesting return
    // shape; args_tuple_t must be tuple<int,double> for all of them.
    void           ret_void(int, double) {}
    int            ret_int(int, double) { return 0; }
    bool           ret_bool(int, double) { return false; }
    double         ret_double(int, double) { return 0.0; }
    std::int64_t   ret_i64(int, double) { return 0; }      // two-slot return type
    std::string    ret_string(int, double) { return {}; }
    void*          ret_voidptr(int, double) { return nullptr; }
    sample_wrapper ret_object(int, double) { return sample_wrapper{}; }
    std::unique_ptr<sample_wrapper> ret_uptr(int, double) { return nullptr; }

    // Reference-returning free functions (R is a reference type).
    static int       g_int{ 0 };
    int&             ret_lref(int, double) { return g_int; }
    const int&       ret_clref(int, double) { return g_int; }

    // ── Free functions: argument-shape matrix (the part R is irrelevant to) ──
    // References / const / pointer parameters are preserved VERBATIM by the
    // trait (the cv/ref normalisation happens downstream in extract_frame_arg,
    // not here).  These pin that preservation specifically on the free-function
    // POINTER specialisation (test_traits_extra.cpp pins it on lambdas).
    void freefn_ref_args(int&, const double&, void*) {}
    // Top-level const on a BY-VALUE parameter is part of the function's
    // *definition* only — it is stripped from the function TYPE by the language
    // (void(const int) and void(int) are the same type).  So the trait, reading
    // the function type, sees plain int/double here.  This is the deliberate
    // contrast with the const-REFERENCE case above, where the const IS part of
    // the type and survives.
    void freefn_const_val_args(const int, const double) {}
    void freefn_ptr_args(int*, const char*, sample_wrapper*) {}

    // Arity ladder 0..8 on the free-function POINTER specialisation, so the
    // variadic argument_types... pack is exercised at every small count.
    void a0() {}
    void a1(int) {}
    void a2(int, int) {}
    void a3(int, int, int) {}
    void a4(int, int, int, int) {}
    void a5(int, int, int, int, int) {}
    void a6(int, int, int, int, int, int) {}
    void a7(int, int, int, int, int, int, int) {}
    void a8(int, int, int, int, int, int, int, int) {}

    // A free function with a leading return_value& (the real detour shape) so
    // the method-args view can be exercised on the pointer specialisation.
    void detour_free(vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                     std::int32_t, std::int64_t) {}

    // ── Member-function pointers, taken DIRECTLY (&Struct::method) ───────────
    // test_traits_extra.cpp reaches the member-pointer specialisations only
    // INDIRECTLY through closures/functors.  These take a member pointer's type
    // straight, pinning specialisations 4 (const) and 5 (non-const) in
    // isolation, including their own return-type independence.
    struct member_host
    {
        void          m_const(int, double) const {}
        int           m_nonconst(long) { return 0; }
        std::string   m_const_ret(char) const { return {}; }
        std::int64_t  m_nonconst_ret(void*) { return 0; }   // two-slot return
        void          m_const_noexcept(int) const noexcept {}   // never detected
        void          m_lref_qualified(int) & {}                // never detected
    };

    // ── Functors with a single concrete operator() (accepted shapes) ─────────
    struct const_functor
    {
        void operator()(vmhook::return_value&, std::int32_t, std::int64_t) const {}
    };
    struct nonconst_functor
    {
        void operator()(vmhook::return_value&, std::int32_t, std::int64_t) {}
    };
    // Return-bearing operator(): the return type must still be discarded.
    struct returning_functor
    {
        std::int64_t operator()(vmhook::return_value&, double) const { return 0; }
    };

    // ── Cleanly-REJECTED shapes (args_tuple_t absent, detector-safe) ─────────
    struct overloaded_functor
    {
        void operator()(vmhook::return_value&, int) const {}
        void operator()(vmhook::return_value&, double) const {}
    };
    struct templated_functor
    {
        template<typename T>
        void operator()(vmhook::return_value&, T) const {}
    };
}

int main()
{
    // ========================================================================
    // 1.  RETURN-TYPE INDEPENDENCE (free-function pointer specialisation).
    //     args_tuple_t == tuple<int,double> for EVERY return shape.  This whole
    //     axis is untested elsewhere — function_traits exposes no return member,
    //     so a regression that accidentally folded R into the tuple (or stopped
    //     matching for a given R) would only show up here.
    // ========================================================================
    using two_args = std::tuple<int, double>;
    check("ret_void_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_void)>, two_args>);
    check("ret_int_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_int)>, two_args>);
    check("ret_bool_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_bool)>, two_args>);
    check("ret_double_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_double)>, two_args>);
    check("ret_i64_two_slot_return_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_i64)>, two_args>);
    check("ret_string_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_string)>, two_args>);
    check("ret_voidptr_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_voidptr)>, two_args>);
    check("ret_object_value_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_object)>, two_args>);
    check("ret_unique_ptr_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_uptr)>, two_args>);
    check("ret_lvalue_ref_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_lref)>, two_args>);
    check("ret_const_lvalue_ref_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_clref)>, two_args>);
    // The whole point in one assertion: ALL eleven return spellings agree.
    check("all_return_shapes_share_one_args_tuple",
          std::is_same_v<args_of<decltype(&ret_void)>, args_of<decltype(&ret_int)>>
          && std::is_same_v<args_of<decltype(&ret_int)>, args_of<decltype(&ret_string)>>
          && std::is_same_v<args_of<decltype(&ret_string)>, args_of<decltype(&ret_i64)>>
          && std::is_same_v<args_of<decltype(&ret_i64)>, args_of<decltype(&ret_uptr)>>
          && std::is_same_v<args_of<decltype(&ret_uptr)>, args_of<decltype(&ret_clref)>>);

    // Return-type independence on the std::function specialisation too.
    check("std_function_void_return_args",
          std::is_same_v<args_of<std::function<void(int, double)>>, two_args>);
    check("std_function_int_return_args",
          std::is_same_v<args_of<std::function<int(int, double)>>, two_args>);
    check("std_function_string_return_args",
          std::is_same_v<args_of<std::function<std::string(int, double)>>, two_args>);
    check("std_function_two_slot_return_args",
          std::is_same_v<args_of<std::function<std::int64_t(int, double)>>, two_args>);
    check("std_function_return_independent_of_void",
          std::is_same_v<args_of<std::function<void(int, double)>>,
                         args_of<std::function<int(int, double)>>>);

    // Return-type independence on a RETURNING functor (operator() with a value
    // return must still discard the return type and keep just the args).
    check("returning_functor_discards_return_keeps_double",
          std::is_same_v<method_args_of<returning_functor>, std::tuple<double>>);

    // ========================================================================
    // 2.  ARGUMENT-SHAPE PRESERVATION on the free-function pointer spec.
    //     References / const / pointer params survive VERBATIM (downstream
    //     extract_frame_arg strips cv/ref; the trait itself does not).
    // ========================================================================
    check("freefn_pointer_preserves_ref_and_const_ref_and_pointer",
          std::is_same_v<args_of<decltype(&freefn_ref_args)>,
                         std::tuple<int&, const double&, void*>>);
    // Top-level const on by-value params is NOT part of the function type, so
    // the trait sees plain int/double — the inverse of the const-REF case.
    check("freefn_pointer_drops_top_level_const_on_value_params",
          std::is_same_v<args_of<decltype(&freefn_const_val_args)>,
                         std::tuple<int, double>>);
    check("top_level_const_value_param_type_equals_unqualified",
          std::is_same_v<args_of<decltype(&freefn_const_val_args)>,
                         args_of<decltype(&ret_void)>>);
    check("freefn_pointer_preserves_pointer_param_spellings",
          std::is_same_v<args_of<decltype(&freefn_ptr_args)>,
                         std::tuple<int*, const char*, sample_wrapper*>>);

    // ========================================================================
    // 3.  ARITY LADDER 0..8 on the free-function pointer specialisation.
    //     Each rung pins tuple_size — proving the variadic argument_types...
    //     pack captures every count, including the empty pack.
    // ========================================================================
    check("arity_0_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a0)>> == 0);
    check("arity_1_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a1)>> == 1);
    check("arity_2_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a2)>> == 2);
    check("arity_3_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a3)>> == 3);
    check("arity_4_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a4)>> == 4);
    check("arity_5_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a5)>> == 5);
    check("arity_6_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a6)>> == 6);
    check("arity_7_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a7)>> == 7);
    check("arity_8_free_fn_pointer", std::tuple_size_v<args_of<decltype(&a8)>> == 8);
    // The empty-arg free function yields a literally empty args_tuple_t.
    check("arity_0_free_fn_pointer_args_tuple_is_empty",
          std::is_same_v<args_of<decltype(&a0)>, std::tuple<>>);

    // Arity ladder 0..6 on the std::function specialisation, for symmetry.
    check("std_function_arity_0", std::tuple_size_v<args_of<std::function<void()>>> == 0);
    check("std_function_arity_1", std::tuple_size_v<args_of<std::function<void(int)>>> == 1);
    check("std_function_arity_2", std::tuple_size_v<args_of<std::function<void(int, int)>>> == 2);
    check("std_function_arity_3", std::tuple_size_v<args_of<std::function<void(int, int, int)>>> == 3);
    check("std_function_arity_4", std::tuple_size_v<args_of<std::function<void(int, int, int, int)>>> == 4);
    check("std_function_arity_5", std::tuple_size_v<args_of<std::function<void(int, int, int, int, int)>>> == 5);
    check("std_function_arity_6", std::tuple_size_v<args_of<std::function<void(int, int, int, int, int, int)>>> == 6);

    // The realistic free-function detour decomposes through tuple_tail exactly.
    check("free_detour_method_args_strip_return_value",
          std::is_same_v<method_args_of<decltype(&detour_free)>,
                         std::tuple<std::unique_ptr<sample_wrapper>,
                                    std::int32_t, std::int64_t>>);

    // ========================================================================
    // 4.  MEMBER-FUNCTION POINTERS, taken DIRECTLY (specialisations 4 and 5).
    //     Reached here without any closure/functor in between, so the const and
    //     non-const member-pointer specs are pinned in isolation — including
    //     their own return-type independence.
    // ========================================================================
    check("const_member_pointer_args",
          std::is_same_v<args_of<decltype(&member_host::m_const)>,
                         std::tuple<int, double>>);
    check("nonconst_member_pointer_args",
          std::is_same_v<args_of<decltype(&member_host::m_nonconst)>,
                         std::tuple<long>>);
    check("const_member_pointer_return_discarded",
          std::is_same_v<args_of<decltype(&member_host::m_const_ret)>,
                         std::tuple<char>>);
    check("nonconst_member_pointer_two_slot_return_discarded",
          std::is_same_v<args_of<decltype(&member_host::m_nonconst_ret)>,
                         std::tuple<void*>>);
    // A member pointer is matched by the member specs, not the free-fn spec:
    // its args_tuple_t exists (positive detection through the direct spec).
    check("const_member_pointer_has_args_tuple",
          has_args_tuple<decltype(&member_host::m_const)>::value);
    check("nonconst_member_pointer_has_args_tuple",
          has_args_tuple<decltype(&member_host::m_nonconst)>::value);

    // ========================================================================
    // 5.  ACCEPTED CLOSURE / FUNCTOR SHAPES (positive detection).
    //     A non-capturing lambda is a const-operator() closure; a mutable lambda
    //     is a non-const-operator() closure; a capturing lambda still has one
    //     concrete operator().  All carry a detectable args_tuple_t.
    // ========================================================================
    {
        auto plain_lambda   = [](vmhook::return_value&, int) {};
        auto capturing      = [v = 0](vmhook::return_value&, int) { (void)v; };
        auto mutable_lambda = [v = 0](vmhook::return_value&, int) mutable { ++v; };
        check("plain_non_capturing_lambda_has_args_tuple",
              has_args_tuple<decltype(plain_lambda)>::value);
        check("capturing_lambda_has_args_tuple",
              has_args_tuple<decltype(capturing)>::value);
        check("mutable_lambda_has_args_tuple",
              has_args_tuple<decltype(mutable_lambda)>::value);
        // Capturing vs non-capturing closures with the same signature decompose
        // to the same method-arg tuple (capture state is invisible to the trait).
        check("capture_state_invisible_to_trait",
              std::is_same_v<method_args_of<decltype(plain_lambda)>,
                             method_args_of<decltype(capturing)>>
              && std::is_same_v<method_args_of<decltype(capturing)>,
                                method_args_of<decltype(mutable_lambda)>>);
    }
    check("const_functor_has_args_tuple", has_args_tuple<const_functor>::value);
    check("nonconst_functor_has_args_tuple", has_args_tuple<nonconst_functor>::value);
    check("returning_functor_has_args_tuple", has_args_tuple<returning_functor>::value);
    // const and non-const operator() functors decompose identically.
    check("const_and_nonconst_functor_decompose_identically",
          std::is_same_v<method_args_of<const_functor>,
                         method_args_of<nonconst_functor>>);

    // ========================================================================
    // 6.  CLEANLY-REJECTED SHAPES (args_tuple_t ABSENT, detector-safe).
    //     Each of these matches none of the five specialisations, so the
    //     undefined primary is selected and args_tuple_t is missing.  These are
    //     the documented "function_traits does NOT accept this" inputs.
    // ========================================================================
    // (a) A bare free-function TYPE — R(args...) with no pointer — matches no
    //     spec (only R(*)(args...) does).  This is the spelling a free function
    //     decays to when passed BY NAME through hook<T>()'s `auto&& user_detour`
    //     (T deduces to R(&)(args...), remove_cvref_t -> R(args...)).  Hence a
    //     free function must be passed as &fn, never by bare name — pinned here.
    check("bare_function_type_has_no_args_tuple",
          !has_args_tuple<void(int, double)>::value);
    // (b) A function REFERENCE type R(&)(args...) likewise matches no spec.
    check("function_reference_type_has_no_args_tuple",
          !has_args_tuple<void(&)(int, double)>::value);
    // (c) remove_cvref_t<R(&)(args...)> is the function TYPE, not a pointer —
    //     this is exactly why pass-by-name does not reach the free-fn-ptr spec.
    check("remove_cvref_of_function_reference_is_function_type",
          std::is_same_v<std::remove_cvref_t<void(&)(int, double)>, void(int, double)>);
    // (d) A noexcept free-function POINTER has a distinct type with no matching
    //     spec (there is no R(*)(args...) noexcept specialisation) -> absent.
    //     This is the DETECTABLE face of the noexcept gap; the noexcept-member
    //     face (m_const_noexcept) is a hard error and is asserted by-construction
    //     below, never fed to the detector.
    check("noexcept_free_fn_pointer_has_no_args_tuple",
          !has_args_tuple<void(*)(int, double) noexcept>::value);
    // (e) A C-style variadic free-function pointer R(*)(args..., ...) is NOT
    //     captured by the R(*)(args...) spec (the trailing ellipsis is part of
    //     the function type and matches no specialisation) -> absent.
    check("c_variadic_free_fn_pointer_has_no_args_tuple",
          !has_args_tuple<int(*)(int, ...)>::value);
    // (f) Overloaded / templated / generic operator() leave &F::operator()
    //     ambiguous or ill-formed; the void_t spec drops and F falls to the
    //     undefined primary -> absent.  ("single concrete operator()" contract.)
    check("overloaded_operator_functor_has_no_args_tuple",
          !has_args_tuple<overloaded_functor>::value);
    check("templated_operator_functor_has_no_args_tuple",
          !has_args_tuple<templated_functor>::value);
    {
        auto generic_lambda = [](vmhook::return_value&, auto) {};
        check("generic_lambda_has_no_args_tuple",
              !has_args_tuple<decltype(generic_lambda)>::value);
    }
    // (g) Plainly non-callable types obviously carry no args_tuple_t.
    check("int_has_no_args_tuple", !has_args_tuple<int>::value);
    check("void_ptr_has_no_args_tuple", !has_args_tuple<void*>::value);
    check("unique_ptr_has_no_args_tuple",
          !has_args_tuple<std::unique_ptr<sample_wrapper>>::value);
    check("std_string_has_no_args_tuple", !has_args_tuple<std::string>::value);
    check("member_data_pointer_has_no_args_tuple",
          !has_args_tuple<int member_host::*>::value);

    // ========================================================================
    // 7.  noexcept does NOT change arity / args for the shapes that DO match.
    //     The free-function POINTER spec has no noexcept variant, so a noexcept
    //     pointer is absent (asserted above) — but a noexcept FREE FUNCTION
    //     reached via &fn where the address yields a plain pointer (compilers
    //     drop the noexcept from the pointed-to type only in narrow cases) is
    //     not relied upon.  What we CAN pin without a hard error: a std::function
    //     wrapping a noexcept-able signature exposes the same args.  (std::function
    //     itself is never noexcept-typed, so this is the practical guarantee.)
    // ========================================================================
    check("std_function_args_match_with_or_without_throwing_body",
          std::is_same_v<args_of<std::function<void(int, double)>>,
                         std::tuple<int, double>>);

    // ========================================================================
    // 8.  BUILD-BREAKER shapes, asserted BY CONSTRUCTION (never via detector).
    //     A functor whose operator() is present-and-addressable but carries a
    //     qualifier the member-pointer specs do not cover (noexcept / lvalue-ref
    //     qualified) makes the void_t spec select a base that is the undefined
    //     primary — reading args_tuple_t off it is a hard error, so these must
    //     NOT be probed.  We instead assert the *shape* of the member pointer
    //     directly: that its type is exactly the unsupported-qualifier spelling,
    //     documenting precisely why function_traits cannot introspect it.  (If a
    //     future header adds noexcept/ref-qualified member specs, these stay
    //     true and a new positive has_args_tuple check can be added then.)
    check("member_pointer_const_noexcept_shape_is_unsupported_qualifier",
          std::is_same_v<decltype(&member_host::m_const_noexcept),
                         void (member_host::*)(int) const noexcept>);
    check("member_pointer_lvalue_ref_qualified_shape_is_unsupported_qualifier",
          std::is_same_v<decltype(&member_host::m_lref_qualified),
                         void (member_host::*)(int) &>);

    // ========================================================================
    // 9.  remove_cvref_t on the CALLABLE itself (hook<T> applies it first).
    //     A const / lvalue-ref / rvalue-ref qualified closure value category
    //     must decompose identically to the bare closure type, because hook<T>()
    //     feeds function_traits<remove_cvref_t<decltype(user_detour)>>.
    // ========================================================================
    {
        auto lam = [](vmhook::return_value&, std::int32_t, std::int64_t) {};
        using bare = std::remove_cvref_t<decltype(lam)>;
        check("const_lvalue_callable_matches_bare",
              std::is_same_v<method_args_of<const decltype(lam)&>, method_args_of<bare>>);
        check("rvalue_callable_matches_bare",
              std::is_same_v<method_args_of<decltype(lam)&&>, method_args_of<bare>>);
        check("volatile_callable_matches_bare",
              std::is_same_v<method_args_of<volatile decltype(lam)&>, method_args_of<bare>>);
    }

    // ========================================================================
    // 10.  COMPILE-TIME ENFORCEMENT.  These never reach runtime if they regress
    //      — the build breaks first, the strongest possible guarantee.  They
    //      mirror the load-bearing facts from each block above.
    // ========================================================================
    // Return-type independence (free-fn pointer + std::function).
    static_assert(std::is_same_v<args_of<decltype(&ret_void)>,
                                 args_of<decltype(&ret_string)>>,
                  "function_traits must ignore the return type: void- and string-"
                  "returning free functions with the same params share args_tuple_t");
    static_assert(std::is_same_v<args_of<decltype(&ret_i64)>, std::tuple<int, double>>,
                  "a two-slot (int64) return type must not leak into args_tuple_t");
    static_assert(std::is_same_v<args_of<std::function<int(int, double)>>,
                                 std::tuple<int, double>>,
                  "std::function return type must be discarded from args_tuple_t");
    // Argument spelling preserved verbatim by the trait.
    static_assert(std::is_same_v<args_of<decltype(&freefn_ref_args)>,
                                 std::tuple<int&, const double&, void*>>,
                  "function_traits must preserve reference / const-ref / pointer "
                  "parameter spellings verbatim (normalisation is downstream)");
    static_assert(std::is_same_v<args_of<decltype(&freefn_const_val_args)>,
                                 std::tuple<int, double>>,
                  "top-level const on by-value params is not part of the function "
                  "type, so args_tuple_t sees the unqualified types — the inverse "
                  "of the const-reference case, which IS preserved");
    // Member-pointer specialisations, direct.
    static_assert(std::is_same_v<args_of<decltype(&member_host::m_const)>,
                                 std::tuple<int, double>>,
                  "const member-function-pointer specialisation must expose its args");
    static_assert(std::is_same_v<args_of<decltype(&member_host::m_nonconst)>,
                                 std::tuple<long>>,
                  "non-const member-function-pointer specialisation must expose its args");
    // Accepted vs rejected boundary.
    static_assert(has_args_tuple<decltype(&ret_void)>::value,
                  "a free-function POINTER must be an accepted detour shape");
    static_assert(!has_args_tuple<void(int, double)>::value,
                  "a bare function TYPE must be rejected (only the pointer matches) — "
                  "this is why a free function must be passed as &fn, not by name");
    static_assert(!has_args_tuple<void(&)(int, double)>::value,
                  "a function REFERENCE type must be rejected (matches no specialisation)");
    static_assert(!has_args_tuple<void(*)(int, double) noexcept>::value,
                  "a noexcept free-function pointer must be rejected (no noexcept spec) — "
                  "detectable face of the noexcept gap");
    static_assert(!has_args_tuple<int(*)(int, ...)>::value,
                  "a C-style variadic free-function pointer must be rejected");
    static_assert(!has_args_tuple<overloaded_functor>::value,
                  "an overloaded operator() must be rejected (single concrete operator() "
                  "required)");
    static_assert(!has_args_tuple<templated_functor>::value,
                  "a templated/generic operator() must be rejected (single concrete "
                  "operator() required)");
    // Arity-ladder endpoints.
    static_assert(std::tuple_size_v<args_of<decltype(&a0)>> == 0,
                  "zero-arg free function must yield an empty args_tuple_t");
    static_assert(std::tuple_size_v<args_of<decltype(&a8)>> == 8,
                  "eight-arg free function must yield an 8-element args_tuple_t");
    // The pass-by-name landing spelling, pinned end to end.
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(ret_void)>, void(int, double)>,
                  "a free function named bare deduces to a function type after "
                  "remove_cvref_t — which function_traits does NOT match");

    std::printf("vmhook traits-function_traits: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
