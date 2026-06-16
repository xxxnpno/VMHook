// Compile-time-only test for the header's type traits.  Every static_assert
// here proves a property of the API surface the rest of the library relies
// on; the executable just succeeds when produced.
//
// SCOPE / non-duplication: the function_traits -> tuple_tail -> java_slot_offsets
// decomposition chain, the value_type-shadow guards on is_unique_ptr / is_vector /
// is_unique_object_ptr, the dependent_false_v helper, the exhaustive
// java_slot_offsets truth tables, and extract_frame_arg cv/ref collapsing are all
// covered exhaustively in tests/test_traits_extra.cpp (the `traits_extra` target)
// and tests/test_traits_function_traits.cpp; value_t_convertible_target_v lives in
// test_field_proxy_value_conversions.cpp / test_unified_call_syntax.cpp, the
// signature_for_arg<T> VALUE table in test_helpers.cpp, and the global_ref
// type-surface in test_global_ref.cpp.  This file therefore does NOT re-assert
// those.  It instead drives EXHAUSTIVE compile-time truth tables — over a
// representative-complete type zoo (fundamentals incl. void / nullptr_t / the
// char family, pointers incl. function / member / void*, lvalue/rvalue refs,
// bounded/unbounded arrays, cv-qualified, scoped/unscoped enums, unions, classes
// empty/poly/abstract/final, std types, and the library's own wrapper/oop types)
// — for the remaining trait/predicate machinery (is_java_double_slot_v,
// is_vector_v / is_unique_ptr_v / is_unique_object_ptr negative space,
// dependent_false_v) AND pins the TYPE-LEVEL contract of the public API surface
// the rest of the library is built on: oop_t / oop_type_t, object_base, object<D>,
// return_value, the vmhook::jni:: forwarders, and the register_class / hook /
// make_unique entry-point signatures.  Every fact is derived from the live header
// and asserted with static_assert (the strongest guarantee — a regression breaks
// the build before it can reach a runtime check); a small deterministic runtime
// tally at the end echoes a representative subset so the produced executable also
// reports a visible pass count.  Nothing here needs a live JVM.
#include <vmhook/vmhook.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

// -----------------------------------------------------------------------------
// is_vector
// -----------------------------------------------------------------------------
static_assert(vmhook::detail::is_vector_v<std::vector<int>>,
              "is_vector_v must recognise std::vector<int>");
static_assert(vmhook::detail::is_vector_v<const std::vector<int>&>,
              "is_vector_v must strip cv-ref before testing");
static_assert(!vmhook::detail::is_vector_v<int>,
              "is_vector_v must reject non-vector types");

// is_vector<...>::value_type_t had the same template-parameter-shadowing bug
// is_unique_ptr had (see below): the std::true_type base inherits a
// `using value_type = bool` typedef which silently won over the template
// parameter, making value_type_t resolve to bool for every vector type.
static_assert(std::is_same_v<typename vmhook::detail::is_vector<std::vector<int>>::value_type_t, int>,
              "is_vector<vector<int>>::value_type_t must be int, NOT bool "
              "(template-parameter shadowing the std::true_type::value_type "
              "typedef would silently regress this)");
static_assert(std::is_same_v<typename vmhook::detail::is_vector<std::vector<double>>::value_type_t, double>,
              "is_vector<vector<double>>::value_type_t must be double");
static_assert(std::is_same_v<typename vmhook::detail::is_vector<std::vector<std::string>>::value_type_t, std::string>,
              "is_vector<vector<string>>::value_type_t must be string");

// -----------------------------------------------------------------------------
// is_unique_ptr
// -----------------------------------------------------------------------------
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>,
              "is_unique_ptr_v must recognise std::unique_ptr<int>");
static_assert(!vmhook::detail::is_unique_ptr_v<int*>,
              "is_unique_ptr_v must reject raw pointers");
static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<int>&>,
              "is_unique_ptr_v must strip cv-ref before testing");

// THIS is the regression test for the bug commit 9466ca5 fixed.  Before the
// fix, the partial specialisation was written as:
//
//   template<typename value_type, typename deleter_type>
//   struct is_unique_ptr<std::unique_ptr<value_type, deleter_type>>
//       : std::true_type
//   { using value_type_t = value_type; };
//
// The std::true_type base (= std::integral_constant<bool, true>) brings in
// `using value_type = bool;`.  Inside the class body, unqualified lookup of
// `value_type` finds the INHERITED typedef before the template parameter of
// the same name, so value_type_t collapsed to bool for every wrapper type.
// Downstream `if constexpr (is_base_of_v<object_base, value_type_t>)` then
// silently skipped the JNI-arg-write branch, leaving values[i].l == 0 and
// dispatching null IChatComponent into Lunar / Forge / vanilla.
static_assert(std::is_same_v<typename vmhook::detail::is_unique_ptr<std::unique_ptr<int>>::value_type_t, int>,
              "is_unique_ptr<unique_ptr<int>>::value_type_t must be int, NOT bool "
              "(template-parameter shadow regression).");
static_assert(std::is_same_v<typename vmhook::detail::is_unique_ptr<std::unique_ptr<std::string>>::value_type_t, std::string>,
              "is_unique_ptr<unique_ptr<string>>::value_type_t must be string");
static_assert(std::is_same_v<typename vmhook::detail::is_unique_ptr<std::unique_ptr<vmhook::object_base>>::value_type_t, vmhook::object_base>,
              "is_unique_ptr<unique_ptr<object_base>>::value_type_t must be object_base "
              "(this is the exact trait usage that drives write_jni_arg_to_slot and "
              "would re-introduce the chat-not-sending bug if it broke).");

// And the indirect chain that makes the bug bite: a typical vmhook wrapper
// like `class my_wrapper : public vmhook::object<my_wrapper>` is what users
// pass through unique_ptr.  Verify the trait + is_base_of combination
// resolves correctly.
namespace {
    struct test_wrapper : public vmhook::object<test_wrapper> {
        using vmhook::object<test_wrapper>::object;
    };
}
static_assert(std::is_base_of_v<vmhook::object_base, test_wrapper>,
              "vmhook::object<T> -> object_base inheritance must hold for the "
              "static_assert in write_jni_arg_to_slot to accept user wrappers");
static_assert(std::is_base_of_v<
                  vmhook::object_base,
                  typename vmhook::detail::is_unique_ptr<std::unique_ptr<test_wrapper>>::value_type_t>,
              "End-to-end: is_unique_ptr<unique_ptr<MyWrapper>>::value_type_t must yield "
              "a type that derives from object_base.  Regression of this is what made "
              "every player->add_chat_message(...) call pass null to the JVM.");

// -----------------------------------------------------------------------------
// is_unique_object_ptr (sibling trait — has bool_constant base, same shadow risk)
// -----------------------------------------------------------------------------
static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<test_wrapper>>::value,
              "is_unique_object_ptr must report true for unique_ptr<MyWrapper>");
static_assert(!vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value,
              "is_unique_object_ptr must report false for unique_ptr<int> "
              "(int is not an object_base)");
static_assert(!vmhook::detail::is_unique_object_ptr<int>::value,
              "is_unique_object_ptr must report false for raw int");

// -----------------------------------------------------------------------------
// dependent_false_v — the lazy static_assert helper used by the new
// fall-through guards in write_jni_arg_to_slot / append_jni_arg.
// -----------------------------------------------------------------------------
static_assert(!vmhook::detail::dependent_false_v<int>,
              "dependent_false_v<T> must always be false (it is meant to be passed "
              "to static_assert in discarded if-constexpr branches and only fire "
              "when its branch is actually reached at instantiation)");
