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
#include <tuple>
#include <type_traits>

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

    std::printf("vmhook traits-extra: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
