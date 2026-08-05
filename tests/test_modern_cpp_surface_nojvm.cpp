// Standalone (no-JVM) contract test for the modern-C++ surface added in v0.6.0:
// the COMPILE-TIME descriptor machinery, std::expected-based try_* error
// reporting, the runtime-typed call path (java_arg / call_packed), the C++26
// feature gates, and the handle-returning allocation helpers.
//
// ===========================================================================
// WHY THESE BELONG IN ONE FILE
// ===========================================================================
// They are the pieces that let vmhook be driven by something that is not a C++
// call site: a script, a viewer, an RPC.  Each removes a different reason the
// library used to be C++-only —
//
//   * descriptor_of / descriptor_for : a descriptor no longer has to be built
//     by concatenating strings at runtime;
//   * java_arg / call_packed         : an argument list no longer has to be a
//     C++ parameter pack;
//   * access_error / try_*           : a failure no longer collapses to an
//     empty optional that says nothing;
//   * new_object / new_array / new_string : an allocation no longer hands back
//     a bare address the caller must keep fresh.
//
// ===========================================================================
// WHAT THIS FILE CAN AND CANNOT PROVE
// ===========================================================================
// No JVM is loaded, so every lookup fails and every allocation returns null.
// That makes this the COMPILE-TIME and GRACEFUL-DEGRADATION test: the
// descriptor assertions are static_asserts (they are compile-time constants, so
// there is nothing else they could be), and every runtime check verifies that
// the cold path degrades to an honest empty/error rather than to a plausible
// wrong answer.
//
// It CANNOT prove that call_packed lays out real interpreter slots correctly —
// that needs a live VM, and the slot rules it shares with call() are covered by
// borrowed_detour_arg's compile-time slot table plus the live JVM modules.
// ===========================================================================
#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace d = vmhook::detail;

// A wrapper deliberately left UNannotated: it is the case that must fall back
// to the runtime registry, and the case that proves the gate actually gates.
class mcs_plain final : public vmhook::object<mcs_plain>
{
public:
    explicit mcs_plain(const vmhook::oop_t oop = nullptr) noexcept
        : vmhook::object<mcs_plain>{ oop }
    {
    }
};

// ===========================================================================
// SECTION 1 -- COMPILE-TIME DESCRIPTORS.  These are constants, so they are
//   asserted as constants; a runtime check here would be strictly weaker.
// ===========================================================================

static_assert(d::descriptor_of_v<void>         == "V");
static_assert(d::descriptor_of_v<bool>         == "Z");
static_assert(d::descriptor_of_v<std::int8_t>  == "B");
static_assert(d::descriptor_of_v<std::int16_t> == "S");
static_assert(d::descriptor_of_v<std::int32_t> == "I");
static_assert(d::descriptor_of_v<std::int64_t> == "J");
static_assert(d::descriptor_of_v<float>        == "F");
static_assert(d::descriptor_of_v<double>       == "D");

// char is UNSIGNED 16-bit in Java: char16_t and uint16_t both mean "C", and
// must be claimed BEFORE the generic 2-byte branch that would say "S".
static_assert(d::descriptor_of_v<char16_t>      == "C");
static_assert(d::descriptor_of_v<std::uint16_t> == "C");
static_assert(d::descriptor_of_v<std::int16_t>  == "S");

// Every string-ish spelling names java.lang.String.
static_assert(d::descriptor_of_v<std::string>      == "Ljava/lang/String;");
static_assert(d::descriptor_of_v<std::string_view> == "Ljava/lang/String;");
static_assert(d::descriptor_of_v<const char*>      == "Ljava/lang/String;");

// The untyped borrow is java/lang/Object BY CONSTRUCTION, not by fallback.
static_assert(d::descriptor_of_v<vmhook::borrowed<>> == "Ljava/lang/Object;");

// cv-ref spellings must not change the answer -- a detour may take any of them.
static_assert(d::descriptor_of_v<const std::int32_t&> == "I");
static_assert(d::descriptor_of_v<std::int64_t&&>      == "J");

// An UNANNOTATED wrapper has no compile-time descriptor.  Empty is the signal
// that the runtime registry has to be consulted; it is NOT a failure.
static_assert(d::descriptor_of_v<mcs_plain>.empty());
static_assert(d::descriptor_of_v<vmhook::borrowed<mcs_plain>>.empty());
static_assert(d::descriptor_of_v<std::unique_ptr<mcs_plain>>.empty());