static_assert(!vmhook::detail::dependent_false_v<std::vector<int>>,
              "dependent_false_v<T> must be false for any T");

// -----------------------------------------------------------------------------
// vmhook::jni namespace — public surface
//
// New public spelling for the JNI helpers that used to require digging into
// vmhook::detail::jni_*.  These static_asserts pin down the type signatures
// of the wrappers so accidental drift between detail::jni_* and jni::* (or
// missing wrappers as the API grows) is caught at build time.
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<vmhook::jni::value, vmhook::detail::jni_value>,
              "vmhook::jni::value must alias the underlying jni_value union");

// These are non-template, non-overloaded - take their address and check the type.
// If the wrapper signature ever drifts from the underlying detail function, the
// types won't match and this fires at compile time.
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::find_class), std::string_view>,
              "vmhook::jni::find_class must accept string_view and return void* (jclass handle)");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::decode_object), void*>,
              "vmhook::jni::decode_object must take a jobject and return the decoded oop");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::new_string_utf), std::string_view>,
              "vmhook::jni::new_string_utf must accept string_view");
static_assert(std::is_invocable_r_v<std::string, decltype(vmhook::jni::get_string_utf), void*>,
              "vmhook::jni::get_string_utf must return std::string");
static_assert(std::is_invocable_r_v<void, decltype(vmhook::jni::exception_clear)>,
              "vmhook::jni::exception_clear must take no args");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::get_object_class), void*>,
              "vmhook::jni::get_object_class must take a jobject and return a jclass");

// signature_for_arg<T> returns std::string (non-constexpr) so the cross-check
// against the underlying detail::jni_signature_for_arg lives in
// test_helpers.cpp where we can call it at runtime.  Here we just confirm the
// wrapper exists and the return type matches.
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<int>()), std::string>,
              "signature_for_arg<T> must return std::string");

// -----------------------------------------------------------------------------
// java_slot_offsets — JVM interpreter slot widths for long / double
//
// HotSpot stores Java `long` and `double` parameters in TWO adjacent locals
// slots; every other type takes one.  Before this trait existed, the wrapper
// in vmhook::hook<T>() handed extract_frame_arg the C++ tuple index directly
// as the slot index, which silently read garbage for every arg following a
// long or double.
// -----------------------------------------------------------------------------
// Slot widths
static_assert( vmhook::detail::is_java_double_slot_v<std::int64_t>,
               "Java long must take 2 slots");
static_assert( vmhook::detail::is_java_double_slot_v<std::uint64_t>,
               "uint64_t (also mapped to Java long) must take 2 slots");
static_assert( vmhook::detail::is_java_double_slot_v<double>,
               "Java double must take 2 slots");
static_assert(!vmhook::detail::is_java_double_slot_v<std::int32_t>,
              "Java int must take 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<bool>,
              "Java boolean must take 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<float>,
              "Java float must take 1 slot (NOT double - different type)");
static_assert(!vmhook::detail::is_java_double_slot_v<void*>,
              "Object refs take 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::string>,
              "String args take 1 slot");

// Slot offset tables
static_assert(vmhook::detail::java_slot_offsets<std::tuple<>>::value.size() == 0,
              "Empty tuple yields empty offsets");

static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<std::int32_t, std::int32_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 1, 2 },
    "Three ints: tuple index == slot index");

// (int, long, int) — the classic regression case: previously the second int
// was read from slot 2 (the high half of the long) instead of slot 3.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<std::int32_t, std::int64_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 1, 3 },
    "(int, long, int): the trailing int must be slot 3, NOT slot 2 - "
    "long occupies slots 1 and 2");

// (long, long, int) — two longs in a row.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<std::int64_t, std::int64_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 2, 4 },
    "(long, long, int): trailing int at slot 4 - each long takes 2 slots");

// (double, int, double) — double has the same slot-width-2 semantics as long.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<double, std::int32_t, double>>::value
    == std::array<std::int32_t, 3>{ 0, 2, 3 },
    "(double, int, double): doubles occupy 2 slots each");

// (this[object], long, int) — instance-method case.  `this` is a 1-slot oop.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<void*, std::int64_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 1, 3 },
    "(this, long, int): `this` is 1 slot, long takes 2, trailing int at slot 3");

// -----------------------------------------------------------------------------
// argument_matches_descriptor disambiguation
//
// The matcher used to fold 1-byte and 2-byte primitives into "matches either"
// branches (`int8_t` -> "B" OR "Z", `int16_t` -> "S" OR "C"), which meant
// resolve_compatible_method could lock onto the WRONG overload when a class
// had e.g. both m(byte) and m(boolean), recreating the same overload-confusion
// bug fixed for the `a`-named EntityPlayerSP methods on vanilla 1.8.9.
//
// argument_matches_descriptor is a private template inside method_proxy so we
// can't reach it directly; instead we exercise the public resolve_compatible_method
// against fake method tables - but vmhook's traits / signatures alone let us
// verify the type intent.  These checks confirm the trait-side invariants the
// matcher relies on.
// -----------------------------------------------------------------------------

// uint16_t is the C++ type used in vmhook to represent Java `char` - it must
// be distinguishable from int16_t (Java `short`) at the trait level so the
// matcher can route to "C" vs "S".
static_assert(!std::is_same_v<std::int16_t, std::uint16_t>,
              "int16_t and uint16_t must be distinct C++ types so the descriptor "
              "matcher can route them to S vs C respectively");

// bool must be distinct from int8_t / uint8_t / signed char / unsigned char
// at the type-trait level so the matcher routes them to "Z" vs "B" respectively.
static_assert(!std::is_same_v<bool, std::int8_t>,
              "bool vs int8_t must be distinct types - regression would let the "
              "matcher fold both into the same descriptor");
static_assert(!std::is_same_v<bool, std::uint8_t>, "bool vs uint8_t distinct");
static_assert(!std::is_same_v<bool, char>, "bool vs char distinct");

// 1-byte integral types must be detected as 1 byte (not promoted) - the
// matcher uses sizeof(T) == 1 to route to "B".
static_assert(sizeof(std::int8_t) == 1, "int8_t must be 1 byte");
static_assert(sizeof(std::uint8_t) == 1, "uint8_t must be 1 byte");
static_assert(sizeof(char) == 1, "char must be 1 byte");

// 2-byte integral types must be detected as 2 bytes.
static_assert(sizeof(std::int16_t) == 2, "int16_t must be 2 bytes");
static_assert(sizeof(std::uint16_t) == 2, "uint16_t must be 2 bytes");
static_assert(sizeof(char16_t) == 2, "char16_t must be 2 bytes");

// =============================================================================
// EXHAUSTIVE EXPANSION
//
// Below: a representative-complete type zoo, then per-trait/per-API truth tables
// over it.  Each section heads with the live-header fact it pins.  These are all
// additive to test_traits_extra.cpp (which keys on the function_traits chain and
// element-type identity); here we sweep the *type taxonomy* and the *public API
// type-signatures* neither extra file touches.
// =============================================================================
namespace zoo
{
    // --- enums (scoped + unscoped, with and without fixed underlying type) ----
    enum unscoped_enum { unscoped_value_a, unscoped_value_b };
    enum class scoped_enum { alpha, beta };
    enum unscoped_enum_u8 : std::uint8_t { u8_lo, u8_hi };
    enum class scoped_enum_i64 : std::int64_t { big_a, big_b };

