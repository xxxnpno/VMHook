// Standalone (no-JVM) unit test for vmhook::return_value — the handle passed as
// the first argument to every hook callback (vmhook.hpp class return_value,
// ~line 1138).  This file PROMOTES the return_value coverage that previously
// lived inside the omnibus tests/test_helpers.cpp (sections 13, 14, 18, 19, 20)
// into a dedicated, EXHAUSTIVE file and expands it to cover "every possible
// input" for the parts that are testable without a live JVM.
//
// What is in scope here (pure, in-memory, no oop / no interpreter required):
//   - return_value::set<T>(value): the 64-bit retval slot write, the
//     sign-extension branch for signed integers < 8 bytes, the memcpy path for
//     everything else (float/double/void*/unsigned/bool), and the cancel flag.
//   - return_value::set<wrapper>(nullptr): the object_base-derived null-return
//     overload that zeroes the oop slot.
//   - return_value::cancel(): the bare cancel flag.
//   - The no-frame default behaviour of caller(), stack_trace(), frame() — they
//     must return the documented empty/null defaults, never crash, when the
//     return_value was constructed with frame == nullptr.
//   - return_value::set_arg() guard / early-return paths (missing frame,
//     negative index, index above the JVM u2 max_locals bound 0xFFFF) — all of
//     which short-circuit to false before any HotSpot read.
//
// What is OUT OF SCOPE (needs a live oop / interpreter frame, covered by the
// JVM integration suite, NOT here): the actual interpreter-local mutation in
// set_arg when a frame IS present, and the saved-rbp walk in caller()/
// stack_trace() when a real interpreted frame is present.  We only assert their
// safe no-frame defaults.
//
// Every assertion below is verified against the header source — see the inline
// vmhook.hpp:<line> references.  No external test framework: a plain int main(),
// a failures counter, and a check() helper printing [PASS]/[FAIL].
#include <vmhook/vmhook.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Re-read the slot's retval bit pattern back into a value of type T.  Mirrors
// how the interpreter would reinterpret the 64-bit cell for a given return
// kind; lets us assert the memcpy path round-trips bit-for-bit.
template<typename value_type>
static auto slot_bits_as(const vmhook::hotspot::return_slot& slot) -> value_type
{
    value_type out{};
    std::memcpy(&out, &slot.retval, sizeof(out));
    return out;
}

// Fresh slot zeroed between cases so a stale retval can never mask a bug: every
// set() case proves it wrote the expected bits AND set cancel from a known
// clean baseline.
static auto reset(vmhook::hotspot::return_slot& slot) -> void
{
    slot.retval = 0;
    slot.cancel = false;
}

