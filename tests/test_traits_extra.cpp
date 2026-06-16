// Standalone (no-JVM) trait test for the hook<T> callback-decomposition chain:
// function_traits -> tuple_tail -> java_slot_offsets, plus the value_type-shadow
// regression guards on is_unique_ptr / is_vector / is_unique_object_ptr and the
// dependent_false_v lazy-static_assert helper.  Every fact asserted here is a
// compile-time property the library's argument-decoding relies on; the checks
// below evaluate each trait into a constexpr bool so a regression shows up as a
// visible [FAIL] line in addition to a hard static_assert.  Nothing here needs a
// live JVM or a real interpreter frame.  (The end-to-end behaviour of these
// traits driving real OOP decoding is covered by JVM integration in example.cpp;
// the g_type_factory_map / register_class round-trip is covered in
// test_helpers.cpp because it needs the runtime factory registry.)
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // A representative user wrapper, exactly the shape a caller passes through
    // unique_ptr into a hook<T> callback: `class W : public vmhook::object<W>`.
    struct sample_wrapper : public vmhook::object<sample_wrapper>
    {
        using vmhook::object<sample_wrapper>::object;
    };

    static_assert(std::is_base_of_v<vmhook::object_base, sample_wrapper>,
                  "sample_wrapper must derive from object_base for the wrapper-arg checks");

    // --- Compile-time facts mirrored as runtime-visible booleans -------------
    //
    // is_unique_ptr<unique_ptr<X>>::value_type_t MUST resolve to X.  Before the
    // shadow fix the std::true_type base's inherited `value_type = bool` typedef
    // won name lookup and value_type_t collapsed to bool for every wrapper,
    // silently skipping the JNI-arg-write branch in write_jni_arg_to_slot.
    template<typename T>
    using unique_value_t = typename vmhook::detail::is_unique_ptr<T>::value_type_t;

    template<typename T>
    using vector_value_t = typename vmhook::detail::is_vector<T>::value_type_t;

    // hook<T> decomposes the user callback exactly like this: take the lambda,
    // pull its full argument tuple via function_traits, then strip the leading
    // vmhook::return_value& via tuple_tail to get the Java-visible arg tuple,
    // which then drives java_slot_offsets.  Reproduce that chain on a stub
    // callable so the decomposition is pinned down without a JVM.
    template<typename Callable>
    using all_args_of = typename vmhook::detail::function_traits<
        std::remove_cvref_t<Callable>>::args_tuple_t;

    template<typename Callable>
    using method_args_of = typename vmhook::detail::tuple_tail<all_args_of<Callable>>::type_t;

    // A free-function-pointer detour: (return_value&, self, int).
    void free_detour(vmhook::return_value&,
                     std::unique_ptr<sample_wrapper>,
                     std::int32_t) {}

    // SFINAE probe: is is_vector<T>::value_type_t a well-formed member type?
    // For non-vector T the primary template has no value_type_t, so this is
    // false; for std::vector<...> it is true.  Lets us assert that the member
    // only exists on the specialisation (the shadow-safe spelling).
    template<typename T, typename = void>
    struct has_vector_value_t : std::false_type {};
    template<typename T>
    struct has_vector_value_t<T, std::void_t<typename vmhook::detail::is_vector<T>::value_type_t>>
        : std::true_type {};

    // SFINAE probe: does function_traits<F>::args_tuple_t resolve to a member
    // type for F?  function_traits' primary template (vmhook.hpp:7482-7483) is
    // declared but left UNDEFINED, so for any callable shape the five
    // specialisations do not match, args_tuple_t is absent and this is false.
    // Used to pin exactly which callable shapes the trait accepts vs rejects.
    //
    // IMPORTANT detectability boundary: this probe is only well-formed for
    // shapes whose `operator()` either (a) yields a plain, non-overloaded,
    // non-template member-pointer the functor spec at 7498 forwards to, or
    // (b) has NO single addressable `operator()` (generic / overloaded /
    // templated call operator), in which case the 7498 spec's void_t
    // substitution fails and F cleanly falls through to the undefined primary
    // -> args_tuple_t absent -> detector reports false.  A functor whose
    // operator() is present-and-addressable but carries a qualifier the
    // member-pointer specs do NOT cover (noexcept / & / && / volatile / C
    // varargs) is a HARD COMPILE ERROR, NOT a detectable absence: 7498 IS
    // selected, its base function_traits<member-ptr> is the undefined primary,
    // and reading args_tuple_t off it is a non-SFINAE error.  Such shapes are
    // therefore documented below as build-breaking and are deliberately NOT
    // fed to this detector (doing so would fail to compile the whole TU).
    template<typename F, typename = void>
    struct has_args_tuple : std::false_type {};
    template<typename F>
    struct has_args_tuple<F, std::void_t<typename vmhook::detail::function_traits<F>::args_tuple_t>>
        : std::true_type {};

    // SFINAE probe: does tuple_tail<T>::type_t exist?  tuple_tail's primary
    // (vmhook.hpp:7524-7525) is undefined; type_t is defined by the
    // <first, rest...> spec (7527-7531) AND, since the flaw #1 fix, by the
    // empty-tuple base specialisation (7545-7549).  So this now reports true
    // for BOTH the empty std::tuple<> and any non-empty tuple; only types that
    // are not std::tuple specialisations at all leave type_t absent.
    template<typename T, typename = void>
    struct has_tuple_tail : std::false_type {};
    template<typename T>
    struct has_tuple_tail<T, std::void_t<typename vmhook::detail::tuple_tail<T>::type_t>>
        : std::true_type {};

    // A representative second wrapper type, used to prove element-type identity
    // is by-type (not by-position) through the decomposition chain.
    struct other_wrapper : public vmhook::object<other_wrapper>
    {
        using vmhook::object<other_wrapper>::object;
    };

    // Free function whose operator-less plain pointer the trait DOES accept;
    // a wide-arg shape so java_slot_offsets has J/D widening to chew on.
    void free_detour_wide(vmhook::return_value&,
                          std::unique_ptr<sample_wrapper>,
                          std::int64_t,
                          std::int32_t,
                          double) {}

    // A non-capturing, non-mutable lambda is a const-operator() functor; an
    // explicit struct lets us name the const / non-const specialisations
    // directly (no closure type involved).
    struct const_call_functor
    {
        void operator()(vmhook::return_value&, std::int32_t, std::int64_t) const {}
    };
    struct nonconst_call_functor
    {
        void operator()(vmhook::return_value&, std::int32_t, std::int64_t) {}
    };

    // Shapes the trait CLEANLY rejects (args_tuple_t absent, detector-safe):
    //  - overloaded operator()  -> &F::operator() ambiguous, 7498 dropped
    //  - templated operator()   -> &F::operator() ill-formed, 7498 dropped
    struct overloaded_call_functor
    {
        void operator()(vmhook::return_value&, std::int32_t) const {}
        void operator()(vmhook::return_value&, double) const {}
    };
    struct templated_call_functor
    {
        template<typename T>
        void operator()(vmhook::return_value&, T) const {}
    };

    // -------------------------------------------------------------------------
    // Representative-complete set of user-defined types, so the truth tables
    // below can probe each trait against every C++ type *category* (not just
    // the std:: types the existing checks use).  These cover the negative and
    // positive space for value_t_convertible_target_v, is_vector_v,
    // is_unique_ptr_v and is_unique_object_ptr in one place.
    // -------------------------------------------------------------------------
    enum unscoped_enum { unscoped_a, unscoped_b };           // unscoped enum
    enum class scoped_enum { sa, sb };                        // scoped enum
    enum class scoped_enum_u8 : std::uint8_t { x, y };        // fixed-underlying

    union plain_union { int i; float f; };                   // a union

    struct empty_struct {};                                  // empty class
    struct data_struct { int a; double b; };                 // non-empty class
    struct polymorphic_struct { virtual ~polymorphic_struct() = default; };
    struct abstract_struct { virtual void f() = 0; virtual ~abstract_struct() = default; };
    struct final_struct final { int v; };                    // final class

    // A wrapper that derives from object_base WITHOUT going through object<T>,
    // to prove is_unique_object_ptr keys on object_base, not on object<>.
    struct direct_object_base_derived : public vmhook::object_base
    {
        using vmhook::object_base::object_base;
    };

    // A class that is NOT related to object_base at all.
    struct unrelated_class { void* p; };

    // Member-object and member-function pointer types (pointer-category probes
    // that are NOT plain object pointers — value_t_convertible_target_v treats
    // a member pointer as "not a pointer" because std::is_pointer_v is false
    // for member pointers, so they pass the trait as a non-pointer target).
    using member_object_ptr_t   = int data_struct::*;
    using member_function_ptr_t = void (data_struct::*)();
    using free_function_ptr_t   = void (*)(int);

    // A small alias so the value_t truth-table rows read cleanly.
    template<typename T>
    inline constexpr bool conv_v = vmhook::detail::value_t_convertible_target_v<T>;
}

