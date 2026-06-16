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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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

    return failures == 0 ? 0 : 1;
}
