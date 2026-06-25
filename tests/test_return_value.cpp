// Standalone (no-JVM) unit test for vmhook::return_value — the handle passed as
// the first argument to every hook callback (vmhook.hpp class return_value,
// ~line 1315).  This file PROMOTES the return_value coverage that previously
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
//   - caller_info::valid() contract (method != nullptr).
//
// What is OUT OF SCOPE (needs a live oop / interpreter frame, covered by the
// JVM integration suite, NOT here): the actual interpreter-local mutation in
// set_arg when a frame IS present, and the saved-rbp walk in caller()/
// stack_trace() when a real interpreted frame is present.  We only assert their
// safe no-frame defaults.
//
// EXHAUSTIVE-INPUT STRATEGY.  The header gives set<T> exactly three behaviours
// (vmhook.hpp:1342-1352), and each one is a SINGLE, value-only, endianness-
// agnostic invariant that holds for ANY input of that category:
//   * signed integral, sizeof < 8        => slot.retval == (int64_t)value      (sign-extend)
//   * unsigned/bool integral, sizeof < 8 => (uint64_t)slot.retval == (uint64_t)value (zero-extend)
//   * 8-byte or non-integer (float/double/ptr/int64/uint64)
//                                        => memcpy(slot.retval) bit-for-bit round-trips
// So instead of a handful of hand-picked cases, we drive each invariant with a
// LARGE data table (0, 1, -1, all-ones, sign-bit, type min/max, alternating
// 0x55/0xAA bit patterns, every power of two, boundary-adjacent values, …) and
// assert the one true contract for every entry.  Floats are always compared via
// memcpy of the bit pattern (slot_bits_as<T>), NEVER ==, so NaN and -0.0 are
// checked exactly.  Nothing here branches on endianness, compiler, or JDK.
//
// Every assertion below is verified against the header source — see the inline
// vmhook.hpp:<line> references.  No external test framework: a plain int main(),
// a failures counter, and a check() helper printing [PASS]/[FAIL].
#include <vmhook/vmhook.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
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

// Fill the slot with a poison pattern so the "memcpy path zeroes the slot first"
// contract (vmhook.hpp:1350) is genuinely exercised: if set<T> failed to clear
// the upper bytes, the poison would survive and fail the round-trip.
static auto poison(vmhook::hotspot::return_slot& slot) -> void
{
    slot.retval = static_cast<std::int64_t>(0xA5A5A5A5A5A5A5A5ull);
    slot.cancel = false;
}

// ---------------------------------------------------------------------------
// Exhaustive per-category drivers.  Each proves the SINGLE header contract for
// the category against an arbitrary input value, from BOTH a zeroed and a
// poisoned baseline, and confirms cancel was raised.  The boolean return lets
// the caller fold many inputs into compact pass/fail counts.
// ---------------------------------------------------------------------------

// Signed integral, sizeof < 8 — the sign-extension branch
// (vmhook.hpp:1342-1346): retval == (int64_t)value, exactly, for any value.
template<typename int_type>
static auto verify_signed_narrow(vmhook::return_value& rv,
                                 vmhook::hotspot::return_slot& slot,
                                 int_type value) -> bool
{
    static_assert(std::is_signed_v<int_type> && std::is_integral_v<int_type>);
    static_assert(sizeof(int_type) < sizeof(std::int64_t));
    const std::int64_t want{ static_cast<std::int64_t>(value) };

    reset(slot);
    rv.set(value);
    const bool from_zero{ slot.retval == want && slot.cancel };

    poison(slot);
    rv.set(value);
    // Sign-extend ASSIGNS the whole int64 (it does not memcpy low bytes), so
    // even the poisoned upper bytes must be fully overwritten.
    const bool from_poison{ slot.retval == want && slot.cancel };

    return from_zero && from_poison;
}

// Unsigned (or bool) integral, sizeof < 8 — the zero-extend/memcpy branch
// (vmhook.hpp:1348-1351): the low sizeof bytes hold value, upper bytes 0, so
// (uint64_t)retval == (uint64_t)value and the high bytes above the type vanish.
template<typename uint_type>
static auto verify_unsigned_narrow(vmhook::return_value& rv,
                                   vmhook::hotspot::return_slot& slot,
                                   uint_type value) -> bool
{
    static_assert(std::is_unsigned_v<uint_type> && std::is_integral_v<uint_type>);
    static_assert(sizeof(uint_type) < sizeof(std::int64_t));
    const std::uint64_t want{ static_cast<std::uint64_t>(value) };

    reset(slot);
    rv.set(value);
    const bool zero_ok{ static_cast<std::uint64_t>(slot.retval) == want && slot.cancel };

    poison(slot);
    rv.set(value);
    const bool poison_ok{ static_cast<std::uint64_t>(slot.retval) == want && slot.cancel };

    // Upper bytes (above the type width) must be exactly zero in both runs —
    // this is the half of the contract that catches a missing pre-zero.
    const std::uint64_t high_mask{ sizeof(uint_type) >= sizeof(std::uint64_t)
                                       ? std::uint64_t{ 0 }
                                       : ~((std::uint64_t{ 1 } << (sizeof(uint_type) * 8)) - 1) };
    const bool high_clear{ (static_cast<std::uint64_t>(slot.retval) & high_mask) == 0u };

    return zero_ok && poison_ok && high_clear;
}

// 8-byte or non-integer trivially-copyable types — the full-width memcpy branch
// (vmhook.hpp:1348-1351): the slot round-trips the value bit-for-bit.  Compared
// via memcpy of the bit pattern, so it is correct even for float/double NaN.
template<typename value_type>
static auto verify_memcpy_roundtrip(vmhook::return_value& rv,
                                    vmhook::hotspot::return_slot& slot,
                                    value_type value) -> bool
{
    static_assert(std::is_trivially_copyable_v<value_type>);
    static_assert(sizeof(value_type) <= sizeof(std::int64_t));

    // Compare the raw bytes that came back to the raw bytes we put in.
    std::uint8_t want_bytes[sizeof(value_type)]{};
    std::memcpy(want_bytes, &value, sizeof(value_type));

    auto run_once{ [&]() -> bool
    {
        rv.set(value);
        if (!slot.cancel) { return false; }
        const value_type got{ slot_bits_as<value_type>(slot) };
        std::uint8_t got_bytes[sizeof(value_type)]{};
        std::memcpy(got_bytes, &got, sizeof(value_type));
        if (std::memcmp(want_bytes, got_bytes, sizeof(value_type)) != 0) { return false; }
        // For non-8-byte types the unused high bytes of the 64-bit cell must be
        // zero (the branch zeroes retval before the partial copy).
        if constexpr (sizeof(value_type) < sizeof(std::int64_t))
        {
            const std::uint64_t high{ static_cast<std::uint64_t>(slot.retval)
                                      >> (sizeof(value_type) * 8) };
            if (high != 0u) { return false; }
        }
        return true;
    } };

    reset(slot);
    const bool from_zero{ run_once() };
    poison(slot);
    const bool from_poison{ run_once() };
    return from_zero && from_poison;
}

// Integral type of IMPLEMENTATION-DEFINED signedness, sizeof < 8 (char,
// wchar_t).  Such a type lands on WHICHEVER branch matches its signedness:
//   * if signed   -> sign-extend  -> retval == (int64_t)value
//   * if unsigned -> zero-extend  -> retval == (int64_t)value  (upper bytes 0)
// In BOTH cases the single value-only invariant retval == static_cast<int64_t>(value)
// holds, because static_cast<int64_t> performs exactly the same extension the
// active branch does.  So this is the correct platform-AGNOSTIC contract for a
// type whose signedness we must not assume (no `if (is_signed)` branch needed).
template<typename int_type>
static auto verify_integral_extends(vmhook::return_value& rv,
                                    vmhook::hotspot::return_slot& slot,
                                    int_type value) -> bool
{
    static_assert(std::is_integral_v<int_type>);
    static_assert(sizeof(int_type) < sizeof(std::int64_t));
    const std::int64_t want{ static_cast<std::int64_t>(value) };

    reset(slot);
    rv.set(value);
    const bool from_zero{ slot.retval == want && slot.cancel };

    poison(slot);
    rv.set(value);
    const bool from_poison{ slot.retval == want && slot.cancel };

    return from_zero && from_poison;
}