    // --- union ----------------------------------------------------------------
    union simple_union { int i; double d; };

    // --- class shapes ---------------------------------------------------------
    struct empty_struct {};
    struct data_struct { int x; double y; };
    struct poly_base { virtual ~poly_base() = default; virtual void f() {} };
    struct poly_derived final : poly_base { void f() override {} };
    struct abstract_base { virtual ~abstract_base() = default; virtual void pure() = 0; };
    struct final_empty final {};

    // --- a member-function-pointer + member-object-pointer target -------------
    struct with_members { int data_member; void member_fn(int) {} };
    using mem_obj_ptr = int with_members::*;
    using mem_fn_ptr  = void (with_members::*)(int);

    // --- function types / pointers --------------------------------------------
    using free_fn_t       = int(double, char);
    using free_fn_ptr_t   = int (*)(double, char);
    using free_fn_ref_t   = int (&)(double, char);
    using free_fn_noex_t  = int (*)(double, char) noexcept;
    using varargs_fn_ptr  = int (*)(int, ...);

    // --- the library's own wrapper hierarchy ----------------------------------
    struct wrapper_a : public vmhook::object<wrapper_a>
    {
        using vmhook::object<wrapper_a>::object;
    };
    struct wrapper_b : public vmhook::object<wrapper_b>
    {
        using vmhook::object<wrapper_b>::object;
    };
    // A wrapper that derives object_base WITHOUT going through object<T>.
    struct direct_object_base : public vmhook::object_base
    {
        using vmhook::object_base::object_base;
    };
    // A deeper wrapper hierarchy: derived-from-a-wrapper.
    struct wrapper_a_child : public wrapper_a
    {
        using wrapper_a::wrapper_a;
    };

    static_assert(std::is_base_of_v<vmhook::object_base, wrapper_a>);
    static_assert(std::is_base_of_v<vmhook::object_base, wrapper_b>);
    static_assert(std::is_base_of_v<vmhook::object_base, direct_object_base>);
    static_assert(std::is_base_of_v<vmhook::object_base, wrapper_a_child>);
    static_assert(!std::is_same_v<wrapper_a, wrapper_b>);

    // Named detour functor — used in the hook<T> signature checks below so the
    // unevaluated decltype operands never contain an inline lambda (maximally
    // portable across GCC / Clang / MSVC in unevaluated contexts).
    struct detour_functor
    {
        void operator()(vmhook::return_value&, const std::unique_ptr<wrapper_a>&) const {}
    };
}

// -----------------------------------------------------------------------------
// is_java_double_slot_v — EXHAUSTIVE type taxonomy (header: vmhook.hpp ~9211).
//
// The trait is true iff remove_cvref_t<T> is std::int64_t, std::uint64_t, or
// double; everything else (every other fundamental, pointer, enum, class, ...)
// is a single slot.  test_traits_extra.cpp checks a runtime subset; this is the
// full compile-time truth table over the whole zoo, asserting BOTH the 3-member
// accepted set and the rejected negative space, with the LP64 `long` caveat
// handled by-identity (NOT by sizeof).
// -----------------------------------------------------------------------------
// (1) The complete ACCEPTED set — exactly these three bare types.
static_assert(vmhook::detail::is_java_double_slot_v<std::int64_t>,  "int64_t is a 2-slot type");
static_assert(vmhook::detail::is_java_double_slot_v<std::uint64_t>, "uint64_t is a 2-slot type");
static_assert(vmhook::detail::is_java_double_slot_v<double>,        "double is a 2-slot type");

// (2) cv / ref qualified forms of each accepted type still accepted (remove_cvref).
static_assert(vmhook::detail::is_java_double_slot_v<const std::int64_t>,           "const int64_t");
static_assert(vmhook::detail::is_java_double_slot_v<volatile std::int64_t>,        "volatile int64_t");
static_assert(vmhook::detail::is_java_double_slot_v<const volatile std::int64_t>,  "cv int64_t");
static_assert(vmhook::detail::is_java_double_slot_v<std::int64_t&>,                "int64_t&");
static_assert(vmhook::detail::is_java_double_slot_v<std::int64_t&&>,               "int64_t&&");
static_assert(vmhook::detail::is_java_double_slot_v<const std::int64_t&>,          "const int64_t&");
static_assert(vmhook::detail::is_java_double_slot_v<const std::uint64_t>,          "const uint64_t");
static_assert(vmhook::detail::is_java_double_slot_v<std::uint64_t&>,               "uint64_t&");
static_assert(vmhook::detail::is_java_double_slot_v<std::uint64_t&&>,              "uint64_t&&");
static_assert(vmhook::detail::is_java_double_slot_v<const std::uint64_t&>,         "const uint64_t&");
static_assert(vmhook::detail::is_java_double_slot_v<const double>,                 "const double");
static_assert(vmhook::detail::is_java_double_slot_v<volatile double>,              "volatile double");
static_assert(vmhook::detail::is_java_double_slot_v<double&>,                      "double&");
static_assert(vmhook::detail::is_java_double_slot_v<double&&>,                     "double&&");
static_assert(vmhook::detail::is_java_double_slot_v<const double&>,                "const double&");