// ...and that empty piece disqualifies the WHOLE signature from compile-time
// assembly, because one unknown piece is enough.
static_assert(d::all_descriptors_static_v<void, std::int32_t, double>);
static_assert(!d::all_descriptors_static_v<void, vmhook::borrowed<mcs_plain>>);
static_assert(!d::all_descriptors_static_v<mcs_plain>);

// Whole descriptors, assembled at compile time.
static_assert(d::descriptor_for<void>::view() == "()V");
static_assert(d::descriptor_for<std::int32_t>::view() == "()I");
static_assert(d::descriptor_for<void, std::int32_t>::view() == "(I)V");
// The shape that catches an off-by-one in the assembly: two-slot types adjacent
// to one-slot ones, and a multi-character piece in the middle.
static_assert(d::descriptor_for<std::int32_t, std::int32_t, std::int64_t,
                                std::int32_t>::view() == "(IJI)I");
static_assert(d::descriptor_for<std::string, std::string,
                                double>::view() == "(Ljava/lang/String;D)Ljava/lang/String;");
static_assert(d::descriptor_for<void, bool, char16_t, std::int8_t, float,
                                vmhook::borrowed<>>::view()
              == "(ZCBFLjava/lang/Object;)V");

// The storage is a real constexpr array, so the view points into rodata rather
// than at a temporary -- that is what makes it safe to hand out.
static_assert(std::is_same_v<
                  decltype(d::descriptor_for<void, std::int32_t>::storage),
                  const std::array<char, 4>>);

// ===========================================================================
// SECTION 2 -- java_arg.  The runtime-typed argument, pinned at compile time
//   where it can be, since its slot classification is what call_packed trusts.
// ===========================================================================

using java_arg = vmhook::method_proxy::java_arg;

static_assert(std::is_same_v<decltype(java_arg::of_int(0)), java_arg>);
static_assert(noexcept(java_arg::of_int(0)));
static_assert(noexcept(java_arg::of_long(0)));
static_assert(noexcept(java_arg::of_object(nullptr)));

// ===========================================================================
// SECTION 3 -- the C++26 gates.  Whatever their value on this toolchain, they
//   must be DEFINED, and the fallbacks must keep the surface intact.
// ===========================================================================

#if !defined(VMHOOK_HAS_REFLECTION)
#error "VMHOOK_HAS_REFLECTION must always be defined (0 or 1)"
#endif
#if !defined(VMHOOK_HAS_STD_EXPECTED)
#error "VMHOOK_HAS_STD_EXPECTED must always be defined (0 or 1)"
#endif
#if !defined(VMHOOK_DELETED)
#error "VMHOOK_DELETED must always be defined"
#endif

// has_annotated_class_name_v must be answerable for ANY type, reflection or
// not -- it is a gate, and a gate that fails to compile is not a gate.
static_assert(!d::has_annotated_class_name_v<mcs_plain>);
static_assert(!d::has_annotated_class_name_v<std::int32_t>);

// The invariants expressed as deletions stay deleted on every toolchain; the
// C++26 reason string is an improvement to the MESSAGE, never to the rule.
static_assert(!std::is_copy_constructible_v<vmhook::oop_pin>);
static_assert(!std::is_copy_assignable_v<vmhook::oop_pin>);
static_assert(std::is_move_constructible_v<vmhook::oop_pin>);
static_assert(!std::is_copy_constructible_v<vmhook::root<mcs_plain>>);
static_assert(!std::is_move_constructible_v<vmhook::root<mcs_plain>>);
static_assert(!std::is_copy_constructible_v<vmhook::hook_handle>);
static_assert(std::is_move_constructible_v<vmhook::hook_handle>);