int main()
{
    // -------------------------------------------------------------------------
    // is_vector_v — std::vector recognition with cv-ref stripping
    // -------------------------------------------------------------------------
    check("is_vector_v_true_for_vector_int",
          vmhook::detail::is_vector_v<std::vector<int>>);
    check("is_vector_v_true_for_vector_string",
          vmhook::detail::is_vector_v<std::vector<std::string>>);
    check("is_vector_v_true_for_vector_unique_ptr_wrapper",
          vmhook::detail::is_vector_v<std::vector<std::unique_ptr<sample_wrapper>>>);
    check("is_vector_v_strips_const_ref",
          vmhook::detail::is_vector_v<const std::vector<int>&>);
    check("is_vector_v_strips_rvalue_ref",
          vmhook::detail::is_vector_v<std::vector<int>&&>);
    check("is_vector_v_false_for_int",
          !vmhook::detail::is_vector_v<int>);
    check("is_vector_v_false_for_unique_ptr",
          !vmhook::detail::is_vector_v<std::unique_ptr<int>>);
    check("is_vector_v_false_for_array",
          !vmhook::detail::is_vector_v<std::array<int, 4>>);
    check("is_vector_v_false_for_pointer",
          !vmhook::detail::is_vector_v<int*>);

    // is_vector<vector<X>>::value_type_t must be X, never bool (shadow guard).
    check("is_vector_value_type_t_int_not_bool",
          std::is_same_v<vector_value_t<std::vector<int>>, int>);
    check("is_vector_value_type_t_double",
          std::is_same_v<vector_value_t<std::vector<double>>, double>);
    check("is_vector_value_type_t_string",
          std::is_same_v<vector_value_t<std::vector<std::string>>, std::string>);
    check("is_vector_value_type_t_is_not_bool",
          !std::is_same_v<vector_value_t<std::vector<int>>, bool>);
    // The member type_t exists only on the specialisation, never on the
    // false_type primary (proves value_type_t is the shadow-safe spelling).
    check("is_vector_value_type_t_absent_on_non_vector",
          !has_vector_value_t<int>::value
          && has_vector_value_t<std::vector<int>>::value);

    // -------------------------------------------------------------------------
    // is_unique_ptr_v / value_type_t — the chat-not-sending regression cluster
    // -------------------------------------------------------------------------
    check("is_unique_ptr_v_true_for_unique_ptr_int",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>);
    check("is_unique_ptr_v_true_for_unique_ptr_wrapper",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<sample_wrapper>>);
    check("is_unique_ptr_v_strips_const_ref",
          vmhook::detail::is_unique_ptr_v<const std::unique_ptr<int>&>);
    check("is_unique_ptr_v_false_for_raw_pointer",
          !vmhook::detail::is_unique_ptr_v<int*>);
    check("is_unique_ptr_v_false_for_shared_ptr",
          !vmhook::detail::is_unique_ptr_v<std::shared_ptr<int>>);
    check("is_unique_ptr_v_false_for_vector",
          !vmhook::detail::is_unique_ptr_v<std::vector<int>>);

    // value_type_t MUST resolve to the pointee, NOT bool (the exact regression
    // that fed null IChatComponent into the JVM when value_type_t collapsed).
    check("is_unique_ptr_value_type_t_int_not_bool",
          std::is_same_v<unique_value_t<std::unique_ptr<int>>, int>);
    check("is_unique_ptr_value_type_t_string",
          std::is_same_v<unique_value_t<std::unique_ptr<std::string>>, std::string>);
    check("is_unique_ptr_value_type_t_object_base",
          std::is_same_v<unique_value_t<std::unique_ptr<vmhook::object_base>>, vmhook::object_base>);
    check("is_unique_ptr_value_type_t_is_not_bool",
          !std::is_same_v<unique_value_t<std::unique_ptr<int>>, bool>);
    // End-to-end: value_type_t of a user wrapper unique_ptr derives from
    // object_base, which is the predicate write_jni_arg_to_slot branches on.
    check("is_unique_ptr_value_type_t_wrapper_derives_object_base",
          std::is_base_of_v<vmhook::object_base, unique_value_t<std::unique_ptr<sample_wrapper>>>);

    // -------------------------------------------------------------------------
    // is_unique_object_ptr — sister trait (bool_constant base, same shadow risk)
    // (status=new in audit/findings/hook_arg_decoding_wrappers.md "## Tests")
    // -------------------------------------------------------------------------
    check("is_unique_object_ptr_true_for_unique_ptr_object_base",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<vmhook::object_base>>::value);
    check("is_unique_object_ptr_true_for_unique_ptr_wrapper",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<sample_wrapper>>::value);
    check("is_unique_object_ptr_false_for_unique_ptr_int",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value);
    check("is_unique_object_ptr_false_for_unique_ptr_string",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<std::string>>::value);
    check("is_unique_object_ptr_false_for_raw_int",
          !vmhook::detail::is_unique_object_ptr<int>::value);
    check("is_unique_object_ptr_false_for_raw_wrapper_pointer",
          !vmhook::detail::is_unique_object_ptr<sample_wrapper*>::value);

    // -------------------------------------------------------------------------
    // dependent_false_v — must be false for every T (it only fires when its
    // discarded if-constexpr branch is actually instantiated for a bad type).
    // -------------------------------------------------------------------------
    check("dependent_false_v_int_is_false",
          !vmhook::detail::dependent_false_v<int>);
    check("dependent_false_v_vector_is_false",
          !vmhook::detail::dependent_false_v<std::vector<int>>);
    check("dependent_false_v_wrapper_is_false",
          !vmhook::detail::dependent_false_v<sample_wrapper>);
    check("dependent_false_v_multi_arg_is_false",
          !vmhook::detail::dependent_false_v<int, double, std::string>);

    // -------------------------------------------------------------------------
    // function_traits — the hook<T> callback decomposition.  hook<T> takes the
    // user detour, reads args_tuple_t via function_traits, then tuple_tail strips
    // the leading vmhook::return_value& to produce method_arg_tuple_t.  Each
    // callable form (lambda, std::function, free function pointer, mutable
    // lambda) MUST decompose identically.
    // -------------------------------------------------------------------------

    // Lambda: (return_value&, unique_ptr<self>, int, long, int).
    {
        auto detour = [](vmhook::return_value&,
                         std::unique_ptr<sample_wrapper>,
                         std::int32_t, std::int64_t, std::int32_t) {};
        using full = all_args_of<decltype(detour)>;
        using method = method_args_of<decltype(detour)>;

        check("function_traits_lambda_full_arity_5",
              std::tuple_size_v<full> == 5);
        check("function_traits_lambda_first_arg_is_return_value_ref",
              std::is_same_v<std::tuple_element_t<0, full>, vmhook::return_value&>);
        check("tuple_tail_strips_return_value_arity_4",
              std::tuple_size_v<method> == 4);
        check("tuple_tail_first_method_arg_is_unique_ptr_self",
              std::is_same_v<std::tuple_element_t<0, method>, std::unique_ptr<sample_wrapper>>);
        check("tuple_tail_preserves_method_arg_order",
              std::is_same_v<std::tuple_element_t<1, method>, std::int32_t>
              && std::is_same_v<std::tuple_element_t<2, method>, std::int64_t>
              && std::is_same_v<std::tuple_element_t<3, method>, std::int32_t>);
    }

    // std::function form must decompose to the identical method-arg tuple.
    {
        using fn_t = std::function<void(vmhook::return_value&,
                                        std::unique_ptr<sample_wrapper>,
                                        std::int32_t, std::int64_t, std::int32_t)>;
        check("function_traits_std_function_matches_lambda",
              std::is_same_v<
                  method_args_of<fn_t>,
                  std::tuple<std::unique_ptr<sample_wrapper>, std::int32_t, std::int64_t, std::int32_t>>);
    }

    // Free function pointer form must decompose the same way.
    {
        check("function_traits_free_function_pointer_method_args",
              std::is_same_v<
                  method_args_of<decltype(&free_detour)>,
                  std::tuple<std::unique_ptr<sample_wrapper>, std::int32_t>>);
    }

    // A no-Java-arg detour: (return_value&) only -> empty method tuple.
    {
        auto void_detour = [](vmhook::return_value&) {};
        check("tuple_tail_empty_for_return_value_only_detour",
              std::tuple_size_v<method_args_of<decltype(void_detour)>> == 0);
    }

    // mutable lambda exercises the non-const operator() specialisation of
    // function_traits (the void_t operator() probe must still resolve).
    {
        auto mutable_detour = [x = 0](vmhook::return_value&, std::int32_t) mutable { ++x; };
        check("function_traits_mutable_lambda_decomposes",
              std::is_same_v<method_args_of<decltype(mutable_detour)>, std::tuple<std::int32_t>>);
    }

    // -------------------------------------------------------------------------
    // java_slot_offsets — fed by method_arg_tuple_t from the chain above.
    // HotSpot stores long/double in TWO adjacent interpreter slots; everything
    // else takes one.  These pin the J/D widening that the decomposition relies
    // on (a regression here silently misreads every arg after a long/double).
    // -------------------------------------------------------------------------
    check("is_java_double_slot_v_long_is_two",
          vmhook::detail::is_java_double_slot_v<std::int64_t>);
    check("is_java_double_slot_v_double_is_two",
          vmhook::detail::is_java_double_slot_v<double>);
    check("is_java_double_slot_v_int_is_one",
          !vmhook::detail::is_java_double_slot_v<std::int32_t>);
    check("is_java_double_slot_v_float_is_one",
          !vmhook::detail::is_java_double_slot_v<float>);
    check("is_java_double_slot_v_pointer_is_one",
          !vmhook::detail::is_java_double_slot_v<void*>);

    check("java_slot_offsets_empty_tuple",
          vmhook::detail::java_slot_offsets<std::tuple<>>::value.size() == 0);
    check("java_slot_offsets_three_ints_identity",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int32_t, std::int32_t, std::int32_t>>::value
           == std::array<std::int32_t, 3>{ 0, 1, 2 }));
    // (this, long, int): the classic regression — trailing int must be slot 3,
    // NOT slot 2 (the high half of the long).  This is the exact tuple shape the
    // decomposition above produces for an instance method with a long arg.
    check("java_slot_offsets_self_long_int_widens",
          (vmhook::detail::java_slot_offsets<std::tuple<void*, std::int64_t, std::int32_t>>::value
           == std::array<std::int32_t, 3>{ 0, 1, 3 }));
    check("java_slot_offsets_double_int_double_widens",
          (vmhook::detail::java_slot_offsets<std::tuple<double, std::int32_t, double>>::value
           == std::array<std::int32_t, 3>{ 0, 2, 3 }));

    // -------------------------------------------------------------------------
    // extract_frame_arg unsupported-type rejection (concept-based negative test;
    // status=new in audit/findings/hook_arg_decoding_primitives.md "## Tests").
    // A wrapper-arg unique_ptr<T> is only meaningful when T derives from
    // object_base, and a std::vector<int> is not representable in one local slot.
    // We can't trigger the static_assert without a JVM frame, but we CAN assert
    // the trait predicates the static_assert dispatch is built on hold here.
    // -------------------------------------------------------------------------
    check("vector_arg_is_neither_unique_ptr_nor_pointer",
          !vmhook::detail::is_unique_ptr_v<std::vector<int>>
          && !std::is_pointer_v<std::vector<int>>);
    check("oversized_pod_is_not_a_single_slot_primitive",
          sizeof(std::array<std::int64_t, 4>) > sizeof(void*)
          && !vmhook::detail::is_unique_ptr_v<std::array<std::int64_t, 4>>);
    check("unique_ptr_of_non_object_base_is_not_object_ptr",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>
          && !vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value);

    // -------------------------------------------------------------------------
    // Pure-logic round trip of a wrapper constructed from a fake (non-heap) OOP.
    // No JVM is touched: object_base just stores the pointer and hands it back,
    // which is what the unique_ptr<T> decode branch ultimately wraps.  This pins
    // the value_type_t -> object_base -> get_instance() chain end to end.
    // -------------------------------------------------------------------------
    {
        void* const fake_oop{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEF)) };
        std::unique_ptr<unique_value_t<std::unique_ptr<sample_wrapper>>> w{
            std::make_unique<sample_wrapper>(fake_oop) };
        check("wrapper_constructed_via_value_type_t_round_trips_instance",
              w != nullptr && w->get_instance() == fake_oop);
        check("default_constructed_wrapper_has_null_instance",
              sample_wrapper{}.get_instance() == nullptr);
    }

    // -------------------------------------------------------------------------
    // Compile-time enforcement (these never reach runtime if they regress —
    // the build breaks first, which is the strongest guarantee).
    // -------------------------------------------------------------------------
    static_assert(std::is_same_v<unique_value_t<std::unique_ptr<int>>, int>,
                  "is_unique_ptr value_type_t must be int, not bool (shadow regression)");
    static_assert(std::is_same_v<vector_value_t<std::vector<int>>, int>,
                  "is_vector value_type_t must be int, not bool (shadow regression)");
    static_assert(std::is_same_v<
                      method_args_of<decltype(&free_detour)>,
                      std::tuple<std::unique_ptr<sample_wrapper>, std::int32_t>>,
                  "function_traits + tuple_tail must strip return_value& and keep Java args");
    static_assert(!vmhook::detail::dependent_false_v<int>,
                  "dependent_false_v must be false");

    // -------------------------------------------------------------------------
    // is_java_double_slot_v — uint64_t is ALSO a two-slot type (vmhook.hpp:7514),
    // alongside int64_t and double.  The existing checks cover int64/double/int/
    // float/void*; pin uint64_t and the cv-ref stripping the trait performs.
    // -------------------------------------------------------------------------
    check("is_java_double_slot_v_uint64_is_two",
          vmhook::detail::is_java_double_slot_v<std::uint64_t>);
    check("is_java_double_slot_v_strips_const_ref_on_long",
          vmhook::detail::is_java_double_slot_v<const std::int64_t&>);
    check("is_java_double_slot_v_strips_ref_on_double",
          vmhook::detail::is_java_double_slot_v<double&>);
    check("is_java_double_slot_v_strips_const_on_uint64",
          vmhook::detail::is_java_double_slot_v<const std::uint64_t>);
    // Narrow / unsigned-narrow / pointer / bool are all single-slot.
    check("is_java_double_slot_v_uint32_is_one",
          !vmhook::detail::is_java_double_slot_v<std::uint32_t>);
    check("is_java_double_slot_v_int16_is_one",
          !vmhook::detail::is_java_double_slot_v<std::int16_t>);
    check("is_java_double_slot_v_bool_is_one",
          !vmhook::detail::is_java_double_slot_v<bool>);
    check("is_java_double_slot_v_int8_is_one",
          !vmhook::detail::is_java_double_slot_v<std::int8_t>);

    // -------------------------------------------------------------------------
    // java_slot_offsets — more tuple shapes.  Every expected array is computed by
    // hand from the rule "each long/double/uint64 advances the cursor by 2, every
    // other type by 1" (vmhook.hpp:7542-7553).
    // -------------------------------------------------------------------------
    // Single double / single long / single uint64: one entry, always slot 0.
    check("java_slot_offsets_single_double",
          (vmhook::detail::java_slot_offsets<std::tuple<double>>::value
           == std::array<std::int32_t, 1>{ 0 }));
    check("java_slot_offsets_single_long",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int64_t>>::value
           == std::array<std::int32_t, 1>{ 0 }));
    check("java_slot_offsets_single_uint64",
          (vmhook::detail::java_slot_offsets<std::tuple<std::uint64_t>>::value
           == std::array<std::int32_t, 1>{ 0 }));
    // Two consecutive longs: 0, then 2 (first long spans 0-1).
    check("java_slot_offsets_two_longs",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int64_t, std::int64_t>>::value
           == std::array<std::int32_t, 2>{ 0, 2 }));
    // long, long, int -> 0, 2, 4.
    check("java_slot_offsets_long_long_int",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int64_t, std::int64_t, std::int32_t>>::value
           == std::array<std::int32_t, 3>{ 0, 2, 4 }));
    // double, double -> 0, 2.
    check("java_slot_offsets_double_double",
          (vmhook::detail::java_slot_offsets<std::tuple<double, double>>::value
           == std::array<std::int32_t, 2>{ 0, 2 }));
    // int, double, int, long, int -> 0, 1, 3, 4, 6.
    check("java_slot_offsets_int_double_int_long_int",
          (vmhook::detail::java_slot_offsets<
               std::tuple<std::int32_t, double, std::int32_t, std::int64_t, std::int32_t>>::value
           == std::array<std::int32_t, 5>{ 0, 1, 3, 4, 6 }));
    // uint64 counts as a double slot too: (uint64, int) -> 0, 2.
    check("java_slot_offsets_uint64_int_widens",
          (vmhook::detail::java_slot_offsets<std::tuple<std::uint64_t, std::int32_t>>::value
           == std::array<std::int32_t, 2>{ 0, 2 }));
    // A long at the very END still only consumes its own two slots; nothing
    // follows it, so the table is just the leading offsets.
    check("java_slot_offsets_int_int_long",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int32_t, std::int32_t, std::int64_t>>::value
           == std::array<std::int32_t, 3>{ 0, 1, 2 }));
    // Pointer + float + bool are all single-slot: identity offsets.
    check("java_slot_offsets_ptr_float_bool_identity",
          (vmhook::detail::java_slot_offsets<std::tuple<void*, float, bool>>::value
           == std::array<std::int32_t, 3>{ 0, 1, 2 }));
    // The computed array size always equals the tuple arity (one entry per arg,
    // regardless of how many slots each arg spans).
    check("java_slot_offsets_size_equals_arity",
          vmhook::detail::java_slot_offsets<
              std::tuple<std::int64_t, double, std::int32_t>>::value.size() == 3);

    // -------------------------------------------------------------------------
    // is_vector_v / value_type_t — more element types and qualifier stripping.
    // -------------------------------------------------------------------------
    check("is_vector_v_true_for_vector_bool",
          vmhook::detail::is_vector_v<std::vector<bool>>);
    check("is_vector_v_true_for_vector_of_vector",
          vmhook::detail::is_vector_v<std::vector<std::vector<int>>>);
    check("is_vector_v_strips_volatile_ref",
          vmhook::detail::is_vector_v<volatile std::vector<int>&>);
    check("is_vector_v_false_for_shared_ptr",
          !vmhook::detail::is_vector_v<std::shared_ptr<std::vector<int>>>);
    check("is_vector_v_false_for_tuple",
          !vmhook::detail::is_vector_v<std::tuple<int, int>>);
    check("is_vector_value_type_t_pointer",
          std::is_same_v<vector_value_t<std::vector<int*>>, int*>);
    check("is_vector_value_type_t_nested_vector",
          std::is_same_v<vector_value_t<std::vector<std::vector<double>>>, std::vector<double>>);
    check("is_vector_value_type_t_wrapper_unique_ptr",
          std::is_same_v<vector_value_t<std::vector<std::unique_ptr<sample_wrapper>>>,
                         std::unique_ptr<sample_wrapper>>);

    // -------------------------------------------------------------------------
    // is_unique_ptr_v / value_type_t — more pointee types and qualifier stripping.
    // -------------------------------------------------------------------------
    check("is_unique_ptr_v_true_for_unique_ptr_vector",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<std::vector<int>>>);
    check("is_unique_ptr_v_strips_rvalue_ref",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>&&>);
    check("is_unique_ptr_v_strips_volatile_const_ref",
          vmhook::detail::is_unique_ptr_v<const volatile std::unique_ptr<int>&>);
    check("is_unique_ptr_v_false_for_object_base_value",
          !vmhook::detail::is_unique_ptr_v<vmhook::object_base*>);
    check("is_unique_ptr_value_type_t_double",
          std::is_same_v<unique_value_t<std::unique_ptr<double>>, double>);
    check("is_unique_ptr_value_type_t_vector",
          std::is_same_v<unique_value_t<std::unique_ptr<std::vector<int>>>, std::vector<int>>);
    check("is_unique_ptr_value_type_t_void_ptr_pointee",
          std::is_same_v<unique_value_t<std::unique_ptr<void*>>, void*>);

    // -------------------------------------------------------------------------
    // is_unique_object_ptr — bool_constant base, more wrapper / non-wrapper cases.
    // -------------------------------------------------------------------------
    check("is_unique_object_ptr_false_for_unique_ptr_vector",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<std::vector<int>>>::value);
    check("is_unique_object_ptr_false_for_unique_ptr_double",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<double>>::value);
    check("is_unique_object_ptr_false_for_shared_ptr_wrapper",
          !vmhook::detail::is_unique_object_ptr<std::shared_ptr<sample_wrapper>>::value);
    check("is_unique_object_ptr_false_for_object_base_value",
          !vmhook::detail::is_unique_object_ptr<vmhook::object_base>::value);
    // The predicate is exactly is_base_of<object_base, pointee>: object_base
    // itself qualifies (is_base_of is reflexive).
    check("is_unique_object_ptr_true_for_unique_ptr_object_base_self",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<vmhook::object_base>>::value);

    // -------------------------------------------------------------------------
    // dependent_false_v — false for more arities and qualifier mixes.
    // -------------------------------------------------------------------------
    check("dependent_false_v_zero_args_is_false",
          !vmhook::detail::dependent_false_v<>);
    check("dependent_false_v_pointer_is_false",
          !vmhook::detail::dependent_false_v<int*>);
    check("dependent_false_v_unique_ptr_is_false",
          !vmhook::detail::dependent_false_v<std::unique_ptr<sample_wrapper>>);
    check("dependent_false_v_five_args_is_false",
          !vmhook::detail::dependent_false_v<int, long, double, std::string, void*>);

    // -------------------------------------------------------------------------
    // function_traits + tuple_tail — more callable shapes and arg lists.
    // -------------------------------------------------------------------------
    // A long-arg-list lambda: (return_value&, self, long, double, int, long).
    {
        auto detour = [](vmhook::return_value&,
                         std::unique_ptr<sample_wrapper>,
                         std::int64_t, double, std::int32_t, std::int64_t) {};
        using method = method_args_of<decltype(detour)>;
        check("tuple_tail_six_arg_lambda_arity_5",
              std::tuple_size_v<method> == 5);
        check("tuple_tail_six_arg_lambda_order_preserved",
              std::is_same_v<method,
                  std::tuple<std::unique_ptr<sample_wrapper>, std::int64_t, double,
                             std::int32_t, std::int64_t>>);
    }
    // The method-arg tuple feeds java_slot_offsets directly; verify the chain end
    // to end for an instance method (self, long, int, double): the trailing
    // double must land past the long's two slots.
    {
        auto detour = [](vmhook::return_value&,
                         std::unique_ptr<sample_wrapper>,
                         std::int64_t, std::int32_t, double) {};
        using method = method_args_of<decltype(detour)>;
        // method tuple = (unique_ptr<self>, long, int, double).
        // slots: self@0(+1), long@1(+2), int@3(+1), double@4(+2) -> [0,1,3,4].
        check("function_traits_to_slot_offsets_self_long_int_double",
              (vmhook::detail::java_slot_offsets<method>::value
               == std::array<std::int32_t, 4>{ 0, 1, 3, 4 }));
    }
    // std::function with an object (non-unique_ptr) arg list.
    {
        using fn_t = std::function<void(vmhook::return_value&, std::int32_t, double)>;
        check("function_traits_std_function_int_double",
              std::is_same_v<method_args_of<fn_t>, std::tuple<std::int32_t, double>>);
    }
    // A const-operator() functor (non-mutable lambda is already const; pin an
    // explicit struct with a const call operator to exercise that specialisation).
    {
        struct const_functor
        {
            void operator()(vmhook::return_value&, std::int32_t) const {}
        };
        check("function_traits_const_functor_decomposes",
              std::is_same_v<method_args_of<const_functor>, std::tuple<std::int32_t>>);
    }
    // tuple_tail directly on a hand-built tuple drops exactly the first element.
    check("tuple_tail_drops_first_element_only",
          std::is_same_v<
              typename vmhook::detail::tuple_tail<
                  std::tuple<vmhook::return_value&, int, double, void*>>::type_t,
              std::tuple<int, double, void*>>);
    // tuple_tail of a single-element tuple yields the empty tuple.
    check("tuple_tail_single_element_yields_empty",
          std::is_same_v<
              typename vmhook::detail::tuple_tail<std::tuple<vmhook::return_value&>>::type_t,
              std::tuple<>>);

    // -------------------------------------------------------------------------
    // Compile-time enforcement of the new uint64 slot fact and a fresh slot
    // table, so a regression breaks the build before it reaches runtime.
    // -------------------------------------------------------------------------
    static_assert(vmhook::detail::is_java_double_slot_v<std::uint64_t>,
                  "uint64_t must occupy two Java slots, like int64_t");
    static_assert(vmhook::detail::java_slot_offsets<
                      std::tuple<std::int64_t, std::int64_t, std::int32_t>>::value
                      == std::array<std::int32_t, 3>{ 0, 2, 4 },
                  "two leading longs must push the trailing int to slot 4");

    // =========================================================================
    // WAVE: exhaustive callable-shape coverage for function_traits.
    //
    // hook<T>() deduces the Java parameter list by instantiating
    //   function_traits<remove_cvref_t<Detour>>::args_tuple_t   (vmhook.hpp:8211-8212)
    // then tuple_tail to drop the leading vmhook::return_value&.  The five
    // function_traits specialisations (vmhook.hpp:7485-7513) are the ENTIRE set
    // of callable shapes the typed hook path accepts.  This block walks every
    // shape — accepted, cleanly-rejected, and (documented) build-breaking — so
    // the contract is pinned for all three CI STLs.  Pure type algebra: no JVM,
    // no interpreter frame, every fact is constexpr / static_assert-checkable.
    //
    // NOTE on the trait's surface: function_traits exposes ONLY args_tuple_t.
    // There is deliberately NO return_type_t / result_type member (verified
    // against the header), so these tests assert the argument decomposition
    // only — the return type a detour declares is never read by this chain
    // (the detour's return is delivered separately via vmhook::return_value).
    // =========================================================================

    // --- Accepted shapes: args_tuple_t present -------------------------------
    check("has_args_tuple_free_function_pointer",
          has_args_tuple<decltype(&free_detour)>::value);
    check("has_args_tuple_std_function",
          has_args_tuple<std::function<void(vmhook::return_value&, std::int32_t)>>::value);
    check("has_args_tuple_const_call_functor",
          has_args_tuple<const_call_functor>::value);
    check("has_args_tuple_nonconst_call_functor",
          has_args_tuple<nonconst_call_functor>::value);
    {
        auto plain_lambda = [](vmhook::return_value&, std::int32_t) {};
        auto mutable_lambda = [x = 0](vmhook::return_value&, std::int32_t) mutable { ++x; };
        check("has_args_tuple_plain_lambda",
              has_args_tuple<decltype(plain_lambda)>::value);
        check("has_args_tuple_mutable_lambda",
              has_args_tuple<decltype(mutable_lambda)>::value);
    }

    // --- Cleanly-rejected shapes: args_tuple_t ABSENT (detector-safe) --------
    // A free-function POINTER is accepted, but a bare free-function TYPE
    // (R(args...), no pointer) matches none of the five specialisations.
    check("has_args_tuple_absent_for_free_function_type",
          !has_args_tuple<void(vmhook::return_value&, std::int32_t)>::value);
    // A NOEXCEPT free-function pointer is now a SUPPORTED shape: function_traits
    // has a dedicated R(*)(args...) noexcept specialisation (noexcept is part of
    // the function type since C++17), so its args_tuple_t is present and matches
    // the throwing twin.  (Previously the detectable face of the noexcept gap;
    // now closed for plain/const noexcept — ref-qualified/volatile remain gaps.)
    check("has_args_tuple_present_for_noexcept_free_function_pointer",
          has_args_tuple<void(*)(vmhook::return_value&, std::int32_t) noexcept>::value);
    // Overloaded / templated / generic operator() leave &F::operator()
    // ambiguous or ill-formed, so the functor spec (7498) is dropped and F
    // falls through to the undefined primary -> args_tuple_t absent.  This is
    // the "single concrete operator() required" contract (library flaw #3).
    check("has_args_tuple_absent_for_overloaded_operator",
          !has_args_tuple<overloaded_call_functor>::value);
    check("has_args_tuple_absent_for_templated_operator",
          !has_args_tuple<templated_call_functor>::value);
    {
        auto generic_lambda = [](vmhook::return_value&, auto) {};
        check("has_args_tuple_absent_for_generic_lambda",
              !has_args_tuple<decltype(generic_lambda)>::value);
    }
    // A plain non-callable type obviously has no args_tuple_t.
    check("has_args_tuple_absent_for_int",
          !has_args_tuple<int>::value);
    check("has_args_tuple_absent_for_unique_ptr",
          !has_args_tuple<std::unique_ptr<sample_wrapper>>::value);

    // --- args_tuple_t arity / element types across shapes --------------------
    // Zero Java args beyond the leading return_value& on every accepted shape.
    {
        auto rv_only = [](vmhook::return_value&) {};
        check("full_arity_1_for_return_value_only_lambda",
              std::tuple_size_v<all_args_of<decltype(rv_only)>> == 1);
        check("full_first_arg_is_return_value_ref_rv_only",
              std::is_same_v<std::tuple_element_t<0, all_args_of<decltype(rv_only)>>,
                             vmhook::return_value&>);
    }
    // const_call_functor: (return_value&, int, long) -> full arity 3.
    check("const_call_functor_full_arity_3",
          std::tuple_size_v<all_args_of<const_call_functor>> == 3);
    check("const_call_functor_method_args_int_long",
          std::is_same_v<method_args_of<const_call_functor>,
                         std::tuple<std::int32_t, std::int64_t>>);
    // non-const operator() decomposes identically to the const one.
    check("nonconst_call_functor_matches_const",
          std::is_same_v<method_args_of<nonconst_call_functor>,
                         method_args_of<const_call_functor>>);
    // A pointer-to-free-function with five params strips to four method args.
    check("free_detour_wide_method_arity_4",
          std::tuple_size_v<method_args_of<decltype(&free_detour_wide)>> == 4);
    check("free_detour_wide_method_args_exact",
          std::is_same_v<method_args_of<decltype(&free_detour_wide)>,
                         std::tuple<std::unique_ptr<sample_wrapper>, std::int64_t,
                                    std::int32_t, double>>);

    // --- All accepted shapes converge on ONE method tuple --------------------
    // The four spellings of the SAME logical detour (lambda, std::function,
    // free-fn pointer, explicit functor) must yield byte-identical method
    // tuples — this is the property hook<T>() relies on to be call-syntax
    // agnostic.  Signature: (return_value&, self, int, long).
    {
        using expected = std::tuple<std::unique_ptr<sample_wrapper>, std::int32_t, std::int64_t>;
        auto lam = [](vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                      std::int32_t, std::int64_t) {};
        using fn_t = std::function<void(vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                                        std::int32_t, std::int64_t)>;
        struct functor
        {
            void operator()(vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                            std::int32_t, std::int64_t) const {}
        };
        check("convergence_lambda_method_tuple",
              std::is_same_v<method_args_of<decltype(lam)>, expected>);
        check("convergence_std_function_method_tuple",
              std::is_same_v<method_args_of<fn_t>, expected>);
        check("convergence_functor_method_tuple",
              std::is_same_v<method_args_of<functor>, expected>);
        check("convergence_all_four_shapes_identical",
              std::is_same_v<method_args_of<decltype(lam)>, method_args_of<fn_t>>
              && std::is_same_v<method_args_of<fn_t>, method_args_of<functor>>);
    }

    // --- cv / ref parameter spelling (library flaw #2 contract) --------------
    // function_traits preserves each argument's cv/ref qualifiers VERBATIM in
    // args_tuple_t: a by-value param and a const-ref param are DIFFERENT tuple
    // element types and therefore different method tuples.  Downstream this is
    // papered over because is_java_double_slot_v (remove_cvref internally) and
    // extract_frame_arg (returns remove_cvref_t) both normalise — so the two
    // spellings still read the same slots and yield the same C++ value type.
    // These checks pin BOTH halves of that contract.
    {
        auto by_val = [](vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                         std::int32_t, std::string) {};
        auto by_ref = [](vmhook::return_value&, const std::unique_ptr<sample_wrapper>&,
                         const std::int32_t&, const std::string&) {};
        using mv = method_args_of<decltype(by_val)>;
        using mr = method_args_of<decltype(by_ref)>;
        // (a) the trait preserves spelling: the element types differ exactly.
        check("cvref_by_value_element0_is_plain_unique_ptr",
              std::is_same_v<std::tuple_element_t<0, mv>, std::unique_ptr<sample_wrapper>>);
        check("cvref_by_ref_element0_is_const_ref_unique_ptr",
              std::is_same_v<std::tuple_element_t<0, mr>, const std::unique_ptr<sample_wrapper>&>);
        check("cvref_by_ref_element1_is_const_ref_int",
              std::is_same_v<std::tuple_element_t<1, mr>, const std::int32_t&>);
        check("cvref_by_ref_element2_is_const_ref_string",
              std::is_same_v<std::tuple_element_t<2, mr>, const std::string&>);
        check("cvref_value_and_ref_method_tuples_differ",
              !std::is_same_v<mv, mr>);
        // (b) is_java_double_slot_v collapses cv/ref, so the slot offset table
        //     is IDENTICAL for both spellings (here all single-slot -> identity).
        check("cvref_slot_offsets_identical_for_both_spellings",
              vmhook::detail::java_slot_offsets<mv>::value
              == vmhook::detail::java_slot_offsets<mr>::value);
        // (c) extract_frame_arg's return type strips cv/ref to the bare value
        //     type, so const-ref and by-value params decode to the same C++ type.
        //     (Type-level only: extract_frame_arg is never invoked here.)
        check("extract_frame_arg_collapses_const_ref_int_to_int",
              std::is_same_v<
                  decltype(vmhook::detail::extract_frame_arg<const std::int32_t&>(nullptr, 0)),
                  std::int32_t>);
        check("extract_frame_arg_collapses_const_ref_string_to_string",
              std::is_same_v<
                  decltype(vmhook::detail::extract_frame_arg<const std::string&>(nullptr, 0)),
                  std::string>);
        check("extract_frame_arg_collapses_const_ref_unique_ptr",
              std::is_same_v<
                  decltype(vmhook::detail::extract_frame_arg<const std::unique_ptr<sample_wrapper>&>(nullptr, 0)),
                  std::unique_ptr<sample_wrapper>>);
        check("extract_frame_arg_collapses_by_value_int_to_int",
              std::is_same_v<
                  decltype(vmhook::detail::extract_frame_arg<std::int32_t>(nullptr, 0)),
                  std::int32_t>);
    }
    // cv/ref on a long/double param must NOT defeat the 2-slot widening:
    // (self, const long&, int) still widens the trailing int to slot 3.
    {
        auto by_ref_long = [](vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                              const std::int64_t&, std::int32_t) {};
        using m = method_args_of<decltype(by_ref_long)>;
        check("cvref_const_ref_long_still_widens_two_slots",
              (vmhook::detail::java_slot_offsets<m>::value
               == std::array<std::int32_t, 3>{ 0, 1, 3 }));
    }

    // --- empty tuple_tail: zero-arg detour support (library flaw #1 FIXED) ----
    // tuple_tail<std::tuple<>> now has the empty-tuple base specialisation
    // (vmhook.hpp:7545-7549), so type_t IS present for the empty tuple and
    // resolves to std::tuple<> — exactly mirroring java_slot_offsets' own
    // empty-tuple case.  This is what lets a zero-argument-method observer
    // detour, [](vmhook::return_value&){}, decompose cleanly through hook<T>()
    // (the chain strips the lone return_value& to tuple<>, and any further
    // strip of that empty tuple no longer hits the undefined primary).  These
    // checks were previously the ABSENT pins for the flaw; they are flipped to
    // the PRESENT/identity form now that the fix is in.
    check("tuple_tail_type_t_present_for_empty_tuple",
          has_tuple_tail<std::tuple<>>::value);
    check("tuple_tail_empty_tuple_yields_empty_tuple",
          std::is_same_v<
              typename vmhook::detail::tuple_tail<std::tuple<>>::type_t,
              std::tuple<>>);
    check("tuple_tail_type_t_present_for_single_element",
          has_tuple_tail<std::tuple<vmhook::return_value&>>::value);
    check("tuple_tail_type_t_present_for_multi_element",
          has_tuple_tail<std::tuple<vmhook::return_value&, int, double>>::value);
    // The minimal SUPPORTED detour, (return_value&), decomposes through
    // tuple_tail<tuple<return_value&>> -> empty tuple WITHOUT touching the
    // undefined empty-tuple primary (the strip happens on a one-element tuple).
    check("minimal_detour_tail_is_empty_and_well_formed",
          has_tuple_tail<all_args_of<decltype([](vmhook::return_value&) {})>>::value
          && std::tuple_size_v<method_args_of<decltype([](vmhook::return_value&) {})>> == 0);
    // java_slot_offsets, by contrast, DOES define the empty-tuple case, so the
    // empty method tuple flows all the way to a zero-length offset table.
    check("empty_method_tuple_yields_empty_offset_table",
          vmhook::detail::java_slot_offsets<
              method_args_of<decltype([](vmhook::return_value&) {})>>::value.size() == 0);

    // --- self-less (static-method) shape -------------------------------------
    // A static Java method has no implicit `this`, so a detour for it omits the
    // leading unique_ptr<self>: (return_value&, int, long, int).  The method
    // tuple is exactly (int, long, int) with NO wrapper element, and widens the
    // trailing int to slot 3 — proving the decomposition adds no implicit self.
    {
        auto static_detour = [](vmhook::return_value&, std::int32_t,
                                std::int64_t, std::int32_t) {};
        using m = method_args_of<decltype(static_detour)>;
        check("static_shape_method_tuple_has_no_self_element",
              std::is_same_v<m, std::tuple<std::int32_t, std::int64_t, std::int32_t>>);
        check("static_shape_first_element_is_not_a_unique_ptr",
              !vmhook::detail::is_unique_ptr_v<std::tuple_element_t<0, m>>);
        check("static_shape_offsets_widen_trailing_int",
              (vmhook::detail::java_slot_offsets<m>::value
               == std::array<std::int32_t, 3>{ 0, 1, 3 }));
    }

    // --- many-arg fold past the small cases ----------------------------------
    // Eight method args mixing every single-slot primitive: identity offsets,
    // confirming java_slot_offsets::compute()'s fold accumulates correctly well
    // past the 3-4 element cases asserted elsewhere.
    {
        auto wide = [](vmhook::return_value&, bool, std::int8_t, std::int16_t,
                       char16_t, std::int32_t, float, void*, char) {};
        using m = method_args_of<decltype(wide)>;
        check("many_arg_single_slot_full_method_arity_8",
              std::tuple_size_v<m> == 8);
        check("many_arg_single_slot_identity_offsets",
              (vmhook::detail::java_slot_offsets<m>::value
               == std::array<std::int32_t, 8>{ 0, 1, 2, 3, 4, 5, 6, 7 }));
    }
    // Nine method args interleaving int and J/D types: the fold must thread the
    // running +2 widening across the whole list.
    {
        using m = std::tuple<std::int32_t, std::int64_t, std::int32_t, double,
                             std::int32_t, std::int64_t, std::int32_t, double, std::int32_t>;
        check("nine_arg_jd_interleave_offsets",
              (vmhook::detail::java_slot_offsets<m>::value
               == std::array<std::int32_t, 9>{ 0, 1, 3, 4, 6, 7, 9, 10, 12 }));
    }

    // --- all-widths offset matrix --------------------------------------------
    // Every single-slot primitive interleaved with both J/D types in one tuple:
    // bool@0(+1), long@1(+2), char16@3(+1), double@4(+2), i8@6(+1), int@7.
    check("all_widths_matrix_offsets",
          (vmhook::detail::java_slot_offsets<
               std::tuple<bool, std::int64_t, char16_t, double, std::int8_t, std::int32_t>>::value
           == std::array<std::int32_t, 6>{ 0, 1, 3, 4, 6, 7 }));
    // A tuple that BEGINS with a J/D: (long, int) -> 0, 2.
    check("offsets_leading_jd_long_int",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int64_t, std::int32_t>>::value
           == std::array<std::int32_t, 2>{ 0, 2 }));
    // A tuple that ENDS with a J/D: (int, long) -> 0, 1 (the trailing long's
    // second slot is consumed but never indexed, since nothing follows).
    check("offsets_trailing_jd_int_long",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int32_t, std::int64_t>>::value
           == std::array<std::int32_t, 2>{ 0, 1 }));
    // A tuple that is ALL J/D: (long, double, uint64) -> 0, 2, 4.
    check("offsets_all_jd_triple",
          (vmhook::detail::java_slot_offsets<
               std::tuple<std::int64_t, double, std::uint64_t>>::value
           == std::array<std::int32_t, 3>{ 0, 2, 4 }));
    // float is a SINGLE slot even though it sits beside doubles:
    // (float, double, float) -> 0, 1, 3.
    check("offsets_float_double_float_widens_only_double",
          (vmhook::detail::java_slot_offsets<std::tuple<float, double, float>>::value
           == std::array<std::int32_t, 3>{ 0, 1, 3 }));

    // --- tuple_element_t round-trip (the exact hook<T> instantiation) --------
    // hook<T>() reads each Java arg with
    //   extract_frame_arg<std::tuple_element_t<k, method_arg_tuple_t>>(...)
    // (vmhook.hpp:8316).  Assert that tuple_element_t<k, method_tuple> equals
    // the declared k-th Java parameter type for every k, for a representative
    // instance detour: (return_value&, self, int, long, double, string).
    {
        auto detour = [](vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                         std::int32_t, std::int64_t, double, std::string) {};
        using m = method_args_of<decltype(detour)>;
        check("tuple_element_roundtrip_arity_5",
              std::tuple_size_v<m> == 5);
        check("tuple_element_roundtrip_k0_self",
              std::is_same_v<std::tuple_element_t<0, m>, std::unique_ptr<sample_wrapper>>);
        check("tuple_element_roundtrip_k1_int",
              std::is_same_v<std::tuple_element_t<1, m>, std::int32_t>);
        check("tuple_element_roundtrip_k2_long",
              std::is_same_v<std::tuple_element_t<2, m>, std::int64_t>);
        check("tuple_element_roundtrip_k3_double",
              std::is_same_v<std::tuple_element_t<3, m>, double>);
        check("tuple_element_roundtrip_k4_string",
              std::is_same_v<std::tuple_element_t<4, m>, std::string>);
        // And the slot the k-th element is read from, end to end:
        // self@0, int@1, long@2(+2), double@4(+2), string@6.
        check("tuple_element_roundtrip_offsets",
              (vmhook::detail::java_slot_offsets<m>::value
               == std::array<std::int32_t, 5>{ 0, 1, 2, 4, 6 }));
    }

    // --- element-type identity is by TYPE, not position ----------------------
    // Two different wrapper types in one detour must each survive as their own
    // distinct unique_ptr element — proving tuple_tail/element extraction keys
    // on the declared type, never collapsing wrappers together.
    {
        auto detour = [](vmhook::return_value&, std::unique_ptr<sample_wrapper>,
                         std::unique_ptr<other_wrapper>) {};
        using m = method_args_of<decltype(detour)>;
        check("distinct_wrapper_elements_preserved",
              std::is_same_v<m, std::tuple<std::unique_ptr<sample_wrapper>,
                                           std::unique_ptr<other_wrapper>>>);
        check("distinct_wrapper_elements_are_not_equal",
              !std::is_same_v<std::tuple_element_t<0, m>, std::tuple_element_t<1, m>>);
    }

    // --- remove_cvref_t on the callable itself (hook<T> uses it) -------------
    // hook<T>() applies remove_cvref_t to decltype(user_detour) before feeding
    // function_traits (vmhook.hpp:8211).  A const / ref-qualified lambda value
    // category therefore decomposes identically to the bare closure type.
    {
        auto lam = [](vmhook::return_value&, std::int32_t, std::int64_t) {};
        using bare = std::remove_cvref_t<decltype(lam)>;
        check("remove_cvref_callable_const_lvalue_matches_bare",
              std::is_same_v<
                  method_args_of<const decltype(lam)&>,
                  method_args_of<bare>>);
        check("remove_cvref_callable_rvalue_matches_bare",
              std::is_same_v<
                  method_args_of<decltype(lam)&&>,
                  method_args_of<bare>>);
    }

    // --- tuple_tail direct edge cases ----------------------------------------
    // Stripping a long single-element-after-first tuple keeps the remainder
    // intact and ordered.
    check("tuple_tail_keeps_long_remainder_ordered",
          std::is_same_v<
              typename vmhook::detail::tuple_tail<
                  std::tuple<vmhook::return_value&, std::int64_t, double,
                             std::int32_t, void*, std::string>>::type_t,
              std::tuple<std::int64_t, double, std::int32_t, void*, std::string>>);
    // tuple_tail does NOT inspect the first element's type — it drops whatever
    // is first.  Here the first element is a plain int (not return_value&), and
    // it is still the one removed.  This documents that the "must be
    // return_value& first" property is a hook<T> authoring contract, NOT
    // something tuple_tail enforces (library flaw #5).
    check("tuple_tail_drops_first_regardless_of_type",
          std::is_same_v<
              typename vmhook::detail::tuple_tail<
                  std::tuple<int, double, void*>>::type_t,
              std::tuple<double, void*>>);

    // --- compile-time enforcement (build breaks before runtime on regress) ---
    static_assert(has_args_tuple<decltype(&free_detour)>::value,
                  "function_traits must accept a free-function pointer detour");
    static_assert(!has_args_tuple<void(vmhook::return_value&, std::int32_t)>::value,
                  "function_traits must NOT match a bare function TYPE (only the pointer)");
    static_assert(!has_args_tuple<overloaded_call_functor>::value,
                  "function_traits requires a single non-overloaded operator() "
                  "(overloaded call operator must decompose to absent args_tuple_t)");
    static_assert(!has_args_tuple<templated_call_functor>::value,
                  "function_traits requires a non-template operator() "
                  "(templated/generic call operator must decompose to absent args_tuple_t)");
    static_assert(has_tuple_tail<std::tuple<>>::value,
                  "tuple_tail<std::tuple<>> must expose type_t (flaw #1 fix: empty-tuple "
                  "base specialisation lets a zero-arg detour decompose cleanly)");
    static_assert(std::is_same_v<typename vmhook::detail::tuple_tail<std::tuple<>>::type_t,
                                 std::tuple<>>,
                  "tuple_tail<std::tuple<>>::type_t must be std::tuple<>");
    // A (vmhook::return_value&)-only detour — the shape used to observe a
    // zero-argument Java method — must deduce an EMPTY Java-arg tuple through
    // the exact function_traits -> tuple_tail chain hook<T>() instantiates.
    static_assert(std::is_same_v<method_args_of<decltype([](vmhook::return_value&) {})>,
                                 std::tuple<>>,
                  "return_value&-only detour must decompose to an empty method-arg tuple "
                  "(zero-arg-method observe-only path, flaw #1 fix)");
    static_assert(
        std::is_same_v<
            method_args_of<const_call_functor>,
            std::tuple<std::int32_t, std::int64_t>>,
        "const operator() functor must strip return_value& and keep (int, long)");
    static_assert(
        vmhook::detail::java_slot_offsets<
            std::tuple<bool, std::int64_t, char16_t, double, std::int8_t, std::int32_t>>::value
            == std::array<std::int32_t, 6>{ 0, 1, 3, 4, 6, 7 },
        "all-widths matrix: each long/double advances the cursor by 2, others by 1");
    static_assert(
        std::is_same_v<
            decltype(vmhook::detail::extract_frame_arg<const std::int32_t&>(nullptr, 0)),
            std::int32_t>,
        "extract_frame_arg must strip cv/ref from its result type");

    // =========================================================================
    // WAVE: exhaustive truth table for value_t_convertible_target_v.
    //
    // This trait (vmhook.hpp:1822-1842) constrains field_proxy::value_t's and
    // method_proxy::value_t's UNCONSTRAINED `template<typename T> operator T()`
    // to its legitimate target set, removing the spurious productions
    // (const char*, char*, W*, nullptr_t) that make a class-target static_cast
    // ambiguous on MSVC /permissive-.  The rule, read straight off the header:
    //
    //   clean = std::remove_cvref_t<T>
    //     clean == std::nullptr_t           -> false
    //     std::is_pointer_v<clean>          -> std::is_void_v<remove_pointer_t<clean>>
    //                                          (i.e. true ONLY for void* of any cv)
    //     otherwise                         -> true
    //
    // It was previously UNTESTED in this suite.  The tables below walk every
    // C++ type category through it: the positive set (arithmetic / void* / class
    // / enum / union / member-pointer / std types / wrappers) and the negative
    // set (nullptr_t and every non-void object/function pointer), plus the
    // cv/ref-stripping the trait performs first.  Every row is a constexpr bool,
    // and the load-bearing ones are also pinned with static_assert below.
    // =========================================================================

    // --- nullptr_t: the one scalar that is rejected -------------------------
    check("value_t_conv_false_for_nullptr_t",
          !conv_v<std::nullptr_t>);
    check("value_t_conv_false_for_const_nullptr_t",
          !conv_v<const std::nullptr_t>);
    check("value_t_conv_false_for_nullptr_t_ref",
          !conv_v<std::nullptr_t&>);
    check("value_t_conv_false_for_const_nullptr_t_ref",
          !conv_v<const std::nullptr_t&>);
    check("value_t_conv_false_for_nullptr_t_rvalue_ref",
          !conv_v<std::nullptr_t&&>);

    // --- void* (any cv) is the SINGLE legitimate pointer target -------------
    check("value_t_conv_true_for_void_ptr",
          conv_v<void*>);
    check("value_t_conv_true_for_const_void_ptr",
          conv_v<const void*>);
    check("value_t_conv_true_for_volatile_void_ptr",
          conv_v<volatile void*>);
    check("value_t_conv_true_for_const_volatile_void_ptr",
          conv_v<const volatile void*>);
    // pointer-TO-void with an outer cv/ref: stripped to the pointer, still void*.
    check("value_t_conv_true_for_void_ptr_const_qualified_pointer",
          conv_v<void* const>);
    check("value_t_conv_true_for_void_ptr_lvalue_ref",
          conv_v<void*&>);
    check("value_t_conv_true_for_void_ptr_const_ref",
          conv_v<void* const&>);
    check("value_t_conv_true_for_void_ptr_rvalue_ref",
          conv_v<void*&&>);
    // oop_type_t is exactly void*, the decode target this trait must admit.
    check("value_t_conv_true_for_oop_type_t",
          conv_v<vmhook::oop_type_t>);

    // --- every NON-void object pointer is rejected --------------------------
    check("value_t_conv_false_for_char_ptr",
          !conv_v<char*>);
    check("value_t_conv_false_for_const_char_ptr",
          !conv_v<const char*>);
    check("value_t_conv_false_for_int_ptr",
          !conv_v<int*>);
    check("value_t_conv_false_for_const_int_ptr",
          !conv_v<const int*>);
    check("value_t_conv_false_for_double_ptr",
          !conv_v<double*>);
    check("value_t_conv_false_for_wrapper_ptr",
          !conv_v<sample_wrapper*>);
    check("value_t_conv_false_for_object_base_ptr",
          !conv_v<vmhook::object_base*>);
    check("value_t_conv_false_for_void_ptr_ptr",
          !conv_v<void**>);                       // void** is a ptr-to-(non-void)
    check("value_t_conv_false_for_const_char_ptr_ptr",
          !conv_v<const char**>);
    // a non-void pointer wearing outer cv/ref is still rejected after stripping.
    check("value_t_conv_false_for_const_char_ptr_const_ref",
          !conv_v<const char* const&>);
    check("value_t_conv_false_for_int_ptr_rvalue_ref",
          !conv_v<int*&&>);

    // --- function pointers are pointers, but NOT object pointers; is_pointer_v
    //     IS true for a free-function pointer and its pointee is not void, so
    //     the trait REJECTS them.  (A member-function pointer, by contrast, is
    //     NOT is_pointer_v, so it is accepted as a non-pointer target.)
    check("value_t_conv_false_for_free_function_pointer",
          !conv_v<free_function_ptr_t>);
    check("value_t_conv_false_for_void_noarg_function_pointer",
          !conv_v<void(*)()>);
    check("value_t_conv_true_for_member_function_pointer",
          conv_v<member_function_ptr_t>);
    check("value_t_conv_true_for_member_object_pointer",
          conv_v<member_object_ptr_t>);

    // --- arithmetic targets all pass (the numeric value_t productions) ------
    check("value_t_conv_true_for_bool",          conv_v<bool>);
    check("value_t_conv_true_for_char",          conv_v<char>);
    check("value_t_conv_true_for_signed_char",   conv_v<signed char>);
    check("value_t_conv_true_for_unsigned_char", conv_v<unsigned char>);
    check("value_t_conv_true_for_char8",         conv_v<char8_t>);
    check("value_t_conv_true_for_char16",        conv_v<char16_t>);
    check("value_t_conv_true_for_char32",        conv_v<char32_t>);
    check("value_t_conv_true_for_wchar",         conv_v<wchar_t>);
    check("value_t_conv_true_for_short",         conv_v<short>);
    check("value_t_conv_true_for_unsigned_short",conv_v<unsigned short>);
    check("value_t_conv_true_for_int",           conv_v<int>);
    check("value_t_conv_true_for_unsigned_int",  conv_v<unsigned int>);
    check("value_t_conv_true_for_long",          conv_v<long>);
    check("value_t_conv_true_for_unsigned_long", conv_v<unsigned long>);
    check("value_t_conv_true_for_long_long",     conv_v<long long>);
    check("value_t_conv_true_for_ulong_long",    conv_v<unsigned long long>);
    check("value_t_conv_true_for_float",         conv_v<float>);
    check("value_t_conv_true_for_double",        conv_v<double>);
    check("value_t_conv_true_for_long_double",   conv_v<long double>);
    // fixed-width aliases (whatever they map to, all arithmetic -> all pass).
    check("value_t_conv_true_for_int8",   conv_v<std::int8_t>);
    check("value_t_conv_true_for_uint8",  conv_v<std::uint8_t>);
    check("value_t_conv_true_for_int16",  conv_v<std::int16_t>);
    check("value_t_conv_true_for_uint16", conv_v<std::uint16_t>);
    check("value_t_conv_true_for_int32",  conv_v<std::int32_t>);
    check("value_t_conv_true_for_uint32", conv_v<std::uint32_t>);
    check("value_t_conv_true_for_int64",  conv_v<std::int64_t>);
    check("value_t_conv_true_for_uint64", conv_v<std::uint64_t>);
    // arithmetic with cv/ref stripped first: const int& -> int -> pass.
    check("value_t_conv_true_for_const_int_ref",   conv_v<const int&>);
    check("value_t_conv_true_for_volatile_double",  conv_v<volatile double>);
    check("value_t_conv_true_for_int_rvalue_ref",   conv_v<int&&>);

    // --- enums / unions / classes are non-pointer non-nullptr -> all pass ---
    check("value_t_conv_true_for_unscoped_enum",   conv_v<unscoped_enum>);
    check("value_t_conv_true_for_scoped_enum",     conv_v<scoped_enum>);
    check("value_t_conv_true_for_scoped_enum_u8",  conv_v<scoped_enum_u8>);
    check("value_t_conv_true_for_union",           conv_v<plain_union>);
    check("value_t_conv_true_for_empty_struct",    conv_v<empty_struct>);
    check("value_t_conv_true_for_data_struct",     conv_v<data_struct>);
    check("value_t_conv_true_for_polymorphic",     conv_v<polymorphic_struct>);
    check("value_t_conv_true_for_final_struct",    conv_v<final_struct>);
    // abstract type as a TARGET: the trait is purely a classification predicate
    // (it never constructs the type), so an abstract class still classifies true.
    check("value_t_conv_true_for_abstract_struct", conv_v<abstract_struct>);
    check("value_t_conv_true_for_enum_const_ref",  conv_v<const scoped_enum&>);

    // --- the legitimate std:: value_t targets all pass ----------------------
    check("value_t_conv_true_for_string",          conv_v<std::string>);
    check("value_t_conv_true_for_string_view",     conv_v<std::string_view>);
    check("value_t_conv_true_for_const_ref_string", conv_v<const std::string&>);
    check("value_t_conv_true_for_vector_int",      conv_v<std::vector<int>>);
    check("value_t_conv_true_for_vector_string",   conv_v<std::vector<std::string>>);
    check("value_t_conv_true_for_unique_ptr_wrapper",
          conv_v<std::unique_ptr<sample_wrapper>>);
    check("value_t_conv_true_for_unique_ptr_rvalue_ref",
          conv_v<std::unique_ptr<sample_wrapper>&&>);
    check("value_t_conv_true_for_optional_int",    conv_v<std::optional<int>>);
    check("value_t_conv_true_for_pair",            conv_v<std::pair<int, double>>);
    check("value_t_conv_true_for_tuple",           conv_v<std::tuple<int, double>>);
    // wrapper types (object<T> and object_base) as targets are classes -> pass.
    check("value_t_conv_true_for_object_base_value", conv_v<vmhook::object_base>);
    check("value_t_conv_true_for_wrapper_value",     conv_v<sample_wrapper>);

    // --- cv/ref-stripping symmetry: every qualifier spelling of one void* and
    //     one rejected pointer classifies the SAME as its bare form ----------
    check("value_t_conv_void_ptr_all_cvref_spellings_agree",
          conv_v<void*> == conv_v<const void*>
          && conv_v<void*> == conv_v<void* const&>
          && conv_v<void*> == conv_v<void*&&>);
    check("value_t_conv_char_ptr_all_cvref_spellings_agree",
          conv_v<char*> == conv_v<const char*>
          && conv_v<char*> == conv_v<char* const&>
          && conv_v<char*> == conv_v<char*&&>);

    // =========================================================================
    // WAVE: is_java_double_slot_v over EVERY fundamental type.
    //
    // The trait (vmhook.hpp:7511-7515) is true for EXACTLY remove_cvref_t<T> in
    // { std::int64_t, std::uint64_t, double } and false for everything else.
    // The existing checks cover a handful; this walks the entire fundamental
    // type list so a regression that (e.g.) accidentally widened `long` on an
    // LP64 build or `float`/`long double` anywhere is caught.  All sizeof-
    // sensitive cases are reasoned about explicitly, never hard-coded.
    // =========================================================================
    // The three true types (and only these three at the std:: alias level).
    check("jds_true_int64",  vmhook::detail::is_java_double_slot_v<std::int64_t>);
    check("jds_true_uint64", vmhook::detail::is_java_double_slot_v<std::uint64_t>);
    check("jds_true_double", vmhook::detail::is_java_double_slot_v<double>);
    // void / bool / nullptr_t.
    check("jds_false_void_ptr", !vmhook::detail::is_java_double_slot_v<void*>);
    check("jds_false_bool",     !vmhook::detail::is_java_double_slot_v<bool>);
    check("jds_false_nullptr_t",!vmhook::detail::is_java_double_slot_v<std::nullptr_t>);
    // every character type is single-slot.
    check("jds_false_char",     !vmhook::detail::is_java_double_slot_v<char>);
    check("jds_false_schar",    !vmhook::detail::is_java_double_slot_v<signed char>);
    check("jds_false_uchar",    !vmhook::detail::is_java_double_slot_v<unsigned char>);
    check("jds_false_char8",    !vmhook::detail::is_java_double_slot_v<char8_t>);
    check("jds_false_char16",   !vmhook::detail::is_java_double_slot_v<char16_t>);
    check("jds_false_char32",   !vmhook::detail::is_java_double_slot_v<char32_t>);
    check("jds_false_wchar",    !vmhook::detail::is_java_double_slot_v<wchar_t>);
    // short / int are single-slot regardless of platform.
    check("jds_false_short",    !vmhook::detail::is_java_double_slot_v<short>);
    check("jds_false_ushort",   !vmhook::detail::is_java_double_slot_v<unsigned short>);
    check("jds_false_int",      !vmhook::detail::is_java_double_slot_v<int>);
    check("jds_false_uint",     !vmhook::detail::is_java_double_slot_v<unsigned int>);
    // `long`/`unsigned long`: the trait keys on std::int64_t/std::uint64_t by
    // TYPE IDENTITY, not on width.  This is subtle across data models:
    //   - Linux LP64:    std::int64_t IS `long`        -> trait true
    //   - Windows LLP64:  `long` is 32-bit, != int64_t  -> trait false
    //   - macOS LP64:     `long` is 64-bit BUT std::int64_t is `long long`,
    //                     so `long` != std::int64_t      -> trait false (despite
    //                     sizeof(long)==8!)
    // Hence derive the expectation from the int64_t/uint64_t identity, NEVER
    // from sizeof (which is wrong on macOS).  `long` is signed so it can only
    // match int64_t; `unsigned long` only uint64_t.
    check("jds_long_matches_int64_identity",
          vmhook::detail::is_java_double_slot_v<long>
              == std::is_same_v<long, std::int64_t>);
    check("jds_ulong_matches_uint64_identity",
          vmhook::detail::is_java_double_slot_v<unsigned long>
              == std::is_same_v<unsigned long, std::uint64_t>);
    // `long long` is 64-bit everywhere we target, but it is a DISTINCT type from
    // std::int64_t on some platforms; the trait keys on std::int64_t exactly.
    // Whether long long == std::int64_t is implementation-defined, so derive the
    // expectation from that identity rather than asserting a fixed answer.
    check("jds_long_long_matches_int64_identity",
          vmhook::detail::is_java_double_slot_v<long long>
              == std::is_same_v<long long, std::int64_t>);
    check("jds_ulong_long_matches_uint64_identity",
          vmhook::detail::is_java_double_slot_v<unsigned long long>
              == std::is_same_v<unsigned long long, std::uint64_t>);
    // float is single-slot; long double is single-slot (it is NOT `double`).
    check("jds_false_float",       !vmhook::detail::is_java_double_slot_v<float>);
    check("jds_false_long_double", !vmhook::detail::is_java_double_slot_v<long double>);
    // fixed-width narrow aliases are all single-slot.
    check("jds_false_int8",   !vmhook::detail::is_java_double_slot_v<std::int8_t>);
    check("jds_false_uint8",  !vmhook::detail::is_java_double_slot_v<std::uint8_t>);
    check("jds_false_int16",  !vmhook::detail::is_java_double_slot_v<std::int16_t>);
    check("jds_false_uint16", !vmhook::detail::is_java_double_slot_v<std::uint16_t>);
    check("jds_false_int32",  !vmhook::detail::is_java_double_slot_v<std::int32_t>);
    check("jds_false_uint32", !vmhook::detail::is_java_double_slot_v<std::uint32_t>);
    // enums / pointers / class types are all single-slot (none are the 3 types).
    check("jds_false_unscoped_enum", !vmhook::detail::is_java_double_slot_v<unscoped_enum>);
    check("jds_false_scoped_enum",   !vmhook::detail::is_java_double_slot_v<scoped_enum>);
    check("jds_false_data_struct",   !vmhook::detail::is_java_double_slot_v<data_struct>);
    check("jds_false_wrapper",       !vmhook::detail::is_java_double_slot_v<sample_wrapper>);
    // cv/ref stripping: every qualifier spelling of int64/double agrees with bare.
    check("jds_int64_all_cvref_spellings_true",
          vmhook::detail::is_java_double_slot_v<const std::int64_t>
          && vmhook::detail::is_java_double_slot_v<volatile std::int64_t>
          && vmhook::detail::is_java_double_slot_v<const volatile std::int64_t&>
          && vmhook::detail::is_java_double_slot_v<std::int64_t&&>);
    check("jds_double_all_cvref_spellings_true",
          vmhook::detail::is_java_double_slot_v<const double>
          && vmhook::detail::is_java_double_slot_v<volatile double&>
          && vmhook::detail::is_java_double_slot_v<double&&>);
    check("jds_int_all_cvref_spellings_false",
          !vmhook::detail::is_java_double_slot_v<const int>
          && !vmhook::detail::is_java_double_slot_v<volatile int&>
          && !vmhook::detail::is_java_double_slot_v<int&&>);

    // =========================================================================
    // WAVE: is_vector_v negative space + element-type matrix.
    //
    // is_vector_v (vmhook.hpp:1745-1746) is is_vector<remove_cvref_t<T>>::value;
    // is_vector<X> is true ONLY for std::vector<E, A>.  Walk the full category
    // grid so any accidental broadening (to array / span / other containers) or
    // narrowing (losing a custom-allocator vector) is caught.
    // =========================================================================
    // Positive: vectors of every category of element are still vectors.
    check("is_vector_true_for_vector_bool",          vmhook::detail::is_vector_v<std::vector<bool>>);
    check("is_vector_true_for_vector_char",          vmhook::detail::is_vector_v<std::vector<char>>);
    check("is_vector_true_for_vector_double",         vmhook::detail::is_vector_v<std::vector<double>>);
    check("is_vector_true_for_vector_void_ptr",       vmhook::detail::is_vector_v<std::vector<void*>>);
    check("is_vector_true_for_vector_enum",           vmhook::detail::is_vector_v<std::vector<scoped_enum>>);
    check("is_vector_true_for_vector_pair",           vmhook::detail::is_vector_v<std::vector<std::pair<int, int>>>);
    check("is_vector_true_for_vector_wrapper_ptr",    vmhook::detail::is_vector_v<std::vector<sample_wrapper*>>);
    // Negative: every other type category is not a vector.
    check("is_vector_false_for_void",        !vmhook::detail::is_vector_v<void>);
    check("is_vector_false_for_bool",        !vmhook::detail::is_vector_v<bool>);
    check("is_vector_false_for_double",      !vmhook::detail::is_vector_v<double>);
    check("is_vector_false_for_nullptr_t",   !vmhook::detail::is_vector_v<std::nullptr_t>);
    check("is_vector_false_for_void_ptr",    !vmhook::detail::is_vector_v<void*>);
    check("is_vector_false_for_enum",        !vmhook::detail::is_vector_v<scoped_enum>);
    check("is_vector_false_for_union",       !vmhook::detail::is_vector_v<plain_union>);
    check("is_vector_false_for_empty_struct",!vmhook::detail::is_vector_v<empty_struct>);
    check("is_vector_false_for_string",      !vmhook::detail::is_vector_v<std::string>);
    check("is_vector_false_for_string_view", !vmhook::detail::is_vector_v<std::string_view>);
    check("is_vector_false_for_array",       !vmhook::detail::is_vector_v<std::array<int, 3>>);
    check("is_vector_false_for_optional_vector",
          !vmhook::detail::is_vector_v<std::optional<std::vector<int>>>);
    check("is_vector_false_for_pair_of_vectors",
          !vmhook::detail::is_vector_v<std::pair<std::vector<int>, std::vector<int>>>);
    check("is_vector_false_for_c_array",     !vmhook::detail::is_vector_v<int[4]>);
    check("is_vector_false_for_function_ptr", !vmhook::detail::is_vector_v<free_function_ptr_t>);
    check("is_vector_false_for_member_ptr",  !vmhook::detail::is_vector_v<member_object_ptr_t>);
    check("is_vector_false_for_wrapper",     !vmhook::detail::is_vector_v<sample_wrapper>);
    // A vector with a custom (but std-conforming) allocator is still a vector,
    // proving the trait matches the two-parameter std::vector<E, A> template.
    check("is_vector_true_for_custom_allocator_vector",
          vmhook::detail::is_vector_v<std::vector<int, std::allocator<int>>>);
    // value_type_t round-trips the element for several element categories.
    check("is_vector_value_type_t_void_ptr",
          std::is_same_v<vector_value_t<std::vector<void*>>, void*>);
    check("is_vector_value_type_t_enum",
          std::is_same_v<vector_value_t<std::vector<scoped_enum>>, scoped_enum>);
    check("is_vector_value_type_t_bool",
          std::is_same_v<vector_value_t<std::vector<bool>>, bool>);   // genuinely bool here
    check("is_vector_value_type_t_data_struct",
          std::is_same_v<vector_value_t<std::vector<data_struct>>, data_struct>);

    // =========================================================================
    // WAVE: is_unique_ptr_v / is_unique_object_ptr full category grids.
    //
    // is_unique_ptr_v (vmhook.hpp:1783-1784) is true ONLY for std::unique_ptr;
    // is_unique_object_ptr (vmhook.hpp:9416-9426) refines that to a unique_ptr
    // whose pointee derives from object_base (is_base_of, hence reflexive).  The
    // two are walked together so the relationship (object-ptr implies unique-ptr,
    // never the converse) is pinned across every pointee category.
    // =========================================================================
    // is_unique_ptr_v positive: pointee category does not matter.
    check("is_unique_ptr_true_for_unique_ptr_void",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<void, void(*)(void*)>>);
    check("is_unique_ptr_true_for_unique_ptr_enum",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<scoped_enum>>);
    check("is_unique_ptr_true_for_unique_ptr_array",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<int[]>>);
    check("is_unique_ptr_true_for_unique_ptr_object_base",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<vmhook::object_base>>);
    // is_unique_ptr_v negative: everything that is not a unique_ptr.
    check("is_unique_ptr_false_for_void",        !vmhook::detail::is_unique_ptr_v<void>);
    check("is_unique_ptr_false_for_nullptr_t",   !vmhook::detail::is_unique_ptr_v<std::nullptr_t>);
    check("is_unique_ptr_false_for_void_ptr",    !vmhook::detail::is_unique_ptr_v<void*>);
    check("is_unique_ptr_false_for_wrapper_ptr", !vmhook::detail::is_unique_ptr_v<sample_wrapper*>);
    check("is_unique_ptr_false_for_shared_ptr_wrapper",
          !vmhook::detail::is_unique_ptr_v<std::shared_ptr<sample_wrapper>>);
    check("is_unique_ptr_false_for_weak_ptr",    !vmhook::detail::is_unique_ptr_v<std::weak_ptr<int>>);
    check("is_unique_ptr_false_for_optional_unique_ptr",
          !vmhook::detail::is_unique_ptr_v<std::optional<std::unique_ptr<int>>>);
    check("is_unique_ptr_false_for_vector_unique_ptr",
          !vmhook::detail::is_unique_ptr_v<std::vector<std::unique_ptr<int>>>);
    check("is_unique_ptr_false_for_enum",        !vmhook::detail::is_unique_ptr_v<scoped_enum>);
    check("is_unique_ptr_false_for_wrapper",     !vmhook::detail::is_unique_ptr_v<sample_wrapper>);
    // value_type_t round-trips the pointee for several categories.
    check("is_unique_ptr_value_type_t_enum",
          std::is_same_v<unique_value_t<std::unique_ptr<scoped_enum>>, scoped_enum>);
    check("is_unique_ptr_value_type_t_data_struct",
          std::is_same_v<unique_value_t<std::unique_ptr<data_struct>>, data_struct>);
    check("is_unique_ptr_value_type_t_other_wrapper",
          std::is_same_v<unique_value_t<std::unique_ptr<other_wrapper>>, other_wrapper>);

    // is_unique_object_ptr: true iff unique_ptr whose pointee : object_base.
    check("is_uop_true_for_unique_ptr_direct_object_base_derived",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<direct_object_base_derived>>::value);
    check("is_uop_true_for_unique_ptr_other_wrapper",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<other_wrapper>>::value);
    check("is_uop_true_for_unique_ptr_object_base_reflexive",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<vmhook::object_base>>::value);
    // Negative: non-object pointees, and non-unique_ptr shapes entirely.
    check("is_uop_false_for_unique_ptr_unrelated_class",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<unrelated_class>>::value);
    check("is_uop_false_for_unique_ptr_enum",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<scoped_enum>>::value);
    check("is_uop_false_for_unique_ptr_void_deleter",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value);
    check("is_uop_false_for_shared_ptr_object_base",
          !vmhook::detail::is_unique_object_ptr<std::shared_ptr<vmhook::object_base>>::value);
    check("is_uop_false_for_raw_object_base_ptr",
          !vmhook::detail::is_unique_object_ptr<vmhook::object_base*>::value);
    check("is_uop_false_for_object_base_value",
          !vmhook::detail::is_unique_object_ptr<vmhook::object_base>::value);
    check("is_uop_false_for_void",
          !vmhook::detail::is_unique_object_ptr<void>::value);
    // The refinement relationship: object-ptr implies unique-ptr for every
    // wrapper pointee, but unique-ptr does NOT imply object-ptr for a non-wrapper.
    check("is_uop_implies_is_unique_ptr_for_wrapper",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<sample_wrapper>>::value
          && vmhook::detail::is_unique_ptr_v<std::unique_ptr<sample_wrapper>>);
    check("is_unique_ptr_does_not_imply_is_uop_for_int",
          vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>
          && !vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value);

    // =========================================================================
    // WAVE: dependent_false_v over many arities and type categories.
    // It is constexpr false for ANY pack (vmhook.hpp:1718-1719).  Enumerate a
    // representative spread so a regression that made it non-false for some
    // pack shape is caught at runtime AND compile time.
    // =========================================================================
    check("dependent_false_v_void",        !vmhook::detail::dependent_false_v<void>);
    check("dependent_false_v_nullptr_t",    !vmhook::detail::dependent_false_v<std::nullptr_t>);
    check("dependent_false_v_void_ptr",     !vmhook::detail::dependent_false_v<void*>);
    check("dependent_false_v_enum",         !vmhook::detail::dependent_false_v<scoped_enum>);
    check("dependent_false_v_union",        !vmhook::detail::dependent_false_v<plain_union>);
    check("dependent_false_v_ref",          !vmhook::detail::dependent_false_v<int&>);
    check("dependent_false_v_array",        !vmhook::detail::dependent_false_v<int[4]>);
    check("dependent_false_v_function_ptr", !vmhook::detail::dependent_false_v<free_function_ptr_t>);
    check("dependent_false_v_object_base",  !vmhook::detail::dependent_false_v<vmhook::object_base>);
    check("dependent_false_v_seven_args",
          !vmhook::detail::dependent_false_v<int, long, double, std::string,
                                             void*, scoped_enum, std::vector<int>>);

    // =========================================================================
    // WAVE: extract_frame_arg result type = remove_cvref_t<value_type>, walked
    // across every category (vmhook.hpp:9264-9268).  The function is NEVER
    // invoked (nullptr frame, type-level only via decltype on an unevaluated
    // call), so this is a pure compile-time identity over the cv/ref lattice.
    // =========================================================================
    {
        // arithmetic: all cv/ref spellings collapse to the bare arithmetic type.
        check("efa_int_identity",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<int>(nullptr, 0)), int>);
        check("efa_const_int_to_int",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<const int>(nullptr, 0)), int>);
        check("efa_volatile_int_to_int",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<volatile int>(nullptr, 0)), int>);
        check("efa_int_ref_to_int",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<int&>(nullptr, 0)), int>);
        check("efa_int_rvalue_ref_to_int",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<int&&>(nullptr, 0)), int>);
        check("efa_cv_ref_int_to_int",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<const volatile int&>(nullptr, 0)), int>);
        check("efa_long_identity",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<std::int64_t>(nullptr, 0)), std::int64_t>);
        check("efa_double_const_ref_to_double",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<const double&>(nullptr, 0)), double>);
        // pointer: cv on the pointer is stripped; the POINTEE cv is preserved
        // (remove_cvref strips top-level only).  const void* stays const void*.
        check("efa_void_ptr_identity",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<void*>(nullptr, 0)), void*>);
        check("efa_const_void_ptr_preserves_pointee_const",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<const void*>(nullptr, 0)), const void*>);
        check("efa_void_ptr_const_ref_to_void_ptr",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<void* const&>(nullptr, 0)), void*>);
        // std::string and unique_ptr targets (the OOP-decoding arms).
        check("efa_string_const_ref_to_string",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<const std::string&>(nullptr, 0)), std::string>);
        check("efa_unique_ptr_wrapper_const_ref",
              std::is_same_v<
                  decltype(vmhook::detail::extract_frame_arg<const std::unique_ptr<sample_wrapper>&>(nullptr, 0)),
                  std::unique_ptr<sample_wrapper>>);
        // enum target keeps its identity after stripping.
        check("efa_enum_const_to_enum",
              std::is_same_v<decltype(vmhook::detail::extract_frame_arg<const scoped_enum>(nullptr, 0)), scoped_enum>);
        // by-value and const-ref of the SAME underlying type decode to one C++
        // type — the property that lets a detour spell a param either way.
        check("efa_byvalue_and_constref_int_agree",
              std::is_same_v<
                  decltype(vmhook::detail::extract_frame_arg<int>(nullptr, 0)),
                  decltype(vmhook::detail::extract_frame_arg<const int&>(nullptr, 0))>);
    }

    // =========================================================================
    // WAVE: java_slot_offsets edge widths, sizeof-reasoned, no hard-coded sizes.
    //
    // The fold (vmhook.hpp:7542-7553) advances the cursor by 2 for any arg with
    // is_java_double_slot_v, else by 1.  Because is_java_double_slot_v keys on
    // std::int64_t/std::uint64_t/double (NOT on `long`), the offset table for a
    // tuple containing `long` is platform-dependent: compute the EXPECTED table
    // from is_java_double_slot_v itself so the assertion holds on LP64 and LLP64
    // alike.  The fixed-width tuples elsewhere in this file are unaffected.
    // =========================================================================
    {
        // (long, int): if `long` is a double-slot here, the int lands at slot 2;
        // otherwise at slot 1.  Derive the expectation from the trait, not a
        // literal — this is the platform-correct way to pin a `long`-bearing row.
        constexpr std::int32_t long_width{
            vmhook::detail::is_java_double_slot_v<long> ? 2 : 1 };
        const std::array<std::int32_t, 2> expected_long_int{ 0, long_width };
        check("java_slot_offsets_long_int_platform_correct",
              vmhook::detail::java_slot_offsets<std::tuple<long, std::int32_t>>::value
                  == expected_long_int);

        // (unsigned long, int): same reasoning via the unsigned-long width.
        constexpr std::int32_t ulong_width{
            vmhook::detail::is_java_double_slot_v<unsigned long> ? 2 : 1 };
        const std::array<std::int32_t, 2> expected_ulong_int{ 0, ulong_width };
        check("java_slot_offsets_ulong_int_platform_correct",
              vmhook::detail::java_slot_offsets<std::tuple<unsigned long, std::int32_t>>::value
                  == expected_ulong_int);
    }
    // A single-element tuple for each of the three two-slot types: the lone entry
    // is always slot 0 (nothing precedes it), independent of its width.
    check("java_slot_offsets_single_int64_is_zero",
          (vmhook::detail::java_slot_offsets<std::tuple<std::int64_t>>::value
           == std::array<std::int32_t, 1>{ 0 }));
    check("java_slot_offsets_single_uint64_is_zero",
          (vmhook::detail::java_slot_offsets<std::tuple<std::uint64_t>>::value
           == std::array<std::int32_t, 1>{ 0 }));
    // long double is NOT a double-slot (single slot): (long double, int) -> 0, 1.
    check("java_slot_offsets_long_double_is_single_slot",
          (vmhook::detail::java_slot_offsets<std::tuple<long double, std::int32_t>>::value
           == std::array<std::int32_t, 2>{ 0, 1 }));
    // enum / pointer / wrapper-unique_ptr args are single-slot; interleave them
    // with a double to prove only the double widens.
    check("java_slot_offsets_enum_double_enum",
          (vmhook::detail::java_slot_offsets<std::tuple<scoped_enum, double, scoped_enum>>::value
           == std::array<std::int32_t, 3>{ 0, 1, 3 }));
    check("java_slot_offsets_uniqueptr_long_int",
          (vmhook::detail::java_slot_offsets<
               std::tuple<std::unique_ptr<sample_wrapper>, std::int64_t, std::int32_t>>::value
           == std::array<std::int32_t, 3>{ 0, 1, 3 }));

    // =========================================================================
    // WAVE: tuple_tail across element categories + the has_tuple_tail detector.
    //
    // tuple_tail drops the first element (vmhook.hpp:7527-7531) and has an
    // empty-tuple base (7545-7549); type_t exists for any std::tuple<...> and is
    // absent for any non-tuple.  Walk both: many element-type tails, and the
    // detector over a spread of non-tuple categories.
    // =========================================================================
    // type_t present ONLY for std::tuple specialisations.
    check("has_tuple_tail_false_for_int",        !has_tuple_tail<int>::value);
    check("has_tuple_tail_false_for_void",       !has_tuple_tail<void>::value);
    check("has_tuple_tail_false_for_void_ptr",   !has_tuple_tail<void*>::value);
    check("has_tuple_tail_false_for_pair",       !has_tuple_tail<std::pair<int, int>>::value);
    check("has_tuple_tail_false_for_array",      !has_tuple_tail<std::array<int, 2>>::value);
    check("has_tuple_tail_false_for_vector",     !has_tuple_tail<std::vector<int>>::value);
    check("has_tuple_tail_true_for_empty_tuple", has_tuple_tail<std::tuple<>>::value);
    check("has_tuple_tail_true_for_singleton",   has_tuple_tail<std::tuple<int>>::value);
    check("has_tuple_tail_true_for_many",        has_tuple_tail<std::tuple<int, double, void*>>::value);
    // tails preserve order and the exact (cv/ref-qualified) element spellings.
    check("tuple_tail_singleton_to_empty",
          std::is_same_v<typename vmhook::detail::tuple_tail<std::tuple<int>>::type_t, std::tuple<>>);
    check("tuple_tail_preserves_cvref_elements",
          std::is_same_v<
              typename vmhook::detail::tuple_tail<
                  std::tuple<void*, const int&, std::string&&, scoped_enum>>::type_t,
              std::tuple<const int&, std::string&&, scoped_enum>>);
    check("tuple_tail_drops_only_first_of_two_wrappers",
          std::is_same_v<
              typename vmhook::detail::tuple_tail<
                  std::tuple<std::unique_ptr<sample_wrapper>, std::unique_ptr<other_wrapper>>>::type_t,
              std::tuple<std::unique_ptr<other_wrapper>>>);

    // -------------------------------------------------------------------------
    // Compile-time enforcement for the new waves (build breaks before runtime
    // on regression — the strongest guarantee).  One representative load-bearing
    // fact per wave; platform-sensitive facts are expressed as derived
    // identities so they hold on every target.
    // -------------------------------------------------------------------------
    // value_t_convertible_target_v: the exact contract (nullptr & non-void ptr
    // rejected; void* and every non-pointer non-nullptr accepted).
    static_assert(!vmhook::detail::value_t_convertible_target_v<std::nullptr_t>,
                  "value_t target must reject std::nullptr_t");
    static_assert(!vmhook::detail::value_t_convertible_target_v<const char*>,
                  "value_t target must reject const char* (the MSVC-ambiguity production)");
    static_assert(!vmhook::detail::value_t_convertible_target_v<sample_wrapper*>,
                  "value_t target must reject a raw wrapper pointer W*");
    static_assert(vmhook::detail::value_t_convertible_target_v<void*>,
                  "value_t target must accept void* (the single legitimate pointer)");
    static_assert(vmhook::detail::value_t_convertible_target_v<const void*>,
                  "value_t target must accept const void* (cv-stripped to void*)");
    static_assert(vmhook::detail::value_t_convertible_target_v<std::string>,
                  "value_t target must accept std::string");
    static_assert(vmhook::detail::value_t_convertible_target_v<std::unique_ptr<sample_wrapper>>,
                  "value_t target must accept std::unique_ptr<W>");
    static_assert(vmhook::detail::value_t_convertible_target_v<int>,
                  "value_t target must accept arithmetic types");
    static_assert(!vmhook::detail::value_t_convertible_target_v<void(*)()>,
                  "value_t target must reject a free-function pointer (non-void pointer)");
    static_assert(vmhook::detail::value_t_convertible_target_v<int data_struct::*>,
                  "value_t target must accept a member pointer (not is_pointer_v)");
    // is_java_double_slot_v: the three accepted std:: aliases, and the
    // platform-derived `long` identity.
    static_assert(vmhook::detail::is_java_double_slot_v<std::int64_t>
                      && vmhook::detail::is_java_double_slot_v<std::uint64_t>
                      && vmhook::detail::is_java_double_slot_v<double>,
                  "int64_t / uint64_t / double are the three two-slot types");
    static_assert(!vmhook::detail::is_java_double_slot_v<float>
                      && !vmhook::detail::is_java_double_slot_v<long double>
                      && !vmhook::detail::is_java_double_slot_v<int>,
                  "float / long double / int are all single-slot");
    static_assert(vmhook::detail::is_java_double_slot_v<long>
                      == std::is_same_v<long, std::int64_t>,
                  "is_java_double_slot_v<long> keys on std::int64_t identity, not width "
                  "(macOS LP64: long is 64-bit yet != int64_t, which is long long)");
    // is_vector / is_unique_ptr / is_unique_object_ptr key facts.
    static_assert(vmhook::detail::is_vector_v<std::vector<int, std::allocator<int>>>,
                  "is_vector_v must match std::vector<E, A> with an explicit allocator");
    static_assert(!vmhook::detail::is_vector_v<std::array<int, 3>>,
                  "is_vector_v must NOT match std::array");
    static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<vmhook::object_base>>::value,
                  "is_unique_object_ptr is reflexive on object_base itself");
    static_assert(!vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value
                      && vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>,
                  "unique_ptr<int> is a unique_ptr but NOT an object-ptr");
    // extract_frame_arg result-type stripping (top-level cv/ref only).
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::extract_frame_arg<const void*>(nullptr, 0)),
                      const void*>,
                  "extract_frame_arg strips TOP-LEVEL cv/ref only; const void* keeps pointee const");
    // tuple_tail element-spelling preservation and the non-tuple absence.
    static_assert(std::is_same_v<
                      typename vmhook::detail::tuple_tail<std::tuple<void*, const int&>>::type_t,
                      std::tuple<const int&>>,
                  "tuple_tail preserves the surviving elements' cv/ref spelling verbatim");
    static_assert(!has_tuple_tail<std::pair<int, int>>::value,
                  "tuple_tail::type_t must be absent for a non-tuple (std::pair)");

    std::printf("vmhook traits-extra: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