// (3) Single-slot fundamentals — the rejected negative space (full char family,
//     bool, every narrower fixed-width int, float, and the wide chars).
static_assert(!vmhook::detail::is_java_double_slot_v<bool>,           "bool is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<char>,           "char is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<signed char>,    "signed char is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<unsigned char>,  "unsigned char is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<char8_t>,        "char8_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<char16_t>,       "char16_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<char32_t>,       "char32_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<wchar_t>,        "wchar_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::int8_t>,    "int8_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::uint8_t>,   "uint8_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::int16_t>,   "int16_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::uint16_t>,  "uint16_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::int32_t>,   "int32_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::uint32_t>,  "uint32_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<float>,          "float is 1 slot (NOT double)");
static_assert(!vmhook::detail::is_java_double_slot_v<short>,          "short is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<unsigned short>, "unsigned short is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<int>,            "int is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<unsigned int>,   "unsigned int is 1 slot");

// (3a) `long` / `unsigned long` — width-VARIANT.  On LP64 (Linux/macOS) `long`
//      is 64-bit but is a DISTINCT type from std::int64_t (= long long there),
//      so is_java_double_slot_v is FALSE despite sizeof(long)==8.  On LLP64
//      (Windows) `long` is 32-bit and also false.  Assert the by-identity fact
//      (the result tracks type-identity with int64_t/uint64_t, never sizeof) so
//      this stays correct on every data model.  This is the exact LP64 trap the
//      task brief calls out.
static_assert(vmhook::detail::is_java_double_slot_v<long>
                  == std::is_same_v<long, std::int64_t>,
              "is_java_double_slot_v<long> must track type-identity with int64_t, "
              "NOT sizeof(long) (LP64 long is 64-bit yet != std::int64_t)");
static_assert(vmhook::detail::is_java_double_slot_v<unsigned long>
                  == std::is_same_v<unsigned long, std::uint64_t>,
              "is_java_double_slot_v<unsigned long> must track type-identity with uint64_t");
// `long long` / `unsigned long long` ARE int64_t/uint64_t on every supported
// data model, so they are always two-slot.
static_assert(vmhook::detail::is_java_double_slot_v<long long>
                  == std::is_same_v<long long, std::int64_t>,
              "long long tracks int64_t identity");
static_assert(vmhook::detail::is_java_double_slot_v<unsigned long long>
                  == std::is_same_v<unsigned long long, std::uint64_t>,
              "unsigned long long tracks uint64_t identity");

// (4) Non-fundamental categories — all single-slot (the trait only matches the
//     three scalar identities, so every pointer / enum / class / array / union /
//     function-pointer / member-pointer / std type is rejected).
static_assert(!vmhook::detail::is_java_double_slot_v<void*>,                         "void* is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<const void*>,                   "const void* is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::int64_t*>,                 "int64_t* (a pointer) is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<double*>,                       "double* (a pointer) is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::nullptr_t>,                "nullptr_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::unscoped_enum>,            "unscoped enum is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::scoped_enum>,              "scoped enum is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::scoped_enum_i64>,          "scoped enum : int64_t is still 1 slot (it is not int64_t)");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::simple_union>,             "union is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::data_struct>,              "class is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::poly_base>,                "polymorphic class is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::string>,                   "std::string is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::array<double, 2>>,         "array of 2 doubles is still 1 slot (it is not `double`)");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::free_fn_ptr_t>,            "function pointer is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::mem_obj_ptr>,              "member-object pointer is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::mem_fn_ptr>,               "member-function pointer is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<vmhook::object_base>,           "object_base is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::unique_ptr<zoo::wrapper_a>>, "unique_ptr<wrapper> is 1 slot");
// Remaining zoo categories (also single-slot): bare function / function ref /
// noexcept-fn-ptr / varargs-fn-ptr, an enum with a u8 fixed underlying type, and
// the abstract / derived / final class shapes.  None is int64/uint64/double.
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::free_fn_t>,                 "function type is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::free_fn_ref_t>,             "function reference is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::free_fn_noex_t>,            "noexcept function pointer is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::varargs_fn_ptr>,            "varargs function pointer is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::unscoped_enum_u8>,          "enum : uint8_t is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::abstract_base>,             "abstract class is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::poly_derived>,              "derived/final class is 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<zoo::final_empty>,               "final empty class is 1 slot");

// A couple of these zoo shapes also exercise the other predicates' negative
// space (and pin the abstract/derived hierarchy facts the wrapper system uses):
static_assert(!vmhook::detail::is_vector_v<zoo::free_fn_ref_t>,                       "function ref is not a vector");
static_assert(!vmhook::detail::is_unique_ptr_v<zoo::free_fn_noex_t>,                  "noexcept fn-ptr is not a unique_ptr");
static_assert(std::is_abstract_v<zoo::abstract_base>,                                 "abstract_base is abstract (sanity for the zoo fixture)");
static_assert(std::is_base_of_v<zoo::poly_base, zoo::poly_derived>,                   "poly_derived derives poly_base (zoo fixture)");
static_assert(std::is_final_v<zoo::final_empty>,                                      "final_empty is final (zoo fixture)");

// -----------------------------------------------------------------------------
// is_vector_v / is_vector<T>::value_type_t — EXHAUSTIVE taxonomy
// (header: vmhook.hpp ~1721-1746).  Accepts iff remove_cvref_t<T> is a
// std::vector specialisation; value_type_t (shadow-safe spelling) yields the
// element type.  Extra covers element identity + qualifier stripping at runtime;
// here is the full compile-time negative space across the type zoo.
// -----------------------------------------------------------------------------
// Accepted, with cv/ref stripping over many element categories.
static_assert(vmhook::detail::is_vector_v<std::vector<int>>,                     "vector<int>");
static_assert(vmhook::detail::is_vector_v<std::vector<bool>>,                    "vector<bool> (the proxy specialisation still matches)");
static_assert(vmhook::detail::is_vector_v<std::vector<std::string>>,            "vector<string>");
static_assert(vmhook::detail::is_vector_v<std::vector<int*>>,                    "vector<int*>");
static_assert(vmhook::detail::is_vector_v<std::vector<zoo::wrapper_a>>,          "vector<wrapper>");
static_assert(vmhook::detail::is_vector_v<std::vector<std::vector<int>>>,        "vector<vector<int>>");
static_assert(vmhook::detail::is_vector_v<std::vector<zoo::scoped_enum>>,        "vector<enum>");
static_assert(vmhook::detail::is_vector_v<const std::vector<int>>,               "const vector<int>");
static_assert(vmhook::detail::is_vector_v<volatile std::vector<int>>,            "volatile vector<int>");
static_assert(vmhook::detail::is_vector_v<const volatile std::vector<int>&>,     "cv vector<int>&");
static_assert(vmhook::detail::is_vector_v<std::vector<int>&>,                    "vector<int>&");
static_assert(vmhook::detail::is_vector_v<std::vector<int>&&>,                   "vector<int>&&");
static_assert(vmhook::detail::is_vector_v<const std::vector<int>&>,              "const vector<int>&");

// Rejected negative space — fundamentals, other containers/smart-ptrs, ptrs,
// refs, arrays, enums, unions, classes, function/member pointers.
static_assert(!vmhook::detail::is_vector_v<int>,                                 "int is not a vector");
static_assert(!vmhook::detail::is_vector_v<bool>,                                "bool is not a vector");
static_assert(!vmhook::detail::is_vector_v<void>,                               "void is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::nullptr_t>,                      "nullptr_t is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::vector<int>*>,                   "pointer-to-vector is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::array<int, 4>>,                  "std::array is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::tuple<int, int>>,               "tuple is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::unique_ptr<int>>,               "unique_ptr is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::shared_ptr<std::vector<int>>>,  "shared_ptr<vector> is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::string>,                        "string is not a vector");
static_assert(!vmhook::detail::is_vector_v<std::string_view>,                   "string_view is not a vector");
static_assert(!vmhook::detail::is_vector_v<int[4]>,                             "C array is not a vector");
static_assert(!vmhook::detail::is_vector_v<int[]>,                              "unbounded C array is not a vector");
static_assert(!vmhook::detail::is_vector_v<zoo::scoped_enum>,                    "enum is not a vector");
static_assert(!vmhook::detail::is_vector_v<zoo::simple_union>,                   "union is not a vector");
static_assert(!vmhook::detail::is_vector_v<zoo::empty_struct>,                   "empty class is not a vector");
static_assert(!vmhook::detail::is_vector_v<zoo::free_fn_ptr_t>,                  "function pointer is not a vector");
static_assert(!vmhook::detail::is_vector_v<zoo::mem_fn_ptr>,                     "member pointer is not a vector");
static_assert(!vmhook::detail::is_vector_v<vmhook::object_base>,                 "object_base is not a vector");

// value_type_t resolves to the element type (never bool) across categories.
static_assert(std::is_same_v<vmhook::detail::is_vector<std::vector<int>>::value_type_t, int>,                                 "vec<int>::value_type_t==int");
static_assert(std::is_same_v<vmhook::detail::is_vector<std::vector<bool>>::value_type_t, bool>,                               "vec<bool>::value_type_t==bool (genuinely bool here, not shadow)");
static_assert(std::is_same_v<vmhook::detail::is_vector<std::vector<int*>>::value_type_t, int*>,                               "vec<int*>::value_type_t==int*");
static_assert(std::is_same_v<vmhook::detail::is_vector<std::vector<zoo::wrapper_a>>::value_type_t, zoo::wrapper_a>,           "vec<wrapper>::value_type_t==wrapper");
static_assert(std::is_same_v<vmhook::detail::is_vector<std::vector<std::vector<int>>>::value_type_t, std::vector<int>>,       "vec<vec<int>>::value_type_t==vec<int>");
static_assert(!std::is_same_v<vmhook::detail::is_vector<std::vector<int>>::value_type_t, bool>,                               "vec<int>::value_type_t is NOT bool (shadow guard)");

// -----------------------------------------------------------------------------
// is_unique_ptr_v / is_unique_ptr<T>::value_type_t — EXHAUSTIVE taxonomy
// (header: vmhook.hpp ~1758-1784).  Accepts iff remove_cvref_t<T> is a
// std::unique_ptr specialisation; value_type_t yields the pointee.
// -----------------------------------------------------------------------------
// Accepted across pointee categories + cv/ref stripping.
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>,                       "unique_ptr<int>");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<std::string>>,              "unique_ptr<string>");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<zoo::wrapper_a>>,            "unique_ptr<wrapper>");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<vmhook::object_base>>,       "unique_ptr<object_base>");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<std::vector<int>>>,          "unique_ptr<vector>");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int[]>>,                     "unique_ptr<int[]> (array specialisation)");
static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<int>>,                 "const unique_ptr<int>");
static_assert(vmhook::detail::is_unique_ptr_v<volatile std::unique_ptr<int>>,              "volatile unique_ptr<int>");
static_assert(vmhook::detail::is_unique_ptr_v<const volatile std::unique_ptr<int>&>,       "cv unique_ptr<int>&");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>&>,                      "unique_ptr<int>&");
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>&&>,                     "unique_ptr<int>&&");
static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<int>&>,                "const unique_ptr<int>&");

