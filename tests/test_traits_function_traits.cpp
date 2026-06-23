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
//   * The specialisations populate `using args_tuple_t = std::tuple<args...>`:
//       1. free-function POINTER       R(*)(args...)
//       2. std::function               std::function<R(args...)>
//       3. generic functor (void_t probe on &F::operator(), forwards to 4-N)
//       4. const member function ptr   R(C::*)(args...) const   (the lambda case)
//       5. non-const member fn ptr     R(C::*)(args...)         (mutable lambda)
//       6. noexcept free-fn POINTER    R(*)(args...) noexcept
//       7. const noexcept member ptr   R(C::*)(args...) const noexcept (noexcept lambda)
//       8. noexcept member ptr         R(C::*)(args...) noexcept       (mutable noexcept lambda)
//       9. the full cv (none/const/volatile/const volatile) x ref (& / &&) x
//          noexcept member-pointer matrix — every ref- and volatile-qualified
//          member operator() form is now accepted, so a hand-written functor
//          whose operator() carries any of those qualifiers decomposes cleanly.
//     C-style variadic member forms (R(C::*)(args..., ...)) remain an intentional
//     gap (the trailing ellipsis is part of the type and matches no spec).
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

#include <array>
#include <cstddef>
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
    // primary -> false.  Every cv- / ref- / noexcept-qualified member operator()
    // now has a matching specialisation, so a functor whose operator() carries
    // any of those qualifiers resolves to a clean POSITIVE (true) here.  The only
    // remaining build-breaker member shape is the C-variadic operator()
    // (R(C::*)(args..., ...)): the void_t spec IS selected, its base is the
    // undefined primary, and reading args_tuple_t off it is a non-SFINAE error,
    // so a C-variadic functor is still never fed to this detector.
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
        void          m_const_noexcept(int) const noexcept {}   // NOW supported (spec 7)
        void          m_lref_qualified(int) & {}                // NOW supported (& ref-qualifier)
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

    // ── STATIC member functions (task-enumerated shape) ──────────────────────
    // A static member function has NO implicit `this`: &Struct::static_fn yields
    // an ORDINARY function pointer R(*)(args...), which is precisely the free-fn
    // POINTER specialisation (#1).  So &Struct::s_xxx must decompose exactly like
    // a free function — accepted, return discarded, args verbatim — and must NOT
    // behave like the member-pointer specialisations (#4/#5).  Nothing in the
    // suite exercised a static member before; this pins that &Struct::static_fn
    // is the free-fn-pointer path, not the member-pointer path.
    struct static_host
    {
        static void          s_void(int, double) {}
        static std::int64_t  s_i64(int, double) { return 0; }       // 2-slot return
        static std::string   s_string(int, double) { return {}; }
        static void          s_detour(vmhook::return_value&,
                                      std::unique_ptr<sample_wrapper>,
                                      std::int32_t, std::int64_t) {}
    };

    // ── MEMBER-function pointers: the full qualifier matrix ──────────────────
    // member_host (above) covers const / non-const / const-noexcept / lvalue-&.
    // These add the REMAINING C++ member-function qualifier spellings.  The full
    // cv (none/const/volatile/const volatile) x ref (& / &&) x noexcept matrix is
    // now enumerated as specialisations, so each of the qualifier spellings below
    // (volatile, const volatile, &&, const &, && noexcept) is ACCEPTED — its
    // args_tuple_t is present and equals the unqualified form's.  The ONLY member
    // shape that still matches no specialisation is the C-style variadic form
    // (m_cvariadic), which remains a documented gap and is asserted by SHAPE.
    struct member_quals
    {
        void m_volatile(int, double) volatile {}                       // NOW supported (volatile)
        void m_const_volatile(int, double) const volatile {}           // NOW supported (const volatile)
        void m_rref(int, double) && {}                                 // NOW supported (&&)
        void m_const_lref(int, double) const& {}                       // NOW supported (const&)
        void m_nonconst_noexcept(long) noexcept {}                     // NOW supported (spec 8)
        void m_rref_noexcept(int) && noexcept {}                       // NOW supported (&& noexcept)
        // A C-style variadic MEMBER function pointer — the member analogue of the
        // free-fn C-variadic gap.  R(C::*)(args..., ...) const matches no spec.
        void m_cvariadic(int, ...) const {}                            // never matched (intentional gap)
        // Wide member pointers feeding the member-pointer arity ladder below.
        void m_const_wide(int, int, int, int, int, int) const {}
        int  m_nonconst_wide(int, int, int, int, int) { return 0; }
    };

    // A member pointer with reference / pointer / const-ref PARAMETERS, so the
    // verbatim-spelling preservation is pinned on the member-pointer spec too
    // (test exercises it on free-fn pointers; this extends it to specs #4/#5).
    struct member_ref_params
    {
        void m_refs(int&, const double&, void*) const {}
        void m_const_val(const int, const double) const {}            // top-level const dropped
    };

    // ── Functors whose operator() carries a (now-SUPPORTED) qualifier ────────
    // These are the FUNCTOR (closure-like) face of the volatile / && qualifier
    // forms.  Their operator() IS addressable (so the void_t functor spec #3
    // selects) and now forwards to a member-pointer specialisation that exists,
    // so reading args_tuple_t off them is well-formed and detectable — they CAN
    // be fed to has_args_tuple<> and report true.  (Before the cv/ref matrix was
    // enumerated this was a hard error; the C-variadic functor is now the sole
    // member shape that still cannot be introspected.)
    struct volatile_call_functor
    {
        void operator()(vmhook::return_value&, int) volatile {}
    };
    struct rref_call_functor
    {
        void operator()(vmhook::return_value&, int) && {}
    };

    // ── Free functions whose RETURN is a reference / pointer / function ptr ───
    // The return-type-independence matrix already covers value / two-slot / lref /
    // const-lref returns.  These add the remaining return shapes the trait must
    // still DISCARD: rvalue-reference return, pointer-to-function return, and a
    // reference-to-array return — args_tuple_t must stay tuple<int,double> for all.
    static int g_int_r{ 0 };
    int&&             ret_rref(int, double) { return std::move(g_int_r); }
    void            (*ret_funcptr(int, double))(int) { return nullptr; } // returns void(*)(int)
    static int        g_arr_r[4]{ 0, 0, 0, 0 };
    int             (&ret_arrayref(int, double))[4] { return g_arr_r; }   // returns int(&)[4]

    // ── Exotic / wide primitive PARAMETER types (verbatim preservation) ──────
    // The arg-spelling tests use int/double/void*/char.  These pin that the full
    // set of C++ primitive parameter spellings survives the trait verbatim — the
    // wide character types, long double, and the unsigned ladder — exactly as
    // declared (downstream extract_frame_arg strips cv/ref; the trait does not).
    void freefn_exotic_prims(long double, char16_t, char32_t, wchar_t) {}
    void freefn_unsigned_ladder(unsigned char, unsigned short,
                                unsigned int, unsigned long long) {}
    void freefn_bool_and_nullptr(bool, std::nullptr_t) {}

    // A free-function detour whose self arg is the SECOND wrapper type, to pin a
    // distinct unique_ptr<W> element survives the pointer spec by type identity.
    struct other_wrapper : public vmhook::object<other_wrapper>
    {
        using vmhook::object<other_wrapper>::object;
    };
    void detour_other(vmhook::return_value&, std::unique_ptr<other_wrapper>,
                      std::int64_t, std::int32_t) {}

    // ── SECOND PASS scaffolding: ONE fixed wide detour signature, expressed in
    //    every accepted callable SHAPE.  The first pass owns the callable-shape
    //    axis (which shapes are accepted, what args_tuple_t each yields) and
    //    test_traits_extra.cpp owns hand-built java_slot_offsets tuples; NEITHER
    //    drives the SAME signature through every shape all the way to the slot
    //    table the consumer instantiates (java_slot_offsets<method_arg_tuple_t>).
    //    This scaffolding lets the new section pin that the end-to-end chain —
    //    function_traits -> tuple_tail -> java_slot_offsets — converges to one
    //    identical offset array regardless of which callable shape carried it.
    //
    //    The fixed shared signature (after the leading return_value& is stripped):
    //        (unique_ptr<self>, long, int, double, int, long, int)
    //    method tuple = (unique_ptr<sample_wrapper>, int64, int32, double,
    //                    int32, int64, int32).  Slot widening, computed by hand
    //    from is_java_double_slot_v (long/double -> 2 slots, else 1):
    //        self  @0 (+1) -> 0
    //        long  @1 (+2) -> 1
    //        int   @3 (+1) -> 3
    //        double@4 (+2) -> 4
    //        int   @6 (+1) -> 6
    //        long  @7 (+2) -> 7
    //        int   @9 (+1) -> 9
    //    -> std::array<int32_t,7>{ 0, 1, 3, 4, 6, 7, 9 }.
    using wide_method_tuple = std::tuple<std::unique_ptr<sample_wrapper>,
                                         std::int64_t, std::int32_t, double,
                                         std::int32_t, std::int64_t, std::int32_t>;

    // Free-function POINTER carrier (spec #1).
    void wide_detour_free(vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                          std::int64_t, std::int32_t, double,
                          std::int32_t, std::int64_t, std::int32_t) {}
    // STATIC member carrier (also spec #1 — ordinary function pointer).
    struct wide_static_host
    {
        static void s_wide(vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                           std::int64_t, std::int32_t, double,
                           std::int32_t, std::int64_t, std::int32_t) {}
    };
    // const MEMBER carrier (spec #4).
    struct wide_member_host
    {
        void m_wide(vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                    std::int64_t, std::int32_t, double,
                    std::int32_t, std::int64_t, std::int32_t) const {}
    };

    // ── cv/ref param-spelling INVARIANCE of the slot table (flaw #2 contract) ──
    // function_traits preserves a parameter's cv/ref VERBATIM in the tuple, but
    // java_slot_offsets calls is_java_double_slot_v which strips cv/ref before
    // deciding 1-vs-2 slots.  So a detour spelled with const-ref long/double must
    // produce the SAME offset table as the by-value spelling, even though the two
    // method tuples are DISTINCT types.  These two carriers differ ONLY by the
    // cv/ref on the widening (long/double) params.
    void wide_detour_byval(vmhook::return_value&, std::int64_t, std::int32_t,
                           double, std::int32_t) {}
    void wide_detour_cref(vmhook::return_value&, const std::int64_t&, std::int32_t,
                          const double&, std::int32_t) {}

    // A single-slot-only interleave of eight DISTINCT primitive spellings — none
    // is a J/D type, so the offset table must be the strict identity 0..7.  This
    // exercises the java_slot_offsets fold past the small cases the suite pins.
    void wide_detour_singles(vmhook::return_value&, bool, char16_t, std::int8_t,
                             float, void*, std::uint32_t, std::int16_t, char32_t) {}

    // A small enum and a pointer-to-double, to extend the is_java_double_slot_v
    // spelling matrix: an enum is single-slot; double* is a pointer (single-slot)
    // even though it points at a double.
    enum class small_enum : std::int32_t { a, b };
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
    // (d) A noexcept free-function POINTER is now a SUPPORTED shape: since C++17
    //     noexcept is part of the function type, and function_traits has a
    //     dedicated R(*)(args...) noexcept specialisation (spec 6), so its
    //     args_tuple_t is present and equals the throwing twin's.  (Pre-fix this
    //     was the detectable face of the noexcept gap, now closed for
    //     plain/const noexcept.)
    check("noexcept_free_fn_pointer_has_args_tuple",
          has_args_tuple<void(*)(int, double) noexcept>::value);
    check("noexcept_free_fn_pointer_args_match_throwing_twin",
          std::is_same_v<args_of<void(*)(int, double) noexcept>,
                         args_of<void(*)(int, double)>>);
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
    // 7.  noexcept does NOT change arity / args for the shapes that match.  Both
    //     the noexcept and non-noexcept forms now have specialisations, so a
    //     noexcept free-fn pointer yields the SAME args tuple as its throwing
    //     twin (asserted above), and a std::function wrapping the same signature
    //     exposes the same args.  (std::function itself is never noexcept-typed.)
    // ========================================================================
    check("std_function_args_match_with_or_without_throwing_body",
          std::is_same_v<args_of<std::function<void(int, double)>>,
                         std::tuple<int, double>>);

    // ========================================================================
    // 8.  noexcept member functions are now SUPPORTED (dedicated const-noexcept
    //     and noexcept member specialisations, 7/8).  A const-noexcept member
    //     pointer decomposes to its args verbatim, exactly like its throwing twin
    //     — the member face of the now-closed noexcept gap.  The lvalue-ref-
    //     qualified member is ALSO supported now (the cv/ref matrix), so its `&`
    //     form decomposes to its args verbatim; we both probe it positively and
    //     pin its exact member-pointer SHAPE so a regression is caught either way.
    check("member_pointer_const_noexcept_now_has_args_tuple",
          has_args_tuple<decltype(&member_host::m_const_noexcept)>::value);
    check("member_pointer_const_noexcept_args_verbatim",
          std::is_same_v<args_of<decltype(&member_host::m_const_noexcept)>,
                         std::tuple<int>>);
    check("member_pointer_lvalue_ref_qualified_now_has_args_tuple",
          has_args_tuple<decltype(&member_host::m_lref_qualified)>::value);
    check("member_pointer_lvalue_ref_qualified_args_verbatim",
          std::is_same_v<args_of<decltype(&member_host::m_lref_qualified)>,
                         std::tuple<int>>);
    check("member_pointer_lvalue_ref_qualified_shape_is_lvalue_ref",
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
    static_assert(has_args_tuple<void(*)(int, double) noexcept>::value,
                  "a noexcept free-function pointer is now accepted (dedicated noexcept "
                  "specialisation) — the noexcept gap is closed for plain/const noexcept");
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

    // ========================================================================
    // 11.  STATIC MEMBER FUNCTIONS (task-enumerated shape).
    //      &Struct::s_xxx is an ORDINARY function pointer R(*)(args...), so it
    //      lands on the free-fn POINTER specialisation — NOT the member-pointer
    //      specs.  Decomposes exactly like a free function: args verbatim, return
    //      discarded, leading return_value& stripped by tuple_tail.  Pins that a
    //      static member detour and a free-function detour are the same path.
    // ========================================================================
    check("static_member_args_are_int_double",
          std::is_same_v<args_of<decltype(&static_host::s_void)>, std::tuple<int, double>>);
    check("static_member_two_slot_return_discarded",
          std::is_same_v<args_of<decltype(&static_host::s_i64)>, std::tuple<int, double>>);
    check("static_member_string_return_discarded",
          std::is_same_v<args_of<decltype(&static_host::s_string)>, std::tuple<int, double>>);
    check("static_member_has_args_tuple",
          has_args_tuple<decltype(&static_host::s_void)>::value);
    // A static member function pointer has the SAME type as the equivalent free
    // function pointer — proving it is the free-fn-ptr spec, not a member spec.
    check("static_member_pointer_type_equals_free_fn_pointer",
          std::is_same_v<decltype(&static_host::s_void), void(*)(int, double)>);
    // Its full detour decomposes through tuple_tail identically to detour_free.
    check("static_member_detour_method_args_match_free_detour",
          std::is_same_v<method_args_of<decltype(&static_host::s_detour)>,
                         method_args_of<decltype(&detour_free)>>);
    check("static_member_detour_method_args_exact",
          std::is_same_v<method_args_of<decltype(&static_host::s_detour)>,
                         std::tuple<std::unique_ptr<sample_wrapper>,
                                    std::int32_t, std::int64_t>>);

    // ========================================================================
    // 12.  MEMBER-FUNCTION POINTER ARITY LADDER (specs #4 and #5).
    //      The free-fn pointer arity ladder is pinned at 0..8 above; the member-
    //      pointer specs had only fixed small counts.  Walk the const member spec
    //      and the non-const member spec across several widths so the variadic
    //      argument_types... pack is exercised on BOTH member specialisations.
    // ========================================================================
    check("const_member_pointer_arity_2",
          std::tuple_size_v<args_of<decltype(&member_host::m_const)>> == 2);
    check("const_member_pointer_arity_6",
          std::tuple_size_v<args_of<decltype(&member_quals::m_const_wide)>> == 6);
    check("const_member_pointer_arity_6_all_ints",
          std::is_same_v<args_of<decltype(&member_quals::m_const_wide)>,
                         std::tuple<int, int, int, int, int, int>>);
    check("nonconst_member_pointer_arity_1",
          std::tuple_size_v<args_of<decltype(&member_host::m_nonconst)>> == 1);
    check("nonconst_member_pointer_arity_5",
          std::tuple_size_v<args_of<decltype(&member_quals::m_nonconst_wide)>> == 5);
    check("nonconst_member_pointer_wide_return_discarded",
          std::is_same_v<args_of<decltype(&member_quals::m_nonconst_wide)>,
                         std::tuple<int, int, int, int, int>>);

    // ========================================================================
    // 13.  MEMBER-POINTER PARAMETER-SPELLING PRESERVATION (specs #4/#5).
    //      Section 2 pins verbatim ref/const-ref/pointer preservation on the
    //      free-fn pointer spec.  Pin the SAME contract on the const member spec:
    //      reference / const-ref / pointer params survive verbatim, and a
    //      top-level const on a by-value param is dropped (not part of the type).
    // ========================================================================
    check("const_member_pointer_preserves_ref_const_ref_pointer",
          std::is_same_v<args_of<decltype(&member_ref_params::m_refs)>,
                         std::tuple<int&, const double&, void*>>);
    check("const_member_pointer_drops_top_level_const_on_value_params",
          std::is_same_v<args_of<decltype(&member_ref_params::m_const_val)>,
                         std::tuple<int, double>>);

    // ========================================================================
    // 14.  THE FULL MEMBER-QUALIFIER MATRIX, now ACCEPTED.  noexcept members are
    //      supported (specs 7/8) and so are every cv- / ref-qualified member form
    //      (the cv x ref x noexcept matrix): volatile, const volatile, &&, const&,
    //      and && noexcept all decompose to their args VERBATIM, with the qualifier
    //      irrelevant to args_tuple_t.  Each is probed positively via has_args_tuple
    //      AND pinned by exact member-pointer SHAPE so a regression in either the
    //      acceptance or the precise type is caught.  (The C-variadic member form
    //      is the sole remaining gap — asserted by shape in §15.)
    // ========================================================================
    check("member_pointer_volatile_now_has_args_tuple",
          has_args_tuple<decltype(&member_quals::m_volatile)>::value);
    check("member_pointer_volatile_args_verbatim",
          std::is_same_v<args_of<decltype(&member_quals::m_volatile)>,
                         std::tuple<int, double>>);
    check("member_pointer_volatile_shape_is_volatile",
          std::is_same_v<decltype(&member_quals::m_volatile),
                         void (member_quals::*)(int, double) volatile>);
    check("member_pointer_const_volatile_now_has_args_tuple",
          has_args_tuple<decltype(&member_quals::m_const_volatile)>::value);
    check("member_pointer_const_volatile_args_verbatim",
          std::is_same_v<args_of<decltype(&member_quals::m_const_volatile)>,
                         std::tuple<int, double>>);
    check("member_pointer_const_volatile_shape_is_const_volatile",
          std::is_same_v<decltype(&member_quals::m_const_volatile),
                         void (member_quals::*)(int, double) const volatile>);
    check("member_pointer_rvalue_ref_now_has_args_tuple",
          has_args_tuple<decltype(&member_quals::m_rref)>::value);
    check("member_pointer_rvalue_ref_args_verbatim",
          std::is_same_v<args_of<decltype(&member_quals::m_rref)>,
                         std::tuple<int, double>>);
    check("member_pointer_rvalue_ref_shape_is_rvalue_ref",
          std::is_same_v<decltype(&member_quals::m_rref),
                         void (member_quals::*)(int, double) &&>);
    check("member_pointer_const_lvalue_ref_now_has_args_tuple",
          has_args_tuple<decltype(&member_quals::m_const_lref)>::value);
    check("member_pointer_const_lvalue_ref_args_verbatim",
          std::is_same_v<args_of<decltype(&member_quals::m_const_lref)>,
                         std::tuple<int, double>>);
    check("member_pointer_const_lvalue_ref_shape_is_const_lref",
          std::is_same_v<decltype(&member_quals::m_const_lref),
                         void (member_quals::*)(int, double) const&>);
    check("member_pointer_nonconst_noexcept_now_has_args_tuple",
          has_args_tuple<decltype(&member_quals::m_nonconst_noexcept)>::value);
    check("member_pointer_nonconst_noexcept_args_verbatim",
          std::is_same_v<args_of<decltype(&member_quals::m_nonconst_noexcept)>,
                         std::tuple<long>>);
    check("member_pointer_rvalue_ref_noexcept_now_has_args_tuple",
          has_args_tuple<decltype(&member_quals::m_rref_noexcept)>::value);
    check("member_pointer_rvalue_ref_noexcept_args_verbatim",
          std::is_same_v<args_of<decltype(&member_quals::m_rref_noexcept)>,
                         std::tuple<int>>);
    check("member_pointer_rvalue_ref_noexcept_shape_is_rref_noexcept",
          std::is_same_v<decltype(&member_quals::m_rref_noexcept),
                         void (member_quals::*)(int) && noexcept>);

    // ========================================================================
    // 15.  C-STYLE VARIADIC functions — free pointer AND member pointer.
    //      The free C-variadic pointer is rejected (asserted §6).  Pin the MEMBER
    //      analogue: R(C::*)(args..., ...) const has the C-variadic member-pointer
    //      type and matches no specialisation — asserted by construction, since
    //      its trailing ellipsis is part of the type and the member specs require
    //      a fixed parameter pack.  (Detector-safe via the free-fn variadic side.)
    // ========================================================================
    check("c_variadic_member_pointer_is_unsupported_shape",
          std::is_same_v<decltype(&member_quals::m_cvariadic),
                         void (member_quals::*)(int, ...) const>);
    // A C-variadic FREE function pointer with a wider fixed prefix is still
    // rejected (the §6 case used one fixed arg; pin a multi-fixed-arg form too).
    check("c_variadic_free_pointer_multi_fixed_has_no_args_tuple",
          !has_args_tuple<void(*)(int, double, ...)>::value);

    // ========================================================================
    // 16.  FUNCTOR operator() carrying a volatile / && qualifier — now ACCEPTED.
    //      The functor void_t spec (#3) is selected for these (their operator()
    //      is addressable) and now forwards to a member-pointer specialisation
    //      that exists, so reading args_tuple_t is well-formed: they CAN be fed
    //      to has_args_tuple and report true, decomposing past the leading
    //      return_value& to the method tuple.  The exact &F::operator() shape is
    //      still pinned so a regression in the qualifier handling is caught.
    // ========================================================================
    check("volatile_call_functor_now_has_args_tuple",
          has_args_tuple<volatile_call_functor>::value);
    check("volatile_call_functor_method_args_drop_return_value",
          std::is_same_v<method_args_of<volatile_call_functor>, std::tuple<int>>);
    check("volatile_call_functor_operator_is_volatile_qualified",
          std::is_same_v<decltype(&volatile_call_functor::operator()),
                         void (volatile_call_functor::*)(vmhook::return_value&, int) volatile>);
    check("rref_call_functor_now_has_args_tuple",
          has_args_tuple<rref_call_functor>::value);
    check("rref_call_functor_method_args_drop_return_value",
          std::is_same_v<method_args_of<rref_call_functor>, std::tuple<int>>);
    check("rref_call_functor_operator_is_rvalue_ref_qualified",
          std::is_same_v<decltype(&rref_call_functor::operator()),
                         void (rref_call_functor::*)(vmhook::return_value&, int) &&>);

    // ========================================================================
    // 17.  RETURN-TYPE INDEPENDENCE, the remaining return shapes.
    //      §1 covered value / two-slot / lref / const-lref returns.  These add
    //      the last return spellings the trait must still DISCARD: an rvalue-
    //      reference return, a pointer-to-function return, and a reference-to-
    //      array return.  args_tuple_t must stay tuple<int,double> for each.
    // ========================================================================
    check("ret_rvalue_ref_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_rref)>, std::tuple<int, double>>);
    check("ret_function_pointer_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_funcptr)>, std::tuple<int, double>>);
    check("ret_array_reference_args_are_int_double",
          std::is_same_v<args_of<decltype(&ret_arrayref)>, std::tuple<int, double>>);
    // All three exotic-return shapes agree with the plain void-return shape.
    check("exotic_return_shapes_share_void_return_args_tuple",
          std::is_same_v<args_of<decltype(&ret_rref)>, args_of<decltype(&ret_void)>>
          && std::is_same_v<args_of<decltype(&ret_funcptr)>, args_of<decltype(&ret_void)>>
          && std::is_same_v<args_of<decltype(&ret_arrayref)>, args_of<decltype(&ret_void)>>);

    // ========================================================================
    // 18.  EXOTIC / WIDE PRIMITIVE PARAMETER TYPES preserved VERBATIM.
    //      §2 used int/double/void*/char.  Pin that the full C++ primitive
    //      spelling set survives the trait unchanged — the wide character types,
    //      long double, the unsigned ladder, bool and nullptr_t.  (These are the
    //      element types extract_frame_arg then classifies downstream.)
    // ========================================================================
    check("freefn_preserves_long_double_and_wide_chars",
          std::is_same_v<args_of<decltype(&freefn_exotic_prims)>,
                         std::tuple<long double, char16_t, char32_t, wchar_t>>);
    check("freefn_preserves_unsigned_ladder",
          std::is_same_v<args_of<decltype(&freefn_unsigned_ladder)>,
                         std::tuple<unsigned char, unsigned short,
                                    unsigned int, unsigned long long>>);
    check("freefn_preserves_bool_and_nullptr_t",
          std::is_same_v<args_of<decltype(&freefn_bool_and_nullptr)>,
                         std::tuple<bool, std::nullptr_t>>);

    // ========================================================================
    // 19.  std::function SPELLING PRESERVATION (the spec #2 face).
    //      §2 pins verbatim ref/const-ref/pointer preservation on the free-fn
    //      POINTER spec.  Pin the SAME on the std::function spec: the wrapped
    //      signature's reference/const-ref/pointer params survive verbatim, and a
    //      by-value vs const-ref spelling produces DIFFERENT args tuples.
    // ========================================================================
    check("std_function_preserves_ref_const_ref_pointer",
          std::is_same_v<args_of<std::function<void(int&, const double&, void*)>>,
                         std::tuple<int&, const double&, void*>>);
    check("std_function_by_value_and_const_ref_args_differ",
          !std::is_same_v<args_of<std::function<void(int, std::string)>>,
                          args_of<std::function<void(const int&, const std::string&)>>>);
    check("std_function_by_value_args_exact",
          std::is_same_v<args_of<std::function<void(int, std::string)>>,
                         std::tuple<int, std::string>>);

    // ========================================================================
    // 20.  DISTINCT WRAPPER ELEMENT-IDENTITY through the free-fn POINTER spec.
    //      A second wrapper type must survive as its own unique_ptr element —
    //      decomposition keys on the declared type, never collapsing wrappers.
    //      (test_traits_extra.cpp pins this on a lambda; here on the pointer spec.)
    // ========================================================================
    check("free_detour_other_wrapper_method_args_exact",
          std::is_same_v<method_args_of<decltype(&detour_other)>,
                         std::tuple<std::unique_ptr<other_wrapper>,
                                    std::int64_t, std::int32_t>>);
    check("two_wrapper_detours_have_distinct_self_element",
          !std::is_same_v<
              std::tuple_element_t<0, method_args_of<decltype(&detour_free)>>,
              std::tuple_element_t<0, method_args_of<decltype(&detour_other)>>>);

    // ========================================================================
    // 21.  COMPILE-TIME ENFORCEMENT for the new shapes (build breaks on regress).
    // ========================================================================
    // Static member function == free-fn-pointer path.
    static_assert(std::is_same_v<args_of<decltype(&static_host::s_void)>,
                                 std::tuple<int, double>>,
                  "a static member function (&S::s) is the free-fn POINTER spec: "
                  "args verbatim, return discarded — never the member-pointer spec");
    static_assert(std::is_same_v<decltype(&static_host::s_void), void(*)(int, double)>,
                  "&Struct::static_fn has an ordinary function-pointer type");
    static_assert(has_args_tuple<decltype(&static_host::s_void)>::value,
                  "a static member function pointer must be an accepted detour shape");
    // Member-pointer arity ladder endpoints (both specs).
    static_assert(std::tuple_size_v<args_of<decltype(&member_quals::m_const_wide)>> == 6,
                  "const member-pointer spec must capture a 6-arg parameter pack");
    static_assert(std::tuple_size_v<args_of<decltype(&member_quals::m_nonconst_wide)>> == 5,
                  "non-const member-pointer spec must capture a 5-arg parameter pack");
    // Member-pointer parameter spelling preserved verbatim, like the free-fn spec.
    static_assert(std::is_same_v<args_of<decltype(&member_ref_params::m_refs)>,
                                 std::tuple<int&, const double&, void*>>,
                  "const member-pointer spec must preserve ref/const-ref/pointer "
                  "parameter spellings verbatim");
    // The full member-qualifier matrix is now accepted: a volatile- and an
    // rvalue-ref-qualified member function each expose args_tuple_t verbatim.
    static_assert(std::is_same_v<args_of<decltype(&member_quals::m_volatile)>,
                                 std::tuple<int, double>>,
                  "a volatile-qualified member function is now accepted and exposes "
                  "its args verbatim (cv/ref/noexcept matrix)");
    static_assert(std::is_same_v<args_of<decltype(&member_quals::m_rref)>,
                                 std::tuple<int, double>>,
                  "an rvalue-ref-qualified member function is now accepted and "
                  "exposes its args verbatim (cv/ref/noexcept matrix)");
    static_assert(std::is_same_v<args_of<decltype(&member_quals::m_const_lref)>,
                                 std::tuple<int, double>>,
                  "a const-lvalue-ref-qualified member function is now accepted");
    static_assert(std::is_same_v<args_of<decltype(&member_quals::m_const_volatile)>,
                                 std::tuple<int, double>>,
                  "a const-volatile-qualified member function is now accepted");
    static_assert(std::is_same_v<args_of<decltype(&member_quals::m_rref_noexcept)>,
                                 std::tuple<int>>,
                  "an rvalue-ref + noexcept qualified member function is now accepted");
    // C-variadic member pointer shape + multi-fixed free variadic rejection.
    static_assert(std::is_same_v<decltype(&member_quals::m_cvariadic),
                                 void (member_quals::*)(int, ...) const>,
                  "a C-style variadic member function pointer matches no specialisation");
    static_assert(!has_args_tuple<void(*)(int, double, ...)>::value,
                  "a multi-fixed-arg C-variadic free-function pointer must be rejected");
    // Exotic return shapes still discarded.
    static_assert(std::is_same_v<args_of<decltype(&ret_rref)>, std::tuple<int, double>>,
                  "an rvalue-reference return type must not leak into args_tuple_t");
    static_assert(std::is_same_v<args_of<decltype(&ret_funcptr)>, std::tuple<int, double>>,
                  "a pointer-to-function return type must not leak into args_tuple_t");
    // Exotic primitive parameter spellings preserved verbatim.
    static_assert(std::is_same_v<args_of<decltype(&freefn_exotic_prims)>,
                                 std::tuple<long double, char16_t, char32_t, wchar_t>>,
                  "long double and the wide character types must survive the trait "
                  "verbatim as declared");
    // std::function spelling preservation + by-value/const-ref divergence.
    static_assert(!std::is_same_v<args_of<std::function<void(int, std::string)>>,
                                  args_of<std::function<void(const int&, const std::string&)>>>,
                  "std::function preserves param cv/ref spelling: by-value and "
                  "const-ref signatures yield DIFFERENT args tuples (spec #2 contract)");

    // ========================================================================
    // 16.  noexcept DETOUR DECOMPOSITION — the regression guard for the noexcept
    //      specialisations (6/7/8).  Pre-fix, feeding ANY noexcept callable
    //      through the hook<T>() chain (args_of -> method_args_of) was a hard
    //      compile error ("no member args_tuple_t"); this whole block now compiles
    //      and every noexcept shape decomposes IDENTICALLY to its throwing twin.
    //      Named detour lambdas used only in decltype/has_args_tuple match the
    //      generic_lambda pattern above (no unused-variable warning on any CI
    //      compiler; avoids the unevaluated-lambda C++20 feature for portability).
    // ========================================================================
    {
        auto throwing_detour      = [](vmhook::return_value&, int, double) {};
        auto noexcept_detour      = [](vmhook::return_value&, int, double) noexcept {};
        auto noexcept_void_detour = [](vmhook::return_value&) noexcept {};
        // A noexcept detour lambda decomposes to the SAME args as its throwing
        // twin — this static_assert would NOT compile before the noexcept specs.
        static_assert(std::is_same_v<args_of<decltype(noexcept_detour)>,
                                     args_of<decltype(throwing_detour)>>,
                      "a noexcept detour lambda must decompose to the same args as its throwing twin");
        check("noexcept_lambda_has_args_tuple",
              has_args_tuple<decltype(noexcept_detour)>::value);
        check("noexcept_lambda_args_match_throwing_twin",
              std::is_same_v<args_of<decltype(noexcept_detour)>,
                             std::tuple<vmhook::return_value&, int, double>>);
        check("noexcept_lambda_method_args_drop_return_value",
              std::is_same_v<method_args_of<decltype(noexcept_detour)>,
                             std::tuple<int, double>>);
        check("noexcept_void_lambda_method_args_empty",
              std::is_same_v<method_args_of<decltype(noexcept_void_detour)>,
                             std::tuple<>>);
    }
    // noexcept free-function POINTER decomposes identically to its throwing twin.
    check("noexcept_fnptr_args_match_throwing_twin",
          std::is_same_v<args_of<void(*)(vmhook::return_value&, int, double) noexcept>,
                         args_of<void(*)(vmhook::return_value&, int, double)>>);

    // ========================================================================
    // SECOND ADDITIVE PASS — the END-TO-END chain hook<T>() actually runs:
    //   function_traits<F>::args_tuple_t -> tuple_tail -> method_arg_tuple_t
    //   -> java_slot_offsets<method_arg_tuple_t>::value
    //   -> extract_frame_arg<std::tuple_element_t<k, method_arg_tuple_t>>.
    // The first pass stops at the method tuple (callable-shape axis) and
    // test_traits_extra.cpp drives only HAND-BUILT tuples into java_slot_offsets.
    // NEITHER drives ONE fixed signature through EVERY accepted callable shape all
    // the way to the slot table, nor pins the per-element tuple_element_t the
    // consumer instantiates, nor the cv/ref-invariance of the slot table.  All of
    // that is new ground.  Every expected value is computed from source: the slot
    // rule is is_java_double_slot_v (long/double/uint64 -> +2, else +1); the slot
    // table for the shared signature is { 0, 1, 3, 4, 6, 7, 9 } (derived above).
    // ========================================================================

    // 22.  SHAPE-CONVERGENCE of the method tuple.  The shared wide signature,
    //      carried by a free-fn pointer, a static member (also a fn pointer), a
    //      const member pointer, a lambda, and a std::function, must all strip the
    //      leading return_value& to the SAME method tuple type.
    {
        auto wide_lambda = [](vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                              std::int64_t, std::int32_t, double,
                              std::int32_t, std::int64_t, std::int32_t) {};
        using fn_t = std::function<void(vmhook::return_value&,
                                        std::unique_ptr<sample_wrapper>,
                                        std::int64_t, std::int32_t, double,
                                        std::int32_t, std::int64_t, std::int32_t)>;
        check("wide_free_pointer_method_tuple_is_canonical",
              std::is_same_v<method_args_of<decltype(&wide_detour_free)>, wide_method_tuple>);
        check("wide_static_member_method_tuple_is_canonical",
              std::is_same_v<method_args_of<decltype(&wide_static_host::s_wide)>, wide_method_tuple>);
        check("wide_const_member_method_tuple_is_canonical",
              std::is_same_v<method_args_of<decltype(&wide_member_host::m_wide)>, wide_method_tuple>);
        check("wide_lambda_method_tuple_is_canonical",
              std::is_same_v<method_args_of<decltype(wide_lambda)>, wide_method_tuple>);
        check("wide_std_function_method_tuple_is_canonical",
              std::is_same_v<method_args_of<fn_t>, wide_method_tuple>);
        // All five shapes agree pairwise — the decomposition is shape-agnostic.
        check("all_wide_shapes_share_one_method_tuple",
              std::is_same_v<method_args_of<decltype(&wide_detour_free)>,
                             method_args_of<decltype(&wide_static_host::s_wide)>>
              && std::is_same_v<method_args_of<decltype(&wide_static_host::s_wide)>,
                                method_args_of<decltype(&wide_member_host::m_wide)>>
              && std::is_same_v<method_args_of<decltype(&wide_member_host::m_wide)>,
                                method_args_of<decltype(wide_lambda)>>
              && std::is_same_v<method_args_of<decltype(wide_lambda)>,
                                method_args_of<fn_t>>);

        // 23.  END-TO-END slot table from EACH shape.  java_slot_offsets over the
        //      method tuple derived from each callable shape must equal the hand-
        //      computed { 0, 1, 3, 4, 6, 7, 9 }.  This is the exact expression
        //      hook<T>()'s wrapper_detour evaluates (java_slot_offsets<
        //      method_arg_tuple_t>::value), driven from a real callable, not a
        //      hand-built tuple.
        constexpr std::array<std::int32_t, 7> expect_wide{ 0, 1, 3, 4, 6, 7, 9 };
        check("wide_free_pointer_slot_table",
              (vmhook::detail::java_slot_offsets<
                   method_args_of<decltype(&wide_detour_free)>>::value == expect_wide));
        check("wide_static_member_slot_table",
              (vmhook::detail::java_slot_offsets<
                   method_args_of<decltype(&wide_static_host::s_wide)>>::value == expect_wide));
        check("wide_const_member_slot_table",
              (vmhook::detail::java_slot_offsets<
                   method_args_of<decltype(&wide_member_host::m_wide)>>::value == expect_wide));
        check("wide_lambda_slot_table",
              (vmhook::detail::java_slot_offsets<
                   method_args_of<decltype(wide_lambda)>>::value == expect_wide));
        check("wide_std_function_slot_table",
              (vmhook::detail::java_slot_offsets<method_args_of<fn_t>>::value == expect_wide));
        // The slot table has exactly one entry per method arg (arity 7).
        check("wide_slot_table_size_equals_arity",
              vmhook::detail::java_slot_offsets<wide_method_tuple>::value.size() == 7);

        // 24.  PER-ELEMENT tuple_element_t round-trip — the precise type each
        //      extract_frame_arg<std::tuple_element_t<k, method_arg_tuple_t>> is
        //      instantiated with, for the lambda shape, k = 0..6.
        check("wide_lambda_element_0_is_unique_ptr_self",
              std::is_same_v<std::tuple_element_t<0, method_args_of<decltype(wide_lambda)>>,
                             std::unique_ptr<sample_wrapper>>);
        check("wide_lambda_element_1_is_long",
              std::is_same_v<std::tuple_element_t<1, method_args_of<decltype(wide_lambda)>>,
                             std::int64_t>);
        check("wide_lambda_element_2_is_int",
              std::is_same_v<std::tuple_element_t<2, method_args_of<decltype(wide_lambda)>>,
                             std::int32_t>);
        check("wide_lambda_element_3_is_double",
              std::is_same_v<std::tuple_element_t<3, method_args_of<decltype(wide_lambda)>>,
                             double>);
        check("wide_lambda_element_4_is_int",
              std::is_same_v<std::tuple_element_t<4, method_args_of<decltype(wide_lambda)>>,
                             std::int32_t>);
        check("wide_lambda_element_5_is_long",
              std::is_same_v<std::tuple_element_t<5, method_args_of<decltype(wide_lambda)>>,
                             std::int64_t>);
        check("wide_lambda_element_6_is_int",
              std::is_same_v<std::tuple_element_t<6, method_args_of<decltype(wide_lambda)>>,
                             std::int32_t>);
    }

    // 25.  cv/ref PARAM SPELLING INVARIANCE of the slot table (flaw #2 contract).
    //      The by-value and const-ref carriers differ ONLY by cv/ref on the
    //      widening params, so their method TUPLES are distinct types — but
    //      java_slot_offsets routes each element through is_java_double_slot_v,
    //      which strips cv/ref, so the offset tables must be IDENTICAL.
    //      Signature (after strip): (long, int, double, int) ->
    //        long@0(+2)->0, int@2(+1)->2, double@3(+2)->3, int@5(+1)->5 = {0,2,3,5}.
    {
        using byval_tuple = method_args_of<decltype(&wide_detour_byval)>;
        using cref_tuple  = method_args_of<decltype(&wide_detour_cref)>;
        // The two method tuples are NOT the same type (cv/ref preserved verbatim).
        check("byval_and_cref_method_tuples_are_distinct_types",
              !std::is_same_v<byval_tuple, cref_tuple>);
        // The by-value tuple is exactly the unqualified spelling.
        check("byval_method_tuple_exact",
              std::is_same_v<byval_tuple,
                             std::tuple<std::int64_t, std::int32_t, double, std::int32_t>>);
        // The const-ref tuple preserves the const-ref on the widening params.
        check("cref_method_tuple_preserves_const_ref",
              std::is_same_v<cref_tuple,
                             std::tuple<const std::int64_t&, std::int32_t,
                                        const double&, std::int32_t>>);
        constexpr std::array<std::int32_t, 4> expect_cref{ 0, 2, 3, 5 };
        check("byval_slot_table_is_0_2_3_5",
              (vmhook::detail::java_slot_offsets<byval_tuple>::value == expect_cref));
        check("cref_slot_table_is_0_2_3_5",
              (vmhook::detail::java_slot_offsets<cref_tuple>::value == expect_cref));
        // The load-bearing fact: DESPITE distinct tuple types, the slot tables
        // are bit-for-bit identical — cv/ref on a param cannot shift any slot.
        check("byval_and_cref_slot_tables_identical",
              (vmhook::detail::java_slot_offsets<byval_tuple>::value
               == vmhook::detail::java_slot_offsets<cref_tuple>::value));
    }

    // 26.  ALL-SINGLE-SLOT INTERLEAVE — eight distinct non-J/D primitive spellings
    //      must produce the strict identity table 0..7 (no widening anywhere).
    {
        using singles_tuple = method_args_of<decltype(&wide_detour_singles)>;
        check("singles_method_tuple_exact",
              std::is_same_v<singles_tuple,
                             std::tuple<bool, char16_t, std::int8_t, float, void*,
                                        std::uint32_t, std::int16_t, char32_t>>);
        constexpr std::array<std::int32_t, 8> expect_singles{ 0, 1, 2, 3, 4, 5, 6, 7 };
        check("singles_slot_table_is_identity_0_to_7",
              (vmhook::detail::java_slot_offsets<singles_tuple>::value == expect_singles));
    }

    // 27.  J/D-FIRST and J/D-LAST boundary tables, derived from real callables,
    //      pinning the fold at both ends of the parameter pack.
    {
        // (long, int): long@0(+2)->0, int@2 -> {0,2}.
        check("jd_first_long_int_slot_table",
              (vmhook::detail::java_slot_offsets<
                   std::tuple<std::int64_t, std::int32_t>>::value
               == std::array<std::int32_t, 2>{ 0, 2 }));
        // (int, long): int@0(+1)->0, long@1 -> {0,1}.  The trailing long consumes
        // its own two slots but nothing follows, so it never shifts another entry.
        check("jd_last_int_long_slot_table",
              (vmhook::detail::java_slot_offsets<
                   std::tuple<std::int32_t, std::int64_t>>::value
               == std::array<std::int32_t, 2>{ 0, 1 }));
        // Three consecutive widening types (long, double, uint64) -> {0,2,4}.
        check("three_consecutive_widening_slot_table",
              (vmhook::detail::java_slot_offsets<
                   std::tuple<std::int64_t, double, std::uint64_t>>::value
               == std::array<std::int32_t, 3>{ 0, 2, 4 }));
    }

    // 28.  is_java_double_slot_v — additional spellings not pinned elsewhere.
    //      An enum is single-slot; a pointer-to-double is a pointer (single-slot)
    //      even though it points at a double; long double is NOT std::int64_t /
    //      uint64_t / double, so it is single-slot by this trait's definition;
    //      a uint64_t lvalue/rvalue-ref still widens (cv/ref stripped first).
    check("is_java_double_slot_v_enum_is_one",
          !vmhook::detail::is_java_double_slot_v<small_enum>);
    check("is_java_double_slot_v_double_pointer_is_one",
          !vmhook::detail::is_java_double_slot_v<double*>);
    check("is_java_double_slot_v_long_double_is_one",
          !vmhook::detail::is_java_double_slot_v<long double>);
    check("is_java_double_slot_v_float_is_one_again",
          !vmhook::detail::is_java_double_slot_v<float>);
    check("is_java_double_slot_v_uint64_rvalue_ref_is_two",
          vmhook::detail::is_java_double_slot_v<std::uint64_t&&>);
    check("is_java_double_slot_v_double_const_volatile_is_two",
          vmhook::detail::is_java_double_slot_v<const volatile double>);

    // 29.  COMPILE-TIME ENFORCEMENT for the second pass (build breaks on regress).
    static_assert(std::is_same_v<method_args_of<decltype(&wide_detour_free)>,
                                 wide_method_tuple>,
                  "the shared wide signature must strip return_value& to the canonical "
                  "method tuple on the free-fn pointer spec");
    static_assert(std::is_same_v<method_args_of<decltype(&wide_static_host::s_wide)>,
                                 method_args_of<decltype(&wide_member_host::m_wide)>>,
                  "static-member and const-member carriers of the same signature must "
                  "decompose to the same method tuple");
    static_assert(vmhook::detail::java_slot_offsets<wide_method_tuple>::value
                      == std::array<std::int32_t, 7>{ 0, 1, 3, 4, 6, 7, 9 },
                  "the canonical wide method tuple must widen to slot table "
                  "{0,1,3,4,6,7,9} (long/double advance the cursor by 2)");
    static_assert(vmhook::detail::java_slot_offsets<
                          method_args_of<decltype(&wide_detour_byval)>>::value
                      == vmhook::detail::java_slot_offsets<
                          method_args_of<decltype(&wide_detour_cref)>>::value,
                  "cv/ref on a parameter must NOT shift any slot: is_java_double_slot_v "
                  "strips cv/ref, so by-value and const-ref signatures share one table");
    static_assert(vmhook::detail::java_slot_offsets<
                          method_args_of<decltype(&wide_detour_singles)>>::value
                      == std::array<std::int32_t, 8>{ 0, 1, 2, 3, 4, 5, 6, 7 },
                  "eight single-slot primitives must produce the strict identity table");
    static_assert(!vmhook::detail::is_java_double_slot_v<small_enum>,
                  "an enum is a single-slot Java argument");
    static_assert(!vmhook::detail::is_java_double_slot_v<double*>,
                  "a pointer-to-double is a single-slot pointer, not a double");

    std::printf("vmhook traits-function_traits: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
