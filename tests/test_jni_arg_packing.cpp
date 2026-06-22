// Standalone unit test: detail::write_jni_arg_to_slot / append_jni_arg union-member
// writes + needs_release tagging (regression guard for the union-aliasing
// DeleteLocalRef bug), the shared convert_jni_arg core, the jni_signature_for_arg
// descriptor builder, and the pure utf8_to_utf16 decoder that decides the exact
// content of every String arg.  No JVM present -> jni_new_string_utf16_local
// (NewString) returns null, so the needs_release tag stays false on every string
// path here; but the UTF-16 DECODE of the forwarded bytes is a pure function and
// IS asserted exhaustively (Sections R/S) — interior NULs and astral scalars
// included.  Anything requiring a live oop / running JVM (the actual
// DeleteLocalRef cleanup loop, Call*MethodA dispatch, result-handle release, and
// the round-trip of a built jstring) is covered by JVM integration modules.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Minimal object_base-derived wrapper so we can exercise the object-arg branch
// of write_jni_arg_to_slot without a JVM. get_instance() just returns the raw
// pointer we hand the base ctor; no oop is dereferenced.
struct fake_object : vmhook::object_base
{
    explicit fake_object(void* p) noexcept : vmhook::object_base{ p } {}
};

// Helper: pack a single arg into a fresh slot and report back the union value,
// the storage cell, and the needs_release tag the library decided on.
template<typename arg_t>
static auto pack_one(arg_t&& arg, vmhook::detail::jni_value& out_value, void*& out_storage)
    -> bool
{
    out_value = vmhook::detail::jni_value{};
    out_storage = nullptr;
    bool needs_release{ true }; // poison: library MUST overwrite this
    vmhook::detail::write_jni_arg_to_slot(out_value, out_storage, needs_release,
                                          std::forward<arg_t>(arg));
    return needs_release;
}