// Rejected negative space.
static_assert(!vmhook::detail::is_unique_ptr_v<int>,                            "int is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<void>,                           "void is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<std::nullptr_t>,                 "nullptr_t is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<int*>,                           "raw pointer is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<void*>,                          "void* is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<vmhook::object_base*>,           "object_base* is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<std::shared_ptr<int>>,           "shared_ptr is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<std::weak_ptr<int>>,             "weak_ptr is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<std::vector<int>>,              "vector is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<std::string>,                   "string is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<zoo::scoped_enum>,               "enum is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<zoo::simple_union>,              "union is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<zoo::empty_struct>,              "empty class is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<zoo::free_fn_ptr_t>,             "function pointer is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<zoo::mem_obj_ptr>,               "member pointer is not a unique_ptr");
static_assert(!vmhook::detail::is_unique_ptr_v<int[4]>,                         "C array is not a unique_ptr");

// value_type_t over pointee categories (never collapses to bool).
static_assert(std::is_same_v<vmhook::detail::is_unique_ptr<std::unique_ptr<int>>::value_type_t, int>,                       "uptr<int>::value_type_t==int");
static_assert(std::is_same_v<vmhook::detail::is_unique_ptr<std::unique_ptr<std::string>>::value_type_t, std::string>,      "uptr<string>::value_type_t==string");
static_assert(std::is_same_v<vmhook::detail::is_unique_ptr<std::unique_ptr<zoo::wrapper_a>>::value_type_t, zoo::wrapper_a>, "uptr<wrapper>::value_type_t==wrapper");
static_assert(std::is_same_v<vmhook::detail::is_unique_ptr<std::unique_ptr<void*>>::value_type_t, void*>,                   "uptr<void*>::value_type_t==void*");
static_assert(!std::is_same_v<vmhook::detail::is_unique_ptr<std::unique_ptr<int>>::value_type_t, bool>,                     "uptr<int>::value_type_t is NOT bool (shadow guard)");

// -----------------------------------------------------------------------------
// is_unique_object_ptr — pointee-must-derive-object_base predicate.
// (Sibling trait with a bool_constant base; extra covers a subset.)  Here we
// sweep the full wrapper hierarchy: object<T> wrappers, a direct object_base
// subclass, a grandchild wrapper, and object_base itself (is_base_of is
// reflexive, so it qualifies), against the non-object negative space.
// -----------------------------------------------------------------------------
static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<vmhook::object_base>>::value,   "uptr<object_base> qualifies (reflexive)");
static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<zoo::wrapper_a>>::value,        "uptr<wrapper_a> qualifies");
static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<zoo::wrapper_b>>::value,        "uptr<wrapper_b> qualifies");
static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<zoo::direct_object_base>>::value, "uptr<direct object_base subclass> qualifies");
static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<zoo::wrapper_a_child>>::value,  "uptr<grandchild wrapper> qualifies");
static_assert(!vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value,                  "uptr<int> does not qualify");
static_assert(!vmhook::detail::is_unique_object_ptr<std::unique_ptr<std::string>>::value,          "uptr<string> does not qualify");
static_assert(!vmhook::detail::is_unique_object_ptr<std::unique_ptr<std::vector<int>>>::value,     "uptr<vector> does not qualify");
static_assert(!vmhook::detail::is_unique_object_ptr<int>::value,                                   "raw int does not qualify");
static_assert(!vmhook::detail::is_unique_object_ptr<zoo::wrapper_a>::value,                        "a bare wrapper value (not a unique_ptr) does not qualify");
static_assert(!vmhook::detail::is_unique_object_ptr<zoo::wrapper_a*>::value,                       "a raw wrapper pointer does not qualify");
static_assert(!vmhook::detail::is_unique_object_ptr<std::shared_ptr<zoo::wrapper_a>>::value,       "shared_ptr<wrapper> does not qualify");

// -----------------------------------------------------------------------------
// dependent_false_v — always false, for every arity and qualifier mix
// (header: vmhook.hpp ~1718).  It only fires when its discarded if-constexpr
// branch is actually instantiated.
// -----------------------------------------------------------------------------
static_assert(!vmhook::detail::dependent_false_v<>,                                       "0 args");
static_assert(!vmhook::detail::dependent_false_v<int>,                                    "1 arg");
static_assert(!vmhook::detail::dependent_false_v<void>,                                   "void");
static_assert(!vmhook::detail::dependent_false_v<int&>,                                   "ref arg");
static_assert(!vmhook::detail::dependent_false_v<const int*>,                             "pointer arg");
static_assert(!vmhook::detail::dependent_false_v<zoo::wrapper_a>,                         "wrapper arg");
static_assert(!vmhook::detail::dependent_false_v<int, double>,                            "2 args");
static_assert(!vmhook::detail::dependent_false_v<int, double, std::string, void*, char>, "5 args");

// -----------------------------------------------------------------------------
// oop_t / oop_type_t — the decoded-OOP pointer alias pair
// (header: vmhook.hpp ~17592-17601).
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<vmhook::oop_type_t, void*>,           "oop_type_t is void*");
static_assert(std::is_same_v<vmhook::oop_t, void*>,                "oop_t is void*");
static_assert(std::is_same_v<vmhook::oop_t, vmhook::oop_type_t>,   "oop_t and oop_type_t are the same alias");
static_assert(std::is_pointer_v<vmhook::oop_t>,                    "oop_t is a pointer type");
static_assert(std::is_void_v<std::remove_pointer_t<vmhook::oop_t>>, "oop_t points at void");

