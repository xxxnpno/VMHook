// Standalone unit test: detail::write_jni_arg_to_slot / append_jni_arg union-member
// writes + needs_release tagging (regression guard for the union-aliasing
// DeleteLocalRef bug). No JVM present -> jni_new_string_utf returns null, so the
// needs_release tag stays false on every path here. Anything requiring a live
// oop / running JVM (the actual DeleteLocalRef cleanup loop, Call*MethodA
// dispatch, result-handle release) is covered by JVM integration in example.cpp.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
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

    return failures == 0 ? 0 : 1;
}