int main()
{
    check("static_asserts_compiled", true);

    // =======================================================================
    // SECTION 4 -- access_error / error_message.  These strings reach users in
    //   logs and assertion text, so an unnamed enumerator is a real defect.
    // =======================================================================
    {
        using vmhook::access_error;
        const access_error all[]{
            access_error::wrapper_not_registered,
            access_error::class_not_loaded,
            access_error::member_not_found,
            access_error::null_instance,
            access_error::mirror_unreadable,
        };
        bool every_named{ true };
        bool all_distinct{ true };
        for (std::size_t i{ 0 }; i < std::size(all); ++i)
        {
            const auto text{ vmhook::error_message(all[i]) };
            if (text.empty() || text == "unknown access error") { every_named = false; }
            for (std::size_t j{ i + 1 }; j < std::size(all); ++j)
            {
                if (vmhook::error_message(all[i]) == vmhook::error_message(all[j]))
                {
                    all_distinct = false;
                }
            }
        }
        check("error_message_names_every_enumerator", every_named);
        check("error_message_is_distinct_per_cause", all_distinct);
        // An out-of-range value must degrade, not fall off the end of a switch.
        check("error_message_handles_unknown",
              vmhook::error_message(static_cast<access_error>(200))
              == "unknown access error");
    }

    // =======================================================================
    // SECTION 5 -- try_field / try_method with no JVM.  The whole point of the
    //   expected-based API is that the caller learns WHICH failure this is.
    //   With no JVM and an unregistered wrapper, that is wrapper_not_registered
    //   -- specifically NOT class_not_loaded, which would send a caller into a
    //   retry loop that can never succeed.
    // =======================================================================
#if VMHOOK_HAS_STD_EXPECTED
    {
        const mcs_plain probe{ nullptr };

        const auto field{ probe.try_field("anything") };
        check("try_field_fails_cold", !field.has_value());
        check("try_field_blames_registration",
              !field.has_value()
              && field.error() == vmhook::access_error::wrapper_not_registered);

        const auto method{ probe.try_method("anything") };
        check("try_method_fails_cold", !method.has_value());
        check("try_method_blames_registration",
              !method.has_value()
              && method.error() == vmhook::access_error::wrapper_not_registered);

        const auto overload{ probe.try_method("anything", "()V") };
        check("try_method_with_descriptor_fails_cold", !overload.has_value());
        check("try_method_with_descriptor_blames_registration",
              !overload.has_value()
              && overload.error() == vmhook::access_error::wrapper_not_registered);

        // The reason must be reportable without the caller writing a switch.
        check("try_field_error_is_reportable",
              !field.has_value()
              && !vmhook::error_message(field.error()).empty());
    }
#else
    check("std_expected_unavailable_on_this_toolchain", true);
#endif

    // =======================================================================
    // SECTION 6 -- java_arg::from_descriptor.  This is the bridge a scripted
    //   caller crosses, so its parsing rules are part of the contract.
    // =======================================================================
    {
        const auto as_int{ java_arg::from_descriptor('I', "1234") };
        check("from_descriptor_int", as_int && as_int->integral == 1234);

        const auto negative{ java_arg::from_descriptor('J', "-9000000000") };
        check("from_descriptor_negative_long",
              negative && negative->integral == -9000000000LL);

        // DECIMAL by default.  strtoull with base 0 would read this as OCTAL and
        // hand back 0 -- silently, for anyone who typed a zero-padded number.
        const auto padded{ java_arg::from_descriptor('I', "09") };
        check("from_descriptor_leading_zero_is_decimal_not_octal",
              padded && padded->integral == 9);
        const auto padded_ten{ java_arg::from_descriptor('I', "010") };
        check("from_descriptor_010_is_ten", padded_ten && padded_ten->integral == 10);

        // An explicit 0x prefix is still hex.
        const auto hexed{ java_arg::from_descriptor('I', "0xFF") };
        check("from_descriptor_hex_prefix", hexed && hexed->integral == 255);

        const auto yes{ java_arg::from_descriptor('Z', "true") };
        const auto no{ java_arg::from_descriptor('Z', "false") };
        check("from_descriptor_bool_true", yes && yes->integral == 1);
        check("from_descriptor_bool_false", no && no->integral == 0);
        const auto one{ java_arg::from_descriptor('Z', "1") };
        check("from_descriptor_bool_accepts_1", one && one->integral == 1);

        // A single character is the character; anything longer is a number.
        const auto letter{ java_arg::from_descriptor('C', "A") };
        check("from_descriptor_char_literal", letter && letter->integral == 65);
        const auto code{ java_arg::from_descriptor('C', "65535") };
        check("from_descriptor_char_numeric", code && code->integral == 65535);

        const auto pi{ java_arg::from_descriptor('D', "2.5") };
        check("from_descriptor_double", pi && pi->floating == 2.5);
        const auto f{ java_arg::from_descriptor('F', "-0.5") };
        check("from_descriptor_float", f && f->floating == -0.5);

        const auto text{ java_arg::from_descriptor('L', "hello") };
        check("from_descriptor_reference_becomes_string",
              text && text->type == java_arg::kind::string_ && text->text == "hello");

        // An unrepresentable letter is a REFUSAL, not a wrong slot.  Silently
        // producing an int here would feed the callee a garbage argument.
        check("from_descriptor_rejects_unknown_letter",
              !java_arg::from_descriptor('Q', "0").has_value());
        check("from_descriptor_rejects_void",
              !java_arg::from_descriptor('V', "").has_value());
    }

    // =======================================================================
    // SECTION 7 -- call_packed with no JVM.  It must refuse, not fault, for
    //   every argument shape -- including the ones that would allocate.
    // =======================================================================
    {
        const vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)V" } };

        const std::vector<java_arg> none{};
        check("call_packed_null_method_is_void", proxy.call_packed(nullptr, none).is_void());

        const std::vector<java_arg> mixed{
            java_arg::of_int(1), java_arg::of_long(2), java_arg::of_double(3.5),
            java_arg::of_string("s"), java_arg::of_object(nullptr),
        };
        check("call_packed_mixed_args_cold_is_void",
              proxy.call_packed(nullptr, mixed).is_void());

        // Over-arity must be refused rather than truncated -- a truncated
        // argument list is a call with silently wrong arguments.
        const std::vector<java_arg> too_many(12, java_arg::of_int(1));
        check("call_packed_over_arity_refused",
              proxy.call_packed(nullptr, too_many).is_void());
    }

    // =======================================================================
    // SECTION 8 -- handle-returning allocation.  With no JVM every allocation
    //   fails, and the handle must be EMPTY rather than a borrow of nullptr.
    // =======================================================================
    {
        const auto s{ vmhook::new_string("hello") };
        check("new_string_cold_is_empty", !static_cast<bool>(s));
        check("new_string_cold_not_expired", !s.expired());
        check("new_string_cold_raw_is_null", s.raw_unsafe() == nullptr);

        const auto o{ vmhook::new_object<mcs_plain>(nullptr, 16) };
        check("new_object_cold_is_empty", !static_cast<bool>(o));
        check("new_object_cold_not_expired", !o.expired());

        const auto a{ vmhook::new_array<>("[I", 4, sizeof(std::int32_t)) };
        check("new_array_cold_is_empty", !static_cast<bool>(a));

        // A negative length is a caller error and must not allocate.
        const auto bad{ vmhook::new_array<>("[I", -1, sizeof(std::int32_t)) };
        check("new_array_negative_length_is_empty", !static_cast<bool>(bad));

        static_assert(std::is_same_v<decltype(vmhook::new_string("")), vmhook::borrowed<>>);
        static_assert(std::is_same_v<decltype(vmhook::new_object<mcs_plain>(nullptr, 0)),
                                     vmhook::borrowed<mcs_plain>>);
    }

    // =======================================================================
    // SECTION 9 -- the runtime descriptor builder must agree with the
    //   compile-time constant.  If these ever disagree, method resolution and
    //   whatever consumed the constant are describing different methods.
    // =======================================================================
    {
        const bool agree{
            d::jvm_descriptor_for_arg<std::int32_t>() == d::descriptor_of_v<std::int32_t>
            && d::jvm_descriptor_for_arg<std::int64_t>() == d::descriptor_of_v<std::int64_t>
            && d::jvm_descriptor_for_arg<bool>() == d::descriptor_of_v<bool>
            && d::jvm_descriptor_for_arg<double>() == d::descriptor_of_v<double>
            && d::jvm_descriptor_for_arg<char16_t>() == d::descriptor_of_v<char16_t>
            && d::jvm_descriptor_for_arg<std::string>() == d::descriptor_of_v<std::string>
            && d::jvm_descriptor_for_arg<vmhook::borrowed<>>()
                   == d::descriptor_of_v<vmhook::borrowed<>> };
        check("runtime_and_consteval_descriptors_agree", agree);

        // The unannotated wrapper is the case with no constant: the runtime
        // builder still answers, degrading to Object with a logged warning.
        check("unannotated_wrapper_degrades_to_object",
              d::jvm_descriptor_for_arg<vmhook::borrowed<mcs_plain>>()
              == "Ljava/lang/Object;");
    }

    return failures == 0 ? 0 : 1;
}