// -----------------------------------------------------------------------------
// object_base — the wrapper base contract (header: vmhook.hpp ~17646).
// Pin the polymorphism, value semantics, and member-function type-signatures
// the whole wrapper system and is_unique_object_ptr dispatch rely on.
// -----------------------------------------------------------------------------
static_assert(std::is_polymorphic_v<vmhook::object_base>,                   "object_base is polymorphic (has a vtable)");
static_assert(std::has_virtual_destructor_v<vmhook::object_base>,           "object_base has a virtual destructor");
static_assert(!std::is_abstract_v<vmhook::object_base>,                     "object_base is concrete (no pure-virtuals)");
static_assert(!std::is_final_v<vmhook::object_base>,                        "object_base is not final (it is a base class)");
// Construction: explicit, noexcept, from an oop / void* / nullptr, and default.
static_assert(std::is_nothrow_default_constructible_v<vmhook::object_base>, "object_base() is noexcept (defaulted oop)");
static_assert(std::is_nothrow_constructible_v<vmhook::object_base, vmhook::oop_t>, "object_base(oop_t) is noexcept");
static_assert(std::is_nothrow_constructible_v<vmhook::object_base, void*>,  "object_base(void*) is noexcept");
static_assert(std::is_nothrow_constructible_v<vmhook::object_base, std::nullptr_t>, "object_base(nullptr) is noexcept");
// The ctor is explicit: a void* must not implicitly convert to an object_base.
static_assert(!std::is_convertible_v<void*, vmhook::object_base>,           "object_base(oop) ctor is explicit");
static_assert(!std::is_convertible_v<std::nullptr_t, vmhook::object_base>,  "object_base(nullptr) ctor is explicit");
// Value semantics: copyable + nothrow-movable (raw-pointer member).
static_assert(std::is_copy_constructible_v<vmhook::object_base>,            "object_base is copy-constructible");
static_assert(std::is_copy_assignable_v<vmhook::object_base>,               "object_base is copy-assignable");
static_assert(std::is_nothrow_move_constructible_v<vmhook::object_base>,    "object_base move-ctor is noexcept");
static_assert(std::is_nothrow_move_assignable_v<vmhook::object_base>,       "object_base move-assign is noexcept");
// get_instance(): const, noexcept, returns oop_type_t.
static_assert(std::is_same_v<decltype(std::declval<const vmhook::object_base&>().get_instance()), vmhook::oop_type_t>,
              "object_base::get_instance() returns oop_type_t");
static_assert(noexcept(std::declval<const vmhook::object_base&>().get_instance()),
              "object_base::get_instance() is noexcept");
// get_field(string_view) const -> optional<field_proxy>;
// get_method(string_view) const -> optional<method_proxy>; plus the
// signature-filtered get_method overload.
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::object_base&>().get_field(std::declval<std::string_view>())),
                  std::optional<vmhook::field_proxy>>,
              "object_base::get_field(string_view) returns optional<field_proxy>");
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::object_base&>().get_method(std::declval<std::string_view>())),
                  std::optional<vmhook::method_proxy>>,
              "object_base::get_method(string_view) returns optional<method_proxy>");
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::object_base&>().get_method(
                      std::declval<std::string_view>(), std::declval<std::string_view>())),
                  std::optional<vmhook::method_proxy>>,
              "object_base::get_method(string_view, string_view) returns optional<method_proxy>");

// -----------------------------------------------------------------------------
// object<derived> — the CRTP wrapper template (header: vmhook.hpp ~18613).
// Pin the inheritance, the default template argument, the inherited ctor, and
// the portable static accessors' type-signatures.  (The deducing-this overloads
// are compiler-gated; we assert the gate macro and the always-present static_*
// accessors that the GCC / Clang>=20 path falls back to.)
// -----------------------------------------------------------------------------
static_assert(std::is_base_of_v<vmhook::object_base, zoo::wrapper_a>,        "object<T> derives object_base");
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<zoo::wrapper_a>>, "object<T> base-subobject is object_base");
static_assert(std::is_polymorphic_v<zoo::wrapper_a>,                         "a wrapper inherits the vtable");
static_assert(std::has_virtual_destructor_v<zoo::wrapper_a>,                 "a wrapper has a virtual destructor (deletable through object_base*)");
static_assert(std::is_nothrow_constructible_v<zoo::wrapper_a, vmhook::oop_t>, "wrapper(oop) inherited ctor is noexcept");
static_assert(std::is_nothrow_default_constructible_v<zoo::wrapper_a>,       "wrapper() is noexcept (defaulted oop)");
static_assert(!std::is_convertible_v<vmhook::oop_t, zoo::wrapper_a>,         "the inherited wrapper(oop) ctor stays explicit");
// object<> defaults its template argument to void, so the bare name names a type.
static_assert(std::is_same_v<vmhook::object<>, vmhook::object<void>>,        "object<> defaults derived=void");
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<>>,      "object<> still derives object_base");
// Portable static accessors: static_field / static_method return the proxies.
static_assert(std::is_same_v<decltype(zoo::wrapper_a::static_field(std::declval<std::string_view>())),
                             std::optional<vmhook::field_proxy>>,
              "wrapper::static_field(name) returns optional<field_proxy>");
static_assert(std::is_same_v<decltype(zoo::wrapper_a::static_method(std::declval<std::string_view>())),
                             std::optional<vmhook::method_proxy>>,
              "wrapper::static_method(name) returns optional<method_proxy>");
static_assert(std::is_same_v<decltype(zoo::wrapper_a::static_method(std::declval<std::string_view>(),
                                                                    std::declval<std::string_view>())),
                             std::optional<vmhook::method_proxy>>,
              "wrapper::static_method(name, sig) returns optional<method_proxy>");
// VMHOOK_HAS_DEDUCING_THIS is a 0/1 capability switch on every toolchain.
static_assert(VMHOOK_HAS_DEDUCING_THIS == 0 || VMHOOK_HAS_DEDUCING_THIS == 1,
              "VMHOOK_HAS_DEDUCING_THIS must be exactly 0 or 1");

// -----------------------------------------------------------------------------
// return_value — the hook-callback handle (header: vmhook.hpp ~1315).
// Pin the constraints on its mutators that the typed-return path relies on:
//   - set<T> is only callable for trivially-copyable T that fit a 64-bit slot;
//   - set(nullptr) is gated on `requires is_base_of_v<object_base, wrapper_type>`;
//   - cancel / set_arg / caller signatures.
// (The sizeof/trivially-copyable checks inside set<T>'s body are hard
// static_asserts at instantiation, NOT SFINAE, so the rejected forms are pinned
// at the trait level here rather than via is_invocable.)
// -----------------------------------------------------------------------------
// Shape: not default-constructible (needs a return_slot*); ctor is explicit + noexcept.
static_assert(!std::is_default_constructible_v<vmhook::return_value>,
              "return_value has no default ctor (it owns a slot pointer)");
static_assert(std::is_nothrow_constructible_v<vmhook::return_value, vmhook::hotspot::return_slot*>,
              "return_value(slot*) is noexcept");
static_assert(std::is_nothrow_constructible_v<vmhook::return_value, vmhook::hotspot::return_slot*, vmhook::hotspot::frame*>,
              "return_value(slot*, frame*) is noexcept");
static_assert(!std::is_convertible_v<vmhook::hotspot::return_slot*, vmhook::return_value>,
              "return_value(slot*) ctor is explicit");
// set<T>(T) primary overload exists for primitives and void* and returns void.
static_assert(std::is_same_v<
                  decltype(std::declval<vmhook::return_value&>().set(std::int32_t{})), void>,
              "return_value::set(int32_t) returns void");
static_assert(std::is_same_v<
                  decltype(std::declval<vmhook::return_value&>().set(std::declval<void*>())), void>,
              "return_value::set(void*) returns void");