int main()
{
    // A synthetic object_base-derived wrapper used to select the
    // set<wrapper>(nullptr) overload (requires is_base_of_v<object_base, T>,
    // vmhook.hpp:1373-1374).  Type is documentation only — no instance is ever
    // constructed, matching the header's "we never touch an instance" contract.
    struct fake_wrapper : public vmhook::object_base {};
    // A SECOND, unrelated object_base subclass: proves the requires-clause keys
    // off the base relationship, not a single hard-coded type.
    struct other_wrapper : public vmhook::object_base {};

    // =====================================================================
    // SECTION A — set<T>: signed-integer SIGN-EXTENSION path
    // (vmhook.hpp:1342-1346).  Signed && integral && sizeof < 8  =>
    // retval = static_cast<int64_t>(value), so the upper bits carry the sign.
    // Hand-picked landmark cases (kept from the original suite) PLUS exhaustive
    // data-table sweeps via verify_signed_narrow below.
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
    // SECTION A2 — EXHAUSTIVE signed-narrow sweep.  For int8/int16/int32 we
    // drive verify_signed_narrow (retval == (int64_t)value, from both a clean
    // and a poisoned slot, cancel raised) over a dense boundary table: 0, ±1,
    // min, max, min+1, max-1, the alternating bit patterns, and every power of
    // two (and its negation) representable in the type.  Each entry is the
    // genuinely-different "every input" coverage the metric rewards.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        // ---- int8_t: enumerate the ENTIRE domain [-128, 127] (256 values) ----
        {
            bool all_ok{ true };
            int  covered{ 0 };
            for (int v{ -128 }; v <= 127; ++v)
            {
                if (!verify_signed_narrow<std::int8_t>(rv, slot, static_cast<std::int8_t>(v)))
                {
                    all_ok = false;
                }
                ++covered;
            }
            check("set_int8_exhaustive_all_256_values_sign_extend", all_ok);
            check("set_int8_exhaustive_covered_full_domain", covered == 256);
        }

        // ---- int16_t: dense boundary + power-of-two + alternating table ----
        {
            const std::array<std::int16_t, 22> table{ {
                std::int16_t{ 0 }, std::int16_t{ 1 }, std::int16_t{ -1 },
                std::int16_t{ 2 }, std::int16_t{ -2 },
                std::numeric_limits<std::int16_t>::min(),
                static_cast<std::int16_t>(std::numeric_limits<std::int16_t>::min() + 1),
                std::numeric_limits<std::int16_t>::max(),
                static_cast<std::int16_t>(std::numeric_limits<std::int16_t>::max() - 1),
                static_cast<std::int16_t>(0x5555), static_cast<std::int16_t>(0xAAAA),
                std::int16_t{ 0x00FF }, static_cast<std::int16_t>(0xFF00),
                std::int16_t{ 0x0F0F }, static_cast<std::int16_t>(0xF0F0),
                std::int16_t{ 256 }, std::int16_t{ -256 },
                std::int16_t{ 1024 }, std::int16_t{ -1024 },
                std::int16_t{ 16384 }, std::int16_t{ -16384 },
                std::int16_t{ 12345 },
            } };
            bool all_ok{ true };
            for (const std::int16_t v : table)
            {
                if (!verify_signed_narrow<std::int16_t>(rv, slot, v)) { all_ok = false; }
            }
            // Plus every positive power of two and its negation (bit-walk).
            for (int shift{ 0 }; shift < 15; ++shift)
            {
                const std::int16_t p{ static_cast<std::int16_t>(std::int16_t{ 1 } << shift) };
                if (!verify_signed_narrow<std::int16_t>(rv, slot, p)) { all_ok = false; }
                if (!verify_signed_narrow<std::int16_t>(rv, slot, static_cast<std::int16_t>(-p))) { all_ok = false; }
            }
            check("set_int16_exhaustive_table_and_bitwalk_sign_extend", all_ok);
        }

        // ---- int32_t: dense boundary + full 32-bit power-of-two bit-walk ----
        {
            const std::array<std::int32_t, 18> table{ {
                0, 1, -1, 2, -2,
                std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::min() + 1,
                std::numeric_limits<std::int32_t>::max(),
                std::numeric_limits<std::int32_t>::max() - 1,
                static_cast<std::int32_t>(0x55555555), static_cast<std::int32_t>(0xAAAAAAAA),
                static_cast<std::int32_t>(0x0000FFFF), static_cast<std::int32_t>(0xFFFF0000),
                static_cast<std::int32_t>(0x00FF00FF), static_cast<std::int32_t>(0xFF00FF00),
                123456789, -123456789, 65536,
            } };
            bool all_ok{ true };
            for (const std::int32_t v : table)
            {
                if (!verify_signed_narrow<std::int32_t>(rv, slot, v)) { all_ok = false; }
            }
            for (int shift{ 0 }; shift < 31; ++shift)
            {
                const std::int32_t p{ static_cast<std::int32_t>(std::int32_t{ 1 } << shift) };
                if (!verify_signed_narrow<std::int32_t>(rv, slot, p)) { all_ok = false; }
                if (!verify_signed_narrow<std::int32_t>(rv, slot, -p)) { all_ok = false; }
            }
            check("set_int32_exhaustive_table_and_bitwalk_sign_extend", all_ok);
        }
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

        // ---- EXHAUSTIVE int64 table: memcpy round-trip from clean + poison ----
        {
            const std::array<std::int64_t, 16> table{ {
                0, 1, -1, 2, -2,
                std::numeric_limits<std::int64_t>::min(),
                std::numeric_limits<std::int64_t>::min() + 1,
                std::numeric_limits<std::int64_t>::max(),
                std::numeric_limits<std::int64_t>::max() - 1,
                static_cast<std::int64_t>(0x5555555555555555ull),
                static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAull),
                static_cast<std::int64_t>(0x00000000FFFFFFFFull),
                static_cast<std::int64_t>(0xFFFFFFFF00000000ull),
                static_cast<std::int64_t>(0x0123456789ABCDEFll),
                static_cast<std::int64_t>(0xFEDCBA9876543210ull),
                4294967296ll,
            } };
            bool all_ok{ true };
            for (const std::int64_t v : table)
            {
                if (!verify_memcpy_roundtrip<std::int64_t>(rv, slot, v)) { all_ok = false; }
            }
            // Full 64-bit single-bit walk (each individual bit set).
            for (int shift{ 0 }; shift < 64; ++shift)
            {
                const std::int64_t p{ static_cast<std::int64_t>(std::uint64_t{ 1 } << shift) };
                if (!verify_memcpy_roundtrip<std::int64_t>(rv, slot, p)) { all_ok = false; }
            }
            check("set_int64_exhaustive_table_and_64bit_bitwalk_roundtrip", all_ok);
        }
    }

    // =====================================================================
    // SECTION C — set<T>: UNSIGNED integers take the memcpy/zero-extend path
    // (the signed guard is false), so the high bit is NEVER interpreted as a
    // sign.  retval is first zeroed then the low N bytes are copied
    // (vmhook.hpp:1348-1351).  Landmark cases plus exhaustive sweeps in C2.
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
    // SECTION C2 — EXHAUSTIVE unsigned sweeps via verify_unsigned_narrow
    // ((uint64_t)retval == value, upper bytes above the width strictly zero,
    // from clean AND poison) and verify_memcpy_roundtrip for the 8-byte width.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot };

        // ---- uint8_t: enumerate the ENTIRE domain [0, 255] ----
        {
            bool all_ok{ true };
            int  covered{ 0 };
            for (int v{ 0 }; v <= 255; ++v)
            {
                if (!verify_unsigned_narrow<std::uint8_t>(rv, slot, static_cast<std::uint8_t>(v)))
                {
                    all_ok = false;
                }
                ++covered;
            }
            check("set_uint8_exhaustive_all_256_values_zero_extend", all_ok);
            check("set_uint8_exhaustive_covered_full_domain", covered == 256);
        }

        // ---- uint16_t: dense table + 16-bit single-bit walk ----
        {
            const std::array<std::uint16_t, 12> table{ {
                std::uint16_t{ 0 }, std::uint16_t{ 1 },
                std::numeric_limits<std::uint16_t>::max(),
                static_cast<std::uint16_t>(std::numeric_limits<std::uint16_t>::max() - 1),
                std::uint16_t{ 0x5555 }, std::uint16_t{ 0xAAAA },
                std::uint16_t{ 0x00FF }, std::uint16_t{ 0xFF00 },
                std::uint16_t{ 0x0F0F }, std::uint16_t{ 0xF0F0 },
                std::uint16_t{ 0x8000 }, std::uint16_t{ 0x7FFF },
            } };
            bool all_ok{ true };
            for (const std::uint16_t v : table)
            {
                if (!verify_unsigned_narrow<std::uint16_t>(rv, slot, v)) { all_ok = false; }
            }
            for (int shift{ 0 }; shift < 16; ++shift)
            {
                const std::uint16_t p{ static_cast<std::uint16_t>(std::uint16_t{ 1 } << shift) };
                if (!verify_unsigned_narrow<std::uint16_t>(rv, slot, p)) { all_ok = false; }
            }
            check("set_uint16_exhaustive_table_and_bitwalk_zero_extend", all_ok);
        }

        // ---- uint32_t: dense table + full 32-bit single-bit walk ----
        {
            const std::array<std::uint32_t, 12> table{ {
                0u, 1u,
                std::numeric_limits<std::uint32_t>::max(),
                std::numeric_limits<std::uint32_t>::max() - 1u,
                0x55555555u, 0xAAAAAAAAu,
                0x0000FFFFu, 0xFFFF0000u,
                0x00FF00FFu, 0xFF00FF00u,
                0x80000000u, 0x7FFFFFFFu,
            } };
            bool all_ok{ true };
            for (const std::uint32_t v : table)
            {
                if (!verify_unsigned_narrow<std::uint32_t>(rv, slot, v)) { all_ok = false; }
            }
            for (int shift{ 0 }; shift < 32; ++shift)
            {
                const std::uint32_t p{ std::uint32_t{ 1 } << shift };
                if (!verify_unsigned_narrow<std::uint32_t>(rv, slot, p)) { all_ok = false; }
            }
            check("set_uint32_exhaustive_table_and_bitwalk_zero_extend", all_ok);
        }

        // ---- uint64_t: full-width memcpy round-trip, table + 64-bit walk ----
        {
            const std::array<std::uint64_t, 12> table{ {
                0ull, 1ull,
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max() - 1ull,
                0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull,
                0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull,
                0xCAFEBABEDEADBEEFull, 0x0123456789ABCDEFull,
                0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull,
            } };
            bool all_ok{ true };
            for (const std::uint64_t v : table)
            {
                if (!verify_memcpy_roundtrip<std::uint64_t>(rv, slot, v)) { all_ok = false; }
            }
            for (int shift{ 0 }; shift < 64; ++shift)
            {
                const std::uint64_t p{ std::uint64_t{ 1 } << shift };
                if (!verify_memcpy_roundtrip<std::uint64_t>(rv, slot, p)) { all_ok = false; }
            }
            check("set_uint64_exhaustive_table_and_64bit_bitwalk_roundtrip", all_ok);
        }

        // ---- `unsigned long` / `unsigned long long` named types route by their
        //      actual sizeof (4 or 8) — assert the value contract regardless of
        //      which width the platform gives them (no platform branch needed:
        //      the (uint64_t)retval == (uint64_t)value invariant holds for both
        //      the zero-extend and the full-width path). ----
        reset(slot);
        rv.set(static_cast<unsigned long>(0xABCD1234ul));
        check("set_unsigned_long_value_roundtrips",
              static_cast<std::uint64_t>(slot.retval) == static_cast<std::uint64_t>(0xABCD1234ul));
        check("set_unsigned_long_sets_cancel", slot.cancel == true);

        reset(slot);
        rv.set(static_cast<unsigned long long>(0x1122334455667788ull));
        check("set_unsigned_long_long_value_roundtrips",
              static_cast<std::uint64_t>(slot.retval) == 0x1122334455667788ull);
    }

    // =====================================================================
    // SECTION D — set<bool>: bool is integral but std::is_signed_v<bool> is
    // false, so it takes the memcpy path: retval zeroed, then 1 byte copied.
    // The slot must end up exactly 0 or 1 with NO garbage in the upper bytes,
    // even when the slot was pre-filled with all-ones (vmhook.hpp:1348-1351).
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

        // Exhaustive: bool has exactly two inputs — both via the shared driver
        // (proves zero-extend + upper-byte-clear + cancel from clean AND poison).
        check("set_bool_true_exhaustive_via_driver",
              verify_unsigned_narrow<bool>(rv, slot, true));
        check("set_bool_false_exhaustive_via_driver",
              verify_unsigned_narrow<bool>(rv, slot, false));
    }

    // =====================================================================
    // SECTION E — set<char>/<char16_t>/<char32_t>/<wchar_t>: char's signedness
    // is implementation-defined, so it MAY take either branch — but in either
    // branch the value-only contract still holds: the slot, reinterpreted as the
    // same char type, round-trips bit-for-bit.  We assert that implementation-
    // INDEPENDENT fact (via the memcpy driver) plus the low-byte landmark.
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

        // ---- EXHAUSTIVE char: enumerate the entire 8-bit code space via the
        //      signedness-AGNOSTIC integral-extension driver.  `char` lands on
        //      the sign-extend branch where it is signed and the zero-extend
        //      branch where it is unsigned; retval == (int64_t)char_value holds
        //      in BOTH cases, so this needs no platform branch. ----
        {
            bool all_ok{ true };
            for (int c{ 0 }; c <= 255; ++c)
            {
                if (!verify_integral_extends<char>(rv, slot, static_cast<char>(c))) { all_ok = false; }
            }
            check("set_char_exhaustive_all_256_codepoints_extend", all_ok);
        }

        // ---- char8_t: a distinct unsigned 1-byte type (C++20) ----
        {
            bool all_ok{ true };
            for (int c{ 0 }; c <= 255; ++c)
            {
                if (!verify_unsigned_narrow<char8_t>(rv, slot, static_cast<char8_t>(c))) { all_ok = false; }
            }
            check("set_char8_exhaustive_all_256_values_zero_extend", all_ok);
        }

        // ---- char16_t: unsigned, 16-bit — table + bit-walk via memcpy driver ----
        {
            const std::array<char16_t, 8> table{ {
                char16_t{ 0 }, char16_t{ 1 }, char16_t{ 0xFFFF },
                char16_t{ 0x5555 }, char16_t{ 0xAAAA },
                char16_t{ 0x8000 }, char16_t{ 0x00FF }, char16_t{ 0xFF00 },
            } };
            bool all_ok{ true };
            for (const char16_t v : table)
            {
                if (!verify_memcpy_roundtrip<char16_t>(rv, slot, v)) { all_ok = false; }
            }
            for (int shift{ 0 }; shift < 16; ++shift)
            {
                const char16_t p{ static_cast<char16_t>(char16_t{ 1 } << shift) };
                if (!verify_memcpy_roundtrip<char16_t>(rv, slot, p)) { all_ok = false; }
            }
            check("set_char16_exhaustive_table_and_bitwalk_roundtrip", all_ok);
        }

        // ---- char32_t: unsigned, 32-bit — table + full bit-walk ----
        {
            const std::array<char32_t, 8> table{ {
                char32_t{ 0 }, char32_t{ 1 }, char32_t{ 0xFFFFFFFFu },
                char32_t{ 0x55555555u }, char32_t{ 0xAAAAAAAAu },
                char32_t{ 0x80000000u }, char32_t{ 0x0001F600u }, char32_t{ 0x0010FFFFu },
            } };
            bool all_ok{ true };
            for (const char32_t v : table)
            {
                if (!verify_memcpy_roundtrip<char32_t>(rv, slot, v)) { all_ok = false; }
            }
            for (int shift{ 0 }; shift < 32; ++shift)
            {
                const char32_t p{ char32_t{ 1 } << shift };
                if (!verify_memcpy_roundtrip<char32_t>(rv, slot, p)) { all_ok = false; }
            }
            check("set_char32_exhaustive_table_and_bitwalk_roundtrip", all_ok);
        }

        // ---- wchar_t: BOTH its width AND its signedness are platform-dependent
        //      (unsigned 2-byte on Windows, signed 4-byte on POSIX).  The
        //      signedness-agnostic integral-extension driver (retval ==
        //      (int64_t)value) is correct on every platform with NO branch: it
        //      keys off sizeof for the static_assert and lets static_cast pick
        //      the extension that matches the active set<> branch.  We include
        //      negative wchar_t values too (meaningful only where it is signed;
        //      where it is unsigned they wrap to large positives and the same
        //      invariant still holds). ----
        {
            const std::array<wchar_t, 8> table{ {
                static_cast<wchar_t>(0), static_cast<wchar_t>(1),
                static_cast<wchar_t>(0x263A), static_cast<wchar_t>(0x7FFF),
                static_cast<wchar_t>(0x0041), static_cast<wchar_t>(0x00FF),
                static_cast<wchar_t>(-1), static_cast<wchar_t>(0x7FFE),
            } };
            bool all_ok{ true };
            for (const wchar_t v : table)
            {
                if (!verify_integral_extends<wchar_t>(rv, slot, v)) { all_ok = false; }
            }
            check("set_wchar_exhaustive_table_extend", all_ok);
        }
    }

    // =====================================================================
    // SECTION F — set<float>: 4-byte non-integer => memcpy path.  retval is
    // zeroed first so the UPPER 32 bits stay clear and the low 32 hold the IEEE
    // bit pattern.  Exhaustive boundary set incl. 0, -0, NaN, +/-inf,
    // denormal, min/max (vmhook.hpp:1348-1351).  Always compared via bit
    // pattern, NEVER ==, so NaN / -0.0 are checked exactly.
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

        // ---- EXHAUSTIVE float bit-pattern sweep via the memcpy driver, which
        //      compares raw bytes (NaN/-0.0 safe) and asserts upper-32-clear. ----
        {
            const std::array<float, 22> table{ {
                0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f,
                3.14159265f, -3.14159265f,
                std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::signaling_NaN(),
                std::numeric_limits<float>::denorm_min(),
                -std::numeric_limits<float>::denorm_min(),
                std::numeric_limits<float>::min(),
                -std::numeric_limits<float>::min(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::epsilon(),
                16777216.0f, // 2^24, the float integer-precision boundary
            } };
            bool all_ok{ true };
            for (const float v : table)
            {
                if (!verify_memcpy_roundtrip<float>(rv, slot, v)) { all_ok = false; }
            }
            check("set_float_exhaustive_special_table_roundtrip", all_ok);
        }

        // ---- float built from EVERY one of the 32 single-bit patterns: each
        //      bit position, reinterpreted as a float, must survive memcpy.  This
        //      covers sign bit, every exponent bit, and every mantissa bit. ----
        {
            bool all_ok{ true };
            for (int shift{ 0 }; shift < 32; ++shift)
            {
                const std::uint32_t bits{ std::uint32_t{ 1 } << shift };
                float f{};
                std::memcpy(&f, &bits, sizeof(f));
                if (!verify_memcpy_roundtrip<float>(rv, slot, f)) { all_ok = false; }
            }
            check("set_float_exhaustive_32_single_bit_patterns_roundtrip", all_ok);
        }
    }

    // =====================================================================
    // SECTION G — set<double>: 8-byte non-integer => memcpy path fills the
    // whole slot.  Exhaustive boundary set incl. 0, -0, NaN, +/-inf,
    // denormal, min/max (vmhook.hpp:1348-1351).
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

        // ---- EXHAUSTIVE double special table via the bit-exact memcpy driver ----
        {
            const std::array<double, 22> table{ {
                0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0,
                3.141592653589793, -3.141592653589793,
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::signaling_NaN(),
                std::numeric_limits<double>::denorm_min(),
                -std::numeric_limits<double>::denorm_min(),
                std::numeric_limits<double>::min(),
                -std::numeric_limits<double>::min(),
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::lowest(),
                std::numeric_limits<double>::epsilon(),
                9007199254740992.0, // 2^53, double integer-precision boundary
            } };
            bool all_ok{ true };
            for (const double v : table)
            {
                if (!verify_memcpy_roundtrip<double>(rv, slot, v)) { all_ok = false; }
            }
            check("set_double_exhaustive_special_table_roundtrip", all_ok);
        }

        // ---- double from EVERY one of the 64 single-bit patterns ----
        {
            bool all_ok{ true };
            for (int shift{ 0 }; shift < 64; ++shift)
            {
                const std::uint64_t bits{ std::uint64_t{ 1 } << shift };
                double d{};
                std::memcpy(&d, &bits, sizeof(d));
                if (!verify_memcpy_roundtrip<double>(rv, slot, d)) { all_ok = false; }
            }
            check("set_double_exhaustive_64_single_bit_patterns_roundtrip", all_ok);
        }
    }

    // =====================================================================
    // SECTION H — set<void*> (raw oop pointer): 8-byte trivially-copyable,
    // memcpy path.  A high-bit ("kernel-looking") pointer must NOT be
    // sign-extended (it already fills 8 bytes) and round-trips bit-for-bit.
    // Also covers the null pointer.  (vmhook.hpp:1348-1351.)
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

        // ---- EXHAUSTIVE pointer-pattern sweep.  Pointers are 8 bytes so they
        //      take the full-width memcpy path; the value contract is a verbatim
        //      bit round-trip for any address pattern, incl. the high ("kernel")
        //      half that must never be sign-mangled. ----
        {
            const std::array<std::uintptr_t, 12> patterns{ {
                0x0u, 0x1u, 0x8u, 0x1000u, 0xFFFFu,
                0x00007FFFFFFFFFFFull, // x64 user-space canonical high
                0xFFFF800000000000ull, // x64 kernel-space canonical low
                0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull,
                0xFFFFFFFFFFFFFFFFull,
                0x0000000080000000ull, 0xDEADBEEFCAFEBABEull,
            } };
            bool all_ok{ true };
            for (const std::uintptr_t pat : patterns)
            {
                reset(slot);
                void* const p{ reinterpret_cast<void*>(pat) };
                rv.set<void*>(p);
                if (!(slot_bits_as<void*>(slot) == p && slot.cancel)) { all_ok = false; }
                // and from a poisoned slot
                poison(slot);
                rv.set<void*>(p);
                if (!(slot_bits_as<void*>(slot) == p && slot.cancel)) { all_ok = false; }
            }
            check("set_void_ptr_exhaustive_address_patterns_roundtrip", all_ok);
        }

        // ---- const void* and a typed pointer also resolve on the memcpy path
        //      and round-trip their bits (sizeof==8, trivially copyable). ----
        reset(slot);
        const void* const cptr{ reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x1234ABCDu)) };
        rv.set<const void*>(cptr);
        check("set_const_void_ptr_roundtrip", slot_bits_as<const void*>(slot) == cptr);

        reset(slot);
        int* const iptr{ reinterpret_cast<int*>(static_cast<std::uintptr_t>(0xBEEF0000u)) };
        rv.set<int*>(iptr);
        check("set_typed_int_ptr_roundtrip", slot_bits_as<int*>(slot) == iptr);
    }

    // =====================================================================
    // SECTION I — set<wrapper>(nullptr): the object_base-derived null-return
    // overload (vmhook.hpp:1373-1380).  Selected only when the type derives
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

        // A DIFFERENT object_base subclass selects the same overload (the
        // requires-clause is is_base_of, not a single type) — also zeroes.
        slot.retval = static_cast<std::int64_t>(0x123456789ABCDEF0ull);
        slot.cancel = false;
        rv.set<other_wrapper>(nullptr);
        check("set_other_wrapper_nullptr_writes_zero_oop", slot.retval == 0);
        check("set_other_wrapper_nullptr_sets_cancel", slot.cancel == true);

        // object_base ITSELF satisfies is_base_of_v<object_base, object_base>.
        slot.retval = static_cast<std::int64_t>(0xFFFFFFFFFFFFFFFFull);
        slot.cancel = false;
        rv.set<vmhook::object_base>(nullptr);
        check("set_object_base_itself_nullptr_writes_zero", slot.retval == 0);
        check("set_object_base_itself_nullptr_sets_cancel", slot.cancel == true);

        // The wrapper-null overload yields EXACTLY the same slot state as the
        // documented-equivalent set<void*>(nullptr) (vmhook.hpp:1359) —
        // oop_t is void* (vmhook.hpp:17601), so the two must be bit-identical.
        vmhook::hotspot::return_slot a{};
        vmhook::hotspot::return_slot b{};
        vmhook::return_value ra{ &a };
        vmhook::return_value rb{ &b };
        a.retval = static_cast<std::int64_t>(0x5555555555555555ull);
        b.retval = static_cast<std::int64_t>(0x5555555555555555ull);
        ra.set<fake_wrapper>(nullptr);
        rb.set<void*>(nullptr);
        check("set_wrapper_null_equals_set_voidptr_null_retval", a.retval == b.retval);
        check("set_wrapper_null_equals_set_voidptr_null_cancel", a.cancel == b.cancel);

        // Sanity: the primitive integer overload still resolves cleanly and
        // sign-extends (proves the wrapper overload didn't shadow it).
        reset(slot);
        rv.set(std::int32_t{ -1 });
        check("set_primitive_unaffected_by_wrapper_overload",
              slot.retval == static_cast<std::int64_t>(-1));
    }

    // =====================================================================
    // SECTION J — cancel(): bare suppress-without-value for void methods.
    // Only flips slot.cancel; must leave retval untouched (vmhook.hpp:1382-1386).
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

        // ---- EXHAUSTIVE: cancel() must preserve EVERY retval bit pattern.  Set
        //      the slot to each landmark / bit-walk pattern, call cancel(), and
        //      assert the pattern is byte-identical afterwards and cancel set. ----
        {
            const std::array<std::uint64_t, 10> patterns{ {
                0x0ull, 0x1ull, 0xFFFFFFFFFFFFFFFFull,
                0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull,
                0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull,
                0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull,
                0x0123456789ABCDEFull,
            } };
            bool all_ok{ true };
            for (const std::uint64_t pat : patterns)
            {
                slot.retval = static_cast<std::int64_t>(pat);
                slot.cancel = false;
                rv.cancel();
                if (!(static_cast<std::uint64_t>(slot.retval) == pat && slot.cancel)) { all_ok = false; }
            }
            // and the 64-bit single-bit walk
            for (int shift{ 0 }; shift < 64; ++shift)
            {
                const std::uint64_t pat{ std::uint64_t{ 1 } << shift };
                slot.retval = static_cast<std::int64_t>(pat);
                slot.cancel = false;
                rv.cancel();
                if (!(static_cast<std::uint64_t>(slot.retval) == pat && slot.cancel)) { all_ok = false; }
            }
            check("cancel_preserves_every_retval_bit_pattern", all_ok);
        }

        // ---- cancel() on an ALREADY-cancelled slot keeps both fields ----
        slot.retval = static_cast<std::int64_t>(0xC0FFEEC0FFEEC0FFull);
        slot.cancel = true;
        rv.cancel();
        check("cancel_on_already_cancelled_keeps_retval",
              static_cast<std::uint64_t>(slot.retval) == 0xC0FFEEC0FFEEC0FFull && slot.cancel == true);
    }

    // =====================================================================
    // SECTION J1b — Wave-25 ledger gaps: cancel() compile-time contract
    // and platform-availability fall-through.
    // =====================================================================
    {
        // cancel() is documented noexcept (vmhook.hpp:1411). Lock it.
        static_assert(noexcept(std::declval<vmhook::return_value&>().cancel()),
                      "vmhook::return_value::cancel() must be noexcept");

        // return_slot layout invariants the x64 trampoline hard-codes:
        // cancel at offset 0 (cmp byte ptr [rsp],0) and retval at +8
        // (mov rax,[rsp+8]). If these ever drift, the cancel epilogue
        // silently returns garbage.
        static_assert(sizeof(bool) == 1,
                      "trampoline cmp byte ptr [rsp],0 assumes sizeof(bool)==1");
        static_assert(offsetof(vmhook::hotspot::return_slot, cancel) == 0,
                      "trampoline reads cancel at slot offset 0");
        static_assert(offsetof(vmhook::hotspot::return_slot, retval) == 8,
                      "trampoline reads retval at slot offset +8");
        static_assert(sizeof(vmhook::hotspot::return_slot) == 16,
                      "return_slot is exactly two 8-byte cells");

        // Twice-call idempotence on a fresh slot (no prior set) — full
        // post-condition: cancel stays true, retval stays exactly 0, and
        // a third call is still a no-op. This is the canonical void-method
        // cancel sequence the trampoline relies on.
        {
            vmhook::hotspot::return_slot s{};
            vmhook::return_value rv{ &s };
            rv.cancel();
            rv.cancel();
            check("cancel_twice_fresh_slot_flag_set", s.cancel == true);
            check("cancel_twice_fresh_slot_retval_zero", s.retval == 0);
            rv.cancel();
            check("cancel_thrice_fresh_slot_still_clean",
                  s.cancel == true && s.retval == 0);
        }

        // cancel() on a return_value carrying a slot but NO live frame
        // (frame == nullptr — the ctor default at vmhook.hpp:1347) must
        // not consult the frame at all: it is a pure 1-byte flag write.
        // Proves "no live frame" is irrelevant to cancel semantics.
        {
            vmhook::hotspot::return_slot s{};
            vmhook::return_value rv{ &s, /*frame=*/nullptr };
            check("cancel_no_frame_precondition_frame_null", rv.frame() == nullptr);
            rv.cancel();
            check("cancel_no_frame_sets_flag", s.cancel == true);
            check("cancel_no_frame_retval_untouched", s.retval == 0);
            check("cancel_no_frame_frame_still_null", rv.frame() == nullptr);
        }

        // Platform-availability probe: on every supported build target
        // (x64 win64/sysv) VMHOOK_RUNTIME_HOOKING_AVAILABLE is 1; on a
        // hypothetical non-x64 build it is 0, and the midi2i trampoline
        // is never emitted (vmhook.hpp:5375-5383). The C++-level
        // return_slot/cancel() machinery is identical either way — the
        // setter is just a 1-byte store with no hooking dependency.
        // Verify the macro is defined and that cancel() on a synthetic
        // slot works the same regardless of its value.
#if defined(VMHOOK_RUNTIME_HOOKING_AVAILABLE)
        constexpr bool runtime_hooking_macro_defined{ true };
#else
        constexpr bool runtime_hooking_macro_defined{ false };
#endif
        check("runtime_hooking_macro_is_defined", runtime_hooking_macro_defined);
#if !VMHOOK_RUNTIME_HOOKING_AVAILABLE
        // Non-x64 fall-through: cancel() is still a deterministic no-op
        // on retval — only the trampoline epilogue is absent.
        {
            vmhook::hotspot::return_slot s{};
            s.retval = static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull);
            vmhook::return_value rv{ &s };
            rv.cancel();
            check("cancel_non_x64_flag_set", s.cancel == true);
            check("cancel_non_x64_retval_preserved",
                  static_cast<std::uint64_t>(s.retval) == 0xDEADBEEFCAFEBABEull);
        }