int main()
{
    // A synthetic object_base-derived wrapper used to select the
    // set<wrapper>(nullptr) overload (requires is_base_of_v<object_base, T>,
    // vmhook.hpp:1196-1197).  Type is documentation only — no instance is ever
    // constructed, matching the header's "we never touch an instance" contract.
    struct fake_wrapper : public vmhook::object_base {};

    // =====================================================================
    // SECTION A — set<T>: signed-integer SIGN-EXTENSION path
    // (vmhook.hpp:1165-1170).  Signed && integral && sizeof < 8  =>
    // retval = static_cast<int64_t>(value), so the upper bits carry the sign.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        // ---- int8_t across its full boundary set ----
        reset(slot);
        rv.set(std::int8_t{ -1 });
        check("set_int8_minus_one_sign_extends_to_neg1",
              slot.retval == static_cast<std::int64_t>(-1));
        check("set_int8_minus_one_sets_cancel", slot.cancel == true);

        reset(slot);
        rv.set(std::int8_t{ 0 });
        check("set_int8_zero_is_zero", slot.retval == 0);

        reset(slot);
        rv.set(std::numeric_limits<std::int8_t>::min()); // -128
        check("set_int8_min_sign_extends",
              slot.retval == static_cast<std::int64_t>(-128));

        reset(slot);
        rv.set(std::numeric_limits<std::int8_t>::max()); // +127
        check("set_int8_max_is_127", slot.retval == 127);

        reset(slot);
        rv.set(std::int8_t{ -128 });
        check("set_int8_neg128_upper_bits_all_one",
              static_cast<std::uint64_t>(slot.retval) == 0xFFFFFFFFFFFFFF80ull);

        // ---- int16_t across its full boundary set ----
        reset(slot);
        rv.set(std::int16_t{ -12345 });
        check("set_int16_neg_sign_extends",
              slot.retval == static_cast<std::int64_t>(-12345));

        reset(slot);
        rv.set(std::numeric_limits<std::int16_t>::min()); // -32768
        check("set_int16_min_sign_extends",
              slot.retval == static_cast<std::int64_t>(-32768));

        reset(slot);
        rv.set(std::numeric_limits<std::int16_t>::max()); // +32767
        check("set_int16_max_is_32767", slot.retval == 32767);

        reset(slot);
        rv.set(std::int16_t{ -1 });
        check("set_int16_minus_one_upper_bits_all_one",
              static_cast<std::uint64_t>(slot.retval) == 0xFFFFFFFFFFFFFFFFull);

        // ---- int32_t across its full boundary set ----
        reset(slot);
        rv.set(std::int32_t{ -1 });
        check("set_int32_minus_one_sign_extends",
              slot.retval == static_cast<std::int64_t>(-1));

        reset(slot);
        rv.set(std::int32_t{ 42 });
        check("set_int32_positive_unchanged", slot.retval == 42);

        reset(slot);
        rv.set(std::numeric_limits<std::int32_t>::min()); // -2147483648
        check("set_int32_min_sign_extends",
              slot.retval == static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()));
        check("set_int32_min_upper_bits_all_one",
              static_cast<std::uint64_t>(slot.retval) == 0xFFFFFFFF80000000ull);

        reset(slot);
        rv.set(std::numeric_limits<std::int32_t>::max()); // +2147483647
        check("set_int32_max_is_2147483647",
              slot.retval == static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()));

        // ---- plain `int` routes through the signed-narrow path (sizeof 4 < 8) ----
        reset(slot);
        rv.set(-7);
        check("set_plain_int_negative_sign_extends",
              slot.retval == static_cast<std::int64_t>(-7));

        // ---- `short` / `signed char` named types also sign-extend ----
        reset(slot);
        rv.set(static_cast<short>(-2));
        check("set_short_negative_sign_extends",
              slot.retval == static_cast<std::int64_t>(-2));

        reset(slot);
        rv.set(static_cast<signed char>(-3));
        check("set_signed_char_negative_sign_extends",
              slot.retval == static_cast<std::int64_t>(-3));

        // every set() must raise cancel — proven once more after the runs above
        check("set_signed_path_sets_cancel", slot.cancel == true);
    }

    // =====================================================================
    // SECTION B — set<T>: signed 64-bit int (int64_t) does NOT take the
    // narrow branch (sizeof == 8 fails the < 8 guard) and instead memcpys the
    // full 8 bytes — which for a signed value is bitwise identical anyway, so
    // negatives still round-trip.  This proves the boundary of the guard.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        reset(slot);
        rv.set(std::int64_t{ -1 });
        check("set_int64_minus_one_full_width",
              slot.retval == static_cast<std::int64_t>(-1));

        reset(slot);
        rv.set(std::numeric_limits<std::int64_t>::min());
        check("set_int64_min_roundtrip",
              slot.retval == std::numeric_limits<std::int64_t>::min());

        reset(slot);
        rv.set(std::numeric_limits<std::int64_t>::max());
        check("set_int64_max_roundtrip",
              slot.retval == std::numeric_limits<std::int64_t>::max());

        reset(slot);
        rv.set(std::int64_t{ 0x1122334455667788ll });
        check("set_int64_arbitrary_pattern_roundtrip",
              slot.retval == 0x1122334455667788ll);
        check("set_int64_sets_cancel", slot.cancel == true);
    }

    // =====================================================================
    // SECTION C — set<T>: UNSIGNED integers take the memcpy/zero-extend path
    // (the signed guard is false), so the high bit is NEVER interpreted as a
    // sign.  retval is first zeroed then the low N bytes are copied
    // (vmhook.hpp:1172-1175).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        reset(slot);
        rv.set(std::uint8_t{ 0xFF });
        check("set_uint8_max_zero_extends_to_255",
              slot.retval == static_cast<std::int64_t>(0xFFu));
        check("set_uint8_max_upper_bits_zero",
              (static_cast<std::uint64_t>(slot.retval) >> 8) == 0u);

        reset(slot);
        rv.set(std::uint8_t{ 0 });
        check("set_uint8_zero_is_zero", slot.retval == 0);

        reset(slot);
        rv.set(std::uint16_t{ 0xFFFF });
        check("set_uint16_max_zero_extends",
              slot.retval == static_cast<std::int64_t>(0xFFFFu));
        check("set_uint16_max_upper_bits_zero",
              (static_cast<std::uint64_t>(slot.retval) >> 16) == 0u);

        reset(slot);
        rv.set(std::uint32_t{ 0x80000000u });
        check("set_uint32_high_bit_no_sign_extend",
              slot.retval == static_cast<std::int64_t>(0x80000000u));
        check("set_uint32_high_bit_upper_bits_zero",
              (static_cast<std::uint64_t>(slot.retval) >> 32) == 0u);

        reset(slot);
        rv.set(std::numeric_limits<std::uint32_t>::max()); // 0xFFFFFFFF
        check("set_uint32_max_zero_extends",
              slot.retval == static_cast<std::int64_t>(0xFFFFFFFFu));
        check("set_uint32_max_upper_bits_zero",
              (static_cast<std::uint64_t>(slot.retval) >> 32) == 0u);

        reset(slot);
        rv.set(std::uint64_t{ 0xCAFEBABEDEADBEEFull });
        check("set_uint64_full_pattern_roundtrip",
              static_cast<std::uint64_t>(slot.retval) == 0xCAFEBABEDEADBEEFull);

        reset(slot);
        rv.set(std::numeric_limits<std::uint64_t>::max());
        check("set_uint64_max_all_bits_set",
              static_cast<std::uint64_t>(slot.retval) == 0xFFFFFFFFFFFFFFFFull);
        check("set_uint_path_sets_cancel", slot.cancel == true);
    }

    // =====================================================================
    // SECTION D — set<bool>: bool is integral but std::is_signed_v<bool> is
    // false, so it takes the memcpy path: retval zeroed, then 1 byte copied.
    // The slot must end up exactly 0 or 1 with NO garbage in the upper bytes,
    // even when the slot was pre-filled with all-ones (vmhook.hpp:1172-1175).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        slot.retval = static_cast<std::int64_t>(0xFFFFFFFFFFFFFF00ull);
        slot.cancel = false;
        rv.set(true);
        check("set_bool_true_is_one_clears_upper_bytes", slot.retval == 1);
        check("set_bool_true_sets_cancel", slot.cancel == true);

        slot.retval = static_cast<std::int64_t>(0xFFFFFFFFFFFFFFFFull);
        slot.cancel = false;
        rv.set(false);
        check("set_bool_false_is_zero_clears_slot", slot.retval == 0);
        check("set_bool_false_sets_cancel", slot.cancel == true);
    }

    // =====================================================================
    // SECTION E — set<char>/<char16_t>/<char32_t>/<wchar_t>: these are NOT
    // matched by the signed-integer branch the same way the fixed-width types
    // are (char's signedness is implementation-defined; char16/char32/wchar
    // are unsigned or take the memcpy path).  We assert only the
    // implementation-INDEPENDENT facts: the low byte(s) round-trip and cancel
    // is set.  (We deliberately avoid asserting sign-extension for plain
    // `char` since that is platform-dependent.)
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        reset(slot);
        rv.set(static_cast<char>('A'));
        check("set_char_low_byte_roundtrips",
              (static_cast<std::uint64_t>(slot.retval) & 0xFFu) == static_cast<unsigned char>('A'));
        check("set_char_sets_cancel", slot.cancel == true);

        reset(slot);
        rv.set(char16_t{ 0xABCD });
        check("set_char16_value_roundtrips",
              static_cast<std::uint64_t>(slot.retval & 0xFFFF) == 0xABCDu);

        reset(slot);
        rv.set(char32_t{ 0x0001F600u });
        check("set_char32_value_roundtrips",
              slot_bits_as<char32_t>(slot) == char32_t{ 0x0001F600u });

        reset(slot);
        rv.set(static_cast<wchar_t>(0x263A));
        check("set_wchar_low_bits_roundtrip",
              (static_cast<std::uint64_t>(slot.retval) & 0xFFFFu) == 0x263Au);
    }

    // =====================================================================
    // SECTION F — set<float>: 4-byte non-integer => memcpy path.  retval is
    // zeroed first so the UPPER 32 bits stay clear and the low 32 hold the IEEE
    // bit pattern.  Exhaustive boundary set incl. 0, -0, NaN, +/-inf,
    // denormal, min/max (vmhook.hpp:1172-1175).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        reset(slot);
        rv.set(3.14f);
        check("set_float_cancel", slot.cancel == true);
        check("set_float_roundtrip", slot_bits_as<float>(slot) == 3.14f);
        check("set_float_upper_32_bits_zero",
              (static_cast<std::uint64_t>(slot.retval) >> 32) == 0u);

        reset(slot);
        rv.set(0.0f);
        check("set_float_zero_roundtrip", slot_bits_as<float>(slot) == 0.0f);
        check("set_float_zero_slot_all_zero", slot.retval == 0);

        reset(slot);
        rv.set(-0.0f);
        check("set_float_neg_zero_bit_pattern",
              slot_bits_as<std::uint32_t>(slot) == 0x80000000u);
        check("set_float_neg_zero_upper_bits_zero",
              (static_cast<std::uint64_t>(slot.retval) >> 32) == 0u);

        reset(slot);
        rv.set(-1.5f);
        check("set_float_negative_roundtrip", slot_bits_as<float>(slot) == -1.5f);

        reset(slot);
        rv.set(std::numeric_limits<float>::infinity());
        check("set_float_pos_inf_roundtrip",
              std::isinf(slot_bits_as<float>(slot)) && slot_bits_as<float>(slot) > 0.0f);

        reset(slot);
        rv.set(-std::numeric_limits<float>::infinity());
        check("set_float_neg_inf_roundtrip",
              std::isinf(slot_bits_as<float>(slot)) && slot_bits_as<float>(slot) < 0.0f);

        reset(slot);
        rv.set(std::numeric_limits<float>::quiet_NaN());
        check("set_float_nan_roundtrip", std::isnan(slot_bits_as<float>(slot)));
        check("set_float_nan_upper_bits_zero",
              (static_cast<std::uint64_t>(slot.retval) >> 32) == 0u);

        reset(slot);
        rv.set(std::numeric_limits<float>::denorm_min());
        check("set_float_denorm_min_roundtrip",
              slot_bits_as<float>(slot) == std::numeric_limits<float>::denorm_min());

        reset(slot);
        rv.set(std::numeric_limits<float>::min()); // smallest positive normal
        check("set_float_min_normal_roundtrip",
              slot_bits_as<float>(slot) == std::numeric_limits<float>::min());

        reset(slot);
        rv.set(std::numeric_limits<float>::max());
        check("set_float_max_roundtrip",
              slot_bits_as<float>(slot) == std::numeric_limits<float>::max());

        reset(slot);
        rv.set(std::numeric_limits<float>::lowest());
        check("set_float_lowest_roundtrip",
              slot_bits_as<float>(slot) == std::numeric_limits<float>::lowest());
    }

    // =====================================================================
    // SECTION G — set<double>: 8-byte non-integer => memcpy path fills the
    // whole slot.  Exhaustive boundary set incl. 0, -0, NaN, +/-inf,
    // denormal, min/max (vmhook.hpp:1172-1175).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        reset(slot);
        rv.set(2.71828);
        check("set_double_cancel", slot.cancel == true);
        check("set_double_roundtrip", slot_bits_as<double>(slot) == 2.71828);

        reset(slot);
        rv.set(0.0);
        check("set_double_zero_slot_all_zero", slot.retval == 0);

        reset(slot);
        rv.set(-0.0);
        check("set_double_neg_zero_bit_pattern",
              static_cast<std::uint64_t>(slot.retval) == 0x8000000000000000ull);

        reset(slot);
        rv.set(-123.456);
        check("set_double_negative_roundtrip", slot_bits_as<double>(slot) == -123.456);

        reset(slot);
        rv.set(std::numeric_limits<double>::infinity());
        check("set_double_pos_inf_roundtrip",
              std::isinf(slot_bits_as<double>(slot)) && slot_bits_as<double>(slot) > 0.0);

        reset(slot);
        rv.set(-std::numeric_limits<double>::infinity());
        check("set_double_neg_inf_roundtrip",
              std::isinf(slot_bits_as<double>(slot)) && slot_bits_as<double>(slot) < 0.0);

        reset(slot);
        rv.set(std::numeric_limits<double>::quiet_NaN());
        check("set_double_nan_roundtrip", std::isnan(slot_bits_as<double>(slot)));

        reset(slot);
        rv.set(std::numeric_limits<double>::denorm_min());
        check("set_double_denorm_min_roundtrip",
              slot_bits_as<double>(slot) == std::numeric_limits<double>::denorm_min());

        reset(slot);
        rv.set(std::numeric_limits<double>::min());
        check("set_double_min_normal_roundtrip",
              slot_bits_as<double>(slot) == std::numeric_limits<double>::min());

        reset(slot);
        rv.set(std::numeric_limits<double>::max());
        check("set_double_max_roundtrip",
              slot_bits_as<double>(slot) == std::numeric_limits<double>::max());

        reset(slot);
        rv.set(std::numeric_limits<double>::lowest());
        check("set_double_lowest_roundtrip",
              slot_bits_as<double>(slot) == std::numeric_limits<double>::lowest());
    }

    // =====================================================================
    // SECTION H — set<void*> (raw oop pointer): 8-byte trivially-copyable,
    // memcpy path.  A high-bit ("kernel-looking") pointer must NOT be
    // sign-extended (it already fills 8 bytes) and round-trips bit-for-bit.
    // Also covers the null pointer.  (vmhook.hpp:1172-1175.)
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        reset(slot);
        void* const sentinel{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0xCAFEBABEDEADBEEFull)) };
        rv.set<void*>(sentinel);
        check("set_void_ptr_cancel", slot.cancel == true);
        check("set_void_ptr_roundtrip", slot_bits_as<void*>(slot) == sentinel);

        reset(slot);
        rv.set<void*>(nullptr);
        check("set_void_ptr_null_writes_zero", slot.retval == 0);
        check("set_void_ptr_null_sets_cancel", slot.cancel == true);

        reset(slot);
        void* const low_ptr{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000u)) };
        rv.set<void*>(low_ptr);
        check("set_void_ptr_low_roundtrip", slot_bits_as<void*>(slot) == low_ptr);
    }

    // =====================================================================
    // SECTION I — set<wrapper>(nullptr): the object_base-derived null-return
    // overload (vmhook.hpp:1196-1203).  Selected only when the type derives
    // from object_base; writes a zero oop and sets cancel REGARDLESS of any
    // garbage already in the slot.  Also proves the primitive integer path is
    // unaffected by the presence of this overload (no ambiguity).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        // Pre-fill with garbage to prove the overload zeroes it.
        slot.retval = static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull);
        slot.cancel = false;
        rv.set<fake_wrapper>(nullptr);
        check("set_wrapper_nullptr_writes_zero_oop", slot.retval == 0);
        check("set_wrapper_nullptr_sets_cancel_flag", slot.cancel == true);

        // Calling it a second time on an already-zero slot is idempotent.
        rv.set<fake_wrapper>(nullptr);
        check("set_wrapper_nullptr_idempotent", slot.retval == 0 && slot.cancel == true);

        // Sanity: the primitive integer overload still resolves cleanly and
        // sign-extends (proves the wrapper overload didn't shadow it).
        reset(slot);
        rv.set(std::int32_t{ -1 });
        check("set_primitive_unaffected_by_wrapper_overload",
              slot.retval == static_cast<std::int64_t>(-1));
    }

    // =====================================================================
    // SECTION J — cancel(): bare suppress-without-value for void methods.
    // Only flips slot.cancel; must leave retval untouched (vmhook.hpp:1205-1209).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        // Default-constructed slot starts cancel == false.
        check("slot_default_cancel_is_false", slot.cancel == false);
        check("slot_default_retval_is_zero", slot.retval == 0);

        // Seed retval so we can prove cancel() does not clobber it.
        slot.retval = static_cast<std::int64_t>(0x0123456789ABCDEFll);
        rv.cancel();
        check("cancel_sets_flag", slot.cancel == true);
        check("cancel_leaves_retval_untouched",
              slot.retval == static_cast<std::int64_t>(0x0123456789ABCDEFll));

        // cancel() is idempotent.
        rv.cancel();
        check("cancel_idempotent", slot.cancel == true);
    }

    // =====================================================================
    // SECTION K — no-frame defaults for caller() / stack_trace() / frame().
    // Constructed with frame == nullptr (the constructor default, vmhook.hpp:1141).
    // These must each return the documented empty/null result and NEVER crash
    // (caller() vmhook.hpp:7599-7605; stack_trace() vmhook.hpp:7679-7690;
    // frame() accessor at vmhook.hpp:1326-1329 returns the stored pointer).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot, /*frame=*/nullptr };

        // frame() exposes exactly the (null) pointer the ctor was handed.
        check("frame_accessor_returns_null", rv.frame() == nullptr);

        // caller(): empty caller_info with valid() == false and all fields blank.
        const auto caller{ rv.caller() };
        check("caller_no_frame_invalid", caller.valid() == false);
        check("caller_no_frame_method_null", caller.method == nullptr);
        check("caller_no_frame_class_empty", caller.class_name.empty());
        check("caller_no_frame_method_name_empty", caller.method_name.empty());
        check("caller_no_frame_signature_empty", caller.signature.empty());

        // caller() is const and side-effect free: a second call yields the same
        // empty result and does not touch the slot.
        const auto caller_again{ rv.caller() };
        check("caller_no_frame_stable_invalid", caller_again.valid() == false);
        check("caller_no_frame_does_not_touch_slot",
              slot.cancel == false && slot.retval == 0);

        // stack_trace(): empty vector for default depth, depth 0 (promoted to
        // 64 internally but still empty with no frame), and small explicit depth.
        check("stack_trace_no_frame_default_empty", rv.stack_trace().empty());
        check("stack_trace_no_frame_zero_depth_empty", rv.stack_trace(0).empty());
        check("stack_trace_no_frame_depth_1_empty", rv.stack_trace(1).empty());
        check("stack_trace_no_frame_depth_4_empty", rv.stack_trace(4).empty());
        check("stack_trace_no_frame_depth_64_empty", rv.stack_trace(64).empty());
        check("stack_trace_no_frame_huge_depth_empty",
              rv.stack_trace(std::numeric_limits<std::size_t>::max()).empty());
        check("stack_trace_no_frame_does_not_touch_slot",
              slot.cancel == false && slot.retval == 0);
    }

    // =====================================================================
    // SECTION L — set_arg() guard / early-return paths (vmhook.hpp:7808-7828).
    // With frame == nullptr EVERY call must return false before any HotSpot
    // read.  The same early-return path also rejects negative indices and any
    // index above the JVM u2 max_locals bound (0xFFFF == 65535), so we sweep
    // the full boundary of that guard.  set_arg must NOT set cancel or touch
    // retval on the guard path.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot, /*frame=*/nullptr };

        // --- missing frame: any valid-looking index still returns false ---
        check("set_arg_no_frame_index_0_false",
              rv.set_arg(0, std::int32_t{ 42 }) == false);
        check("set_arg_no_frame_index_1_false",
              rv.set_arg(1, std::int32_t{ 42 }) == false);
        check("set_arg_no_frame_index_1000_false",
              rv.set_arg(1000, std::int32_t{ 42 }) == false);

        // --- negative indices: rejected by the index < 0 clause ---
        check("set_arg_negative_index_minus1_false",
              rv.set_arg(-1, std::int32_t{ 42 }) == false);
        check("set_arg_negative_index_int_min_false",
              rv.set_arg(std::numeric_limits<std::int32_t>::min(), std::int32_t{ 42 }) == false);

        // --- max_locals boundary (0xFFFF): exactly at the bound and one past ---
        check("set_arg_index_at_max_locals_0xFFFF_false",
              rv.set_arg(0xFFFF, std::int32_t{ 42 }) == false);
        check("set_arg_index_above_max_locals_0x10000_false",
              rv.set_arg(0x10000, std::int32_t{ 42 }) == false);
        check("set_arg_index_int_max_false",
              rv.set_arg(std::numeric_limits<std::int32_t>::max(), std::int32_t{ 42 }) == false);

        // --- guard path must not mutate the slot ---
        check("set_arg_guard_does_not_set_cancel", slot.cancel == false);
        check("set_arg_guard_does_not_touch_retval", slot.retval == 0);

        // --- guard fires identically for other value_types (the frame/index
        //     check runs before any type-specific handling) ---
        check("set_arg_no_frame_int64_value_false",
              rv.set_arg(0, std::int64_t{ 7 }) == false);
        check("set_arg_no_frame_double_value_false",
              rv.set_arg(2, 3.5) == false);
        check("set_arg_no_frame_float_value_false",
              rv.set_arg(3, 1.0f) == false);
        check("set_arg_no_frame_bool_value_false",
              rv.set_arg(4, true) == false);
        check("set_arg_no_frame_after_typed_calls_slot_clean",
              slot.cancel == false && slot.retval == 0);
    }

    return failures == 0 ? 0 : 1;
}