static_assert(noexcept(std::declval<vmhook::return_value&>().set(std::int32_t{})),
              "return_value::set(int32_t) is noexcept");
// The set<T>(T) primary overload is selected and well-formed (returning void)
// for every legitimate single-slot return type — bool / char / a full-width
// 64-bit int / a scoped enum — i.e. the trivially-copyable, fits-a-64-bit-slot
// payloads.  (The size / trivially-copyable gate inside the body is a hard
// static_assert that fires when the body is instantiated by an actual call; the
// type-level facts pinned here are overload selection + the void return type.)
static_assert(std::is_same_v<decltype(std::declval<vmhook::return_value&>().set(bool{})), void>,
              "return_value::set(bool) is well-formed and returns void");
static_assert(std::is_same_v<decltype(std::declval<vmhook::return_value&>().set(char{})), void>,
              "return_value::set(char) is well-formed and returns void");
static_assert(std::is_same_v<decltype(std::declval<vmhook::return_value&>().set(std::int64_t{})), void>,
              "return_value::set(int64_t) (full-width slot) is well-formed and returns void");
static_assert(std::is_same_v<decltype(std::declval<vmhook::return_value&>().set(zoo::scoped_enum{})), void>,
              "return_value::set(scoped_enum) (trivially-copyable single slot) is well-formed and returns void");
// set(nullptr_t): the constrained typed-null overload.  Selectable for any
// object_base-derived wrapper_type; the EXPLICIT template argument is required
// because nullptr_t cannot deduce wrapper_type.
static_assert(std::is_same_v<
                  decltype(std::declval<vmhook::return_value&>().set<zoo::wrapper_a>(nullptr)), void>,
              "return_value::set<wrapper>(nullptr) returns void (constrained overload selected)");
static_assert(std::is_same_v<
                  decltype(std::declval<vmhook::return_value&>().set<vmhook::object_base>(nullptr)), void>,
              "return_value::set<object_base>(nullptr) is selectable (is_base_of reflexive)");
static_assert(noexcept(std::declval<vmhook::return_value&>().set<zoo::wrapper_a>(nullptr)),
              "return_value::set<wrapper>(nullptr) is noexcept");
// cancel() -> void, noexcept.
static_assert(std::is_same_v<decltype(std::declval<vmhook::return_value&>().cancel()), void>,
              "return_value::cancel() returns void");
static_assert(noexcept(std::declval<vmhook::return_value&>().cancel()),
              "return_value::cancel() is noexcept");
// set_arg(index, value&&) -> bool, noexcept.
static_assert(std::is_same_v<
                  decltype(std::declval<vmhook::return_value&>().set_arg(std::int32_t{}, std::int32_t{})), bool>,
              "return_value::set_arg(index, value) returns bool");
static_assert(noexcept(std::declval<vmhook::return_value&>().set_arg(std::int32_t{}, std::int32_t{})),
              "return_value::set_arg is noexcept");
// caller() const -> caller_info, noexcept; caller_info::valid() -> bool noexcept.
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::return_value&>().caller()),
                  vmhook::return_value::caller_info>,
              "return_value::caller() returns caller_info");
static_assert(noexcept(std::declval<const vmhook::return_value&>().caller()),
              "return_value::caller() is noexcept");
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::return_value::caller_info&>().valid()), bool>,
              "caller_info::valid() returns bool");

// -----------------------------------------------------------------------------
// vmhook::jni:: — the public JNI forwarder surface (header: vmhook.hpp ~13273).
// The original file pins a handful; this completes the type-signature coverage
// of the non-template, non-overloaded forwarders (so any drift from the
// underlying detail::jni_* is caught) plus signature_for_arg<T>'s return type
// across the full argument-type set.  These are TYPE checks only — no JVM call.
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<vmhook::jni::value, vmhook::detail::jni_value>,
              "jni::value aliases the detail jni_value union");
// Pointer-handle / oop forwarders.
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::decode_object), void*>,
              "jni::decode_object(jobject) -> void*");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::oop_handle), void*, void*&>,
              "jni::oop_handle(oop, storage&) -> void*");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::find_class), std::string_view>,
              "jni::find_class(name) -> void*");
static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                  decltype(vmhook::jni::find_class_with_context_loader), std::string_view>,
              "jni::find_class_with_context_loader(name) -> klass*");
static_assert(std::is_invocable_r_v<void, decltype(vmhook::jni::exception_clear)>,
              "jni::exception_clear() -> void");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::get_object_class), void*>,
              "jni::get_object_class(jobject) -> jclass(void*)");
// ID lookups.
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::get_method_id),
                  void*, const std::string&, const std::string&>,
              "jni::get_method_id(klass, name, sig) -> void*");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::get_static_method_id),
                  void*, const std::string&, const std::string&>,
              "jni::get_static_method_id(klass, name, sig) -> void*");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::get_static_field_id),
                  void*, const std::string&, const std::string&>,
              "jni::get_static_field_id(klass, name, sig) -> void*");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::get_static_object_field),
                  void*, void*>,
              "jni::get_static_object_field(klass, field_id) -> void*");
// Calls.  (std::is_invocable ignores default arguments — they are not part of
// the function TYPE — so we pass all three parameters; the third has a default
// at the declaration, which is a separate, call-site-only convenience.)
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::call_object_method),
                  void*, void*, const vmhook::jni::value*>,
              "jni::call_object_method(obj, mid, args) -> void*");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::call_static_object_method),
                  void*, void*, const vmhook::jni::value*>,
              "jni::call_static_object_method(klass, mid, args) -> void*");
// klass <-> mirror, strings.
static_assert(std::is_invocable_r_v<vmhook::hotspot::klass*,
                  decltype(vmhook::jni::klass_from_class_mirror), void*>,
              "jni::klass_from_class_mirror(jclass) -> klass*");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::new_string_utf), std::string_view>,
              "jni::new_string_utf(text) -> jstring(void*)");
static_assert(std::is_invocable_r_v<std::string, decltype(vmhook::jni::get_string_utf), void*>,
              "jni::get_string_utf(jstring) -> std::string");
// signature_for_arg<T>() returns std::string for EVERY supported argument type
// (the VALUE table is in test_helpers.cpp; here we pin the return type uniformly
// across the whole accepted set — primitives, wide ints, string spellings, and a
// registered-or-not wrapper, which all resolve to std::string at the type level).
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<bool>()),               std::string>, "sig<bool> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::int8_t>()),        std::string>, "sig<int8> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::uint8_t>()),       std::string>, "sig<uint8> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::int16_t>()),       std::string>, "sig<int16> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::uint16_t>()),      std::string>, "sig<uint16> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::int32_t>()),       std::string>, "sig<int32> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::int64_t>()),       std::string>, "sig<int64> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::uint64_t>()),      std::string>, "sig<uint64> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<float>()),              std::string>, "sig<float> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<double>()),             std::string>, "sig<double> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::string>()),        std::string>, "sig<string> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::string_view>()),   std::string>, "sig<string_view> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<const char*>()),        std::string>, "sig<const char*> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<char*>()),              std::string>, "sig<char*> -> string");
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<std::unique_ptr<zoo::wrapper_a>>()), std::string>, "sig<unique_ptr<wrapper>> -> string");
// And the public wrapper forwards to the detail implementation (same type).
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<int>()),
                             decltype(vmhook::detail::jni_signature_for_arg<int>())>,
              "jni::signature_for_arg<T> return type matches detail::jni_signature_for_arg<T>");