// Helper: drive the SHARED CORE convert_jni_arg directly (the single source of
// truth that both write_jni_arg_to_slot and append_jni_arg delegate to).  Same
// semantics as pack_one above, but exercises the core itself so a divergence
// between the core and either wrapper would surface.  Poisons every out param
// so we prove the core overwrites them.
template<typename arg_t>
static auto pack_one_core(arg_t&& arg, vmhook::detail::jni_value& out_value, void*& out_storage)
    -> bool
{
    out_value.j = static_cast<std::int64_t>(0xA5A5'A5A5'A5A5'A5A5ULL); // poison
    out_storage = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEAD'F00Dul)); // poison
    bool needs_release{ true }; // poison: core MUST overwrite this
    vmhook::detail::convert_jni_arg(out_value, out_storage, needs_release,
                                    std::forward<arg_t>(arg));
    return needs_release;
}

// Helper: the compile-time JNI descriptor the library derives for a given C++
// arg type (the parallel builder that MUST agree with the packers).
template<typename arg_t>
static auto sig() -> std::string
{
    return vmhook::detail::jni_signature_for_arg<arg_t>();
}

// Helper: bit-exact comparison of two floats / doubles via memcpy (so NaN
// payloads and signed zero are compared by bit pattern, not by == which is
// false for NaN and true across +0/-0).
static auto bits_eq(float a, float b) -> bool
{
    std::uint32_t ua{ 0 };
    std::uint32_t ub{ 0 };
    std::memcpy(&ua, &a, sizeof ua);
    std::memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}
static auto bits_eq(double a, double b) -> bool
{
    std::uint64_t ua{ 0 };
    std::uint64_t ub{ 0 };
    std::memcpy(&ua, &a, sizeof ua);
    std::memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

// Helper: exact, length-counted comparison of a UTF-16 code-unit vector (what
// vmhook::detail::utf8_to_utf16 returns, and what the counted-length NewString
// the String-arg packer calls consumes) against an explicit code-unit list.
// The expected units are spelled as plain ints so call sites can use hex
// literals without std::uint16_t{} noise; each is range-checked back to 16 bits
// so a typo'd >0xFFFF expectation cannot silently pass via truncation.
static auto units_eq(const std::vector<std::uint16_t>& got,
                     std::initializer_list<int> expected) -> bool
{
    if (got.size() != expected.size()) { return false; }
    std::size_t k{ 0 };
    for (const int want : expected)
    {
        if (want < 0 || want > 0xFFFF) { return false; }
        if (got[k] != static_cast<std::uint16_t>(want)) { return false; }
        ++k;
    }
    return true;
}

// A second object_base-derived wrapper so we can register a distinct JVM class
// name and assert the unique_ptr / by-value object signature branch resolves
// `Lpkg/Name;` from vmhook::type_to_class_map.  (register_class<T>() itself
// needs a live JVM via find_class, so the tests insert into the public map
// directly — jni_signature_for_arg reads the map with no JVM dependency.)
struct registered_wrapper : vmhook::object_base
{
    explicit registered_wrapper(void* p = nullptr) noexcept : vmhook::object_base{ p } {}
};

// ---------------------------------------------------------------------------
// Compile-time ACCEPTANCE predicate (test-local mirror of convert_jni_arg /
// jni_signature_for_arg's if-constexpr ladder).  The library packers do NOT
// SFINAE-reject unsupported args — they fire a hard static_assert in the `else`
// branch (vmhook.hpp convert_jni_arg:10938 / jni_signature_for_arg:10794), so a
// detector that *instantiates* the packer on a bad type would hard-error rather
// than fail gracefully.  Instead we replicate the library's exact dispatch
// predicate here as a constexpr bool, then assert (Section O) that it ACCEPTS
// every documented arg type and REJECTS everything else.  This is the
// compile-time "type -> jvalue-field / signature mapping" + "non-JNI-convertible
// arg is rejected" contract the harness brief calls for, expressed as a single
// boolean so a drift in the supported set surfaces as a failing static_assert.
//
// MUST stay in lockstep with convert_jni_arg.  Acceptance set (post std::decay):
//   string family (std::string / std::string_view / const char* / char*)
//   | unique_ptr<T : object_base> | T : object_base | bool
//   | integral && (sizeof<=4 || sizeof==8) | float | double.
template<typename arg_t>
inline constexpr bool jni_arg_accepted_v = [] {
    using clean_t = std::decay_t<arg_t>;
    if constexpr (std::is_same_v<clean_t, std::string>
                  || std::is_same_v<clean_t, std::string_view>
                  || std::is_same_v<clean_t, const char*>
                  || std::is_same_v<clean_t, char*>)
        return true;
    else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
        // unique_ptr is accepted by convert_jni_arg's dispatch arm; whether its
        // T derives from object_base is enforced by an *inner* static_assert,
        // so at the dispatch level the unique_ptr branch is "accepted".
        return true;
    else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
        return true;
    else if constexpr (std::is_same_v<clean_t, bool>)
        return true;
    else if constexpr (std::is_integral_v<clean_t>
                       && (sizeof(clean_t) <= sizeof(std::int32_t)
                           || sizeof(clean_t) == sizeof(std::int64_t)))
        return true;
    else if constexpr (std::is_same_v<clean_t, float> || std::is_same_v<clean_t, double>)
        return true;
    else
        return false;
}();

// A plain aggregate that is NOT object_base-derived: an unsupported arg type.
struct not_an_object
{
    int a;
    double b;
};

// A scoped enum (NOT integral per std::is_integral) -> unsupported.
enum class some_scoped_enum : int { x, y };

int main()
{
    // --- Precondition: confirm we really are running without a JVM ----------
    // write_jni_arg_to_slot's string branches call jni_new_string_utf, which
    // returns null when current_jni_env is null. The whole "needs_release stays
    // false for strings" cluster below depends on this being null.
    check("precondition_no_jvm_env_is_null",
          vmhook::hotspot::current_jni_env == nullptr);

    // --- union jni_value layout sanity --------------------------------------
    // All members alias the same storage; this is exactly why reading value.l
    // back to classify a slot is unsound and a dedicated needs_release tag is
    // required. Guard the assumptions the cleanup-loop bug report relies on.
    check("union_jni_value_is_pointer_sized",
          sizeof(vmhook::detail::jni_value) == sizeof(void*));
    {
        vmhook::detail::jni_value v{};
        v.j = static_cast<std::int64_t>(0x1234'5678'1234'5678LL);
        check("union_long_aliases_pointer_member",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(v.j)));
        v = vmhook::detail::jni_value{};
        v.z = true;
        check("union_bool_true_aliases_pointer_member",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)));
    }

    // --- needs_release tag: every primitive leaves it false -----------------
    // This is the core regression guard. A non-zero primitive must NOT be
    // tagged for release (otherwise the cleanup loop DeleteLocalRef's garbage).
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        check("needs_release_false_for_bool_true",
              pack_one(true, v, storage) == false);
        check("needs_release_false_for_bool_false",
              pack_one(false, v, storage) == false);
        check("needs_release_false_for_int32_minus_one",
              pack_one(std::int32_t{ -1 }, v, storage) == false);
        check("needs_release_false_for_int32_high_bit",
              pack_one(std::int32_t{ static_cast<std::int32_t>(0x8000'0000) }, v, storage) == false);
        check("needs_release_false_for_int64_sentinel",
              pack_one(std::int64_t{ static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL) }, v, storage) == false);
        check("needs_release_false_for_int16",
              pack_one(std::int16_t{ 0x1234 }, v, storage) == false);
        check("needs_release_false_for_uint16",
              pack_one(std::uint16_t{ 0xBEEF }, v, storage) == false);
        check("needs_release_false_for_int8",
              pack_one(std::int8_t{ -7 }, v, storage) == false);
        check("needs_release_false_for_float_one",
              pack_one(float{ 1.0f }, v, storage) == false);
        check("needs_release_false_for_double_one",
              pack_one(double{ 1.0 }, v, storage) == false);
    }

    // --- needs_release tag: object-reference args leave it false ------------
    // Synthetic stack handles (value.l points INTO `storage`) are not JNI local
    // refs and must not be tagged for release.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        fake_object obj{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xABCD'0000)) };
        const bool rel{ pack_one(obj, v, storage) };
        check("needs_release_false_for_object_arg", rel == false);
        // value.l must point at the caller's storage cell, and storage must hold
        // the object's instance pointer (the synthetic-handle indirection).
        check("object_arg_value_l_points_at_storage",
              v.l == static_cast<void*>(&storage));
        check("object_arg_storage_holds_instance",
              storage == obj.get_instance());
    }

    // --- needs_release tag: null c-string leaves it false -------------------
    // A null const char* never calls NewStringUTF, so value.l is null and the
    // tag is false. (A non-null c-string WOULD call NewStringUTF, but with no
    // JVM that returns null too -> still false; asserted below.)
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        const char* null_cstr{ nullptr };
        const bool rel{ pack_one(null_cstr, v, storage) };
        check("needs_release_false_for_null_cstring", rel == false);
        check("null_cstring_value_l_is_null", v.l == nullptr);
    }

    // --- needs_release tag: string args stay false WITHOUT a JVM ------------
    // jni_new_string_utf returns null (no env) so value.l == nullptr and the
    // library tags needs_release = (value.l != nullptr) == false. This is the
    // explicitly-requested "assert that path too". With a live JVM these would
    // flip to true and be released -- covered by JVM integration in example.cpp.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        check("needs_release_false_for_std_string_no_jvm",
              pack_one(std::string{ "hi" }, v, storage) == false);
        check("std_string_value_l_null_no_jvm", v.l == nullptr);

        check("needs_release_false_for_string_view_no_jvm",
              pack_one(std::string_view{ "world" }, v, storage) == false);
        check("string_view_value_l_null_no_jvm", v.l == nullptr);

        const char* cstr{ "literal" };
        check("needs_release_false_for_nonnull_cstring_no_jvm",
              pack_one(cstr, v, storage) == false);
        check("nonnull_cstring_value_l_null_no_jvm", v.l == nullptr);
    }

    // --- union member writes land in the RIGHT member -----------------------
    // The cluster focus: each primitive must write the documented union field.
    // bool -> .z, integral<=4B -> .i (NOT .s/.b/.c), integral==8B -> .j,
    // float -> .f, double -> .d. We read .l back too to document the aliasing.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        pack_one(true, v, storage);
        check("bool_true_writes_z_member", v.z == true);

        pack_one(false, v, storage);
        check("bool_false_writes_z_member", v.z == false);

        pack_one(std::int32_t{ 42 }, v, storage);
        check("int32_writes_i_member", v.i == 42);

        pack_one(std::int32_t{ -1 }, v, storage);
        check("int32_minus_one_writes_i_member", v.i == -1);

        // int16/uint16/int8 are promoted into the .i (int32) member, NOT .s/.c/.b.
        // This is the documented behaviour of write_jni_arg_to_slot's
        // `integral && sizeof<=int32` branch.
        pack_one(std::int16_t{ 0x1234 }, v, storage);
        check("int16_writes_i_member_widened", v.i == 0x1234);

        pack_one(std::uint16_t{ 0xBEEF }, v, storage);
        check("uint16_writes_i_member_widened", v.i == static_cast<std::int32_t>(0xBEEF));

        pack_one(std::int8_t{ -7 }, v, storage);
        check("int8_writes_i_member_widened", v.i == -7);

        pack_one(std::int64_t{ 0x1234'5678'90AB'CDEFLL }, v, storage);
        check("int64_writes_j_member", v.j == 0x1234'5678'90AB'CDEFLL);

        pack_one(float{ 3.5f }, v, storage);
        check("float_writes_f_member", v.f == 3.5f);

        pack_one(double{ 2.71828 }, v, storage);
        check("double_writes_d_member", v.d == 2.71828);
    }

    // --- union-aliasing footgun, demonstrated on the real packer ------------
    // A long sentinel written via .j re-reads through .l as a non-null, bogus
    // pointer. The cleanup loop would DeleteLocalRef THIS if it trusted .l --
    // which is precisely why needs_release stayed false above. Pin the hazard.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        const std::int64_t sentinel{ static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL) };
        const bool rel{ pack_one(sentinel, v, storage) };
        check("long_sentinel_aliases_nonnull_l",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(sentinel)));
        check("long_sentinel_l_is_not_storage_cell",
              v.l != static_cast<void*>(&storage));
        check("long_sentinel_not_tagged_for_release", rel == false);
    }

    // --- value is zero-initialized before the member write ------------------
    // write_jni_arg_to_slot does `value = jni_value{}` first, so a bool false
    // leaves the entire pointer-sized cell zero (no stale upper bits).
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        // Pre-dirty the slot, then pack a 'false' and confirm full-width clear.
        v.j = static_cast<std::int64_t>(0xFFFF'FFFF'FFFF'FFFFULL);
        bool needs_release{ true };
        vmhook::detail::write_jni_arg_to_slot(v, storage, needs_release, false);
        check("bool_false_clears_full_union_width", v.l == nullptr);
    }

    // --- vector path: make_jni_args / append_jni_arg parity -----------------
    // jni_make_unique uses the std::vector<char> needs_release tag rather than a
    // single bool, but the classification logic is identical. Confirm the same
    // invariant: primitives tagged 0, no JVM means string args also tagged 0.
    {
        std::vector<void*> object_handles{};
        std::vector<char>  needs_release{};
        std::vector<vmhook::detail::jni_value> values{
            vmhook::detail::make_jni_args(
                object_handles, needs_release,
                std::int64_t{ static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL) },
                std::int32_t{ -1 },
                true,
                float{ 1.0f },
                double{ 2.0 },
                std::int16_t{ 0x1234 },
                std::string{ "no_jvm_so_null" })
        };

        check("make_jni_args_value_count_matches", values.size() == 7);
        check("make_jni_args_tag_count_matches", needs_release.size() == 7);

        // Every tag must be 0: six primitives + one string that NewStringUTF
        // could not build (no env). Not a single slot is releasable here.
        bool all_zero{ true };
        for (const char tag : needs_release)
        {
            if (tag != 0) { all_zero = false; }
        }
        check("make_jni_args_no_slot_tagged_for_release", all_zero);

        // Spot-check the union members landed correctly through the vector path.
        check("make_jni_args_long_in_j_member",
              values[0].j == static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL));
        check("make_jni_args_int_in_i_member", values[1].i == -1);
        check("make_jni_args_bool_in_z_member", values[2].z == true);
        check("make_jni_args_float_in_f_member", values[3].f == 1.0f);
        check("make_jni_args_double_in_d_member", values[4].d == 2.0);
        check("make_jni_args_int16_widened_in_i_member", values[5].i == 0x1234);
        check("make_jni_args_string_l_null_no_jvm", values[6].l == nullptr);
    }

    // =====================================================================
    // EXPANDED COVERAGE (additive — every expected value derived from the
    // convert_jni_arg core in vmhook.hpp:
    //   out.j = 0 (full-width clear); needs_release = false; then by type:
    //   bool->.z, integral&&sizeof<=4 -> .i (static_cast<int32>),
    //   integral&&sizeof==8 -> .j, float->.f, double->.d, string/c-string ->
    //   .l via NewStringUTF (null w/o JVM), object/unique_ptr -> .l=&storage.)
    // All targets are little-endian (CI matrix), matching this file's existing
    // union-aliasing assertions.
    // =====================================================================

    // ---- More integral types route through the .i (int32) branch ------------
    // sizeof<=4 integrals all land in .i via static_cast<int32_t>: the cast
    // SIGN-extends signed sources and ZERO-extends unsigned sources.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // plain int -> .i verbatim.
        pack_one(int{ -123456 }, v, storage);
        check("plain_int_writes_i_member", v.i == -123456);

        // unsigned int (uint32) -> .i is a bit-reinterpret (0xFFFFFFFF -> -1).
        pack_one(std::uint32_t{ 0xFFFFFFFFu }, v, storage);
        check("uint32_max_writes_i_member_as_minus_one", v.i == -1);
        check("uint32_max_not_tagged_for_release",
              pack_one(std::uint32_t{ 0xFFFFFFFFu }, v, storage) == false);

        // plain char -> .i (sizeof 1, integral).  Value 'A' == 65.
        pack_one(char{ 'A' }, v, storage);
        check("plain_char_writes_i_member_65", v.i == 65);

        // char16_t (Java char width) -> .i (sizeof 2, integral), zero-extended.
        pack_one(char16_t{ 0x4E2D }, v, storage);
        check("char16_writes_i_member_zero_extended", v.i == 0x4E2D);

        // uint8_t max -> .i == 255 (zero-extended), int8_t min -> .i == -128.
        pack_one(std::uint8_t{ 0xFF }, v, storage);
        check("uint8_max_writes_i_member_255", v.i == 255);
        pack_one(std::int8_t{ -128 }, v, storage);
        check("int8_min_writes_i_member_minus_128", v.i == -128);

        // int16 boundary values -> .i sign-extended.
        pack_one(std::int16_t{ -32768 }, v, storage);
        check("int16_min_writes_i_member_minus_32768", v.i == -32768);
        pack_one(std::int16_t{ 32767 }, v, storage);
        check("int16_max_writes_i_member_32767", v.i == 32767);

        // uint16 max -> .i == 65535 (zero-extended; the unsigned-16 source is
        // widened to int32, NOT sign-extended).
        pack_one(std::uint16_t{ 0xFFFF }, v, storage);
        check("uint16_max_writes_i_member_65535", v.i == 65535);
    }

    // ---- The out.j=0 full-width clear: a narrow write leaves high bits zero --
    // convert_jni_arg writes out.j=0 BEFORE the member store, so a 4-byte .i
    // write over the zeroed cell leaves the upper 4 bytes zero.  Reading the
    // whole 8-byte cell back as .j therefore yields the ZERO-EXTENDED low word,
    // even for a negative .i — the high bits are not sign-filled (the int32
    // store only touches 4 bytes).  (Little-endian: low word == the .i value's
    // bit pattern, high word == 0.)
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        pack_one(std::int32_t{ -1 }, v, storage);
        check("int32_minus_one_leaves_high_word_zero",
              v.j == static_cast<std::int64_t>(0x00000000FFFFFFFFLL));  // 4294967295
        check("int32_minus_one_l_is_zero_extended_pointer",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xFFFFFFFFull)));

        pack_one(std::int32_t{ 1 }, v, storage);
        check("int32_one_high_word_zero", v.j == 1);

        // bool false over a pre-dirtied cell clears the whole width (already
        // asserted via .l==nullptr elsewhere; pin via .j here for completeness).
        v.j = static_cast<std::int64_t>(0xFFFFFFFFFFFFFFFFULL);
        {
            bool needs_release{ true };
            vmhook::detail::write_jni_arg_to_slot(v, storage, needs_release, false);
        }
        check("bool_false_clears_full_width_j_is_zero", v.j == 0);
    }

    // ---- float / double occupy the documented members with a clean high word -
    // float writes .f (low 4 bytes) over the zeroed cell, so reading .j back is
    // the float's IEEE-754 bit pattern in the low word with a zero high word.
    // 1.0f == 0x3F800000; 2.0f == 0x40000000.  This pins both the member choice
    // AND the full-width clear for the 4-byte float path.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        pack_one(float{ 1.0f }, v, storage);
        check("float_one_in_f_member", v.f == 1.0f);
        check("float_one_low_word_is_ieee_bits_high_zero",
              v.j == static_cast<std::int64_t>(0x000000003F800000LL));

        pack_one(float{ 2.0f }, v, storage);
        check("float_two_low_word_is_ieee_bits_high_zero",
              v.j == static_cast<std::int64_t>(0x0000000040000000LL));

        // double writes the full 8-byte .d member; 1.0 == 0x3FF0000000000000.
        pack_one(double{ 1.0 }, v, storage);
        check("double_one_in_d_member", v.d == 1.0);
        check("double_one_full_width_ieee_bits",
              v.j == static_cast<std::int64_t>(0x3FF0000000000000LL));
    }

    // ---- int64 boundary values land in .j at full width ---------------------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        pack_one(std::int64_t{ -9223372036854775807LL - 1 }, v, storage);
        check("int64_min_in_j_member",
              v.j == (std::int64_t{ -9223372036854775807LL } - 1));
        pack_one(std::int64_t{ 9223372036854775807LL }, v, storage);
        check("int64_max_in_j_member", v.j == 9223372036854775807LL);
        check("int64_max_not_tagged_for_release",
              pack_one(std::int64_t{ 9223372036854775807LL }, v, storage) == false);

        // uint64 max -> .j == -1 (bit-reinterpret); .l aliases the all-ones ptr.
        pack_one(std::uint64_t{ 0xFFFFFFFFFFFFFFFFULL }, v, storage);
        check("uint64_max_in_j_member_is_minus_one", v.j == -1);
        check("uint64_max_l_is_all_ones_pointer",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xFFFFFFFFFFFFFFFFULL)));
    }

    // ---- object-arg branch: storage indirection for several pointer values --
    // For an object_base-derived arg the core writes storage = get_instance()
    // and points value.l at &storage (the synthetic-handle indirection), and
    // leaves needs_release false.  Verify across distinct instance pointers,
    // including a null instance (storage becomes null, .l still points at the
    // storage cell — NOT null).
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (std::uintptr_t raw : { std::uintptr_t{ 0x1000u },
                                    std::uintptr_t{ 0xDEADBEEFu },
                                    std::uintptr_t{ 0x7FFFFFFFFFFFFFFFull } })
        {
            fake_object obj{ reinterpret_cast<void*>(raw) };
            const bool rel{ pack_one(obj, v, storage) };
            check("object_arg_not_tagged_for_release", rel == false);
            check("object_arg_l_points_at_storage_cell", v.l == static_cast<void*>(&storage));
            check("object_arg_storage_holds_instance", storage == obj.get_instance());
            check("object_arg_storage_matches_raw",
                  storage == reinterpret_cast<void*>(raw));
        }

        // Null-instance object: storage becomes null, but value.l still points
        // at &storage (a non-null cell), and needs_release stays false.
        fake_object null_obj{ nullptr };
        const bool rel{ pack_one(null_obj, v, storage) };
        check("null_object_arg_not_tagged_for_release", rel == false);
        check("null_object_arg_storage_is_null", storage == nullptr);
        check("null_object_arg_l_still_points_at_storage",
              v.l == static_cast<void*>(&storage));
    }

    // ---- vector path parity for the EXTRA types -----------------------------
    // make_jni_args over uint32 / char16 / uint8 / int8 / uint16 / uint64 plus
    // a c-string: every primitive tag must be 0 and the union members must land
    // exactly as the single-slot path placed them.  The c-string is non-null
    // but NewStringUTF returns null w/o JVM, so its tag is 0 and .l is null.
    {
        std::vector<void*> object_handles{};
        std::vector<char>  needs_release{};
        const char* cstr{ "still_no_jvm" };
        std::vector<vmhook::detail::jni_value> values{
            vmhook::detail::make_jni_args(
                object_handles, needs_release,
                std::uint32_t{ 0xFFFFFFFFu },
                char16_t{ 0x4E2D },
                std::uint8_t{ 0xFF },
                std::int8_t{ -128 },
                std::uint16_t{ 0xFFFF },
                std::uint64_t{ 0xFFFFFFFFFFFFFFFFULL },
                cstr)
        };

        check("extra_vector_value_count", values.size() == 7);
        check("extra_vector_tag_count", needs_release.size() == 7);

        bool all_zero{ true };
        for (const char tag : needs_release) { if (tag != 0) { all_zero = false; } }
        check("extra_vector_no_slot_tagged_for_release", all_zero);

        check("extra_vector_uint32_in_i_member", values[0].i == -1);
        check("extra_vector_char16_in_i_member", values[1].i == 0x4E2D);
        check("extra_vector_uint8_in_i_member", values[2].i == 255);
        check("extra_vector_int8_in_i_member", values[3].i == -128);
        check("extra_vector_uint16_in_i_member", values[4].i == 65535);
        check("extra_vector_uint64_in_j_member", values[5].j == -1);
        check("extra_vector_cstring_l_null_no_jvm", values[6].l == nullptr);
    }

    // #####################################################################
    // ##  EXHAUSTIVE EXPANSION (additive).  Every assertion below is      ##
    // ##  derived directly from convert_jni_arg (vmhook.hpp:10681) — the  ##
    // ##  single source of truth both write_jni_arg_to_slot and           ##
    // ##  append_jni_arg delegate to — and jni_signature_for_arg          ##
    // ##  (vmhook.hpp:10530), the parallel descriptor builder that MUST   ##
    // ##  agree with the packer.  No live JVM: every string arm returns   ##
    // ##  null (jni_new_string_utf has no env), so needs_release stays     ##
    // ##  false on every string path.  Targets are little-endian (the     ##
    // ##  whole CI matrix), matching the file's existing union-aliasing   ##
    // ##  assertions; the union write-member-vs-narrow-read claims below  ##
    // ##  are explicitly LE.                                              ##
    // #####################################################################

    // =====================================================================
    // SECTION A — convert_jni_arg CORE drives identically to the wrappers.
    // The two wrappers (write_jni_arg_to_slot / append_jni_arg) are thin
    // shims over convert_jni_arg.  Drive the core DIRECTLY (poisoning all
    // out-params first) and confirm it produces the same member writes and
    // tags.  A divergence between core and wrapper would surface here.
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // Core overwrites the poisoned cell: bool false -> full-width clear.
        check("core_bool_false_clears_full_width",
              (pack_one_core(false, v, storage), v.j) == 0);
        check("core_bool_false_tag_false",
              pack_one_core(false, v, storage) == false);
        check("core_bool_true_writes_z",
              (pack_one_core(true, v, storage), v.z) == true);

        // Core int32 / int64 / float / double member writes.
        check("core_int32_writes_i", (pack_one_core(std::int32_t{ 0x0BADF00D }, v, storage), v.i) == 0x0BADF00D);
        check("core_int64_writes_j", (pack_one_core(std::int64_t{ 0x0123456789ABCDEFLL }, v, storage), v.j) == 0x0123456789ABCDEFLL);
        check("core_float_writes_f", (pack_one_core(float{ 6.25f }, v, storage), bits_eq(v.f, 6.25f)));
        check("core_double_writes_d", (pack_one_core(double{ -123.5 }, v, storage), bits_eq(v.d, -123.5)));

        // Core overwrites the poisoned tag for a primitive (must become false).
        check("core_primitive_overwrites_poisoned_tag",
              pack_one_core(std::int32_t{ 7 }, v, storage) == false);

        // Core object arm: storage <- get_instance(), out.l = &storage.  The
        // poisoned storage value must be replaced by the instance pointer.
        fake_object obj{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5151'5151)) };
        check("core_object_tag_false", pack_one_core(obj, v, storage) == false);
        check("core_object_storage_is_instance", storage == obj.get_instance());
        check("core_object_l_points_at_storage", v.l == static_cast<void*>(&storage));

        // Core null c-string -> Java null, no release.
        const char* nul{ nullptr };
        check("core_null_cstring_tag_false", pack_one_core(nul, v, storage) == false);
        check("core_null_cstring_l_null", v.l == nullptr);

        // Core string (no JVM) -> .l null, tag false.
        check("core_std_string_tag_false_no_jvm", pack_one_core(std::string{ "x" }, v, storage) == false);
        check("core_std_string_l_null_no_jvm", v.l == nullptr);
    }

    // =====================================================================
    // SECTION B — full per-type union-member table.  ONE block per C++ type
    // asserting (1) the documented member is written, AND (2) reading the
    // OTHER members back yields the LE-aliased view of the same cell (no
    // truncation / wrong-width store).  convert_jni_arg writes out.j=0
    // first, so for every NARROW write the high bytes of the 8-byte cell are
    // zero and .j is the zero-extended bit pattern of the narrow store.
    // =====================================================================

    // ---- bool -> .z ; both true and false ; .l aliases 1 / 0 --------------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        pack_one(true, v, storage);
        check("B_bool_true_z", v.z == true);
        check("B_bool_true_j_is_one", v.j == 1);
        check("B_bool_true_i_low_is_one", v.i == 1);
        check("B_bool_true_b_low_is_one", v.b == std::int8_t{ 1 });
        check("B_bool_true_l_is_one_ptr",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)));

        pack_one(false, v, storage);
        check("B_bool_false_z", v.z == false);
        check("B_bool_false_j_zero", v.j == 0);
        check("B_bool_false_l_null", v.l == nullptr);
    }

    // ---- int8 -> .i (NOT .b) ; sign-extends ; .b low byte aliases ---------
    // The packer routes int8 through .i (the 32-bit member) even though the
    // declared descriptor is "B".  On little-endian .b reads back the low
    // byte of that int32 store, which equals the original value.  This is the
    // LE-aliasing invariant the byte/short/char JNI descriptor relies on.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::int8_t src : { std::int8_t{ 0 }, std::int8_t{ 1 }, std::int8_t{ -1 },
                                       std::int8_t{ 127 }, std::int8_t{ -128 },
                                       static_cast<std::int8_t>(0x80), static_cast<std::int8_t>(0x7F) })
        {
            pack_one(src, v, storage);
            check("B_int8_in_i_sign_extended", v.i == static_cast<std::int32_t>(src));
            check("B_int8_low_byte_b_aliases_src", v.b == src);
            check("B_int8_high_word_zero", (v.j >> 32) == 0);
        }
    }

    // ---- uint8 -> .i ; zero-extends ; .b low byte is the raw bit pattern --
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::uint8_t src : { std::uint8_t{ 0 }, std::uint8_t{ 1 }, std::uint8_t{ 0x7F },
                                        std::uint8_t{ 0x80 }, std::uint8_t{ 0xFF } })
        {
            pack_one(src, v, storage);
            check("B_uint8_in_i_zero_extended", v.i == static_cast<std::int32_t>(src));
            check("B_uint8_i_is_nonnegative", v.i >= 0 && v.i <= 255);
            check("B_uint8_low_byte_b_bit_pattern",
                  v.b == static_cast<std::int8_t>(src));
        }
    }

    // ---- int16 -> .i (NOT .s) ; sign-extends ; .s low half aliases --------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::int16_t src : { std::int16_t{ 0 }, std::int16_t{ 1 }, std::int16_t{ -1 },
                                        std::int16_t{ 32767 }, std::int16_t{ -32768 },
                                        static_cast<std::int16_t>(0x8000), static_cast<std::int16_t>(0x1234) })
        {
            pack_one(src, v, storage);
            check("B_int16_in_i_sign_extended", v.i == static_cast<std::int32_t>(src));
            check("B_int16_low_half_s_aliases_src", v.s == src);
            check("B_int16_high_word_zero", (v.j >> 32) == 0);
        }
    }

    // ---- uint16 -> .i ; zero-extends ; .c (jchar member) low half aliases -
    // uint16 maps to the Java `char` descriptor "C"; the packer stores it in
    // .i but the .c union member (also uint16) reads back the same code unit.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::uint16_t src : { std::uint16_t{ 0 }, std::uint16_t{ 1 }, std::uint16_t{ 0x7FFF },
                                         std::uint16_t{ 0x8000 }, std::uint16_t{ 0xFFFF },
                                         std::uint16_t{ 0x4E2D } /* CJK 中 */ })
        {
            pack_one(src, v, storage);
            check("B_uint16_in_i_zero_extended", v.i == static_cast<std::int32_t>(src));
            check("B_uint16_i_is_nonnegative", v.i >= 0 && v.i <= 0xFFFF);
            check("B_uint16_c_member_aliases_src", v.c == src);
            check("B_uint16_high_word_zero", (v.j >> 32) == 0);
        }
    }

    // ---- int32 -> .i verbatim ; boundary + sign-bit values ----------------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::int32_t src : { std::int32_t{ 0 }, std::int32_t{ 1 }, std::int32_t{ -1 },
                                        (std::numeric_limits<std::int32_t>::max)(),
                                        (std::numeric_limits<std::int32_t>::min)(),
                                        static_cast<std::int32_t>(0x8000'0000),
                                        static_cast<std::int32_t>(0x7FFF'FFFF),
                                        std::int32_t{ -123456789 } })
        {
            pack_one(src, v, storage);
            check("B_int32_in_i_verbatim", v.i == src);
            // LE: low word of the 8-byte cell == the int32 bit pattern,
            // high word == 0 (the narrow store does not sign-fill the cell).
            check("B_int32_low_word_is_bits",
                  static_cast<std::uint32_t>(v.j & 0xFFFFFFFFLL) == static_cast<std::uint32_t>(src));
            check("B_int32_high_word_zero", (v.j >> 32) == 0);
        }
    }

    // ---- uint32 -> .i (bit-reinterpret) ; high-bit values flip sign -------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::uint32_t src : { std::uint32_t{ 0 }, std::uint32_t{ 1 }, std::uint32_t{ 0x7FFFFFFFu },
                                         std::uint32_t{ 0x80000000u }, std::uint32_t{ 0xFFFFFFFFu },
                                         std::uint32_t{ 0xDEADBEEFu } })
        {
            pack_one(src, v, storage);
            check("B_uint32_in_i_bit_reinterpret",
                  v.i == static_cast<std::int32_t>(src));
            check("B_uint32_high_word_zero", (v.j >> 32) == 0);
        }
    }

    // ---- int64 -> .j verbatim at full width ; boundary + sign-bit ---------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::int64_t src : { std::int64_t{ 0 }, std::int64_t{ 1 }, std::int64_t{ -1 },
                                        (std::numeric_limits<std::int64_t>::max)(),
                                        (std::numeric_limits<std::int64_t>::min)(),
                                        static_cast<std::int64_t>(0x8000'0000'0000'0000ULL),
                                        static_cast<std::int64_t>(0x0000'0001'0000'0000LL) /* >32 bits */,
                                        static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL) })
        {
            pack_one(src, v, storage);
            check("B_int64_in_j_verbatim", v.j == src);
            // The full 64 bits must survive (no truncation to 32): a value
            // with set high bits must read back identically through .l too.
            check("B_int64_l_aliases_full_width",
                  v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(src)));
        }
    }

    // ---- uint64 -> .j (bit-reinterpret) ; full 64-bit preservation --------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        for (const std::uint64_t src : { std::uint64_t{ 0 }, std::uint64_t{ 1 },
                                         std::uint64_t{ 0x7FFFFFFFFFFFFFFFULL },
                                         std::uint64_t{ 0x8000000000000000ULL },
                                         std::uint64_t{ 0xFFFFFFFFFFFFFFFFULL },
                                         std::uint64_t{ 0x0000'0001'0000'0000ULL } })
        {
            pack_one(src, v, storage);
            check("B_uint64_in_j_bit_reinterpret",
                  v.j == static_cast<std::int64_t>(src));
        }
    }

    // ---- float -> .f ; representative + edge IEEE-754 values --------------
    // bits_eq compares the IEEE-754 bit pattern so NaN and signed zero are
    // distinguished.  The high 4 bytes of the cell stay zero (narrow store
    // over the j=0 clear), so reading .j yields the float bits in the low
    // word with a zero high word.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        const float floats[]{ 0.0f, -0.0f, 1.0f, -1.0f, 3.5f,
                              (std::numeric_limits<float>::min)(),
                              (std::numeric_limits<float>::max)(),
                              (std::numeric_limits<float>::lowest)(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::denorm_min() };
        for (const float src : floats)
        {
            pack_one(src, v, storage);
            check("B_float_in_f_bit_exact", bits_eq(v.f, src));
            std::uint32_t fbits{ 0 };
            std::memcpy(&fbits, &src, sizeof fbits);
            check("B_float_low_word_is_ieee_bits",
                  static_cast<std::uint32_t>(v.j & 0xFFFFFFFFLL) == fbits);
            check("B_float_high_word_zero", (v.j >> 32) == 0);
        }
        // -0.0f must not be flattened to +0.0f by the packer (sign bit kept).
        pack_one(-0.0f, v, storage);
        check("B_float_negative_zero_sign_bit_kept",
              (static_cast<std::uint32_t>(v.j & 0xFFFFFFFFLL) & 0x80000000u) != 0u);
    }

    // ---- double -> .d ; representative + edge IEEE-754 values -------------
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        const double doubles[]{ 0.0, -0.0, 1.0, -1.0, 2.71828,
                               (std::numeric_limits<double>::min)(),
                               (std::numeric_limits<double>::max)(),
                               (std::numeric_limits<double>::lowest)(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::denorm_min() };
        for (const double src : doubles)
        {
            pack_one(src, v, storage);
            check("B_double_in_d_bit_exact", bits_eq(v.d, src));
            std::uint64_t dbits{ 0 };
            std::memcpy(&dbits, &src, sizeof dbits);
            check("B_double_full_width_is_ieee_bits",
                  static_cast<std::uint64_t>(v.j) == dbits);
        }
        pack_one(-0.0, v, storage);
        check("B_double_negative_zero_sign_bit_kept",
              (static_cast<std::uint64_t>(v.j) & 0x8000000000000000ULL) != 0ULL);
    }

    // =====================================================================
    // SECTION C — char-family integral types (char / signed char / unsigned
    // char / char16_t / char32_t / wchar_t).  All are integral so they route
    // through the size-based integral arms of convert_jni_arg: sizeof<=4 ->
    // .i, sizeof==8 -> .j.  These document flaw #3 at the PACKER layer (they
    // pack fine; the SIGNATURE builder is checked separately in Section E).
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // plain char (distinct type, sizeof 1) -> .i.  Char sign-ness is
        // implementation-defined; convert_jni_arg's static_cast<int32_t>
        // follows it, so compare against that same cast (not a hardcoded int).
        for (const char src : { char{ 'A' }, char{ '\0' }, char{ '\x7F' },
                                static_cast<char>('\x80'), static_cast<char>('\xFF') })
        {
            pack_one(src, v, storage);
            check("C_plain_char_in_i", v.i == static_cast<std::int32_t>(src));
            check("C_plain_char_tag_false", pack_one(src, v, storage) == false);
        }

        // signed char / unsigned char (distinct from int8_t/uint8_t on some
        // toolchains, identical on others — either way sizeof 1 -> .i).
        pack_one(static_cast<signed char>(-100), v, storage);
        check("C_signed_char_in_i", v.i == -100);
        pack_one(static_cast<unsigned char>(200), v, storage);
        check("C_unsigned_char_in_i", v.i == 200);

        // char16_t (sizeof 2) -> .i, zero-extended (it is an unsigned type).
        for (const char16_t src : { char16_t{ 0 }, char16_t{ u'A' }, char16_t{ 0x4E2D },
                                    char16_t{ 0xFFFF }, char16_t{ 0xD83D } /* high surrogate */ })
        {
            pack_one(src, v, storage);
            check("C_char16_in_i_zero_extended", v.i == static_cast<std::int32_t>(src));
            check("C_char16_i_nonnegative", v.i >= 0 && v.i <= 0xFFFF);
        }

        // char32_t (sizeof 4) -> .i as a Java int (NEVER jchar).  A
        // supplementary code point (> 0xFFFF) survives as a 32-bit value.
        for (const char32_t src : { char32_t{ 0 }, char32_t{ U'A' }, char32_t{ 0x4E2D },
                                    char32_t{ 0x1F600 } /* emoji, > BMP */,
                                    char32_t{ 0x10FFFF } /* max code point */ })
        {
            pack_one(src, v, storage);
            check("C_char32_in_i_as_int", v.i == static_cast<std::int32_t>(src));
        }
        // The supplementary code point really kept its high bits (not masked
        // to 16): 0x1F600 != its low 16 bits 0xF600.
        pack_one(char32_t{ 0x1F600 }, v, storage);
        check("C_char32_supplementary_not_masked_to_16",
              v.i == 0x1F600 && v.i != 0xF600);

        // wchar_t: sizeof varies by platform (2 on Windows/MSVC-ABI, 4 on
        // most Unix).  Either way it is integral and <= 4 bytes -> .i.  We
        // therefore branch the expected member on sizeof at compile time so
        // the assertion is correct on every CI target.
        {
            const wchar_t src{ L'Z' };
            pack_one(src, v, storage);
            if constexpr (sizeof(wchar_t) <= sizeof(std::int32_t))
            {
                check("C_wchar_in_i", v.i == static_cast<std::int32_t>(src));
            }
            else
            {
                check("C_wchar_in_j", v.j == static_cast<std::int64_t>(src));
            }
            check("C_wchar_tag_false", pack_one(src, v, storage) == false);
        }
    }

    // =====================================================================
    // SECTION D — 8-byte integral types that are NOT spelled int64_t/uint64_t
    // (long / unsigned long / size_t / ptrdiff_t / intmax_t).  The packer's
    // generic `integral && sizeof==8 -> .j` arm accepts ANY 8-byte integral,
    // so these pack to .j regardless of their exact spelling.  (The SIGNATURE
    // builder is stricter — see Section E / flaw #2 — but PACKING is generic.)
    // On targets where `long`/size_t are 4 bytes (e.g. Windows LLP64 `long`)
    // they route through .i instead; branch on sizeof so the assertion holds.
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        {
            const long src{ -1L };
            pack_one(src, v, storage);
            if constexpr (sizeof(long) == sizeof(std::int64_t))
            {
                check("D_long_in_j", v.j == static_cast<std::int64_t>(src));
            }
            else
            {
                check("D_long_in_i", v.i == static_cast<std::int32_t>(src));
            }
        }
        {
            const unsigned long src{ 0xFFFFFFFFFFFFFFFFULL & static_cast<unsigned long>(-1) };
            pack_one(src, v, storage);
            if constexpr (sizeof(unsigned long) == sizeof(std::int64_t))
            {
                check("D_ulong_in_j", v.j == static_cast<std::int64_t>(src));
            }
            else
            {
                check("D_ulong_in_i", v.i == static_cast<std::int32_t>(src));
            }
        }
        {
            const long long src{ 0x0123456789ABCDEFLL };
            pack_one(src, v, storage);
            check("D_longlong_in_j", v.j == static_cast<std::int64_t>(src)); // long long is always 8B here
        }
        {
            const std::size_t src{ static_cast<std::size_t>(0xDEADBEEFCAFEull) };
            pack_one(src, v, storage);
            if constexpr (sizeof(std::size_t) == sizeof(std::int64_t))
            {
                check("D_size_t_in_j", v.j == static_cast<std::int64_t>(src));
            }
            else
            {
                check("D_size_t_in_i", v.i == static_cast<std::int32_t>(src));
            }
        }
        {
            const std::ptrdiff_t src{ static_cast<std::ptrdiff_t>(-987654321) };
            pack_one(src, v, storage);
            if constexpr (sizeof(std::ptrdiff_t) == sizeof(std::int64_t))
            {
                check("D_ptrdiff_in_j", v.j == static_cast<std::int64_t>(src));
            }
            else
            {
                check("D_ptrdiff_in_i", v.i == static_cast<std::int32_t>(src));
            }
        }
        // All of the above are primitives -> never tagged for release.
        check("D_long_not_tagged", pack_one(long{ 5 }, v, storage) == false);
        check("D_size_t_not_tagged", pack_one(std::size_t{ 5 }, v, storage) == false);
    }

    // =====================================================================
    // SECTION E — jni_signature_for_arg vs the packer (cross-consistency).
    // The descriptor builder MUST agree with the packer about which Java
    // type each C++ arg becomes.  These assertions document the sub-int
    // descriptor map (flaw #1: int8/uint8->B, int16->S, uint16->C while the
    // packer widens all into .i), and that 64-bit / 32-bit / fp / string /
    // bool descriptors match the union member the packer used.
    // =====================================================================
    {
        // Primitive descriptors — the canonical fixed-width spellings.
        check("E_sig_bool_Z",   sig<bool>() == "Z");
        check("E_sig_int8_B",   sig<std::int8_t>() == "B");
        check("E_sig_uint8_B",  sig<std::uint8_t>() == "B");
        check("E_sig_int16_S",  sig<std::int16_t>() == "S");
        check("E_sig_uint16_C", sig<std::uint16_t>() == "C");
        check("E_sig_int32_I",  sig<std::int32_t>() == "I");
        check("E_sig_uint32_I", sig<std::uint32_t>() == "I"); // sizeof==4 generic arm
        check("E_sig_int64_J",  sig<std::int64_t>() == "J");
        check("E_sig_uint64_J", sig<std::uint64_t>() == "J");
        check("E_sig_float_F",  sig<float>() == "F");
        check("E_sig_double_D", sig<double>() == "D");

        // String family -> Ljava/lang/String; (every spelling).
        check("E_sig_std_string_String",  sig<std::string>() == "Ljava/lang/String;");
        check("E_sig_string_view_String", sig<std::string_view>() == "Ljava/lang/String;");
        check("E_sig_const_char_String",  sig<const char*>() == "Ljava/lang/String;");
        check("E_sig_char_ptr_String",    sig<char*>() == "Ljava/lang/String;");

        // decay: cv / ref qualified spellings collapse to the same descriptor.
        check("E_sig_decays_const_ref_int", sig<const std::int32_t&>() == "I");
        check("E_sig_decays_int_rref",      sig<std::int64_t&&>() == "J");
        check("E_sig_decays_const_string",  sig<const std::string&>() == "Ljava/lang/String;");

        // PAIRED INVARIANT (flaw #1): the SAME sub-int type whose descriptor
        // is B/S/C is PACKED into .i by the packer, and on little-endian the
        // narrow union member (.b/.s/.c) reads back the truncated value the
        // JVM would read from the slot via the descriptor.  This pins the
        // load-bearing LE-aliasing assumption end to end (sans JVM).
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        pack_one(std::int8_t{ -2 }, v, storage); // descriptor "B"
        check("E_int8_packs_i_but_b_reads_descriptor_value",
              sig<std::int8_t>() == "B" && v.i == -2 && v.b == std::int8_t{ -2 });

        pack_one(std::int16_t{ -3 }, v, storage); // descriptor "S"
        check("E_int16_packs_i_but_s_reads_descriptor_value",
              sig<std::int16_t>() == "S" && v.i == -3 && v.s == std::int16_t{ -3 });

        pack_one(std::uint16_t{ 0xABCD }, v, storage); // descriptor "C"
        check("E_uint16_packs_i_but_c_reads_descriptor_value",
              sig<std::uint16_t>() == "C" && v.i == 0xABCD && v.c == std::uint16_t{ 0xABCD });

        // int64 / uint64 both pack to .j and both describe as "J".
        pack_one(std::int64_t{ -5 }, v, storage);
        check("E_int64_packs_j_and_sig_J", sig<std::int64_t>() == "J" && v.j == -5);
        pack_one(std::uint64_t{ 0xFFFFFFFFFFFFFFFFULL }, v, storage);
        check("E_uint64_packs_j_and_sig_J", sig<std::uint64_t>() == "J" && v.j == -1);

        // float/double pack to .f/.d and describe as F/D.
        pack_one(float{ 9.0f }, v, storage);
        check("E_float_packs_f_and_sig_F", sig<float>() == "F" && bits_eq(v.f, 9.0f));
        pack_one(double{ 9.0 }, v, storage);
        check("E_double_packs_d_and_sig_D", sig<double>() == "D" && bits_eq(v.d, 9.0));
    }

    // =====================================================================
    // SECTION F — object / unique_ptr arg signature derivation (no JVM).
    // jni_signature_for_arg reads vmhook::type_to_class_map (a plain inline
    // map) with NO JVM dependency.  Register a wrapper by inserting directly
    // (register_class<T>() needs find_class -> a live JVM).  Covers: the
    // registered -> Lpkg/Name; branch, and the UNREGISTERED -> fallback
    // Ljava/lang/Object; branch, for both unique_ptr<T> and by-value T.
    // =====================================================================
    {
        // Unregistered first: fake_object is never inserted into the map, so
        // both the by-value and unique_ptr signature arms fall back to
        // Ljava/lang/Object; (the deliberate non-static_assert fallback).
        check("F_sig_unregistered_object_falls_back",
              sig<fake_object>() == "Ljava/lang/Object;");
        check("F_sig_unregistered_unique_ptr_falls_back",
              sig<std::unique_ptr<fake_object>>() == "Ljava/lang/Object;");

        // Now register registered_wrapper -> "com/example/Widget" directly in
        // the public map and assert the L...; descriptor resolves for BOTH
        // the by-value object arm and the unique_ptr arm.
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(registered_wrapper) }, std::string{ "com/example/Widget" });

        check("F_sig_registered_object_Lname",
              sig<registered_wrapper>() == "Lcom/example/Widget;");
        check("F_sig_registered_unique_ptr_Lname",
              sig<std::unique_ptr<registered_wrapper>>() == "Lcom/example/Widget;");
        // cv/ref qualified spellings decay to the same descriptor.
        check("F_sig_registered_const_ref_Lname",
              sig<const registered_wrapper&>() == "Lcom/example/Widget;");
    }

    // =====================================================================
    // SECTION G — object / unique_ptr PACKING arms (no JVM).  The packer
    // writes storage = get_instance() (or nullptr for a null unique_ptr) and
    // points value.l at &storage, leaving needs_release false.  Covers the
    // null unique_ptr branch (10727) which the original test never exercised.
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // Non-null unique_ptr<fake_object>: storage holds the instance, .l
        // points at storage, no release.
        {
            auto up{ std::make_unique<fake_object>(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4242'4242))) };
            void* const expected_instance{ up->get_instance() };
            const bool rel{ pack_one(std::move(up), v, storage) };
            check("G_unique_ptr_tag_false", rel == false);
            check("G_unique_ptr_storage_is_instance", storage == expected_instance);
            check("G_unique_ptr_l_points_at_storage", v.l == static_cast<void*>(&storage));
            check("G_unique_ptr_storage_matches_raw",
                  storage == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4242'4242)));
        }

        // NULL unique_ptr: storage becomes nullptr, but value.l STILL points
        // at &storage (a non-null cell — NOT null).  This is the
        // `arg ? arg->get_instance() : nullptr` branch.
        {
            std::unique_ptr<fake_object> null_up{};
            const bool rel{ pack_one(std::move(null_up), v, storage) };
            check("G_null_unique_ptr_tag_false", rel == false);
            check("G_null_unique_ptr_storage_is_null", storage == nullptr);
            check("G_null_unique_ptr_l_still_points_at_storage",
                  v.l == static_cast<void*>(&storage));
        }

        // unique_ptr to the REGISTERED wrapper packs identically (the packing
        // arm does not consult the class map — only the signature builder
        // does).  Non-null and null both behave as above.
        {
            auto up{ std::make_unique<registered_wrapper>(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x9999'8888))) };
            void* const inst{ up->get_instance() };
            check("G_registered_unique_ptr_tag_false",
                  pack_one(std::move(up), v, storage) == false);
            check("G_registered_unique_ptr_storage_is_instance", storage == inst);
            check("G_registered_unique_ptr_l_points_at_storage",
                  v.l == static_cast<void*>(&storage));
        }
    }

    // =====================================================================
    // SECTION H — string args: null / empty / non-empty, every spelling,
    // WITHOUT a JVM.  Deterministic facts: a null const char* -> Java null
    // (value.l == nullptr, no release); every OTHER string spelling (incl
    // empty "" and embedded-NUL std::string) routes through the length-counted
    // UTF-16 encoder jni_new_string_utf16_local (NewString, slot 163 — NOT the
    // legacy NewStringUTF), which has no env so returns null -> value.l ==
    // nullptr, tag false.  (With a JVM the non-null cases flip to a real local
    // ref + release; that is JVM-integration territory.)  Pins the
    // convert_jni_arg null/empty contract in vmhook.hpp:12874-12903.  The
    // CONTENT each non-null spelling would encode to (interior NULs preserved,
    // astral scalars -> surrogate pairs) is pinned at the byte level — without
    // a JVM — by the pure-decoder Sections R/S below.
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // const char*: null vs empty vs non-empty.
        const char* null_c{ nullptr };
        check("H_null_const_char_tag_false", pack_one(null_c, v, storage) == false);
        check("H_null_const_char_l_null", v.l == nullptr);

        const char* empty_c{ "" };
        check("H_empty_const_char_tag_false_no_jvm", pack_one(empty_c, v, storage) == false);
        check("H_empty_const_char_l_null_no_jvm", v.l == nullptr);

        const char* nonempty_c{ "data" };
        check("H_nonempty_const_char_tag_false_no_jvm", pack_one(nonempty_c, v, storage) == false);
        check("H_nonempty_const_char_l_null_no_jvm", v.l == nullptr);

        // char* (non-const) — distinct overload spelling, same behaviour.
        char mutable_buf[]{ "mut" };
        char* mutable_c{ mutable_buf };
        check("H_char_ptr_tag_false_no_jvm", pack_one(mutable_c, v, storage) == false);
        check("H_char_ptr_l_null_no_jvm", v.l == nullptr);
        char* null_mutable{ nullptr };
        check("H_null_char_ptr_tag_false", pack_one(null_mutable, v, storage) == false);
        check("H_null_char_ptr_l_null", v.l == nullptr);

        // std::string: empty, non-empty, embedded NUL.  Without a JVM all
        // collapse to .l==null/tag false (NewString has no env).  The library
        // no longer truncates at the interior NUL (it uses the counted UTF-16
        // path, not NewStringUTF) — Section S pins that lossless decode for the
        // exact forwarded bytes without needing a live JVM.
        check("H_empty_std_string_tag_false_no_jvm",
              pack_one(std::string{}, v, storage) == false);
        check("H_empty_std_string_l_null_no_jvm", v.l == nullptr);
        check("H_nonempty_std_string_tag_false_no_jvm",
              pack_one(std::string{ "hello" }, v, storage) == false);
        check("H_embedded_nul_std_string_tag_false_no_jvm",
              pack_one(std::string{ "a\0b", 3 }, v, storage) == false);
        check("H_embedded_nul_std_string_l_null_no_jvm", v.l == nullptr);

        // std::string_view: empty, non-empty, view over embedded NUL.
        check("H_empty_string_view_tag_false_no_jvm",
              pack_one(std::string_view{}, v, storage) == false);
        check("H_empty_string_view_l_null_no_jvm", v.l == nullptr);
        check("H_nonempty_string_view_tag_false_no_jvm",
              pack_one(std::string_view{ "view" }, v, storage) == false);
        check("H_nul_string_view_tag_false_no_jvm",
              pack_one(std::string_view{ "x\0y", 3 }, v, storage) == false);
        check("H_nul_string_view_l_null_no_jvm", v.l == nullptr);
    }

    // =====================================================================
    // SECTION I — boolean canonicalisation (CHARACTERIZATION).  The JNI
    // contract is jboolean in {0,1}.  convert_jni_arg's bool arm does
    // `out.z = arg` with NO canonicalisation — it stores whatever byte the
    // C++ bool object holds.  For a bool produced by the normal C++ object
    // model (static_cast / comparison) that byte is already 0 or 1, so the
    // happy path is contract-clean.  But for a bool whose object representation
    // is a NON-{0,1} "trap" byte (reachable in practice when a bool is
    // memcpy'd / type-punned from foreign or uninitialised memory), the raw
    // byte flows straight through to the union cell — the packer does NOT
    // clamp it to {0,1}.  These assertions PIN the observed behaviour (and
    // surface the non-canonical-jboolean audit note at the end of this file).
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // A bool made from a non-zero integer is canonical true; cell == 1.
        const bool from_two{ static_cast<bool>(2) };
        pack_one(from_two, v, storage);
        check("I_bool_from_two_is_true", v.z == true);
        check("I_bool_from_two_cell_is_canonical_one", v.j == 1);

        const bool from_zero{ static_cast<bool>(0) };
        pack_one(from_zero, v, storage);
        check("I_bool_from_zero_is_false", v.z == false);
        check("I_bool_from_zero_cell_is_zero", v.j == 0);

        // A bool whose underlying byte is 0xFF (type-punned): the packer
        // stores the RAW 0xFF byte into the cell — it is NOT canonicalised to
        // 1.  The j=0 full-width clear means the high 7 bytes are zero, so the
        // whole cell reads back as 0x00000000000000FF.  This is a deliberate
        // characterization of the no-clamp bool arm (see end-of-file note):
        // a non-canonical jboolean (0xFF) would be handed to the JVM verbatim.
        unsigned char raw{ 0xFF };
        bool aliased{};
        std::memcpy(&aliased, &raw, sizeof aliased);
        pack_one(aliased, v, storage);
        // pack_one's source `aliased` is a bool object holding the non-canonical
        // byte 0xFF; the lvalue-to-rvalue read of it inside the packer is UNDEFINED
        // BEHAVIOUR, so the resulting LOW byte is compiler-/codegen-defined and
        // genuinely UNPREDICTABLE — not even constrained to {0xFF, 1}: MinGW after
        // an unrelated codegen shift produced a third value (CI runs 27409143538 /
        // 27409860975 both flipped purely from a distant header edit).  Asserting
        // ANY specific low-byte value from a UB read can only flake across
        // toolchains, so the low byte is deliberately NOT asserted.  The one
        // WELL-DEFINED, portable property is that the packer wrote into the LOW
        // cell only and left the HIGH 7 bytes zero (no garbage, no wide write).
        check("I_bool_from_0xFF_byte_high_bytes_zero", (v.j >> 8) == 0);
    }

    // =====================================================================
    // SECTION J — make_jni_args ARITY: 0, 1, and a wide mixed pack.  The heap
    // path imposes no arity bound (flaw #6); confirm it produces matching
    // value/tag counts for an empty pack, a single arg, and a large mixed
    // pack, with the object_handles vector pre-reserved so &back() is stable.
    // =====================================================================
    {
        // --- zero args: empty vectors, no crash --------------------------
        {
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};
            std::vector<vmhook::detail::jni_value> values{
                vmhook::detail::make_jni_args(object_handles, needs_release) };
            check("J_zero_args_empty_values", values.empty());
            check("J_zero_args_empty_tags", needs_release.empty());
            check("J_zero_args_empty_handles", object_handles.empty());
        }

        // --- single arg --------------------------------------------------
        {
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};
            std::vector<vmhook::detail::jni_value> values{
                vmhook::detail::make_jni_args(object_handles, needs_release,
                                              std::int32_t{ 0x1337 }) };
            check("J_single_arg_value_count", values.size() == 1);
            check("J_single_arg_tag_count", needs_release.size() == 1);
            check("J_single_arg_value", values[0].i == 0x1337);
            check("J_single_arg_tag_zero", needs_release[0] == 0);
        }

        // --- 12-arg mixed pack: covers more slots than call_jni's cap (8) -
        // The heap path has no static cap, so this is legal here.  Verify
        // every slot lands in the right member, in order (a transposition
        // or off-by-one would surface immediately).
        {
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};
            std::vector<vmhook::detail::jni_value> values{
                vmhook::detail::make_jni_args(
                    object_handles, needs_release,
                    bool{ true },                                              // 0 .z
                    std::int8_t{ -1 },                                         // 1 .i
                    std::uint8_t{ 0xFF },                                      // 2 .i
                    std::int16_t{ -2 },                                        // 3 .i
                    std::uint16_t{ 0xBEEF },                                   // 4 .i
                    std::int32_t{ 0x01020304 },                                // 5 .i
                    std::uint32_t{ 0xFFFFFFFFu },                              // 6 .i
                    std::int64_t{ 0x1122334455667788LL },                      // 7 .j
                    std::uint64_t{ 0xFFFFFFFFFFFFFFFFULL },                    // 8 .j
                    float{ 1.5f },                                             // 9 .f
                    double{ -2.5 },                                            // 10 .d
                    char16_t{ 0x4E2D }) };                                     // 11 .i

            check("J_wide_pack_value_count", values.size() == 12);
            check("J_wide_pack_tag_count", needs_release.size() == 12);

            check("J_wide_0_bool", values[0].z == true);
            check("J_wide_1_int8", values[1].i == -1);
            check("J_wide_2_uint8", values[2].i == 255);
            check("J_wide_3_int16", values[3].i == -2);
            check("J_wide_4_uint16", values[4].i == static_cast<std::int32_t>(0xBEEF));
            check("J_wide_5_int32", values[5].i == 0x01020304);
            check("J_wide_6_uint32", values[6].i == -1);
            check("J_wide_7_int64", values[7].j == 0x1122334455667788LL);
            check("J_wide_8_uint64", values[8].j == -1);
            check("J_wide_9_float", bits_eq(values[9].f, 1.5f));
            check("J_wide_10_double", bits_eq(values[10].d, -2.5));
            check("J_wide_11_char16", values[11].i == 0x4E2D);

            bool all_zero{ true };
            for (const char tag : needs_release) { if (tag != 0) { all_zero = false; } }
            check("J_wide_pack_no_release_tags", all_zero);
            // No object args -> object_handles must be empty.
            check("J_wide_pack_no_object_handles", object_handles.empty());
        }
    }

    // =====================================================================
    // SECTION K — make_jni_args with OBJECT args interleaved among
    // primitives + strings.  Every object slot's .l must point INTO
    // object_handles (not at a primitive's aliased bits), and the reserve
    // must keep those pointers stable across the whole pack (regression
    // guard for the realloc-dangling .l class — make_jni_args pre-reserves
    // object_handles to sizeof...(args), so push_back never reallocates).
    // =====================================================================
    {
        std::vector<void*> object_handles{};
        std::vector<char>  needs_release{};

        fake_object o0{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xAAAA0000)) };
        fake_object o1{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xBBBB0000)) };
        fake_object o2{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xCCCC0000)) };

        // Interleave: obj, int, obj, string(null-w/o-JVM), long, obj.
        std::vector<vmhook::detail::jni_value> values{
            vmhook::detail::make_jni_args(
                object_handles, needs_release,
                o0,                                  // 0 object
                std::int32_t{ 0x11111111 },          // 1 primitive
                o1,                                  // 2 object
                std::string{ "no_jvm" },             // 3 string -> .l null
                std::int64_t{ 0x2222222233333333LL },// 4 primitive
                o2) };                               // 5 object

        check("K_interleaved_value_count", values.size() == 6);
        check("K_interleaved_handle_count", object_handles.size() == 3);

        // Primitives land in their members untouched by the object re-homing.
        check("K_interleaved_int_in_i", values[1].i == 0x11111111);
        check("K_interleaved_long_in_j", values[4].j == 0x2222222233333333LL);
        check("K_interleaved_string_l_null", values[3].l == nullptr);

        // Each object slot's .l points INTO object_handles, and the pointed-to
        // cell holds the right instance.  Because object_handles was reserved
        // to sizeof...(args) (=6), &back() never moved during the pack, so the
        // earlier slots' .l are still valid (the realloc-dangling guard).
        auto* const base{ object_handles.data() };
        auto in_handles = [&](void* p) -> bool {
            return p >= static_cast<void*>(base)
                && p <  static_cast<void*>(base + object_handles.size());
        };
        check("K_obj0_l_points_into_handles", in_handles(values[0].l));
        check("K_obj1_l_points_into_handles", in_handles(values[2].l));
        check("K_obj2_l_points_into_handles", in_handles(values[5].l));

        // The three handle cells hold the three instances, in order.
        check("K_handle0_is_o0", object_handles[0] == o0.get_instance());
        check("K_handle1_is_o1", object_handles[1] == o1.get_instance());
        check("K_handle2_is_o2", object_handles[2] == o2.get_instance());

        // Dereferencing each object slot's .l yields the instance OOP (the
        // synthetic-handle indirection: .l -> &handle_cell -> instance).
        check("K_obj0_deref_is_instance",
              *static_cast<void**>(values[0].l) == o0.get_instance());
        check("K_obj1_deref_is_instance",
              *static_cast<void**>(values[2].l) == o1.get_instance());
        check("K_obj2_deref_is_instance",
              *static_cast<void**>(values[5].l) == o2.get_instance());

        // The earliest object slot's .l must NOT have been left dangling at a
        // stale, since-reallocated address: it must equal &object_handles[0].
        check("K_obj0_l_is_exactly_handle0_addr",
              values[0].l == static_cast<void*>(&object_handles[0]));
        check("K_obj2_l_is_exactly_handle2_addr",
              values[5].l == static_cast<void*>(&object_handles[2]));

        // No string built a local ref (no JVM) -> zero release tags overall.
        bool all_zero{ true };
        for (const char tag : needs_release) { if (tag != 0) { all_zero = false; } }
        check("K_interleaved_no_release_tags", all_zero);
    }

    // =====================================================================
    // SECTION L — append_jni_arg (single-append heap path) parity with the
    // stack path.  append_jni_arg appends ONE value+tag and, for object args,
    // re-homes the handle into object_handles and re-points .l at &back().
    // Confirm the per-call append for a primitive, a string (null w/o JVM),
    // and an object behaves consistently with convert_jni_arg/pack_one.
    // =====================================================================
    {
        // primitive append
        {
            std::vector<vmhook::detail::jni_value> values{};
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};
            object_handles.reserve(1);
            vmhook::detail::append_jni_arg(values, object_handles, needs_release,
                                           std::int64_t{ 0x7777'8888'9999'AAAALL });
            check("L_append_primitive_one_value", values.size() == 1);
            check("L_append_primitive_in_j", values[0].j == 0x7777'8888'9999'AAAALL);
            check("L_append_primitive_tag_zero", needs_release.size() == 1 && needs_release[0] == 0);
            check("L_append_primitive_no_handle", object_handles.empty());
        }

        // object append: handle re-homed into object_handles, .l == &back().
        {
            std::vector<vmhook::detail::jni_value> values{};
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};
            object_handles.reserve(1);
            fake_object obj{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1357'2468)) };
            vmhook::detail::append_jni_arg(values, object_handles, needs_release, obj);
            check("L_append_object_one_value", values.size() == 1);
            check("L_append_object_one_handle", object_handles.size() == 1);
            check("L_append_object_handle_is_instance",
                  object_handles[0] == obj.get_instance());
            check("L_append_object_l_points_at_back",
                  values[0].l == static_cast<void*>(&object_handles.back()));
            check("L_append_object_tag_zero", needs_release[0] == 0);
        }

        // null unique_ptr append: handle cell is null, .l points at &back().
        {
            std::vector<vmhook::detail::jni_value> values{};
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};
            object_handles.reserve(1);
            std::unique_ptr<fake_object> null_up{};
            vmhook::detail::append_jni_arg(values, object_handles, needs_release,
                                           std::move(null_up));
            check("L_append_null_unique_ptr_handle_pushed", object_handles.size() == 1);
            check("L_append_null_unique_ptr_handle_is_null", object_handles[0] == nullptr);
            check("L_append_null_unique_ptr_l_points_at_back",
                  values[0].l == static_cast<void*>(&object_handles.back()));
            check("L_append_null_unique_ptr_tag_zero", needs_release[0] == 0);
        }

        // string append (no JVM): .l null, tag zero, no handle pushed.
        {
            std::vector<vmhook::detail::jni_value> values{};
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};
            vmhook::detail::append_jni_arg(values, object_handles, needs_release,
                                           std::string{ "no_jvm" });
            check("L_append_string_l_null_no_jvm", values[0].l == nullptr);
            check("L_append_string_tag_zero", needs_release[0] == 0);
            check("L_append_string_no_handle", object_handles.empty());
        }
    }

    // =====================================================================
    // SECTION M — convert_jni_arg never tags a primitive/object whose aliased
    // .l bits look like a "real" pointer.  This is the union-aliasing footgun
    // generalised across MANY bit patterns: any primitive whose value happens
    // to alias a plausible heap pointer MUST still report needs_release ==
    // false (otherwise the cleanup loop DeleteLocalRef's garbage).  Drives
    // the CORE directly to prove the tag decision is value-independent.
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // 64-bit patterns that alias non-null pointers — none may be tagged.
        const std::int64_t ptr_like[]{
            static_cast<std::int64_t>(0x0000'7FFF'FFFF'0000ULL), // user-space-ish
            static_cast<std::int64_t>(0x0000'0000'0040'1000ULL), // small code addr
            static_cast<std::int64_t>(0xFFFF'8000'0000'0000ULL), // kernel-ish
            static_cast<std::int64_t>(0x1)                       // tiny non-null
        };
        bool all_false{ true };
        for (const std::int64_t pat : ptr_like)
        {
            if (pack_one_core(pat, v, storage)) { all_false = false; }
            // And .l really does alias a non-null pointer for these.
            check("M_int64_l_aliases_nonnull",
                  v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(pat)));
        }
        check("M_no_ptr_like_int64_tagged_for_release", all_false);

        // 32-bit pointer-like patterns via .i (LE low word) — also untagged.
        check("M_int32_ptr_like_not_tagged",
              pack_one_core(std::int32_t{ static_cast<std::int32_t>(0x00401000) }, v, storage) == false);
    }

    // =====================================================================
    // SECTION N — make_jni_args storage-vector independence.  The caller may
    // pass NON-empty object_handles / needs_release vectors; make_jni_args
    // reserve()s (does not clear) and appends.  Confirm the RETURNED values
    // vector is sized to the arg count (it is freshly created inside), and
    // the tags it appends match.  (Guards against an accidental dependence on
    // the caller pre-clearing the scratch vectors.)
    // =====================================================================
    {
        std::vector<void*> object_handles{};
        std::vector<char>  needs_release{};
        // Pre-populate the scratch vectors with junk to ensure make_jni_args
        // does not assume they start empty for the VALUES sizing (values is
        // created fresh internally, so its size == arg count regardless).
        object_handles.push_back(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEAD)));
        needs_release.push_back(char{ 1 });

        const std::size_t handles_before{ object_handles.size() };
        const std::size_t tags_before{ needs_release.size() };

        std::vector<vmhook::detail::jni_value> values{
            vmhook::detail::make_jni_args(object_handles, needs_release,
                                          std::int32_t{ 5 }, double{ 1.0 }, bool{ false }) };

        // values is created fresh inside make_jni_args -> exactly arg count.
        check("N_values_sized_to_arg_count_only", values.size() == 3);
        check("N_values_int_in_i", values[0].i == 5);
        check("N_values_double_in_d", bits_eq(values[1].d, 1.0));
        check("N_values_bool_in_z", values[2].z == false);

        // The scratch tag vector is APPENDED to (3 new primitive tags, all 0)
        // — it grew by exactly the arg count and the pre-existing junk tag is
        // untouched at the front.
        check("N_tags_appended_not_cleared", needs_release.size() == tags_before + 3);
        check("N_preexisting_junk_tag_untouched", needs_release.front() == char{ 1 });
        bool new_tags_zero{ true };
        for (std::size_t k{ tags_before }; k < needs_release.size(); ++k)
        {
            if (needs_release[k] != 0) { new_tags_zero = false; }
        }
        check("N_new_primitive_tags_all_zero", new_tags_zero);
        // No object args -> object_handles did not grow.
        check("N_object_handles_not_grown_no_objects",
              object_handles.size() == handles_before);
    }

    // =====================================================================
    // SECTION O — COMPILE-TIME contract: type -> jvalue-member / signature
    // mapping and the non-JNI-convertible rejection.  Everything here is a
    // static_assert (zero runtime cost) split into two halves:
    //   (O1) STRUCTURAL: the jni_value union's member TYPES are exactly the
    //        widths the packer's union-member dispatch depends on.  If the
    //        union ever changed (e.g. `i` widened to int64), the packer's
    //        `out.i = static_cast<int32_t>(arg)` narrow store + the LE
    //        narrow-read aliasing the whole file relies on would silently
    //        break; this pins the layout at compile time.
    //   (O2) ACCEPTANCE: the test-local predicate jni_arg_accepted_v (a faithful
    //        mirror of convert_jni_arg's if-constexpr ladder) ACCEPTS every
    //        documented arg type and REJECTS everything else — the "compile-time
    //        rejection contract via a detector" the brief asks for.  Because the
    //        library uses a hard static_assert (not SFINAE) for the reject path,
    //        the predicate is the only way to assert rejection without forcing a
    //        deliberate build break.
    // =====================================================================
    {
        // ---- (O1) union member-type / width invariants --------------------
        // bool/int8/uint16/int16/int32/int64/float/double/void* members map to
        // the exact C++ types the packer stores into.  decltype on a union
        // member is well-defined and needs no live object.
        static_assert(sizeof(vmhook::detail::jni_value) == sizeof(void*),
                      "jni_value must stay pointer-sized (the whole union-aliasing model).");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::z), bool>,
                      "jni_value::z must be bool (jboolean slot).");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::b), std::int8_t>,
                      "jni_value::b must be int8 (jbyte slot).");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::c), std::uint16_t>,
                      "jni_value::c must be uint16 (jchar slot).");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::s), std::int16_t>,
                      "jni_value::s must be int16 (jshort slot).");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::i), std::int32_t>,
                      "jni_value::i must be int32 (jint slot) — the <=4B integral target.");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::j), std::int64_t>,
                      "jni_value::j must be int64 (jlong slot) — the 8B integral target.");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::f), float>,
                      "jni_value::f must be float (jfloat slot).");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::d), double>,
                      "jni_value::d must be double (jdouble slot).");
        static_assert(std::is_same_v<decltype(vmhook::detail::jni_value::l), void*>,
                      "jni_value::l must be void* (jobject slot).");

        // ---- (O2a) ACCEPTANCE: every documented arg type is accepted -------
        // Primitives (and their cv/ref-qualified spellings, which decay).
        static_assert(jni_arg_accepted_v<bool>, "bool must be an accepted arg.");
        static_assert(jni_arg_accepted_v<std::int8_t>, "int8 accepted.");
        static_assert(jni_arg_accepted_v<std::uint8_t>, "uint8 accepted.");
        static_assert(jni_arg_accepted_v<std::int16_t>, "int16 accepted.");
        static_assert(jni_arg_accepted_v<std::uint16_t>, "uint16 accepted.");
        static_assert(jni_arg_accepted_v<std::int32_t>, "int32 accepted.");
        static_assert(jni_arg_accepted_v<std::uint32_t>, "uint32 accepted.");
        static_assert(jni_arg_accepted_v<std::int64_t>, "int64 accepted.");
        static_assert(jni_arg_accepted_v<std::uint64_t>, "uint64 accepted.");
        static_assert(jni_arg_accepted_v<float>, "float accepted.");
        static_assert(jni_arg_accepted_v<double>, "double accepted.");
        // char-family (Section C runtime-packs these; here pin acceptance).
        static_assert(jni_arg_accepted_v<char>, "plain char accepted (integral, sizeof 1).");
        static_assert(jni_arg_accepted_v<signed char>, "signed char accepted.");
        static_assert(jni_arg_accepted_v<unsigned char>, "unsigned char accepted.");
        static_assert(jni_arg_accepted_v<char16_t>, "char16_t accepted.");
        static_assert(jni_arg_accepted_v<char32_t>, "char32_t accepted.");
        static_assert(jni_arg_accepted_v<wchar_t>, "wchar_t accepted.");
        // 8-byte non-intN spellings (Section D) — accepted via the size arm.
        static_assert(jni_arg_accepted_v<long long>, "long long accepted (8B integral).");
        static_assert(jni_arg_accepted_v<unsigned long long>, "unsigned long long accepted.");
        static_assert(jni_arg_accepted_v<std::size_t>, "size_t accepted (integral, 4 or 8B).");
        static_assert(jni_arg_accepted_v<std::ptrdiff_t>, "ptrdiff_t accepted.");
        static_assert(jni_arg_accepted_v<std::intmax_t>, "intmax_t accepted.");
        static_assert(jni_arg_accepted_v<std::uintmax_t>, "uintmax_t accepted.");
        // String family + cv/ref spellings (decay to one of the four).
        static_assert(jni_arg_accepted_v<std::string>, "std::string accepted.");
        static_assert(jni_arg_accepted_v<std::string_view>, "string_view accepted.");
        static_assert(jni_arg_accepted_v<const char*>, "const char* accepted.");
        static_assert(jni_arg_accepted_v<char*>, "char* accepted.");
        static_assert(jni_arg_accepted_v<const std::string&>, "const std::string& decays -> accepted.");
        static_assert(jni_arg_accepted_v<std::int32_t&>, "int32& decays -> accepted.");
        static_assert(jni_arg_accepted_v<const double&&>, "const double&& decays -> accepted.");
        // Object / unique_ptr (by value, ref, and smart-pointer forms).
        static_assert(jni_arg_accepted_v<fake_object>, "object_base-derived accepted.");
        static_assert(jni_arg_accepted_v<const fake_object&>, "const object& decays -> accepted.");
        static_assert(jni_arg_accepted_v<std::unique_ptr<fake_object>>, "unique_ptr<object> accepted.");
        static_assert(jni_arg_accepted_v<registered_wrapper>, "registered wrapper accepted.");

        // ---- (O2b) REJECTION: non-JNI-convertible args are NOT accepted ----
        // These are exactly the types convert_jni_arg's `else` static_assert
        // would reject at the call site.  void*, raw object pointers, aggregates
        // that are not object_base-derived, containers, scoped enums, long
        // double, function pointers, std::nullptr_t — none of them.
        static_assert(!jni_arg_accepted_v<void*>, "raw void* must be rejected (not a string/object).");
        static_assert(!jni_arg_accepted_v<int*>, "raw int* must be rejected.");
        static_assert(!jni_arg_accepted_v<fake_object*>, "raw object POINTER must be rejected (pass by value or unique_ptr).");
        static_assert(!jni_arg_accepted_v<not_an_object>, "a non-object aggregate must be rejected.");
        static_assert(!jni_arg_accepted_v<std::vector<int>>, "std::vector<int> must be rejected.");
        static_assert(!jni_arg_accepted_v<some_scoped_enum>, "scoped enum (not integral) must be rejected.");
        static_assert(!jni_arg_accepted_v<long double>, "long double (neither float nor double) must be rejected.");
        static_assert(!jni_arg_accepted_v<std::nullptr_t>, "std::nullptr_t must be rejected.");
        static_assert(!jni_arg_accepted_v<void (*)()>, "function pointer must be rejected.");
        static_assert(!jni_arg_accepted_v<std::string*>, "std::string* (pointer to string) must be rejected — only the value/view/c-string forms convert.");
        // NOTE the asymmetry, deliberately pinned: a unique_ptr<NON-object> (e.g.
        // unique_ptr<int>) is matched by convert_jni_arg's unique_ptr DISPATCH arm
        // (so the detector reports it "accepted" at the dispatch level), but the
        // arm immediately fires an INNER static_assert demanding T : object_base.
        // The detector models only the outer dispatch ladder, not the inner gate,
        // so it must report `true` here; the actual library would HARD-ERROR at
        // compile time if such an arg were ever passed.  Asserting `accepted`
        // documents exactly where the rejection happens (inner arm, not the
        // else-branch) — flipping this to `!accepted` would itself be a lie.
        static_assert(jni_arg_accepted_v<std::unique_ptr<int>>,
                      "unique_ptr<int> reaches the unique_ptr dispatch arm (accepted at the "
                      "dispatch level); its T:object_base requirement is enforced by an INNER "
                      "static_assert in convert_jni_arg, not by the outer ladder this detector "
                      "mirrors.  See the note above.");

        // A purely-compile-time check has no runtime assertion; emit one PASS so
        // the section is visible in the [PASS]/[FAIL] log and counted.
        check("O_compile_time_contract_holds", true);
    }

    // =====================================================================
    // SECTION P — extra 8-byte integral spellings + pure-object packs.
    //   (P1) intmax_t / uintmax_t pack to .j (Section D covered long / size_t /
    //        ptrdiff_t but not the *max_t spellings the brief's flaw #2 lists).
    //   (P2) make_jni_args over an ALL-object pack (no primitives): every slot
    //        re-homes into object_handles and each .l points at its own cell —
    //        a denser stress of the realloc-stability guard than the interleaved
    //        Section K (consecutive push_backs, no primitive slots in between).
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // (P1) intmax_t / uintmax_t — both 8 bytes on every CI target -> .j.
        {
            const std::intmax_t s{ static_cast<std::intmax_t>(0x0123456789ABCDEFLL) };
            pack_one(s, v, storage);
            check("P_intmax_in_j", v.j == static_cast<std::int64_t>(s));
            check("P_intmax_not_tagged", pack_one(s, v, storage) == false);

            const std::uintmax_t u{ 0xFFFFFFFFFFFFFFFFULL };
            pack_one(u, v, storage);
            check("P_uintmax_in_j_is_minus_one", v.j == -1);
        }

        // (P2) all-object make_jni_args: four objects, no primitives.
        {
            std::vector<void*> object_handles{};
            std::vector<char>  needs_release{};

            fake_object a{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x0A0A0000)) };
            fake_object b{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x0B0B0000)) };
            fake_object c{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x0C0C0000)) };
            fake_object d{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x0D0D0000)) };

            std::vector<vmhook::detail::jni_value> values{
                vmhook::detail::make_jni_args(object_handles, needs_release, a, b, c, d) };

            check("P_all_object_value_count", values.size() == 4);
            check("P_all_object_handle_count", object_handles.size() == 4);

            // Each .l points at its OWN distinct handle cell, in order, and the
            // pre-reserve kept the earliest cell's address stable to the end.
            check("P_all_object_l0_is_handle0", values[0].l == static_cast<void*>(&object_handles[0]));
            check("P_all_object_l1_is_handle1", values[1].l == static_cast<void*>(&object_handles[1]));
            check("P_all_object_l2_is_handle2", values[2].l == static_cast<void*>(&object_handles[2]));
            check("P_all_object_l3_is_handle3", values[3].l == static_cast<void*>(&object_handles[3]));

            // Cells hold the four instances, in order, and deref round-trips.
            check("P_all_object_h0", object_handles[0] == a.get_instance());
            check("P_all_object_h1", object_handles[1] == b.get_instance());
            check("P_all_object_h2", object_handles[2] == c.get_instance());
            check("P_all_object_h3", object_handles[3] == d.get_instance());
            check("P_all_object_deref3_is_d",
                  *static_cast<void**>(values[3].l) == d.get_instance());

            // Distinctness: no two object slots share a handle cell (a re-home
            // bug that pointed every .l at the same back() would fail here).
            check("P_all_object_distinct_cells",
                  values[0].l != values[1].l && values[1].l != values[2].l
                  && values[2].l != values[3].l && values[0].l != values[3].l);

            bool all_zero{ true };
            for (const char tag : needs_release) { if (tag != 0) { all_zero = false; } }
            check("P_all_object_no_release_tags", all_zero);
        }
    }

    // =====================================================================
    // SECTION Q — runtime tie-back: the accepted-type predicate (Section O)
    // is consistent with what actually packs.  For a representative arg of
    // each accepted *category* confirm convert_jni_arg writes a member (the
    // cell is touched) AND leaves needs_release false (no JVM, no real local
    // ref).  This bridges the compile-time acceptance contract to observed
    // packer behaviour without re-asserting the per-bit tables of B/C/D.
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // bool / integral<=4 / integral==8 / float / double categories: each
        // writes a non-poison member and never tags for release.  We poison
        // via the core helper (pack_one_core) so "a member was written" is
        // observable as "the poison is gone".
        check("Q_bool_category_packs", pack_one_core(true, v, storage) == false && v.z == true);
        check("Q_i4_category_packs",  pack_one_core(std::int16_t{ 9 }, v, storage) == false && v.i == 9);
        check("Q_i8_category_packs",  pack_one_core(std::int64_t{ 9 }, v, storage) == false && v.j == 9);
        check("Q_float_category_packs",  pack_one_core(float{ 9.0f }, v, storage) == false && bits_eq(v.f, 9.0f));
        check("Q_double_category_packs", pack_one_core(double{ 9.0 }, v, storage) == false && bits_eq(v.d, 9.0));

        // object category: .l re-homed to &storage, tag false, storage = inst.
        fake_object obj{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x7007'7007)) };
        check("Q_object_category_packs",
              pack_one_core(obj, v, storage) == false
              && v.l == static_cast<void*>(&storage)
              && storage == obj.get_instance());

        // string category (no JVM): .l null, tag false (the deterministic
        // no-env outcome — a real local ref + tag true is JVM-integration).
        check("Q_string_category_packs_null_no_jvm",
              pack_one_core(std::string{ "q" }, v, storage) == false && v.l == nullptr);

        // null c-string category: Java null, tag false.
        const char* nul{ nullptr };
        check("Q_null_cstring_category_packs",
              pack_one_core(nul, v, storage) == false && v.l == nullptr);
    }

    // =====================================================================
    // SECTION R — utf8_to_utf16 EXHAUSTIVE DECODE (the pure, JVM-free core
    // that decides the EXACT content of every String arg).  convert_jni_arg's
    // string arms (std::string / string_view / const char*) all route through
    // jni_new_string_utf16_local, which calls vmhook::detail::utf8_to_utf16 to
    // produce the length-counted UTF-16 code units it then hands to NewString
    // (slot 163).  Without a JVM the NewString call returns null (asserted in
    // Section H), but the DECODE itself is a pure function whose output fully
    // determines what the Java String would contain.  This is exactly the
    // "interior-NUL + astral string handling" + "exact packed bytes/value"
    // surface the packer owns; it had ZERO no-JVM coverage here (only JVM
    // modules exercised it).  Every expected code-unit list is derived
    // directly from utf8_to_utf16's body (vmhook.hpp:12522): ASCII 1:1, valid
    // 2/3/4-byte sequences -> the decoded scalar, astral (>= U+10000) -> a
    // surrogate pair, and malformed / truncated input -> U+FFFD (advance 1).
    // The in-memory unit order is host-endian-agnostic (a std::uint16_t value
    // comparison, not a byte comparison), so these are cross-platform.
    // =====================================================================
    {
        using vmhook::detail::utf8_to_utf16;

        // ---- empty input -> empty units (the NewString(nullptr,0) form) ----
        check("R_empty_decodes_empty", utf8_to_utf16(std::string_view{}).empty());
        check("R_empty_literal_decodes_empty", utf8_to_utf16(std::string_view{ "" }).empty());

        // ---- pure ASCII: each byte maps 1:1 to a BMP code unit -------------
        check("R_ascii_A", units_eq(utf8_to_utf16(std::string_view{ "A" }), { 0x41 }));
        check("R_ascii_hello",
              units_eq(utf8_to_utf16(std::string_view{ "hello" }),
                       { 'h', 'e', 'l', 'l', 'o' }));
        // Lowest and highest single-byte scalars (0x00 and 0x7F).
        check("R_ascii_del_0x7F", units_eq(utf8_to_utf16(std::string_view{ "\x7F" }), { 0x7F }));

        // ---- INTERIOR NUL: a 0x00 byte is scalar U+0000, NOT a terminator --
        // This is the heart of old flaw #4's fix: a counted decode keeps the
        // NUL as a real code unit, so "a\0b" decodes to THREE units, not one.
        check("R_interior_nul_preserved",
              units_eq(utf8_to_utf16(std::string_view{ "a\0b", 3 }), { 'a', 0x00, 'b' }));
        check("R_leading_nul_preserved",
              units_eq(utf8_to_utf16(std::string_view{ "\0z", 2 }), { 0x00, 'z' }));
        check("R_trailing_nul_preserved",
              units_eq(utf8_to_utf16(std::string_view{ "z\0", 2 }), { 'z', 0x00 }));
        check("R_only_nul_preserved",
              units_eq(utf8_to_utf16(std::string_view{ std::string(1, '\0') }), { 0x00 }));
        // A run of NULs all survive (length-counted, never truncated).
        check("R_multiple_nuls_preserved",
              units_eq(utf8_to_utf16(std::string_view{ "\0\0\0", 3 }), { 0x00, 0x00, 0x00 }));

        // ---- 2-byte UTF-8 (U+0080 .. U+07FF) -> single BMP unit -----------
        // U+00E9 'é' == C3 A9 ; U+00A2 '¢' == C2 A2 ; U+07FF (max 2-byte) ==
        // DF BF.  Derived: ((b0&0x1F)<<6) | (b1&0x3F).
        check("R_2byte_eacute_00E9",
              units_eq(utf8_to_utf16(std::string_view{ "\xC3\xA9" }), { 0x00E9 }));
        check("R_2byte_cent_00A2",
              units_eq(utf8_to_utf16(std::string_view{ "\xC2\xA2" }), { 0x00A2 }));
        check("R_2byte_max_07FF",
              units_eq(utf8_to_utf16(std::string_view{ "\xDF\xBF" }), { 0x07FF }));

        // ---- 3-byte UTF-8 (U+0800 .. U+FFFF) -> single BMP unit -----------
        // U+4E2D '中' == E4 B8 AD ; U+20AC '€' == E2 82 AC ; U+FFFF (max
        // 3-byte BMP) == EF BF BF.  Derived: ((b0&0x0F)<<12)|((b1&0x3F)<<6)|
        // (b2&0x3F).
        check("R_3byte_cjk_4E2D",
              units_eq(utf8_to_utf16(std::string_view{ "\xE4\xB8\xAD" }), { 0x4E2D }));
        check("R_3byte_euro_20AC",
              units_eq(utf8_to_utf16(std::string_view{ "\xE2\x82\xAC" }), { 0x20AC }));
        check("R_3byte_max_FFFF",
              units_eq(utf8_to_utf16(std::string_view{ "\xEF\xBF\xBF" }), { 0xFFFF }));
        // U+0800 — the lowest scalar that needs three bytes (E0 A0 80).
        check("R_3byte_min_0800",
              units_eq(utf8_to_utf16(std::string_view{ "\xE0\xA0\x80" }), { 0x0800 }));

        // ---- 4-byte UTF-8 (ASTRAL, U+10000 .. U+10FFFF) -> SURROGATE PAIR -
        // This is old flaw #5's fix: a standard 4-byte sequence becomes a
        // proper UTF-16 surrogate pair, NOT mojibake.  U+1F600 (emoji) ==
        // F0 9F 98 80.  Hand-derived pair: cp-0x10000 = 0xF600;
        // high = 0xD800 + (0xF600>>10)=0xD83D ; low = 0xDC00 + (0xF600&0x3FF)
        // = 0xDE00.
        check("R_4byte_emoji_1F600_surrogate_pair",
              units_eq(utf8_to_utf16(std::string_view{ "\xF0\x9F\x98\x80" }),
                       { 0xD83D, 0xDE00 }));
        // U+10000 — the lowest astral scalar (F0 90 80 80): pair D800 DC00.
        check("R_4byte_min_10000_surrogate_pair",
              units_eq(utf8_to_utf16(std::string_view{ "\xF0\x90\x80\x80" }),
                       { 0xD800, 0xDC00 }));
        // U+10FFFF — the maximum Unicode scalar (F4 8F BF BF): pair DBFF DFFF.
        check("R_4byte_max_10FFFF_surrogate_pair",
              units_eq(utf8_to_utf16(std::string_view{ "\xF4\x8F\xBF\xBF" }),
                       { 0xDBFF, 0xDFFF }));
        // U+1D11E (musical G clef, F0 9D 84 9E): cp-0x10000 = 0xD11E ;
        // high = 0xD800 + 0x34 = 0xD834 ; low = 0xDC00 + 0x11E = 0xDD1E.
        check("R_4byte_gclef_1D11E_surrogate_pair",
              units_eq(utf8_to_utf16(std::string_view{ "\xF0\x9D\x84\x9E" }),
                       { 0xD834, 0xDD1E }));

        // ---- mixed ASCII + multi-byte + astral, contiguous ----------------
        // "A中" + emoji: 0x41, 0x4E2D, then the D83D DE00 pair.  Proves the
        // decoder advances by the right width between code points and emits
        // the pair inline (a width-advance bug would shift everything after).
        check("R_mixed_ascii_bmp_astral",
              units_eq(utf8_to_utf16(std::string_view{ "A\xE4\xB8\xAD\xF0\x9F\x98\x80" }),
                       { 0x41, 0x4E2D, 0xD83D, 0xDE00 }));
        // Interior NUL flanked by an astral scalar — both survive together
        // (the two fixes compose: counted length keeps the NUL, decoder makes
        // the surrogate pair).  "\0" + emoji + "x".
        check("R_nul_then_astral_then_ascii",
              units_eq(utf8_to_utf16(std::string_view{ "\0\xF0\x9F\x98\x80x", 6 }),
                       { 0x00, 0xD83D, 0xDE00, 'x' }));

        // ---- MALFORMED / TRUNCATED input -> U+FFFD, advance one byte -------
        // A lone continuation byte (0x80) matches no lead-byte pattern -> the
        // cp stays the U+FFFD default and adv stays 1.
        check("R_lone_continuation_is_fffd",
              units_eq(utf8_to_utf16(std::string_view{ "\x80" }), { 0xFFFD }));
        // A 2-byte lead with NO following byte (truncated at end): the
        // (i+1)<size guard fails, so it falls through to U+FFFD, advance 1.
        check("R_truncated_2byte_lead_is_fffd",
              units_eq(utf8_to_utf16(std::string_view{ "\xC3" }), { 0xFFFD }));
        // A 3-byte lead with only one trailing byte (needs two): guard fails,
        // lead -> U+FFFD (advance 1), then the stray 0x80 -> another U+FFFD.
        check("R_truncated_3byte_lead_two_fffd",
              units_eq(utf8_to_utf16(std::string_view{ "\xE4\xB8" }), { 0xFFFD, 0xFFFD }));
        // A 4-byte lead with only two trailing bytes (needs three): guard
        // fails -> U+FFFD per byte until consumed (lead + two continuations).
        check("R_truncated_4byte_lead_three_fffd",
              units_eq(utf8_to_utf16(std::string_view{ "\xF0\x9F\x98" }),
                       { 0xFFFD, 0xFFFD, 0xFFFD }));
        // 0xFF is never a valid UTF-8 lead byte -> U+FFFD.
        check("R_invalid_FF_byte_is_fffd",
              units_eq(utf8_to_utf16(std::string_view{ "\xFF" }), { 0xFFFD }));
        // Recovery: a malformed byte does not desync the stream — a valid
        // ASCII char after a lone continuation still decodes correctly.
        check("R_recovers_after_malformed",
              units_eq(utf8_to_utf16(std::string_view{ "\x80\x41" }), { 0xFFFD, 0x41 }));

        // ---- decoded unit COUNT for an astral string (the length the packer
        // hands to NewString): one emoji is encoded as 2 UTF-16 units, so the
        // jstring length the JVM would build is 2, not 1.  Pin that count.
        check("R_astral_decodes_to_two_units",
              utf8_to_utf16(std::string_view{ "\xF0\x9F\x98\x80" }).size() == 2);
        check("R_bmp_decodes_to_one_unit",
              utf8_to_utf16(std::string_view{ "\xE4\xB8\xAD" }).size() == 1);
    }

    // =====================================================================
    // SECTION S — String-ARG byte-level tie-back (no JVM).  Section H pins the
    // no-env outcome (.l == nullptr, tag false) for every string spelling; but
    // WHAT the Java String would contain is decided by utf8_to_utf16 run over
    // the EXACT bytes convert_jni_arg forwards.  Here we tie the three string
    // arg spellings (std::string with interior NUL, std::string_view over the
    // same, const char* of an astral literal) to their expected UTF-16, so the
    // packer's "interior-NUL + astral handled losslessly" contract is pinned at
    // the byte level — the part the original suite could only mark as
    // "characterization needs a live JVM".  These remain deterministic and
    // cross-platform because they assert the PURE decode of the forwarded
    // bytes, independent of any JNIEnv.
    // =====================================================================
    {
        using vmhook::detail::utf8_to_utf16;

        // (S1) std::string with an interior NUL: the bytes a packer forwards
        // are exactly the 3-byte string; its lossless decode is { 'x',0,'y' }.
        // (Old NewStringUTF would have truncated this to "x".)
        {
            const std::string arg{ "x\0y", 3 };
            check("S_std_string_interior_nul_bytes_len3", arg.size() == 3);
            check("S_std_string_interior_nul_decodes_lossless",
                  units_eq(utf8_to_utf16(arg), { 'x', 0x00, 'y' }));
        }

        // (S2) std::string_view over an interior-NUL buffer decodes the same
        // way (the view's COUNTED length is what matters, not a terminator).
        {
            static const char buf[]{ 'p', '\0', 'q', '\0', 'r' };
            const std::string_view arg{ buf, sizeof buf };
            check("S_string_view_counted_len5", arg.size() == 5);
            check("S_string_view_interior_nuls_decodes_lossless",
                  units_eq(utf8_to_utf16(arg), { 'p', 0x00, 'q', 0x00, 'r' }));
        }

        // (S3) const char* of an astral literal: a C string has no interior
        // NUL, but its standard-UTF-8 astral bytes must still become a proper
        // surrogate pair (the packer routes const char* through the same
        // counted UTF-16 encoder, NOT NewStringUTF).  "A" + emoji.
        {
            const char* const arg{ "A\xF0\x9F\x98\x80" };
            // The packer builds std::string_view{ arg } (strlen-bounded); model
            // that exactly so the tie-back matches convert_jni_arg's behaviour.
            check("S_const_char_astral_decodes_surrogate_pair",
                  units_eq(utf8_to_utf16(std::string_view{ arg }),
                           { 0x41, 0xD83D, 0xDE00 }));
        }

        // (S4) sanity: a pure-ASCII arg is byte-identical under the UTF-16
        // path (each byte -> one BMP unit), so ASCII callers are unaffected by
        // the lossless-UTF-16 routing.
        check("S_ascii_arg_is_one_unit_per_byte",
              units_eq(utf8_to_utf16(std::string_view{ "abc" }), { 'a', 'b', 'c' }));

        // (S5) cross-check: the decoded unit count equals the Java String
        // length the JVM would report — astral arg "A"+emoji is 3 UTF-16 units
        // (1 + surrogate pair), NOT 2 code points.  Pins the length contract.
        check("S_astral_arg_unit_count_is_three",
              utf8_to_utf16(std::string_view{ "A\xF0\x9F\x98\x80" }).size() == 3);
    }

    // =====================================================================
    // SECTION T — jni_signature_for_arg GENERIC INTEGRAL LADDER (the library
    // FIX for the brief's flaws #2/#3).  The descriptor builder no longer has
    // only explicit fixed-width intN_t branches + a hard static_assert else;
    // it now classifies ANY integral by sizeof after two special cases:
    //   bool                       -> "Z"  (claimed first, before sizeof==1)
    //   char16_t / std::uint16_t   -> "C"  (claimed before the sizeof==2 arm)
    //   integral && sizeof==1      -> "B"
    //   integral && sizeof==2      -> "S"
    //   integral && sizeof==4      -> "I"
    //   integral && sizeof==8      -> "J"
    // (vmhook.hpp:12962-13002).  So plain char / signed char / unsigned char /
    // char8_t -> "B"; wchar_t -> "S" or "I" (platform sizeof); char32_t -> "I";
    // long / unsigned long / size_t / ptrdiff_t -> "J" or "I" (LP64 vs LLP64);
    // long long / intmax_t / uintmax_t -> "J".  Section E pinned only the
    // fixed-width spellings; these spellings had ZERO signature coverage.  All
    // sizeof-dependent expectations are branched at compile time so the
    // assertion is exact on every CI target.
    // =====================================================================
    {
        // char-family of width 1 -> "B" (NOT special-cased; plain char's
        // sign-ness is irrelevant to the descriptor, only its sizeof matters).
        check("T_sig_char_B",        sig<char>() == "B");
        check("T_sig_signed_char_B", sig<signed char>() == "B");
        check("T_sig_unsigned_char_B", sig<unsigned char>() == "B");
        check("T_sig_char8_B",       sig<char8_t>() == "B");

        // char16_t -> "C" (special-cased before the generic sizeof==2 arm,
        // exactly like std::uint16_t which Section E pinned).
        check("T_sig_char16_C", sig<char16_t>() == "C");

        // char32_t -> "I" (sizeof 4, integral, not a 16-bit char) — a UTF-32
        // code unit is described as Java int, never jchar.
        check("T_sig_char32_I", sig<char32_t>() == "I");

        // wchar_t: sizeof varies (2 on Windows/MSVC-ABI -> "S"; 4 on most Unix
        // -> "I").  It is integral and is NOT char16_t/uint16_t, so it falls to
        // the generic ladder, NOT the "C" special case.
        if constexpr (sizeof(wchar_t) == 2)
        {
            check("T_sig_wchar_S", sig<wchar_t>() == "S");
        }
        else
        {
            check("T_sig_wchar_I", sig<wchar_t>() == "I");
        }

        // 8-byte-or-platform integrals: long / unsigned long / size_t /
        // ptrdiff_t describe by sizeof (LP64 Unix -> "J"; LLP64 Windows `long`
        // is 4 bytes -> "I").  The brief's flaw #2 (these failing to compile)
        // is resolved by the generic ladder; pin the descriptor either way.
        if constexpr (sizeof(long) == 8)
        {
            check("T_sig_long_J",  sig<long>() == "J");
            check("T_sig_ulong_J", sig<unsigned long>() == "J");
        }
        else
        {
            check("T_sig_long_I",  sig<long>() == "I");
            check("T_sig_ulong_I", sig<unsigned long>() == "I");
        }
        if constexpr (sizeof(std::size_t) == 8)
        {
            check("T_sig_size_t_J", sig<std::size_t>() == "J");
        }
        else
        {
            check("T_sig_size_t_I", sig<std::size_t>() == "I");
        }
        if constexpr (sizeof(std::ptrdiff_t) == 8)
        {
            check("T_sig_ptrdiff_J", sig<std::ptrdiff_t>() == "J");
        }
        else
        {
            check("T_sig_ptrdiff_I", sig<std::ptrdiff_t>() == "I");
        }

        // long long / unsigned long long are 8 bytes on every CI target -> "J".
        check("T_sig_longlong_J",  sig<long long>() == "J");
        check("T_sig_ulonglong_J", sig<unsigned long long>() == "J");
        // intmax_t / uintmax_t are 8 bytes on every CI target -> "J".
        check("T_sig_intmax_J",  sig<std::intmax_t>() == "J");
        check("T_sig_uintmax_J", sig<std::uintmax_t>() == "J");

        // Re-pin the ORDER-DEPENDENT special cases against their generic-width
        // neighbours, so a future reorder that dropped bool/uint16's priority
        // would surface: bool (sizeof 1) is "Z" NOT "B"; uint16 is "C" NOT "S".
        check("T_sig_bool_is_Z_not_B", sig<bool>() == "Z" && sig<bool>() != "B");
        check("T_sig_uint16_is_C_not_S", sig<std::uint16_t>() == "C" && sig<std::uint16_t>() != "S");
        // int16_t (sizeof 2, NOT uint16/char16) takes the generic "S" arm.
        check("T_sig_int16_is_S", sig<std::int16_t>() == "S");

        // cv/ref-qualified spellings of the new types decay to the same letter.
        check("T_sig_decays_const_char_ref", sig<const char&>() == "B");
        check("T_sig_decays_char32_rref",    sig<char32_t&&>() == "I");
        check("T_sig_decays_const_longlong", sig<const long long&>() == "J");
    }

    // =====================================================================
    // SECTION U — PAIRED signature/packer consistency for the char-family and
    // platform-width integrals (the Section E pairing, extended to the types
    // Section T newly describes).  For each: the descriptor letter (Section T)
    // and the union member the packer writes (Section C / D) must agree about
    // the Java width, and on little-endian the narrow union member reads back
    // the truncated value the JVM would take from the slot via the descriptor.
    // =====================================================================
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        // char -> "B" descriptor, packed into .i; .b low byte aliases the value
        // (char's sign-ness is implementation-defined, so compare via the same
        // static_cast<int32_t> the packer applies, and read .b for the byte).
        {
            const char src{ static_cast<char>('\x80') };
            pack_one(src, v, storage);
            check("U_char_packs_i_sig_B",
                  sig<char>() == "B" && v.i == static_cast<std::int32_t>(src)
                  && v.b == static_cast<std::int8_t>(src));
        }

        // char8_t -> "B" descriptor, packed into .i, zero-extended (unsigned);
        // .b low byte holds the raw bit pattern.
        {
            const char8_t src{ char8_t{ 0xFF } };
            pack_one(src, v, storage);
            check("U_char8_packs_i_sig_B",
                  sig<char8_t>() == "B" && v.i == 0xFF
                  && v.b == static_cast<std::int8_t>(0xFF));
        }

        // char16_t -> "C" descriptor, packed into .i, zero-extended; .c (jchar
        // member, also uint16) reads back the same code unit.
        {
            const char16_t src{ char16_t{ 0xABCD } };
            pack_one(src, v, storage);
            check("U_char16_packs_i_sig_C",
                  sig<char16_t>() == "C" && v.i == 0xABCD
                  && v.c == std::uint16_t{ 0xABCD });
        }

        // char32_t -> "I" descriptor, packed into .i verbatim (a >BMP value
        // keeps its high bits — never masked to 16, never a surrogate here).
        {
            const char32_t src{ char32_t{ 0x1F600 } };
            pack_one(src, v, storage);
            check("U_char32_packs_i_sig_I",
                  sig<char32_t>() == "I" && v.i == 0x1F600);
        }

        // long long -> "J" descriptor, packed into .j at full width.
        {
            const long long src{ static_cast<long long>(0x0123456789ABCDEFLL) };
            pack_one(src, v, storage);
            check("U_longlong_packs_j_sig_J",
                  sig<long long>() == "J" && v.j == static_cast<std::int64_t>(src));
        }

        // wchar_t: the descriptor width (S/I) must match the member the packer
        // used (.i for sizeof<=4 — always true for wchar_t).  L'Z' == 0x5A.
        {
            const wchar_t src{ L'Z' };
            pack_one(src, v, storage);
            check("U_wchar_packs_i", v.i == static_cast<std::int32_t>(src));
            if constexpr (sizeof(wchar_t) == 2)
            {
                check("U_wchar_sig_S_matches_packed_i",
                      sig<wchar_t>() == "S" && v.s == static_cast<std::int16_t>(src));
            }
            else
            {
                check("U_wchar_sig_I_matches_packed_i", sig<wchar_t>() == "I");
            }
        }

        // long / size_t: the descriptor and the packed member agree about
        // whether the value is 8-byte (.j / "J") or 4-byte (.i / "I").
        {
            const long src{ -123456789L };
            pack_one(src, v, storage);
            if constexpr (sizeof(long) == 8)
            {
                check("U_long_sig_J_packs_j",
                      sig<long>() == "J" && v.j == static_cast<std::int64_t>(src));
            }
            else
            {
                check("U_long_sig_I_packs_i",
                      sig<long>() == "I" && v.i == static_cast<std::int32_t>(src));
            }
        }
    }

    // =====================================================================
    // SECTION V — utf8_to_utf16 CHARACTERIZATION of inputs the decoder accepts
    // WITHOUT validation (overlong forms, surrogate-range scalars from 3-byte
    // sequences, and the invalid lead bytes 0xC0/0xC1/0xF5..0xFF).  These pin
    // the decoder's EXACT behaviour as derived from its body (vmhook.hpp:12762):
    // it matches purely on the high-bit pattern + a following-byte length guard,
    // then arithmetically combines the payload bits — it does NOT reject
    // overlong encodings, surrogate code points, or scalars > U+10FFFF.  Every
    // expected unit list is hand-derived from the masking/shift arithmetic.
    // =====================================================================
    {
        using vmhook::detail::utf8_to_utf16;

        // ---- OVERLONG 2-byte forms are decoded, not rejected --------------
        // 0xC0 0x80 : (0x00<<6)|0x00 = 0x0000 -> single unit U+0000 (an overlong
        // NUL — the modified-UTF-8 NUL encoding — decodes to a real 0 code unit).
        check("V_overlong_C080_is_nul",
              units_eq(utf8_to_utf16(std::string_view{ "\xC0\x80", 2 }), { 0x0000 }));
        // 0xC1 0xBF : (0x01<<6)|0x3F = 0x7F -> overlong ASCII DEL, single unit.
        check("V_overlong_C1BF_is_7F",
              units_eq(utf8_to_utf16(std::string_view{ "\xC1\xBF" }), { 0x7F }));
        // 0xC0 0xBF : (0x00<<6)|0x3F = 0x3F -> '?' as a single unit.
        check("V_overlong_C0BF_is_3F",
              units_eq(utf8_to_utf16(std::string_view{ "\xC0\xBF" }), { 0x3F }));

        // ---- SURROGATE-range scalars from a 3-byte sequence are passed
        // through as a single (ill-formed) UTF-16 unit, NOT split or replaced.
        // 0xED 0xA0 0x80 : (0x0D<<12)|(0x20<<6)|0x00 = 0xD800 (lead surrogate).
        check("V_3byte_EDA080_is_D800_unit",
              units_eq(utf8_to_utf16(std::string_view{ "\xED\xA0\x80" }), { 0xD800 }));
        // 0xED 0xBF 0xBF : (0x0D<<12)|(0x3F<<6)|0x3F = 0xDFFF (trail surrogate).
        check("V_3byte_EDBFBF_is_DFFF_unit",
              units_eq(utf8_to_utf16(std::string_view{ "\xED\xBF\xBF" }), { 0xDFFF }));
        // It stays a SINGLE unit (cp < 0x10000 -> the no-surrogate-split arm).
        check("V_surrogate_scalar_is_single_unit",
              utf8_to_utf16(std::string_view{ "\xED\xA0\x80" }).size() == 1);

        // ---- INVALID lead bytes that match no length pattern -> U+FFFD ------
        // 0xC0/0xC1 with a NON-continuation follower still take the 2-byte arm
        // (the decoder checks only the 0xE0-mask + a following byte EXISTS, not
        // that it is a 0x80-0xBF continuation): 0xC0 0x41 -> (0)|(0x41&0x3F)=1.
        check("V_C0_then_A_low6_masked",
              units_eq(utf8_to_utf16(std::string_view{ "\xC0\x41" }), { 0x01 }));
        // 0xF5..0xFF: 0xF5 has (0xF5 & 0xF8) == 0xF0, so with 3 trailing bytes it
        // IS taken as a 4-byte lead -> (0x05<<18)|... .  0xF5 0x80 0x80 0x80 =
        // (0x05<<18) = 0x140000, which is >= 0x10000 -> surrogate-pair split:
        // cp-0x10000 = 0x130000; high = 0xD800 + (0x130000>>10)=0xD800+0x4C0 =
        // 0xDCC0 ; low = 0xDC00 + (0x130000 & 0x3FF)=0xDC00.  Pins that the
        // decoder does NOT clamp scalars above U+10FFFF.
        check("V_F5_4byte_overmax_surrogate_pair",
              units_eq(utf8_to_utf16(std::string_view{ "\xF5\x80\x80\x80" }),
                       { 0xDCC0, 0xDC00 }));
        // 0xFF matches none of the masks (0xFF & 0xE0 == 0xE0 but 0xFF & 0xF0 ==
        // 0xF0 and 0xFF & 0xF8 == 0xF8 != 0xF0; the 2-byte mask 0xFF & 0xE0 !=
        // 0xC0) -> stays U+FFFD, advance 1.  (Already pinned in Section R for a
        // lone 0xFF; here confirm it desyncs nothing: 0xFF then 'A'.)
        check("V_FF_then_ascii_recovers",
              units_eq(utf8_to_utf16(std::string_view{ "\xFF\x41" }), { 0xFFFD, 0x41 }));

        // ---- a 3-byte lead whose third byte is MISSING at end-of-buffer:
        // the (i+2)<size guard fails, so the lead falls through to U+FFFD and
        // advances ONE byte; the surviving middle byte 0xA0 is then a lone
        // continuation -> another U+FFFD.  (Distinct from Section R's E4 B8
        // which used different continuation bytes; same structural outcome.)
        check("V_3byte_missing_third_two_fffd",
              units_eq(utf8_to_utf16(std::string_view{ "\xE0\xA0", 2 }), { 0xFFFD, 0xFFFD }));

        // ---- BMP boundary just below astral: U+FFFF is a single unit, U+10000
        // is the first that splits.  Pin the < / >= 0x10000 branch edge.
        check("V_bmp_max_FFFF_single_unit",
              utf8_to_utf16(std::string_view{ "\xEF\xBF\xBF" }).size() == 1);
        check("V_astral_min_10000_two_units",
              utf8_to_utf16(std::string_view{ "\xF0\x90\x80\x80" }).size() == 2);

        // ---- reserve()/size relationship: an all-ASCII buffer of N bytes
        // decodes to exactly N units (each byte one unit), so the count the
        // packer hands NewString equals the byte length for ASCII.
        check("V_ascii_count_equals_byte_count",
              utf8_to_utf16(std::string_view{ "0123456789" }).size() == 10);
    }

    // =====================================================================
    // SECTION W — extra compile-time acceptance for the char-family spellings
    // exercised at runtime in Sections T/U but not asserted in Section O, plus
    // a redundant cross-check that the descriptor builder accepts every type
    // jni_arg_accepted_v reports as accepted (the two ladders share a domain).
    // Pure static_asserts (zero runtime cost) + one visible PASS.
    // =====================================================================
    {
        // char8_t / cv-qualified char spellings are accepted (integral arms).
        static_assert(jni_arg_accepted_v<char8_t>, "char8_t accepted (integral, sizeof 1).");
        static_assert(jni_arg_accepted_v<const char>, "const char decays -> accepted.");
        static_assert(jni_arg_accepted_v<char16_t&>, "char16_t& decays -> accepted.");
        static_assert(jni_arg_accepted_v<const char32_t&&>, "const char32_t&& decays -> accepted.");
        static_assert(jni_arg_accepted_v<unsigned long>, "unsigned long accepted (integral).");
        static_assert(jni_arg_accepted_v<long>, "long accepted (integral).");

        // Negative: long double is the canonical NEITHER-float-nor-double type
        // the descriptor builder's terminal static_assert rejects; the predicate
        // must agree (already in O — re-affirm here next to its positive peers).
        static_assert(!jni_arg_accepted_v<long double>, "long double rejected (not float/double).");
        // char8_t* is a pointer, not one of the four string spellings -> reject.
        static_assert(!jni_arg_accepted_v<char8_t*>, "char8_t* rejected (not const char*/char*).");
        // const char32_t* (pointer-to-char32) is NOT a string spelling -> reject.
        static_assert(!jni_arg_accepted_v<const char32_t*>, "const char32_t* rejected.");

        check("W_compile_time_char_family_acceptance_holds", true);
    }

    return failures == 0 ? 0 : 1;
}