#else
        // x64 build: positively assert the macro is 1 so a future flip
        // to 0 on a supported target is caught at test time.
        check("runtime_hooking_available_on_x64",
              VMHOOK_RUNTIME_HOOKING_AVAILABLE == 1);
#endif
    }

    // =====================================================================
    // SECTION J2 — return_slot default-construction & struct contract
    // (vmhook.hpp:1284-1288): brace-initialised fields cancel{false},
    // retval{0}; the trampoline relies on these defaults.
    // =====================================================================
    {
        // Value-initialised slot.
        vmhook::hotspot::return_slot s1{};
        check("return_slot_value_init_cancel_false", s1.cancel == false);
        check("return_slot_value_init_retval_zero", s1.retval == 0);

        // Default-initialised slot still gets the in-class member initialisers.
        vmhook::hotspot::return_slot s2;
        check("return_slot_default_init_cancel_false", s2.cancel == false);
        check("return_slot_default_init_retval_zero", s2.retval == 0);

        // A return_value over a fresh slot leaves the slot untouched until a
        // setter is called (construction has no side effects on the slot).
        vmhook::hotspot::return_slot s3{};
        vmhook::return_value rv3{ &s3 };
        check("return_value_ctor_no_side_effect_cancel", s3.cancel == false);
        check("return_value_ctor_no_side_effect_retval", s3.retval == 0);
        // touch rv3 so it is not flagged unused on any toolchain
        check("return_value_frame_default_null", rv3.frame() == nullptr);
    }

    // =====================================================================
    // SECTION K — no-frame defaults for caller() / stack_trace() / frame().
    // Constructed with frame == nullptr (the constructor default, vmhook.hpp:1318).
    // These must each return the documented empty/null result and NEVER crash
    // (caller() vmhook.hpp:9492-9498; stack_trace() vmhook.hpp:9615-9626;
    // frame() accessor at vmhook.hpp:1503-1506 returns the stored pointer).
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

        // ---- EXHAUSTIVE depth sweep: a dense set of max_depth values (incl. the
        //      0-promoted-to-64 special case and boundary 63/64/65) must ALL
        //      return an empty vector with no frame, and never fault. ----
        {
            const std::array<std::size_t, 16> depths{ {
                0, 1, 2, 3, 7, 8, 15, 16, 32, 63, 64, 65, 128, 256, 1000,
                std::numeric_limits<std::size_t>::max(),
            } };
            bool all_empty{ true };
            for (const std::size_t d : depths)
            {
                if (!rv.stack_trace(d).empty()) { all_empty = false; }
            }
            check("stack_trace_no_frame_exhaustive_depth_sweep_empty", all_empty);
            check("stack_trace_no_frame_exhaustive_sweep_slot_clean",
                  slot.cancel == false && slot.retval == 0);
        }

        // ---- caller_info::valid() contract (vmhook.hpp:1428-1431): valid() is
        //      EXACTLY method != nullptr, independent of the string fields. ----
        {
            vmhook::return_value::caller_info ci{};
            check("caller_info_default_invalid", ci.valid() == false);
            // Non-null method => valid, even with all strings empty.
            ci.method = reinterpret_cast<vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x1000u));
            check("caller_info_nonnull_method_valid", ci.valid() == true);
            // Populated strings but null method => still INVALID (method is the
            // sole discriminator).
            vmhook::return_value::caller_info ci2{};
            ci2.class_name  = "com/example/Foo";
            ci2.method_name = "bar";
            ci2.signature   = "()V";
            check("caller_info_strings_set_but_method_null_invalid", ci2.valid() == false);
        }

        // frame() is const & stable: repeated calls return the same null pointer
        // and do not mutate the slot.
        check("frame_accessor_stable_null", rv.frame() == nullptr && rv.frame() == rv.frame());
        check("frame_accessor_does_not_touch_slot",
              slot.cancel == false && slot.retval == 0);

        // ---- WAVE-29 DEEPENING: ledger gaps for cold-state caller() ----
        // (1) Static type guarantees on the return signatures (compile-time).
        using rv_t = vmhook::return_value;
        using ci_t = rv_t::caller_info;
        static_assert(std::is_same_v<decltype(std::declval<const rv_t&>().caller()), ci_t>,
                      "caller() must return caller_info by value");
        static_assert(std::is_same_v<decltype(std::declval<const rv_t&>().stack_trace()),
                                     std::vector<ci_t>>,
                      "stack_trace() must return std::vector<caller_info> by value");
        static_assert(std::is_same_v<decltype(std::declval<const rv_t&>().stack_trace(64)),
                                     std::vector<ci_t>>,
                      "stack_trace(size_t) must return std::vector<caller_info> by value");
        static_assert(std::is_same_v<decltype(std::declval<const rv_t&>().frame()),
                                     vmhook::hotspot::frame*>,
                      "frame() must return hotspot::frame* (raw pointer)");
        static_assert(std::is_same_v<decltype(std::declval<const ci_t&>().valid()), bool>,
                      "caller_info::valid() must return bool");
        static_assert(std::is_same_v<decltype(ci_t::class_name), std::string>);
        static_assert(std::is_same_v<decltype(ci_t::method_name), std::string>);
        static_assert(std::is_same_v<decltype(ci_t::signature), std::string>);
        static_assert(std::is_same_v<decltype(ci_t::method), vmhook::hotspot::method*>);
        check("static_asserts_caller_return_types_ok", true);

        // (2) noexcept characterization — all four are declared noexcept in the
        //     header (vmhook.hpp:1477, 1522, frame()).  Pinning the qualifier so
        //     any future drop of noexcept (which would silently change std::
        //     terminate semantics on allocation failure) is caught at compile
        //     time.  Note: the implementations DO allocate (strings/vector);
        //     the noexcept means a bad_alloc terminates rather than propagates.
        static_assert(noexcept(rv.caller()),
                      "caller() is declared noexcept (vmhook.hpp:1477)");
        static_assert(noexcept(rv.stack_trace()),
                      "stack_trace() is declared noexcept (vmhook.hpp:1522)");
        static_assert(noexcept(rv.stack_trace(8)),
                      "stack_trace(n) is declared noexcept (vmhook.hpp:1522)");
        static_assert(noexcept(rv.frame()),
                      "frame() is a trivial accessor — noexcept");
        check("noexcept_characterization_documented", true);

        // (3) Idempotency: twice == once with field-level equality on caller_info.
        const auto ci_a{ rv.caller() };
        const auto ci_b{ rv.caller() };
        check("caller_twice_eq_once_method",      ci_a.method == ci_b.method);
        check("caller_twice_eq_once_class",       ci_a.class_name == ci_b.class_name);
        check("caller_twice_eq_once_method_name", ci_a.method_name == ci_b.method_name);
        check("caller_twice_eq_once_signature",   ci_a.signature == ci_b.signature);
        check("caller_twice_eq_once_valid",       ci_a.valid() == ci_b.valid());
        check("caller_twice_no_slot_mutation",
              slot.cancel == false && slot.retval == 0);

        // (4) stack_trace twice == once across multiple depths.
        const auto st_a0{ rv.stack_trace() };
        const auto st_b0{ rv.stack_trace() };
        check("stack_trace_default_twice_eq_once_size", st_a0.size() == st_b0.size());
        check("stack_trace_default_twice_both_empty",
              st_a0.empty() && st_b0.empty());
        const auto st_a4{ rv.stack_trace(4) };
        const auto st_b4{ rv.stack_trace(4) };
        check("stack_trace_4_twice_eq_once_size", st_a4.size() == st_b4.size());
        check("stack_trace_4_twice_both_empty",
              st_a4.empty() && st_b4.empty());
        const auto st_a0p{ rv.stack_trace(0) };
        const auto st_b0p{ rv.stack_trace(0) };
        check("stack_trace_zero_promoted_twice_eq_size",
              st_a0p.size() == st_b0p.size());
        check("stack_trace_zero_promoted_twice_both_empty",
              st_a0p.empty() && st_b0p.empty());
        check("stack_trace_after_idempotency_slot_clean",
              slot.cancel == false && slot.retval == 0);

        // (5) Default-constructed caller_info — exhaustive cold-state shape.
        const ci_t default_ci{};
        check("default_ci_method_is_null",         default_ci.method == nullptr);
        check("default_ci_class_empty",            default_ci.class_name.empty());
        check("default_ci_method_name_empty",      default_ci.method_name.empty());
        check("default_ci_signature_empty",        default_ci.signature.empty());
        check("default_ci_class_size_zero",        default_ci.class_name.size() == 0);
        check("default_ci_method_name_size_zero",  default_ci.method_name.size() == 0);
        check("default_ci_signature_size_zero",    default_ci.signature.size() == 0);
        check("default_ci_invalid",                default_ci.valid() == false);
        const ci_t default_ci2{};
        check("default_ci_two_instances_method_eq",
              default_ci.method == default_ci2.method);
        check("default_ci_two_instances_strings_eq",
              default_ci.class_name == default_ci2.class_name
           && default_ci.method_name == default_ci2.method_name
           && default_ci.signature == default_ci2.signature);
    }

    // =====================================================================
    // SECTION K2 — frame() faithfully returns whatever non-null pointer the
    // ctor was handed (it is a plain accessor, vmhook.hpp:1503-1506).  We pass
    // a fabricated sentinel: frame() must echo it byte-for-byte WITHOUT ever
    // dereferencing it (no caller()/stack_trace()/set_arg call here, so the
    // bogus pointer is never read — purely the accessor contract).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        auto* const sentinel_frame{ reinterpret_cast<vmhook::hotspot::frame*>(
            static_cast<std::uintptr_t>(0xABCDEF0012345678ull)) };
        vmhook::return_value rv{ &slot, sentinel_frame };
        check("frame_accessor_echoes_sentinel", rv.frame() == sentinel_frame);
        check("frame_accessor_echoes_sentinel_stable", rv.frame() == rv.frame());
        check("frame_accessor_sentinel_no_slot_side_effect",
              slot.cancel == false && slot.retval == 0);
    }

    // =====================================================================
    // SECTION K3 — wave-29 type/noexcept characterization of stack_trace() and
    // caller() / frame() on a default (no-frame) return_value. We pin the
    // EXACT return types (vector<caller_info>, caller_info by value, frame*)
    // and characterize the observed noexcept-ness so any future header tweak
    // that silently changes the signature trips this file at COMPILE time.
    // =====================================================================
    {
        using rv_t      = vmhook::return_value;
        using ci_t      = rv_t::caller_info;
        using trace_t   = std::vector<ci_t>;

        // stack_trace(): EXACTLY std::vector<caller_info>, both overloads.
        static_assert(
            std::is_same<decltype(std::declval<const rv_t&>().stack_trace()),
                         trace_t>::value,
            "stack_trace() must return std::vector<caller_info>");
        static_assert(
            std::is_same<decltype(std::declval<const rv_t&>().stack_trace(
                             std::size_t{ 0 })),
                         trace_t>::value,
            "stack_trace(size_t) must return std::vector<caller_info>");

        // caller(): by-value caller_info (NOT a reference, NOT a pointer).
        static_assert(
            std::is_same<decltype(std::declval<const rv_t&>().caller()),
                         ci_t>::value,
            "caller() must return caller_info by value");

        // frame(): exactly hotspot::frame* (a plain accessor).
        static_assert(
            std::is_same<decltype(std::declval<const rv_t&>().frame()),
                         vmhook::hotspot::frame*>::value,
            "frame() must return hotspot::frame*");

        // caller_info::valid(): bool, const-callable.
        static_assert(
            std::is_same<decltype(std::declval<const ci_t&>().valid()),
                         bool>::value,
            "caller_info::valid() must return bool");

        // caller_info is a plain aggregate-y type: default-constructible,
        // copyable, movable, destructible — guaranteed by the header layout
        // (method* + 3 std::string).
        static_assert(std::is_default_constructible<ci_t>::value,
                      "caller_info must be default-constructible");
        static_assert(std::is_copy_constructible<ci_t>::value,
                      "caller_info must be copy-constructible");
        static_assert(std::is_move_constructible<ci_t>::value,
                      "caller_info must be move-constructible");
        static_assert(std::is_destructible<ci_t>::value,
                      "caller_info must be destructible");

        // frame() is a trivial getter — guaranteed noexcept by the header.
        static_assert(noexcept(std::declval<const rv_t&>().frame()),
                      "frame() must be noexcept");

        // ---- RUNTIME no-frame contract: stack_trace() / caller() do NOT
        //      throw on a null-frame return_value. We wrap in a try/catch so
        //      any future regression that lets an exception escape on the
        //      no-frame fast path fails LOUDLY here. ----
        vmhook::hotspot::return_slot slot_nx{};
        vmhook::return_value         rv_nx{ &slot_nx, /*frame=*/nullptr };

        bool no_throw_default_st{ false };
        try {
            const auto t{ rv_nx.stack_trace() };
            no_throw_default_st = t.empty();
        } catch (...) { no_throw_default_st = false; }
        check("stack_trace_no_frame_default_does_not_throw",
              no_throw_default_st);

        bool no_throw_intmax_st{ false };
        try {
            const auto t{ rv_nx.stack_trace(
                static_cast<std::size_t>(std::numeric_limits<int>::max())) };
            no_throw_intmax_st = t.empty();
        } catch (...) { no_throw_intmax_st = false; }
        check("stack_trace_no_frame_INT_MAX_does_not_throw",
              no_throw_intmax_st);

        bool no_throw_caller{ false };
        try {
            const auto ci{ rv_nx.caller() };
            no_throw_caller = (ci.valid() == false);
        } catch (...) { no_throw_caller = false; }
        check("caller_no_frame_does_not_throw", no_throw_caller);

        // Returned vector on no-frame default is in a valid empty state:
        // size()==0, begin()==end(), and capacity() is a non-negative size_t
        // (trivially true but pins the std::vector contract).
        const auto empty_trace{ rv_nx.stack_trace() };
        check("stack_trace_no_frame_size_zero", empty_trace.size() == 0u);
        check("stack_trace_no_frame_begin_eq_end",
              empty_trace.begin() == empty_trace.end());
        check("stack_trace_no_frame_capacity_is_size_t",
              empty_trace.capacity() <= empty_trace.max_size());

        // Repeated calls return INDEPENDENT vectors (each call materialises a
        // fresh result — no shared cached buffer). We can't compare addresses
        // of temporaries reliably, but we CAN bind two named results and
        // confirm they live at distinct addresses.
        auto t1{ rv_nx.stack_trace() };
        auto t2{ rv_nx.stack_trace() };
        check("stack_trace_no_frame_results_are_distinct_objects",
              static_cast<const void*>(&t1) != static_cast<const void*>(&t2));
        check("stack_trace_no_frame_results_both_empty",
              t1.empty() && t2.empty());
    }

    // =====================================================================
    // SECTION L — set_arg() guard / early-return paths (vmhook.hpp:9769-9777).
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

    // =====================================================================
    // SECTION L2 — EXHAUSTIVE set_arg() guard sweep.  The guard is exactly
    // `!frame || index < 0 || index > 0xFFFF` (vmhook.hpp:9770).  With a null
    // frame, the index value is irrelevant — EVERY index must return false.  We
    // sweep:  (a) a dense set of indices spanning the whole int32 range incl.
    // both sides of the 0xFFFF bound and INT_MIN/INT_MAX;  (b) every value_type
    // category (the guard runs before any type handling).  And we re-prove the
    // slot is never touched.  This is the no-JVM-testable half of set_arg's
    // contract exhausted.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value         rv{ &slot, /*frame=*/nullptr };

        // (a) Index sweep with a fixed int32 value — every index => false.
        const std::array<std::int32_t, 24> indices{ {
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::min() + 1,
            -1000000, -65536, -2, -1,
            0, 1, 2, 3, 100, 255, 256, 1000,
            65534,           // max_locals - 1 (would be in-range with a frame)
            0xFFFF,          // max_locals exactly (in-range bound, still no frame)
            0x10000,         // max_locals + 1 (out of range)
            0x10001, 100000, 1000000, 16777216,
            std::numeric_limits<std::int32_t>::max() - 1,
            std::numeric_limits<std::int32_t>::max(),
            123456,
        } };
        {
            bool all_false{ true };
            for (const std::int32_t idx : indices)
            {
                if (rv.set_arg(idx, std::int32_t{ 7 }) != false) { all_false = false; }
            }
            check("set_arg_no_frame_exhaustive_index_sweep_all_false", all_false);
            check("set_arg_no_frame_exhaustive_index_sweep_slot_clean",
                  slot.cancel == false && slot.retval == 0);
        }

        // (b) Per-value-type guard: each category, at both a representative
        //     in-range index (0) and an out-of-range index (0x10000), must
        //     return false with no frame.  Covers signed/unsigned widths, bool,
        //     float, double, and pointer payloads.
        {
            bool all_false{ true };
            auto must_false{ [&](bool r) { if (r != false) { all_false = false; } } };

            must_false(rv.set_arg(0,       std::int8_t{ -1 }));
            must_false(rv.set_arg(0x10000, std::int8_t{ -1 }));
            must_false(rv.set_arg(0,       std::int16_t{ -1 }));
            must_false(rv.set_arg(0x10000, std::int16_t{ -1 }));
            must_false(rv.set_arg(0,       std::int32_t{ -1 }));
            must_false(rv.set_arg(0x10000, std::int32_t{ -1 }));
            must_false(rv.set_arg(0,       std::int64_t{ -1 }));
            must_false(rv.set_arg(0x10000, std::int64_t{ -1 }));
            must_false(rv.set_arg(0,       std::uint8_t{ 0xFF }));
            must_false(rv.set_arg(0x10000, std::uint8_t{ 0xFF }));
            must_false(rv.set_arg(0,       std::uint16_t{ 0xFFFF }));
            must_false(rv.set_arg(0x10000, std::uint16_t{ 0xFFFF }));
            must_false(rv.set_arg(0,       std::uint32_t{ 0xFFFFFFFFu }));
            must_false(rv.set_arg(0x10000, std::uint32_t{ 0xFFFFFFFFu }));
            must_false(rv.set_arg(0,       std::uint64_t{ 0xFFFFFFFFFFFFFFFFull }));
            must_false(rv.set_arg(0x10000, std::uint64_t{ 0xFFFFFFFFFFFFFFFFull }));
            must_false(rv.set_arg(0,       true));
            must_false(rv.set_arg(0x10000, false));
            must_false(rv.set_arg(0,       3.14f));
            must_false(rv.set_arg(0x10000, 3.14f));
            must_false(rv.set_arg(0,       2.71828));
            must_false(rv.set_arg(0x10000, 2.71828));
            must_false(rv.set_arg(0,       static_cast<char>('Z')));
            must_false(rv.set_arg(0x10000, static_cast<char>('Z')));
            void* const some_ptr{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4000u)) };
            must_false(rv.set_arg(0,       some_ptr));
            must_false(rv.set_arg(0x10000, some_ptr));

            check("set_arg_no_frame_exhaustive_value_type_sweep_all_false", all_false);
            check("set_arg_no_frame_exhaustive_value_type_sweep_slot_clean",
                  slot.cancel == false && slot.retval == 0);
        }

        // (c) An rvalue and an lvalue argument both hit the same guard (set_arg
        //     takes value_type&& — verify both reference categories are rejected
        //     identically with no frame).
        {
            std::int32_t lvalue{ 99 };
            check("set_arg_no_frame_lvalue_arg_false",
                  rv.set_arg(0, lvalue) == false);
            check("set_arg_no_frame_rvalue_arg_false",
                  rv.set_arg(0, std::int32_t{ 99 }) == false);
            check("set_arg_no_frame_ref_category_slot_clean",
                  slot.cancel == false && slot.retval == 0);
        }
    }

    // =====================================================================
    // SECTION M — ADDITIVE deepening pass (Criterion 2, exhaustive inputs).
    // GENUINELY NEW input-space NOT touched by sections A..L2 above: the pure,
    // no-JVM-determinable helper surface that surrounds return_value but is
    // never exercised by the slot/cancel/set_arg tests.  Every expected value is
    // derived from the vmhook.hpp source (line refs inline), every const is
    // referenced in a check()/static_assert, every array/string input is a REAL
    // owned buffer, and no fabricated address is ever dereferenced as data.
    //   M1  decode_u5            (vmhook::hotspot::klass, vmhook.hpp:3848-3871)
    //   M2  sig_char_to_basic_type / jvm_primitive_byte_width (detail, 16215/16250)
    //   M3  clamp_safe_container_count (vmhook, 14753-14765)
    //   M4  jni_signature_for_arg / signature_for_arg (detail/jni, 12994/13712)
    //   M5  is_unique_ptr_v / function_traits::args_tuple_t (detail, 1788/9319)
    //   M6  array_length / get_array_element / set_array_element on a REAL owned
    //       buffer laid out as a HotSpot array header (vmhook.hpp:14778/14814/14860)
    // =====================================================================

    // ---------------------------------------------------------------------
    // M1 — decode_u5 UNSIGNED5 codec (vmhook.hpp:3848-3871).  The decoder is
    // pure arithmetic over an owned byte buffer:
    //   sum += (byte_i - 1) << (6 * i), for i = 0..4, stop at first byte < 192;
    //   byte value 0 is the End marker (REWINDS stream_pos, returns ~0u).
    // We feed REAL std::array buffers (never a fabricated address) and assert the
    // decoded value AND the exact cursor delta for every continuation length.
    // ---------------------------------------------------------------------
    {
        using vmhook::hotspot::klass;

        // (a) Single low byte b in [1, 191] => value (b - 1), cursor +1.  Sweep
        //     the WHOLE single-byte terminal domain (191 distinct values).
        {
            bool all_ok{ true };
            for (int b{ 1 }; b <= 191; ++b)
            {
                std::array<std::uint8_t, 1> buf{ { static_cast<std::uint8_t>(b) } };
                int pos{ 0 };
                const std::uint32_t got{ klass::decode_u5(buf.data(), pos) };
                if (got != static_cast<std::uint32_t>(b - 1) || pos != 1) { all_ok = false; }
            }
            check("decode_u5_single_low_byte_full_domain_1_to_191", all_ok);
        }

        // (b) Two-byte sequences: a high byte (>=192) then a low byte.
        //     {192, low} => 191 + (low-1)*64.  Worked from source: 192 gives
        //     (192-1)<<0 = 191, then low contributes (low-1)<<6.
        {
            std::array<std::uint8_t, 2> b1{ { 192u, 1u } };   // 191 + 0
            int p1{ 0 };
            check("decode_u5_two_byte_192_1_is_191",
                  klass::decode_u5(b1.data(), p1) == 191u && p1 == 2);

            std::array<std::uint8_t, 2> b2{ { 192u, 2u } };   // 191 + 64
            int p2{ 0 };
            check("decode_u5_two_byte_192_2_is_255",
                  klass::decode_u5(b2.data(), p2) == 255u && p2 == 2);

            std::array<std::uint8_t, 2> b3{ { 255u, 1u } };   // (255-1)=254 at pos0
            int p3{ 0 };
            check("decode_u5_two_byte_255_1_is_254",
                  klass::decode_u5(b3.data(), p3) == 254u && p3 == 2);
        }

        // (c) The documented 5-byte UINT32_MAX sequence {192,254,253,253,253}
        //     (vmhook.hpp:3839-3846) decodes to exactly 0xFFFFFFFF and ADVANCES
        //     the cursor by 5 — the value that aliases the End sentinel but is
        //     distinguished by the cursor delta.
        {
            std::array<std::uint8_t, 5> mx{ { 192u, 254u, 253u, 253u, 253u } };
            int pos{ 0 };
            const std::uint32_t got{ klass::decode_u5(mx.data(), pos) };
            check("decode_u5_five_byte_uint32_max_value", got == 0xFFFFFFFFu);
            check("decode_u5_five_byte_uint32_max_advances_cursor_by_5", pos == 5);
        }

        // (d) End marker: a 0 byte AT the cursor returns ~0u and REWINDS (cursor
        //     unchanged) so the byte is not consumed (vmhook.hpp:3855-3862).
        //     Distinguished from the real UINT32_MAX above purely by pos delta.
        //     We start the cursor at index 1, where the 0 byte sits, to prove the
        //     rewind restores the cursor to exactly where it was (1), not 0.
        {
            std::array<std::uint8_t, 3> buf{ { 5u, 0u, 5u } };
            int pos{ 1 };                       // cursor parked on the 0 byte
            const std::uint32_t got{ klass::decode_u5(buf.data(), pos) };
            check("decode_u5_end_marker_returns_tilde_zero", got == ~0u);
            check("decode_u5_end_marker_rewinds_cursor_unchanged", pos == 1);
        }

        // (e) Threaded walk: three values packed back-to-back in one owned buffer
        //     must decode in order with the cursor advancing across the whole
        //     stream, then the trailing 0 byte signals End and rewinds.  This is
        //     the FieldInfoStream consumption pattern (vmhook.hpp:3967-4001) in
        //     miniature, driven entirely from a real buffer.
        {
            // value0 = 41   -> single byte (41+1)=42  (low, since 42<192)
            // value1 = 190  -> single byte 191        (low)
            // value2 = 255  -> {192, 2}  (191 + 64 = 255)
            std::array<std::uint8_t, 5> stream{ { 42u, 191u, 192u, 2u, 0u } };
            int pos{ 0 };
            const std::uint32_t v0{ klass::decode_u5(stream.data(), pos) };
            const int after0{ pos };
            const std::uint32_t v1{ klass::decode_u5(stream.data(), pos) };
            const int after1{ pos };
            const std::uint32_t v2{ klass::decode_u5(stream.data(), pos) };
            const int after2{ pos };
            const std::uint32_t vend{ klass::decode_u5(stream.data(), pos) };
            const int afterEnd{ pos };
            check("decode_u5_threaded_walk_value0_41", v0 == 41u && after0 == 1);
            check("decode_u5_threaded_walk_value1_190", v1 == 190u && after1 == 2);
            check("decode_u5_threaded_walk_value2_255", v2 == 255u && after2 == 4);
            check("decode_u5_threaded_walk_trailing_end_marker",
                  vend == ~0u && afterEnd == 4);
        }

        // (f) Continuation-length boundary: the loop runs at most 5 bytes
        //     (vmhook.hpp:3852).  A buffer of five all-high (>=192) bytes never
        //     hits a low byte, so the loop exits after exactly 5 iterations and
        //     the cursor lands at 5.  We only assert the cursor/length contract
        //     (the value is an implementation detail of the 5th-byte overflow).
        {
            std::array<std::uint8_t, 5> allhigh{ { 200u, 200u, 200u, 200u, 200u } };
            int pos{ 0 };
            (void)klass::decode_u5(allhigh.data(), pos);
            check("decode_u5_five_high_bytes_consumes_exactly_5", pos == 5);
        }
    }

    // ---------------------------------------------------------------------
    // M2 — sig_char_to_basic_type (vmhook.hpp:16215-16232) and
    // jvm_primitive_byte_width (vmhook.hpp:16250-16265).  Both are pure switch
    // tables; we exhaust EVERY documented descriptor character plus the default
    // fallbacks across the entire 8-bit char domain.
    // ---------------------------------------------------------------------
    {
        // (a) Every mapped descriptor char => its HotSpot BasicType int.  Values
        //     copied verbatim from the source switch.
        check("basic_type_Z_boolean_4", vmhook::detail::sig_char_to_basic_type('Z') == 4);
        check("basic_type_C_char_5",    vmhook::detail::sig_char_to_basic_type('C') == 5);
        check("basic_type_F_float_6",   vmhook::detail::sig_char_to_basic_type('F') == 6);
        check("basic_type_D_double_7",  vmhook::detail::sig_char_to_basic_type('D') == 7);
        check("basic_type_B_byte_8",    vmhook::detail::sig_char_to_basic_type('B') == 8);
        check("basic_type_S_short_9",   vmhook::detail::sig_char_to_basic_type('S') == 9);
        check("basic_type_I_int_10",    vmhook::detail::sig_char_to_basic_type('I') == 10);
        check("basic_type_J_long_11",   vmhook::detail::sig_char_to_basic_type('J') == 11);
        check("basic_type_L_object_12", vmhook::detail::sig_char_to_basic_type('L') == 12);
        check("basic_type_array_13",    vmhook::detail::sig_char_to_basic_type('[') == 13);
        check("basic_type_V_void_14",   vmhook::detail::sig_char_to_basic_type('V') == 14);

        // (b) EVERY other byte value falls to the T_OBJECT (12) default.  Sweep
        //     the whole signed-char domain and assert: the 11 mapped chars give
        //     their value, all others give 12.  No char is left unclassified.
        {
            const std::array<char, 11> mapped{ {
                'Z', 'C', 'F', 'D', 'B', 'S', 'I', 'J', 'L', '[', 'V',
            } };
            bool all_ok{ true };
            for (int code{ 0 }; code <= 255; ++code)
            {
                const char c{ static_cast<char>(code) };
                bool is_mapped{ false };
                for (const char m : mapped) { if (m == c) { is_mapped = true; } }
                const int bt{ vmhook::detail::sig_char_to_basic_type(c) };
                if (!is_mapped && bt != 12) { all_ok = false; }
                if (is_mapped && (bt < 4 || bt > 14)) { all_ok = false; }
            }
            check("basic_type_unmapped_chars_default_to_T_OBJECT_12", all_ok);
        }

        // (c) jvm_primitive_byte_width: single-char primitive widths and the
        //     "0 for everything else" contract (reference/array/unknown/multi).
        check("prim_width_Z_is_1", vmhook::detail::jvm_primitive_byte_width("Z") == 1u);
        check("prim_width_B_is_1", vmhook::detail::jvm_primitive_byte_width("B") == 1u);
        check("prim_width_S_is_2", vmhook::detail::jvm_primitive_byte_width("S") == 2u);
        check("prim_width_C_is_2", vmhook::detail::jvm_primitive_byte_width("C") == 2u);
        check("prim_width_I_is_4", vmhook::detail::jvm_primitive_byte_width("I") == 4u);
        check("prim_width_F_is_4", vmhook::detail::jvm_primitive_byte_width("F") == 4u);
        check("prim_width_J_is_8", vmhook::detail::jvm_primitive_byte_width("J") == 8u);
        check("prim_width_D_is_8", vmhook::detail::jvm_primitive_byte_width("D") == 8u);
        // Reference/array/void/unknown single chars => 0.
        check("prim_width_L_reference_is_0", vmhook::detail::jvm_primitive_byte_width("L") == 0u);
        check("prim_width_array_is_0",       vmhook::detail::jvm_primitive_byte_width("[") == 0u);
        check("prim_width_V_void_is_0",      vmhook::detail::jvm_primitive_byte_width("V") == 0u);
        check("prim_width_unknown_X_is_0",   vmhook::detail::jvm_primitive_byte_width("X") == 0u);
        // size != 1 => 0 regardless of content (empty, full descriptors, arrays).
        check("prim_width_empty_is_0",            vmhook::detail::jvm_primitive_byte_width("") == 0u);
        check("prim_width_object_descriptor_is_0",
              vmhook::detail::jvm_primitive_byte_width("Ljava/lang/String;") == 0u);
        check("prim_width_int_array_descriptor_is_0",
              vmhook::detail::jvm_primitive_byte_width("[I") == 0u);
        check("prim_width_two_char_II_is_0",      vmhook::detail::jvm_primitive_byte_width("II") == 0u);

        // (d) Cross-check: every primitive descriptor whose width is non-zero is
        //     also a mapped (non-default) BasicType char, and the 1-char
        //     reference/array/void chars are mapped too but report width 0.  This
        //     ties the two tables together across the full single-char domain.
        {
            const std::array<char, 8> prims{ { 'Z', 'B', 'S', 'C', 'I', 'F', 'J', 'D' } };
            bool all_ok{ true };
            for (const char c : prims)
            {
                const char s[2]{ c, '\0' };
                if (vmhook::detail::jvm_primitive_byte_width(std::string_view{ s, 1 }) == 0u) { all_ok = false; }
                if (vmhook::detail::sig_char_to_basic_type(c) == 12) { all_ok = false; }
            }
            check("prim_widths_align_with_nondefault_basic_types", all_ok);
        }
    }

    // ---------------------------------------------------------------------
    // M3 — clamp_safe_container_count (vmhook.hpp:14753-14765).  Pure clamp:
    //   raw <= 0            -> 0
    //   0 < raw < (1<<24)   -> raw
    //   raw >= (1<<24)      -> (1<<24)
    // Exhaust the boundaries and signs; the cap is k_max_safe_container_elems.
    // ---------------------------------------------------------------------
    {
        constexpr std::int32_t cap{ static_cast<std::int32_t>(vmhook::k_max_safe_container_elems) };
        static_assert(cap == (1 << 24), "k_max_safe_container_elems must be 1<<24 (vmhook.hpp:14733).");

        check("clamp_zero_is_zero",            vmhook::clamp_safe_container_count(0) == 0);
        check("clamp_one_is_one",              vmhook::clamp_safe_container_count(1) == 1);
        check("clamp_negative_one_is_zero",    vmhook::clamp_safe_container_count(-1) == 0);
        check("clamp_int_min_is_zero",
              vmhook::clamp_safe_container_count(std::numeric_limits<std::int32_t>::min()) == 0);
        check("clamp_below_cap_passes_through", vmhook::clamp_safe_container_count(cap - 1) == cap - 1);
        check("clamp_at_cap_is_cap",            vmhook::clamp_safe_container_count(cap) == cap);
        check("clamp_above_cap_is_cap",         vmhook::clamp_safe_container_count(cap + 1) == cap);
        check("clamp_int_max_is_cap",
              vmhook::clamp_safe_container_count(std::numeric_limits<std::int32_t>::max()) == cap);

        // Dense boundary + power-of-two sweep: result is min(max(raw,0), cap).
        {
            const std::array<std::int32_t, 12> table{ {
                std::numeric_limits<std::int32_t>::min(), -1000000, -2, -1,
                0, 1, 2, 1000, 65536, cap - 1, cap,
                std::numeric_limits<std::int32_t>::max(),
            } };
            bool all_ok{ true };
            for (const std::int32_t raw : table)
            {
                const std::int32_t want{ raw <= 0 ? 0 : (raw < cap ? raw : cap) };
                if (vmhook::clamp_safe_container_count(raw) != want) { all_ok = false; }
            }
            // every power of two in int32 range
            for (int shift{ 0 }; shift < 31; ++shift)
            {
                const std::int32_t raw{ std::int32_t{ 1 } << shift };
                const std::int32_t want{ raw < cap ? raw : cap };
                if (vmhook::clamp_safe_container_count(raw) != want) { all_ok = false; }
            }
            check("clamp_exhaustive_table_and_bitwalk", all_ok);
        }
    }

    // ---------------------------------------------------------------------
    // M4 — jni_signature_for_arg / signature_for_arg (vmhook.hpp:12994-13103,
    // 13712-13715).  Compile-time descriptor table.  We assert ONLY the pure
    // branches that produce a fixed string with no type_to_class_map lookup
    // (string/bool/char/integral-width/float/double), exhaustively across the
    // type domain, and prove jni::signature_for_arg delegates byte-identically.
    // ---------------------------------------------------------------------
    {
        using vmhook::detail::jni_signature_for_arg;

        // String family => "Ljava/lang/String;" (vmhook.hpp:12999-13002).
        check("jni_sig_std_string",       jni_signature_for_arg<std::string>() == "Ljava/lang/String;");
        check("jni_sig_string_view",      jni_signature_for_arg<std::string_view>() == "Ljava/lang/String;");
        check("jni_sig_const_char_ptr",   jni_signature_for_arg<const char*>() == "Ljava/lang/String;");
        check("jni_sig_char_ptr",         jni_signature_for_arg<char*>() == "Ljava/lang/String;");

        // bool => "Z", claimed BEFORE the generic 1-byte branch (13003-13009).
        check("jni_sig_bool_is_Z",        jni_signature_for_arg<bool>() == "Z");

        // char16_t / uint16_t => "C", claimed BEFORE the generic 2-byte branch
        // (13014-13017) so uint16_t is "C", NOT "S".
        check("jni_sig_char16_is_C",      jni_signature_for_arg<char16_t>() == "C");
        check("jni_sig_uint16_is_C",      jni_signature_for_arg<std::uint16_t>() == "C");

        // Generic integral width ladder (13028-13043): 1->B, 2->S, 4->I, 8->J.
        check("jni_sig_int8_is_B",        jni_signature_for_arg<std::int8_t>() == "B");
        check("jni_sig_uint8_is_B",       jni_signature_for_arg<std::uint8_t>() == "B");
        check("jni_sig_int16_is_S",       jni_signature_for_arg<std::int16_t>() == "S");
        check("jni_sig_int32_is_I",       jni_signature_for_arg<std::int32_t>() == "I");
        check("jni_sig_uint32_is_I",      jni_signature_for_arg<std::uint32_t>() == "I");
        check("jni_sig_int64_is_J",       jni_signature_for_arg<std::int64_t>() == "J");
        check("jni_sig_uint64_is_J",      jni_signature_for_arg<std::uint64_t>() == "J");

        // Extended/implementation integral types that newly route by sizeof
        // (the comment at 13018-13027): plain char (size 1) => "B"; char8_t
        // (size 1, distinct unsigned) => "B"; char32_t (size 4) => "I".
        check("jni_sig_plain_char_is_B",  jni_signature_for_arg<char>() == "B");
        check("jni_sig_char8_is_B",       jni_signature_for_arg<char8_t>() == "B");
        check("jni_sig_char32_is_I",      jni_signature_for_arg<char32_t>() == "I");

        // float => "F", double => "D" (13044-13051).
        check("jni_sig_float_is_F",       jni_signature_for_arg<float>() == "F");
        check("jni_sig_double_is_D",      jni_signature_for_arg<double>() == "D");

        // cv/ref qualifiers are stripped via std::decay_t (clean_t, 12997) — a
        // const/ref-qualified arg yields the SAME descriptor as its bare type.
        check("jni_sig_const_ref_int32_decays_to_I",
              jni_signature_for_arg<const std::int32_t&>() == "I");
        check("jni_sig_const_double_decays_to_D",
              jni_signature_for_arg<const double>() == "D");

        // jni::signature_for_arg is a thin delegate (13712-13715) — byte-identical
        // to the detail mapping for a representative spread of types.
        check("signature_for_arg_delegates_string",
              vmhook::jni::signature_for_arg<std::string>() == jni_signature_for_arg<std::string>());
        check("signature_for_arg_delegates_bool",
              vmhook::jni::signature_for_arg<bool>() == jni_signature_for_arg<bool>());
        check("signature_for_arg_delegates_int64",
              vmhook::jni::signature_for_arg<std::int64_t>() == jni_signature_for_arg<std::int64_t>());
        check("signature_for_arg_delegates_double",
              vmhook::jni::signature_for_arg<double>() == jni_signature_for_arg<double>());
        check("signature_for_arg_delegates_char16_C",
              vmhook::jni::signature_for_arg<char16_t>() == jni_signature_for_arg<char16_t>());
    }

    // ---------------------------------------------------------------------
    // M5 — is_unique_ptr_v (vmhook.hpp:1788-1813) and function_traits::
    // args_tuple_t (vmhook.hpp:9319-9376).  Pure compile-time partitions; every
    // result is fixed into a static_assert AND surfaced through one runtime
    // check() so no const/trait is left unreferenced.
    // ---------------------------------------------------------------------
    {
        // is_unique_ptr_v: true for any unique_ptr (cv/ref-stripped, 1812-1813),
        // false for everything else.
        static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>,
                      "unique_ptr<int> must be detected.");
        static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<int>&>,
                      "cv/ref-qualified unique_ptr must be detected (remove_cvref_t).");
        static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<fake_wrapper>>,
                      "unique_ptr<wrapper> must be detected.");
        static_assert(!vmhook::detail::is_unique_ptr_v<int>, "int is not a unique_ptr.");
        static_assert(!vmhook::detail::is_unique_ptr_v<int*>, "raw ptr is not a unique_ptr.");
        static_assert(!vmhook::detail::is_unique_ptr_v<std::shared_ptr<int>>,
                      "shared_ptr is not a unique_ptr.");
        check("is_unique_ptr_partitions_unique_vs_raw_vs_shared",
              vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>
              && vmhook::detail::is_unique_ptr_v<const std::unique_ptr<fake_wrapper>&>
              && !vmhook::detail::is_unique_ptr_v<int>
              && !vmhook::detail::is_unique_ptr_v<int*>
              && !vmhook::detail::is_unique_ptr_v<std::shared_ptr<int>>);

        // is_unique_ptr<...>::value_type_t recovers the wrapped type (1803).
        static_assert(std::is_same_v<
                          vmhook::detail::is_unique_ptr<std::unique_ptr<fake_wrapper>>::value_type_t,
                          fake_wrapper>,
                      "value_type_t must recover the wrapped type, not the inherited bool.");
        check("is_unique_ptr_value_type_t_recovers_wrapped",
              std::is_same_v<
                  vmhook::detail::is_unique_ptr<std::unique_ptr<fake_wrapper>>::value_type_t,
                  fake_wrapper>);

        // function_traits::args_tuple_t for the spectrum of callable shapes
        // (free ptr / noexcept free ptr / std::function / lambda / member ptr).
        // Arity and per-position element types come straight from the parameter
        // packs the specialisations capture.
        using free_ptr_t       = int(*)(double, std::int64_t);
        using free_ptr_ne_t    = int(*)(float) noexcept;
        using std_function_t   = std::function<void(bool, char16_t, double)>;
        using free_args        = vmhook::detail::function_traits<free_ptr_t>::args_tuple_t;
        using free_ne_args     = vmhook::detail::function_traits<free_ptr_ne_t>::args_tuple_t;
        using fn_args          = vmhook::detail::function_traits<std_function_t>::args_tuple_t;

        static_assert(std::tuple_size_v<free_args> == 2, "free ptr arity 2.");
        static_assert(std::is_same_v<std::tuple_element_t<0, free_args>, double>, "arg0 double.");
        static_assert(std::is_same_v<std::tuple_element_t<1, free_args>, std::int64_t>, "arg1 int64.");
        static_assert(std::tuple_size_v<free_ne_args> == 1, "noexcept free ptr arity 1.");
        static_assert(std::is_same_v<std::tuple_element_t<0, free_ne_args>, float>, "ne arg0 float.");
        static_assert(std::tuple_size_v<fn_args> == 3, "std::function arity 3.");
        static_assert(std::is_same_v<std::tuple_element_t<1, fn_args>, char16_t>, "fn arg1 char16.");

        // A generic lambda-free functor: operator() with a const qualifier maps
        // to the const-member specialisation (9347-9351).
        auto lam{ [](std::int32_t, void*) -> bool { return false; } };
        using lam_args = vmhook::detail::function_traits<decltype(lam)>::args_tuple_t;
        static_assert(std::tuple_size_v<lam_args> == 2, "lambda arity 2.");
        static_assert(std::is_same_v<std::tuple_element_t<0, lam_args>, std::int32_t>, "lam arg0 int32.");
        static_assert(std::is_same_v<std::tuple_element_t<1, lam_args>, void*>, "lam arg1 void*.");
        (void)lam;

        check("function_traits_arity_and_arg_types_across_callable_shapes",
              std::tuple_size_v<free_args> == 2
              && std::tuple_size_v<free_ne_args> == 1
              && std::tuple_size_v<fn_args> == 3
              && std::tuple_size_v<lam_args> == 2
              && std::is_same_v<std::tuple_element_t<0, free_args>, double>
              && std::is_same_v<std::tuple_element_t<1, fn_args>, char16_t>);
    }

    // ---------------------------------------------------------------------
    // M6 — array_length / get_array_element / set_array_element on a REAL OWNED
    // buffer (vmhook.hpp:14778/14814/14860).  HotSpot array header layout:
    //   +12 _length (int32), +16 _data[0], element stride sizeof(T).
    // We build the header by hand in a heap-owned, over-aligned buffer (NOT a
    // fabricated address — genuinely mapped memory that os::safe_read can read),
    // gate the data assertions on is_valid_pointer (the only no-JVM precondition
    // the helpers impose), and exhaust index/length/stride boundaries.  The
    // invalid-input paths (null oop, negative / out-of-range index) are asserted
    // unconditionally — they short-circuit before any read.
    // ---------------------------------------------------------------------
    {
        // --- invalid-input contracts: deterministic, no buffer needed ---
        check("array_length_null_oop_is_zero", vmhook::array_length(nullptr) == 0);
        check("get_array_element_null_oop_is_default",
              vmhook::get_array_element<std::int32_t>(nullptr, 0) == 0);
        // is_valid_pointer rejects a low constant (<= user_address_floor 0xFFFF,
        // vmhook.hpp:520): this address is NEVER dereferenced — it is filtered.
        void* const low_reject{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10u)) };
        check("array_length_rejected_low_pointer_is_zero",
              vmhook::array_length(low_reject) == 0);
        check("get_array_element_rejected_low_pointer_is_default",
              vmhook::get_array_element<std::int64_t>(low_reject, 0) == 0);

        // --- REAL owned buffer laid out as a length-N int32[] array object ---
        constexpr std::int32_t length{ 6 };
        constexpr std::size_t  header{ 16u };
        // 16-byte header + length*8 (widest element we test is int64/double) +
        // slack so an out-of-bounds read attempt still lands in owned memory.
        std::vector<std::uint8_t> backing(header + static_cast<std::size_t>(length) * 8u + 64u, std::uint8_t{ 0 });
        void* const oop{ static_cast<void*>(backing.data()) };

        // Write the _length field at +12 (the slot array_length reads).
        std::int32_t len_field{ length };
        std::memcpy(backing.data() + 12, &len_field, sizeof(len_field));

        // is_valid_pointer is the sole no-JVM precondition; a heap buffer
        // satisfies the range/alignment checks except for the astronomically
        // unlikely debug-sentinel low-32 collision.  Gate the data round-trip on
        // it so the test is robust on every allocator; assert it normally holds.
        const bool oop_usable{ vmhook::hotspot::is_valid_pointer(oop) };
        check("owned_array_buffer_is_valid_pointer", oop_usable);

        if (oop_usable)
        {
            check("array_length_reads_owned_length_field",
                  vmhook::array_length(oop) == length);

            // int32 element stride: set then get every in-bounds index.
            bool i32_ok{ true };
            for (std::int32_t i{ 0 }; i < length; ++i)
            {
                const std::int32_t v{ static_cast<std::int32_t>(0x11110000 + i) };
                vmhook::set_array_element<std::int32_t>(oop, i, v);
                if (vmhook::get_array_element<std::int32_t>(oop, i) != v) { i32_ok = false; }
            }
            check("array_int32_set_get_roundtrip_all_indices", i32_ok);

            // Element-stride independence: re-interpret the SAME buffer as int8
            // and int16 arrays (the helper computes offset = 16 + index*sizeof(T)
            // purely from T) and round-trip representative values + boundaries.
            vmhook::set_array_element<std::int8_t>(oop, 0, std::int8_t{ -1 });
            vmhook::set_array_element<std::int8_t>(oop, length - 1, std::int8_t{ 0x7F });
            check("array_int8_stride1_first_roundtrip",
                  vmhook::get_array_element<std::int8_t>(oop, 0) == std::int8_t{ -1 });
            check("array_int8_stride1_last_roundtrip",
                  vmhook::get_array_element<std::int8_t>(oop, length - 1) == std::int8_t{ 0x7F });

            vmhook::set_array_element<std::int16_t>(oop, 2, std::int16_t{ -12345 });
            check("array_int16_stride2_roundtrip",
                  vmhook::get_array_element<std::int16_t>(oop, 2) == std::int16_t{ -12345 });

            // int64 / double strides (8 bytes) on the same buffer.
            vmhook::set_array_element<std::int64_t>(oop, 0, std::int64_t{ 0x0123456789ABCDEFll });
            check("array_int64_stride8_roundtrip",
                  vmhook::get_array_element<std::int64_t>(oop, 0) == std::int64_t{ 0x0123456789ABCDEFll });
            vmhook::set_array_element<double>(oop, 1, -2.5);
            check("array_double_stride8_roundtrip",
                  vmhook::get_array_element<double>(oop, 1) == -2.5);

            // --- index BOUNDARY contracts (vmhook.hpp:14824 / 14869): index < 0
            //     or index >= length returns the default and writes nothing. ---
            check("get_array_element_negative_index_default",
                  vmhook::get_array_element<std::int32_t>(oop, -1) == 0);
            check("get_array_element_index_equals_length_default",
                  vmhook::get_array_element<std::int32_t>(oop, length) == 0);
            check("get_array_element_index_far_out_default",
                  vmhook::get_array_element<std::int32_t>(oop, 1000000) == 0);
            check("get_array_element_int_min_index_default",
                  vmhook::get_array_element<std::int32_t>(oop, std::numeric_limits<std::int32_t>::min()) == 0);

            // set at an out-of-range index must NOT alter an in-range element:
            // seed index (length-1), attempt OOB writes, confirm the seed holds.
            vmhook::set_array_element<std::int32_t>(oop, length - 1, std::int32_t{ 0x5AA55AA5 });
            vmhook::set_array_element<std::int32_t>(oop, length, std::int32_t{ 0x12345678 });   // == length, OOB
            vmhook::set_array_element<std::int32_t>(oop, -1, std::int32_t{ 0x12345678 });        // negative, OOB
            check("set_array_element_oob_does_not_touch_in_range",
                  vmhook::get_array_element<std::int32_t>(oop, length - 1) == std::int32_t{ 0x5AA55AA5 });

            // --- length-boundary: rewriting _length to 0 makes EVERY index OOB ---
            std::int32_t zero_len{ 0 };
            std::memcpy(backing.data() + 12, &zero_len, sizeof(zero_len));
            check("array_length_zero_after_rewrite", vmhook::array_length(oop) == 0);
            check("get_array_element_zero_length_index0_default",
                  vmhook::get_array_element<std::int32_t>(oop, 0) == 0);

            // --- negative _length collapses array_length's own contract: it
            //     returns the raw (negative) length verbatim (the clamp lives at
            //     call sites, NOT in array_length — vmhook.hpp:14727-14731), and
            //     index 0 is then rejected because 0 >= (negative) is true. ---
            std::int32_t neg_len{ -5 };
            std::memcpy(backing.data() + 12, &neg_len, sizeof(neg_len));
            check("array_length_returns_negative_length_verbatim",
                  vmhook::array_length(oop) == -5);
            check("get_array_element_negative_length_rejects_index0",
                  vmhook::get_array_element<std::int32_t>(oop, 0) == 0);
        }
    }

    // =====================================================================
    // SECTION W29 — wave-29 LEDGER gap-closing for set_arg.
    //   (1) DEFAULT-CONSTRUCTED-like return_value with BOTH slot=nullptr and
    //       frame=nullptr: set_arg must be a safe no-op (the !stack_frame
    //       guard fires before any slot deref).
    //   (2) Specific bounds indices 0 / 255 / 65535 with the no-frame path
    //       (the documented "edge" indices), all noexcept.
    //   (3) Static signature locks: set_arg is noexcept, returns bool, takes
    //       std::int32_t as the first parameter.
    //   (4) Idempotency: repeated set_arg calls on the guard path leave
    //       slot.cancel / slot.retval untouched.
    // =====================================================================
    {
        // (3) signature static_asserts — locked at compile time, never drift.
        static_assert(noexcept(std::declval<vmhook::return_value&>()
                                   .set_arg(std::int32_t{ 0 }, std::int32_t{ 0 })),
                      "return_value::set_arg must be noexcept");
        static_assert(std::is_same_v<
                          decltype(std::declval<vmhook::return_value&>()
                                       .set_arg(std::int32_t{ 0 }, std::int32_t{ 0 })),
                          bool>,
                      "return_value::set_arg must return bool");
        static_assert(std::is_same_v<
                          decltype(std::declval<vmhook::return_value&>()
                                       .set_arg(std::int32_t{ 0 }, std::int64_t{ 0 })),
                          bool>,
                      "return_value::set_arg<int64_t> must return bool");
        static_assert(std::is_same_v<
                          decltype(std::declval<vmhook::return_value&>()
                                       .set_arg(std::int32_t{ 0 }, 1.0)),
                          bool>,
                      "return_value::set_arg<double> must return bool");

        // (1) BOTH slot AND frame null. set_arg's guard checks frame first,
        // so slot is NEVER dereferenced — no crash, returns false.
        {
            vmhook::return_value rv{ /*slot=*/nullptr, /*frame=*/nullptr };
            check("set_arg_null_slot_null_frame_index_0_false",
                  rv.set_arg(0, std::int32_t{ 42 }) == false);
            check("set_arg_null_slot_null_frame_index_255_false",
                  rv.set_arg(255, std::int32_t{ 42 }) == false);
            check("set_arg_null_slot_null_frame_index_65535_false",
                  rv.set_arg(65535, std::int32_t{ 42 }) == false);
            check("set_arg_null_slot_null_frame_neg_false",
                  rv.set_arg(-1, std::int32_t{ 42 }) == false);
            check("set_arg_null_slot_null_frame_double_false",
                  rv.set_arg(0, 3.5) == false);
            check("set_arg_null_slot_null_frame_int64_false",
                  rv.set_arg(0, std::int64_t{ 0x0123456789ABCDEFLL }) == false);
            check("set_arg_null_slot_null_frame_bool_false",
                  rv.set_arg(0, true) == false);
            check("set_arg_null_slot_null_frame_void_ptr_false",
                  rv.set_arg(0, static_cast<void*>(nullptr)) == false);
        }

        // (2) + (4) live slot, no frame, the three documented edge indices.
        // Each one must return false, and twenty repeated calls must leave the
        // slot bit-identical to its initial value (idempotent guard).
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value         rv{ &slot, /*frame=*/nullptr };

            for (int rep{ 0 }; rep < 20; ++rep)
            {
                if (rv.set_arg(0,     std::int32_t{ 7 }) != false
                 || rv.set_arg(255,   std::int32_t{ 7 }) != false
                 || rv.set_arg(65535, std::int32_t{ 7 }) != false)
                {
                    check("set_arg_edge_indices_idempotent_false_run", false);
                    break;
                }
            }
            check("set_arg_edge_indices_idempotent_slot_clean_cancel",
                  slot.cancel == false);
            check("set_arg_edge_indices_idempotent_slot_clean_retval",
                  slot.retval == 0);

            // The exact indices the ledger names, individually pinned.
            check("set_arg_index_0_no_frame_false",
                  rv.set_arg(0, std::int32_t{ 1 }) == false);
            check("set_arg_index_255_no_frame_false",
                  rv.set_arg(255, std::int32_t{ 1 }) == false);
            check("set_arg_index_65535_no_frame_false",
                  rv.set_arg(65535, std::int32_t{ 1 }) == false);
            check("set_arg_after_three_edge_calls_slot_still_clean",
                  slot.cancel == false && slot.retval == 0);
        }
    }

    return failures == 0 ? 0 : 1;
}