// -----------------------------------------------------------------------------
// Public entry-point signatures: register_class<T>, hook<T>, make_unique<T>.
// These need a JVM to DO anything, but their type-signatures are pure compile
// facts.  Pin the return types and that they are invocable with the documented
// argument shapes (so accidental signature drift is a build break).
// -----------------------------------------------------------------------------
// register_class<T>(string_view) -> bool, noexcept.
static_assert(std::is_same_v<decltype(vmhook::register_class<zoo::wrapper_a>(std::declval<std::string_view>())), bool>,
              "register_class<T>(string_view) returns bool");
static_assert(noexcept(vmhook::register_class<zoo::wrapper_a>(std::declval<std::string_view>())),
              "register_class<T> is noexcept");
// hook<T>(name, detour) and hook<T>(name, sig, detour, already_hooked?) -> bool.
static_assert(std::is_same_v<
                  decltype(vmhook::hook<zoo::wrapper_a>(
                      std::declval<std::string_view>(),
                      std::declval<zoo::detour_functor>())),
                  bool>,
              "hook<T>(name, detour) returns bool");
static_assert(std::is_same_v<
                  decltype(vmhook::hook<zoo::wrapper_a>(
                      std::declval<std::string_view>(), std::declval<std::string_view>(),
                      std::declval<zoo::detour_functor>(),
                      std::declval<bool*>())),
                  bool>,
              "hook<T>(name, sig, detour, already_hooked) returns bool");
// make_unique<T>(args...) -> unique_ptr<T>.
static_assert(std::is_same_v<decltype(vmhook::make_unique<zoo::wrapper_a>()), std::unique_ptr<zoo::wrapper_a>>,
              "make_unique<T>() returns unique_ptr<T>");
static_assert(std::is_same_v<decltype(vmhook::make_unique<zoo::wrapper_a>(std::int32_t{}, std::declval<std::string>())),
                             std::unique_ptr<zoo::wrapper_a>>,
              "make_unique<T>(args...) returns unique_ptr<T>");

// -----------------------------------------------------------------------------
// Platform / compiler / arch self-check (unchanged)
// -----------------------------------------------------------------------------
#if (VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS \
   + VMHOOK_OS_IOS    + VMHOOK_OS_ANDROID) != 1
#  error "exactly one VMHOOK_OS_* macro should be 1"
#endif

// VMHOOK_OS_POSIX is the OR of all POSIX-flavored backends.
#if VMHOOK_OS_WINDOWS && VMHOOK_OS_POSIX
#  error "POSIX detection is inconsistent with Windows detection"
#endif

#if VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_GCC + VMHOOK_COMPILER_CLANG != 1
#  error "exactly one compiler macro should be 1"
#endif

#if (VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64) != 1
#  error "exactly one arch macro should be 1"
#endif

// -----------------------------------------------------------------------------
// Deterministic runtime tally.
//
// Everything above is enforced at COMPILE time (static_assert) — the build
// breaks before this runs if any fact regresses.  This runtime block re-checks a
// representative subset as constexpr-evaluated booleans so the produced
// executable also reports a visible, byte-identical pass count on every run.
// No JVM, no I/O beyond stdout, no nondeterminism: each `ok` is a constant
// expression, so the output is identical across invocations and platforms.
// -----------------------------------------------------------------------------
namespace
{
    int g_failures{ 0 };

    auto check(const char* const name, const bool ok) -> void
    {
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
        if (!ok) { ++g_failures; }
    }
}

int main()
{
    // is_java_double_slot_v accepted set + a slice of the negative space.
    check("is_java_double_slot_v<int64_t>",  vmhook::detail::is_java_double_slot_v<std::int64_t>);
    check("is_java_double_slot_v<uint64_t>", vmhook::detail::is_java_double_slot_v<std::uint64_t>);
    check("is_java_double_slot_v<double>",   vmhook::detail::is_java_double_slot_v<double>);
    check("!is_java_double_slot_v<float>",   !vmhook::detail::is_java_double_slot_v<float>);
    check("!is_java_double_slot_v<int32_t>", !vmhook::detail::is_java_double_slot_v<std::int32_t>);
    check("!is_java_double_slot_v<void*>",   !vmhook::detail::is_java_double_slot_v<void*>);
    check("is_java_double_slot_v<long> tracks int64 identity",
          vmhook::detail::is_java_double_slot_v<long> == std::is_same_v<long, std::int64_t>);

    // is_vector_v / is_unique_ptr_v / is_unique_object_ptr representative cells.
    check("is_vector_v<vector<int>>",        vmhook::detail::is_vector_v<std::vector<int>>);
    check("!is_vector_v<int>",               !vmhook::detail::is_vector_v<int>);
    check("is_unique_ptr_v<unique_ptr<int>>", vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>);
    check("!is_unique_ptr_v<shared_ptr<int>>", !vmhook::detail::is_unique_ptr_v<std::shared_ptr<int>>);
    check("is_unique_object_ptr<unique_ptr<wrapper_a>>",
          vmhook::detail::is_unique_object_ptr<std::unique_ptr<zoo::wrapper_a>>::value);
    check("!is_unique_object_ptr<unique_ptr<int>>",
          !vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value);

    // dependent_false_v is false for every arity.
    check("!dependent_false_v<>",            !vmhook::detail::dependent_false_v<>);
    check("!dependent_false_v<int,double>",  !vmhook::detail::dependent_false_v<int, double>);

    // oop alias identity.
    check("oop_t == void*",                  std::is_same_v<vmhook::oop_t, void*>);
    check("oop_t == oop_type_t",             std::is_same_v<vmhook::oop_t, vmhook::oop_type_t>);

    // object_base / object<T> contract.
    check("object_base is polymorphic",      std::is_polymorphic_v<vmhook::object_base>);
    check("object_base has virtual dtor",    std::has_virtual_destructor_v<vmhook::object_base>);
    check("object<T> derives object_base",   std::is_base_of_v<vmhook::object_base, zoo::wrapper_a>);
    check("object<> == object<void>",        std::is_same_v<vmhook::object<>, vmhook::object<void>>);

    // VMHOOK_HAS_DEDUCING_THIS is a clean 0/1 switch.
    check("VMHOOK_HAS_DEDUCING_THIS in {0,1}",
          VMHOOK_HAS_DEDUCING_THIS == 0 || VMHOOK_HAS_DEDUCING_THIS == 1);

    // return_value shape.
    check("return_value not default-constructible",
          !std::is_default_constructible_v<vmhook::return_value>);

    // A live (deterministic, no-JVM) value-table sanity for the pure-primitive
    // signature_for_arg branches: these never touch the JVM (the wrapper branches
    // do; we avoid those here).  Matches the authoritative table in
    // test_helpers.cpp but proves the traits-target binary agrees.
    check("signature_for_arg<bool> == Z",    vmhook::jni::signature_for_arg<bool>() == "Z");
    check("signature_for_arg<int32_t> == I", vmhook::jni::signature_for_arg<std::int32_t>() == "I");
    check("signature_for_arg<int64_t> == J", vmhook::jni::signature_for_arg<std::int64_t>() == "J");
    check("signature_for_arg<double> == D",  vmhook::jni::signature_for_arg<double>() == "D");
    check("signature_for_arg<string> == Ljava/lang/String;",
          vmhook::jni::signature_for_arg<std::string>() == "Ljava/lang/String;");

    std::printf("vmhook traits: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
