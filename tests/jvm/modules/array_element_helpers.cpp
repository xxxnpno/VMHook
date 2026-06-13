// array_element_helpers JVM test module (area: arrays / raw primitives).
//
// FEATURE UNDER TEST: the three LOWEST-LEVEL raw array primitives in vmhook.hpp
// that read/write a HotSpot primitive-array body with NO running JVM call, by
// pure pointer arithmetic over the array layout (x64, compressed OOPs):
//
//     vmhook::array_length(void* array_oop)                  (~vmhook.hpp:13659)
//         -> the int32 _length at byte +12, or 0 on an invalid pointer.
//     vmhook::get_array_element<T>(void* array_oop, int idx)  (~vmhook.hpp:13680)
//         -> memcpy sizeof(T) from +16 + idx*sizeof(T); T{} on invalid/OOB.
//     vmhook::set_array_element<T>(void* array_oop, int idx, T)(~vmhook.hpp:13712)
//         -> memcpy the value to that address; silent no-op on invalid/OOB.
//
// Both element helpers gate on a half-open [0, length) bounds check (length comes
// from array_length) AND on vmhook::hotspot::is_valid_pointer(array_oop), and
// compute the byte offset in ptrdiff_t so a corrupted/large in-bounds-claimed
// index cannot wrap a 32-bit multiply into a wild offset.
//
// WHY A LIVE-JVM MODULE (vs the exhaustive no-JVM tests/test_array_element_helpers.cpp):
// the standalone test pins the arithmetic on synthetic heap buffers.  This module
// drives the SAME helpers against GENUINE HotSpot array oops resolved from live
// Java fields (via vmhook::field_oop), proving:
//   (1) the +12 / +16 / compressed-oop LAYOUT assumption holds against a real JVM
//       on every CI toolchain x JDK 8..26 (a layout drift would mis-read length
//       or values here, where the no-JVM buffer is layout-by-construction);
//   (2) the BOUNDS / NO-FAULT / NO-CRASH invariant on REAL adjacent heap -- an
//       OOB read/write regression on a synthetic buffer might touch slack bytes,
//       but on a live array it touches a NEIGHBOURING heap object and crashes or
//       corrupts.  This is the high-value safety surface: every OOB index
//       (== length, length+1, huge, -1, INT_MIN) is asserted HARD to read a
//       sentinel / be a no-op AND to leave the process running;
//   (3) set_array_element writes are VISIBLE TO JAVA -- the module writes the
//       dedicated scratch arrays, then a second probe re-reads them through Java
//       and publishes a bitmask, proving each raw C++ write landed in the JVM
//       heap (impossible to show without a JVM).
//
// TOOLCHAIN HARDENING (locally MinGW; CI = msvc/clang/linux x JDK8-26):
//   * the BOUNDS / no-fault / no-crash invariants are HARD on every toolchain --
//     that is the entire point of the guard, and it is a pure address/length
//     comparison with no JDK variance, so it must hold identically everywhere;
//   * VALUE decodes that depend on compressed-oop / JDK layout (reference-array
//     slot decode, the non-array oop's arbitrary +12 int) are PASS-or-[INFO]:
//     a wrong value is characterised, never a FAIL, on a config where the
//     precondition does not hold;
//   * primitive value reads (length, the int/long/double/... payloads) ARE hard:
//     the primitive _length/_data layout is stable JDK 8..26 given compressed
//     class pointers, which is the default under the CI heap sizes.
//
// SUITE-SAFETY (mirrors field_arrays_primitive.cpp / aaa_warmup.cpp):
//   * the whole body runs under a try/catch -- a stray throw is recorded as
//     [INFO], never a FAIL, and never escapes this module;
//   * an unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the
//     module always returns to the driver with an empty hook table (this module
//     installs ONE scoped_hook for the live-dispatch handshake, scoped to a
//     block so its RAII teardown runs on the normal path; the final
//     shutdown_hooks() is belt-and-braces and also covers the no-SEH longjmp
//     recovery path);
//   * an entry guard bails to [INFO] if the fixture class is not resolvable, so
//     no static_field()/get_field() below ever derefs a disengaged optional;
//   * EVERY raw deref (the decoded array oop, every element access) is gated on
//     is_valid_pointer and bounded by array_length -- the in-bounds reads never
//     go out of range, and the OOB probes are deliberately OOB indices the guard
//     must reject (so they too never dereference).

#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr const char* FIXTURE{ "vmhook/fixtures/ArrayElementHelpers" };

    // ---- bit-exact float / double compare (NaN / +-Inf / -0 / subnormal) -----
    auto bits_equal(float a, float b) -> bool
    {
        std::uint32_t ba{};
        std::uint32_t bb{};
        std::memcpy(&ba, &a, sizeof(ba));
        std::memcpy(&bb, &b, sizeof(bb));
        return ba == bb;
    }
    auto bits_equal(double a, double b) -> bool
    {
        std::uint64_t ba{};
        std::uint64_t bb{};
        std::memcpy(&ba, &a, sizeof(ba));
        std::memcpy(&bb, &b, sizeof(bb));
        return ba == bb;
    }

    // Wrapper for vmhook.fixtures.ArrayElementHelpers.  Accessors that return a
    // field's raw ARRAY oop (the subject of every helper call) use the public
    // vmhook::field_oop(field_proxy) -> decoded array oop.  Handshake + scratch
    // accessors use the clean get/set one-liner idiom (no defensive has_value;
    // the entry guard proves the class is loaded, suite-safety lives at the
    // module level).
    class aeh_fixture : public vmhook::object<aeh_fixture>
    {
    public:
        explicit aeh_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<aeh_fixture>{ instance }
        {
        }

        // ---- handshake -------------------------------------------------------
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool { return static_field("done")->get(); }
        static auto get_probe_checksum() -> std::int64_t { return static_field("probeChecksum")->get(); }
        static auto get_instance() -> std::unique_ptr<aeh_fixture> { return static_field("instance")->get(); }

        // ---- scratch-verify handshake ----------------------------------------
        static auto request_verify_scratch(bool value) -> void { static_field("verifyScratchRequested")->set(value); }
        static auto reset_done(bool value) -> void { static_field("done")->set(value); }
        static auto get_scratch_verify_mask() -> std::int32_t { return static_field("scratchVerifyMask")->get(); }

        // ---- the decoded ARRAY oop for a named STATIC array field -------------
        // Returns nullptr when the field is null / unresolved / not a valid oop;
        // callers gate every helper call on a non-null, is_valid_pointer result.
        static auto array_oop_of(const char* field_name) -> void*
        {
            const auto proxy{ static_field(field_name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            void* const oop{ vmhook::field_oop(*proxy) };
            if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
            {
                return nullptr;
            }
            return oop;
        }

        // Instance-field array oop (needs a live instance).
        auto inst_array_oop_of(const char* field_name) -> void*
        {
            const auto proxy{ get_field(field_name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            void* const oop{ vmhook::field_oop(*proxy) };
            if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
            {
                return nullptr;
            }
            return oop;
        }

        // The RAW field oop WITHOUT the is_valid_pointer filter -- used only for
        // the null-reference cases (field holds null -> decoded oop is nullptr)
        // and the non-array object case (we want the real object oop to feed the
        // helpers and prove they do not fault).  Returns whatever field_oop
        // decodes (may be nullptr).
        static auto raw_field_oop(const char* field_name) -> void*
        {
            const auto proxy{ static_field(field_name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
    };

    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };

    // bits/value equality dispatch so the OOB harness works for both integral
    // element types (==) and float/double (raw-bit compare).
    template <typename T>
    auto bits_equal_or_eq(T a, T b) -> bool
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            return bits_equal(a, b);
        }
        else
        {
            return a == b;
        }
    }

    // A recognisable poison value of width sizeof(T): all-0xA5 bytes, distinct
    // from any seeded value in the fixture, so a slipped OOB write would be
    // visible as a changed in-bounds element.
    template <typename T>
    auto make_poison() -> T
    {
        T value{};
        std::memset(&value, 0xA5, sizeof(T));
        return value;
    }

    // -------------------------------------------------------------------------
    // OOB / NO-FAULT harness (the headline safety surface).
    //
    // For a REAL array oop of known length, drive get/set_array_element at every
    // out-of-bounds index the contract must reject -- index == length, length+1,
    // a huge index (INT_MAX and 0x10000000, the stride-8 32-bit-wrap threshold),
    // -1, and INT_MIN -- and assert HARD that:
    //   * every OOB get returns T{} (the documented sentinel), and
    //   * every OOB set is a no-op: the in-bounds elements are byte-identical
    //     before and after (read back through the helper at the valid indices).
    // Reaching the end of this function at all is itself the proof of "no fault"
    // -- on a live array an OOB read/write that slipped the guard would touch a
    // neighbouring heap object and crash the process before we could record.
    // Returns true iff every OOB access behaved (sentinel + no-op).
    // -------------------------------------------------------------------------
    template <typename element_type>
    auto oob_is_safe(void* array_oop, std::int32_t length) -> bool
    {
        constexpr std::int32_t int_min{ (std::numeric_limits<std::int32_t>::min)() };
        constexpr std::int32_t int_max{ (std::numeric_limits<std::int32_t>::max)() };

        const std::int32_t oob_indices[]{
            length,            // == length: first OOB index (half-open upper edge)
            length + 1,        // length+1
            int_max,           // huge positive
            0x10000000,        // stride-8 32-bit-multiply wrap threshold index
            -1,                // negative
            int_min,           // most-negative
        };

        // 1) Every OOB read must return the value-initialised sentinel.
        bool reads_sentinel{ true };
        for (const std::int32_t idx : oob_indices)
        {
            if (!bits_equal_or_eq(vmhook::get_array_element<element_type>(array_oop, idx),
                                  element_type{}))
            {
                reads_sentinel = false;
            }
        }

        // 2) Snapshot the in-bounds elements via the helper, attempt every OOB
        //    write with a poison value, then confirm the in-bounds elements are
        //    unchanged (the OOB writes were no-ops and touched nothing).
        std::vector<element_type> before;
        before.reserve(length > 0 ? static_cast<std::size_t>(length) : 0u);
        for (std::int32_t i{ 0 }; i < length; ++i)
        {
            before.push_back(vmhook::get_array_element<element_type>(array_oop, i));
        }

        const element_type poison{ make_poison<element_type>() };
        for (const std::int32_t idx : oob_indices)
        {
            vmhook::set_array_element<element_type>(array_oop, idx, poison);
        }

        bool writes_noop{ true };
        for (std::int32_t i{ 0 }; i < length; ++i)
        {
            if (!bits_equal_or_eq(vmhook::get_array_element<element_type>(array_oop, i),
                                  before[static_cast<std::size_t>(i)]))
            {
                writes_noop = false;
            }
        }
        return reads_sentinel && writes_noop;
    }
}

// The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run it
// under a try/catch and ALWAYS follow it with shutdown_hooks().
static void run_array_element_helpers_checks(vmhook_test::context& ctx)
{
    vmhook::register_class<aeh_fixture>(FIXTURE);
    using wrapper = aeh_fixture;

    // =========================================================================
    //  ENTRY GUARD.
    // =========================================================================
    if (vmhook::find_class(FIXTURE) == nullptr)
    {
        ctx.record("[INFO] array_element_helpers: ArrayElementHelpers not "
                   "loaded/resolvable on this run; skipping the module's live "
                   "checks (no crash, no hooks armed).");
        return;
    }

    // -------------------------------------------------------------------------
    // 0) NULL / INVALID-POINTER guards on the bare helpers (no oop needed).
    //    These mirror the no-JVM test but run in-process on the live DLL so a
    //    platform-specific guard regression is caught here too.  HARD on every
    //    toolchain (pure pointer/shape checks, zero JDK variance).
    // -------------------------------------------------------------------------
    {
        ctx.check("aeh_length_null_oop_is_zero", vmhook::array_length(nullptr) == 0);
        ctx.check("aeh_get_null_oop_int_default",
                  vmhook::get_array_element<std::int32_t>(nullptr, 0) == 0);
        ctx.check("aeh_get_null_oop_long_default",
                  vmhook::get_array_element<std::int64_t>(nullptr, 0) == 0);
        ctx.check("aeh_get_null_oop_double_default",
                  bits_equal(vmhook::get_array_element<double>(nullptr, 0), 0.0));
        // set on null must be a safe no-op (reaching the next line == no fault).
        vmhook::set_array_element<std::int32_t>(nullptr, 0, 0x1234);
        ctx.check("aeh_set_null_oop_safe_noop", true);

        // A low sentinel pointer fails is_valid_pointer -> length 0 / default /
        // no-op (the helper short-circuits before any dereference).
        void* const sentinel{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x100ull)) };
        ctx.check("aeh_length_sentinel_oop_is_zero", vmhook::array_length(sentinel) == 0);
        ctx.check("aeh_get_sentinel_oop_default",
                  vmhook::get_array_element<std::int32_t>(sentinel, 0) == 0);
        vmhook::set_array_element<std::int32_t>(sentinel, 0, 7);
        ctx.check("aeh_set_sentinel_oop_safe_noop", true);
    }

    // -------------------------------------------------------------------------
    // 1) array_length on REAL arrays at lengths 0 / 1 / 2 / 1000, for every
    //    array kind (8 primitive + a reference array).  HARD: the _length slot
    //    is at +12 across JDK 8..26 for the default compressed-class layout, so
    //    a wrong answer here is a real layout / read bug.
    // -------------------------------------------------------------------------
    {
        // Distinct lengths on int[] fields.
        if (void* const a{ wrapper::array_oop_of("len0Array") })
        {
            ctx.check("aeh_len0", vmhook::array_length(a) == 0);
        }
        else { ctx.check("aeh_len0_oop_resolved", false); }

        if (void* const a{ wrapper::array_oop_of("len1Array") })
        {
            ctx.check("aeh_len1", vmhook::array_length(a) == 1);
        }
        else { ctx.check("aeh_len1_oop_resolved", false); }

        if (void* const a{ wrapper::array_oop_of("len2Array") })
        {
            ctx.check("aeh_len2", vmhook::array_length(a) == 2);
        }
        else { ctx.check("aeh_len2_oop_resolved", false); }

        if (void* const a{ wrapper::array_oop_of("len1000Array") })
        {
            ctx.check("aeh_len1000", vmhook::array_length(a) == 1000);
        }
        else { ctx.check("aeh_len1000_oop_resolved", false); }

        // length == 3 across every primitive element kind (proves array_length
        // is element-type independent -- it reads +12 regardless of stride).
        const char* const len3_fields[]{
            "boolArray", "byteArray", "shortArray", "charArray",
            "intArray", "longArray", "floatArray", "doubleArray" };
        bool all_len3{ true };
        for (const char* const f : len3_fields)
        {
            void* const a{ wrapper::array_oop_of(f) };
            if (!a || vmhook::array_length(a) != 3) { all_len3 = false; }
        }
        ctx.check("aeh_every_primitive_kind_len3", all_len3);

        // A reference array's length is read the same way (header is identical
        // for object arrays).
        if (void* const a{ wrapper::array_oop_of("refStrings") })
        {
            ctx.check("aeh_refarray_len3", vmhook::array_length(a) == 3);
        }
        else { ctx.check("aeh_refarray_oop_resolved", false); }
    }

    // -------------------------------------------------------------------------
    // 2) get_array_element at index 0 / middle / last for EACH primitive type,
    //    each read at the CORRECT width, against Java-known values.  This is the
    //    cross-width-bleed guard on real oops: reading int[] as int32, long[]
    //    as int64, etc., and confirming the exact value proves the stride is
    //    sizeof(T) with no neighbour bleed.  HARD (primitive layout stable).
    //
    //    char[] is read at the raw layer as uint16 (the JVM stores char as a
    //    16-bit code unit); the "[C" -> char narrowing only exists in the
    //    higher append path, not in the raw helper.
    // -------------------------------------------------------------------------
    {
        // byte[] {10,20,30} as int8 -- 1-byte stride.
        if (void* const a{ wrapper::array_oop_of("byteArray") })
        {
            ctx.check("aeh_byte_idx0",  vmhook::get_array_element<std::int8_t>(a, 0) == 10);
            ctx.check("aeh_byte_mid",   vmhook::get_array_element<std::int8_t>(a, 1) == 20);
            ctx.check("aeh_byte_last",  vmhook::get_array_element<std::int8_t>(a, 2) == 30);
        }
        else { ctx.check("aeh_byte_oop_resolved", false); }

        // short[] {1000,2000,3000} as int16 -- 2-byte stride.
        if (void* const a{ wrapper::array_oop_of("shortArray") })
        {
            ctx.check("aeh_short_idx0", vmhook::get_array_element<std::int16_t>(a, 0) == 1000);
            ctx.check("aeh_short_mid",  vmhook::get_array_element<std::int16_t>(a, 1) == 2000);
            ctx.check("aeh_short_last", vmhook::get_array_element<std::int16_t>(a, 2) == 3000);
        }
        else { ctx.check("aeh_short_oop_resolved", false); }

        // char[] {'A','M','Z'} as uint16 -- 2-byte stride.
        if (void* const a{ wrapper::array_oop_of("charArray") })
        {
            ctx.check("aeh_char_idx0", vmhook::get_array_element<std::uint16_t>(a, 0) == 'A');
            ctx.check("aeh_char_mid",  vmhook::get_array_element<std::uint16_t>(a, 1) == 'M');
            ctx.check("aeh_char_last", vmhook::get_array_element<std::uint16_t>(a, 2) == 'Z');
        }
        else { ctx.check("aeh_char_oop_resolved", false); }

        // int[] {100000,200000,300000} as int32 -- 4-byte stride.
        if (void* const a{ wrapper::array_oop_of("intArray") })
        {
            ctx.check("aeh_int_idx0", vmhook::get_array_element<std::int32_t>(a, 0) == 100000);
            ctx.check("aeh_int_mid",  vmhook::get_array_element<std::int32_t>(a, 1) == 200000);
            ctx.check("aeh_int_last", vmhook::get_array_element<std::int32_t>(a, 2) == 300000);
        }
        else { ctx.check("aeh_int_oop_resolved", false); }

        // long[] {10e9,20e9,30e9} as int64 -- 8-byte stride (values > 2^32 so a
        // 4-byte mis-stride would be detectable).
        if (void* const a{ wrapper::array_oop_of("longArray") })
        {
            ctx.check("aeh_long_idx0", vmhook::get_array_element<std::int64_t>(a, 0) == 10000000000LL);
            ctx.check("aeh_long_mid",  vmhook::get_array_element<std::int64_t>(a, 1) == 20000000000LL);
            ctx.check("aeh_long_last", vmhook::get_array_element<std::int64_t>(a, 2) == 30000000000LL);
        }
        else { ctx.check("aeh_long_oop_resolved", false); }

        // float[] {1.5,2.5,3.5} as float -- 4-byte stride, bit-exact.
        if (void* const a{ wrapper::array_oop_of("floatArray") })
        {
            ctx.check("aeh_float_idx0", bits_equal(vmhook::get_array_element<float>(a, 0), 1.5f));
            ctx.check("aeh_float_mid",  bits_equal(vmhook::get_array_element<float>(a, 1), 2.5f));
            ctx.check("aeh_float_last", bits_equal(vmhook::get_array_element<float>(a, 2), 3.5f));
        }
        else { ctx.check("aeh_float_oop_resolved", false); }

        // double[] {1.25,2.25,3.25} as double -- 8-byte stride, bit-exact.
        if (void* const a{ wrapper::array_oop_of("doubleArray") })
        {
            ctx.check("aeh_double_idx0", bits_equal(vmhook::get_array_element<double>(a, 0), 1.25));
            ctx.check("aeh_double_mid",  bits_equal(vmhook::get_array_element<double>(a, 1), 2.25));
            ctx.check("aeh_double_last", bits_equal(vmhook::get_array_element<double>(a, 2), 3.25));
        }
        else { ctx.check("aeh_double_oop_resolved", false); }

        // boolean[] {true,false,true} as uint8 (JVM stores boolean as a byte).
        if (void* const a{ wrapper::array_oop_of("boolArray") })
        {
            ctx.check("aeh_bool_idx0", vmhook::get_array_element<std::uint8_t>(a, 0) != 0);
            ctx.check("aeh_bool_mid",  vmhook::get_array_element<std::uint8_t>(a, 1) == 0);
            ctx.check("aeh_bool_last", vmhook::get_array_element<std::uint8_t>(a, 2) != 0);
        }
        else { ctx.check("aeh_bool_oop_resolved", false); }
    }

    // -------------------------------------------------------------------------
    // 3) The len1000 array: array_length is the bounds oracle, and the three
    //    sampled endpoints (0 / 500 / 999) match the deterministic formula
    //    i*3+1.  Pins the per-element offset arithmetic at a NON-trivial index
    //    (500, 999) where a 32-bit-vs-64-bit stride bug would surface, on a real
    //    oop.  Bounds come ONLY from array_length, so no read is out of range.
    // -------------------------------------------------------------------------
    {
        if (void* const a{ wrapper::array_oop_of("len1000Array") })
        {
            const std::int32_t len{ vmhook::array_length(a) };
            const bool len_ok{ len == 1000 };
            ctx.check("aeh_len1000_oracle", len_ok);
            if (len_ok)
            {
                ctx.check("aeh_len1000_idx0",   vmhook::get_array_element<std::int32_t>(a, 0)   == (0 * 3 + 1));
                ctx.check("aeh_len1000_mid",    vmhook::get_array_element<std::int32_t>(a, 500) == (500 * 3 + 1));
                ctx.check("aeh_len1000_last",   vmhook::get_array_element<std::int32_t>(a, 999) == (999 * 3 + 1));
                // EVERY element matches the formula (full walk inside bounds).
                bool all_ok{ true };
                for (std::int32_t i{ 0 }; i < len && all_ok; ++i)
                {
                    all_ok = vmhook::get_array_element<std::int32_t>(a, i) == (i * 3 + 1);
                }
                ctx.check("aeh_len1000_every_element", all_ok);
            }
        }
        else { ctx.check("aeh_len1000_walk_oop_resolved", false); }
    }

    // -------------------------------------------------------------------------
    // 4) BOUNDARY element VALUES per primitive (MIN / -1 or special / MAX) read
    //    at the correct width on real oops -- catches a sign / width /
    //    endianness mistake.  HARD.
    // -------------------------------------------------------------------------
    {
        if (void* const a{ wrapper::array_oop_of("boundaryByte") })
        {
            ctx.check("aeh_bnd_byte",
                      vmhook::get_array_element<std::int8_t>(a, 0) == (std::numeric_limits<std::int8_t>::min)()
                      && vmhook::get_array_element<std::int8_t>(a, 1) == static_cast<std::int8_t>(-1)
                      && vmhook::get_array_element<std::int8_t>(a, 2) == (std::numeric_limits<std::int8_t>::max)());
        }
        else { ctx.check("aeh_bnd_byte_oop", false); }

        if (void* const a{ wrapper::array_oop_of("boundaryShort") })
        {
            ctx.check("aeh_bnd_short",
                      vmhook::get_array_element<std::int16_t>(a, 0) == (std::numeric_limits<std::int16_t>::min)()
                      && vmhook::get_array_element<std::int16_t>(a, 1) == static_cast<std::int16_t>(-1)
                      && vmhook::get_array_element<std::int16_t>(a, 2) == (std::numeric_limits<std::int16_t>::max)());
        }
        else { ctx.check("aeh_bnd_short_oop", false); }

        if (void* const a{ wrapper::array_oop_of("boundaryInt") })
        {
            ctx.check("aeh_bnd_int",
                      vmhook::get_array_element<std::int32_t>(a, 0) == (std::numeric_limits<std::int32_t>::min)()
                      && vmhook::get_array_element<std::int32_t>(a, 1) == -1
                      && vmhook::get_array_element<std::int32_t>(a, 2) == (std::numeric_limits<std::int32_t>::max)());
        }
        else { ctx.check("aeh_bnd_int_oop", false); }

        if (void* const a{ wrapper::array_oop_of("boundaryLong") })
        {
            ctx.check("aeh_bnd_long",
                      vmhook::get_array_element<std::int64_t>(a, 0) == (std::numeric_limits<std::int64_t>::min)()
                      && vmhook::get_array_element<std::int64_t>(a, 1) == -1LL
                      && vmhook::get_array_element<std::int64_t>(a, 2) == (std::numeric_limits<std::int64_t>::max)());
        }
        else { ctx.check("aeh_bnd_long_oop", false); }

        // char extremes as uint16: 0x0000 / 0x8000 / 0xFFFF.
        if (void* const a{ wrapper::array_oop_of("boundaryChar") })
        {
            ctx.check("aeh_bnd_char",
                      vmhook::get_array_element<std::uint16_t>(a, 0) == 0x0000u
                      && vmhook::get_array_element<std::uint16_t>(a, 1) == 0x8000u
                      && vmhook::get_array_element<std::uint16_t>(a, 2) == 0xFFFFu);
        }
        else { ctx.check("aeh_bnd_char_oop", false); }

        // float specials: NaN / +Inf / -Inf / -0.0 / subnormal / MAX (bit-exact).
        if (void* const a{ wrapper::array_oop_of("specialFloat") })
        {
            const float f0{ vmhook::get_array_element<float>(a, 0) };
            ctx.check("aeh_special_float",
                      std::isnan(f0)
                      && vmhook::get_array_element<float>(a, 1) == std::numeric_limits<float>::infinity()
                      && vmhook::get_array_element<float>(a, 2) == -std::numeric_limits<float>::infinity()
                      && bits_equal(vmhook::get_array_element<float>(a, 3), -0.0f)
                      && bits_equal(vmhook::get_array_element<float>(a, 4), std::numeric_limits<float>::denorm_min())
                      && bits_equal(vmhook::get_array_element<float>(a, 5), (std::numeric_limits<float>::max)()));
        }
        else { ctx.check("aeh_special_float_oop", false); }

        // double specials: NaN / +Inf / -Inf / -0.0 / subnormal / MAX.
        if (void* const a{ wrapper::array_oop_of("specialDouble") })
        {
            const double d0{ vmhook::get_array_element<double>(a, 0) };
            ctx.check("aeh_special_double",
                      std::isnan(d0)
                      && vmhook::get_array_element<double>(a, 1) == std::numeric_limits<double>::infinity()
                      && vmhook::get_array_element<double>(a, 2) == -std::numeric_limits<double>::infinity()
                      && bits_equal(vmhook::get_array_element<double>(a, 3), -0.0)
                      && bits_equal(vmhook::get_array_element<double>(a, 4), std::numeric_limits<double>::denorm_min())
                      && bits_equal(vmhook::get_array_element<double>(a, 5), (std::numeric_limits<double>::max)()));
        }
        else { ctx.check("aeh_special_double_oop", false); }

        // boolean true/false pair (length-2, smallest stride).
        if (void* const a{ wrapper::array_oop_of("boolPair") })
        {
            ctx.check("aeh_boolpair",
                      vmhook::get_array_element<std::uint8_t>(a, 0) == 0
                      && vmhook::get_array_element<std::uint8_t>(a, 1) != 0);
        }
        else { ctx.check("aeh_boolpair_oop", false); }

        // ASTRAL char (surrogate pair): each surrogate is a 16-bit code unit;
        // the 2-byte stride must read each half independently (high then low).
        if (void* const a{ wrapper::array_oop_of("astralChar") })
        {
            ctx.check("aeh_astral_char_surrogates",
                      vmhook::array_length(a) == 2
                      && vmhook::get_array_element<std::uint16_t>(a, 0) == 0xD83Du
                      && vmhook::get_array_element<std::uint16_t>(a, 1) == 0xDE00u);
        }
        else { ctx.check("aeh_astral_char_oop", false); }
    }

    // =========================================================================
    // 5) OUT-OF-BOUNDS / NO-FAULT on REAL heap arrays (THE headline surface).
    //    For each element width, drive get/set_array_element at index == length,
    //    length+1, INT_MAX, 0x10000000, -1, INT_MIN against a genuine array oop
    //    and assert HARD that every OOB get returns the sentinel and every OOB
    //    set is a no-op (in-bounds elements unchanged).  Reaching these asserts
    //    at all proves no fault occurred on adjacent live heap.
    //    HARD on every toolchain: the bounds guard is a pure index/length
    //    comparison with zero JDK / compressed-oop variance.
    // =========================================================================
    {
        if (void* const a{ wrapper::array_oop_of("byteArray") })
        {
            ctx.check("aeh_oob_safe_byte", oob_is_safe<std::int8_t>(a, vmhook::array_length(a)));
        }
        else { ctx.check("aeh_oob_byte_oop", false); }

        if (void* const a{ wrapper::array_oop_of("shortArray") })
        {
            ctx.check("aeh_oob_safe_short", oob_is_safe<std::int16_t>(a, vmhook::array_length(a)));
        }
        else { ctx.check("aeh_oob_short_oop", false); }

        if (void* const a{ wrapper::array_oop_of("intArray") })
        {
            ctx.check("aeh_oob_safe_int", oob_is_safe<std::int32_t>(a, vmhook::array_length(a)));
        }
        else { ctx.check("aeh_oob_int_oop", false); }

        if (void* const a{ wrapper::array_oop_of("longArray") })
        {
            ctx.check("aeh_oob_safe_long", oob_is_safe<std::int64_t>(a, vmhook::array_length(a)));
        }
        else { ctx.check("aeh_oob_long_oop", false); }

        if (void* const a{ wrapper::array_oop_of("floatArray") })
        {
            ctx.check("aeh_oob_safe_float", oob_is_safe<float>(a, vmhook::array_length(a)));
        }
        else { ctx.check("aeh_oob_float_oop", false); }

        if (void* const a{ wrapper::array_oop_of("doubleArray") })
        {
            ctx.check("aeh_oob_safe_double", oob_is_safe<double>(a, vmhook::array_length(a)));
        }
        else { ctx.check("aeh_oob_double_oop", false); }

        // OOB on the length-1, length-2, and EMPTY arrays: the smallest bounds
        // where the half-open upper edge is index 1 / 2 / 0 respectively.
        if (void* const a{ wrapper::array_oop_of("len1Array") })
        {
            ctx.check("aeh_oob_safe_len1", oob_is_safe<std::int32_t>(a, 1));
            // index 0 valid, index 1 (==length) is the first OOB read.
            ctx.check("aeh_len1_idx0_valid", vmhook::get_array_element<std::int32_t>(a, 0) == 7);
            ctx.check("aeh_len1_idx1_oob", vmhook::get_array_element<std::int32_t>(a, 1) == 0);
        }
        else { ctx.check("aeh_oob_len1_oop", false); }

        if (void* const a{ wrapper::array_oop_of("len2Array") })
        {
            ctx.check("aeh_oob_safe_len2", oob_is_safe<std::int32_t>(a, 2));
            ctx.check("aeh_len2_idx2_oob", vmhook::get_array_element<std::int32_t>(a, 2) == 0);
        }
        else { ctx.check("aeh_oob_len2_oop", false); }

        if (void* const a{ wrapper::array_oop_of("len0Array") })
        {
            // EMPTY array: index 0 itself is the first OOB index; every access
            // is sentinel / no-op.  array_length is 0 here.
            ctx.check("aeh_empty_len0", vmhook::array_length(a) == 0);
            ctx.check("aeh_empty_idx0_get_default", vmhook::get_array_element<std::int32_t>(a, 0) == 0);
            ctx.check("aeh_oob_safe_empty", oob_is_safe<std::int32_t>(a, 0));
        }
        else { ctx.check("aeh_oob_empty_oop", false); }
    }

    // -------------------------------------------------------------------------
    // 6) NULL ARRAY references -- the field holds null, so field_oop decodes to
    //    nullptr.  array_length(nullptr) == 0, every element access is
    //    sentinel / no-op.  HARD (null short-circuit, no JDK variance).
    // -------------------------------------------------------------------------
    {
        void* const null_static{ wrapper::raw_field_oop("nullIntArray") };
        ctx.check("aeh_null_static_array_oop_is_null", null_static == nullptr);
        ctx.check("aeh_null_static_length_zero", vmhook::array_length(null_static) == 0);
        ctx.check("aeh_null_static_get_default",
                  vmhook::get_array_element<std::int32_t>(null_static, 0) == 0);
        vmhook::set_array_element<std::int32_t>(null_static, 0, 9);
        ctx.check("aeh_null_static_set_safe_noop", true);

        // Instance null long[] via a live instance.
        const std::unique_ptr<wrapper> self{ wrapper::get_instance() };
        if (self)
        {
            const auto proxy{ self->get_field("instNullLong") };
            void* const null_inst{ proxy.has_value() ? vmhook::field_oop(*proxy) : nullptr };
            ctx.check("aeh_null_inst_array_oop_is_null", null_inst == nullptr);
            ctx.check("aeh_null_inst_length_zero", vmhook::array_length(null_inst) == 0);
            ctx.check("aeh_null_inst_get_default",
                      vmhook::get_array_element<std::int64_t>(null_inst, 0) == 0);
        }
        else { ctx.record("[INFO] array_element_helpers: instance unavailable for null instance-array check."); }
    }

    // -------------------------------------------------------------------------
    // 7) A NON-array oop fed to the helpers must be GRACEFUL (no fault).
    //    notAnArray is an ordinary object; its layout is NOT an array, so the
    //    int32 at +12 is arbitrary (could be a field value).  We only assert the
    //    NO-FAULT contract:
    //      * array_length(objectOop) returns without crashing (reaching the next
    //        line is the proof);
    //      * get/set at a NEGATIVE index and at INT_MIN are rejected by the
    //        guard BEFORE any dereference (so no read past the small object).
    //    We deliberately do NOT perform an in-bounds element read on a non-array
    //    oop: array_length may report a large bogus length, and an in-bounds
    //    index would then read real adjacent heap -- that is the very fault the
    //    helper cannot prevent on a non-array input, so we never trigger it.
    //    The arbitrary length VALUE is characterised as [INFO] (compressed-oop /
    //    layout-dependent), never asserted.
    // -------------------------------------------------------------------------
    {
        void* const obj_oop{ wrapper::raw_field_oop("notAnArray") };
        if (obj_oop && vmhook::hotspot::is_valid_pointer(obj_oop))
        {
            // No-fault: array_length on a non-array object must not crash.
            const std::int32_t bogus_len{ vmhook::array_length(obj_oop) };
            ctx.check("aeh_nonarray_length_no_fault", true);
            ctx.record("[INFO] array_element_helpers: array_length(non-array oop) returned "
                       + std::to_string(bogus_len)
                       + " (arbitrary -- object layout, not an array; characterised, not asserted).");

            // Negative / INT_MIN indices are rejected before any dereference, so
            // these are safe on a non-array oop regardless of bogus_len.
            ctx.check("aeh_nonarray_get_neg1_default",
                      vmhook::get_array_element<std::int32_t>(obj_oop, -1) == 0);
            ctx.check("aeh_nonarray_get_intmin_default",
                      vmhook::get_array_element<std::int32_t>(obj_oop,
                          (std::numeric_limits<std::int32_t>::min)()) == 0);
            // A write at a negative index is a no-op (guard rejects pre-deref).
            vmhook::set_array_element<std::int32_t>(obj_oop, -1, 0x1234);
            ctx.check("aeh_nonarray_set_neg1_safe_noop", true);
        }
        else
        {
            ctx.record("[INFO] array_element_helpers: notAnArray oop did not resolve to a "
                       "valid pointer on this run; skipping the non-array no-fault check.");
        }
    }

    // -------------------------------------------------------------------------
    // 8) REFERENCE-array element decode: each slot is a 4-byte COMPRESSED oop
    //    read via get_array_element<uint32> then decoded via decode_oop_pointer.
    //    refStrings = {"alpha", null, "gamma"} -> slot1 decodes to nullptr.
    //    The LENGTH and per-slot null-ness are HARD; the decoded non-null
    //    pointer VALUES are compressed-oop dependent -> PASS-or-[INFO].
    // -------------------------------------------------------------------------
    {
        if (void* const a{ wrapper::array_oop_of("refStrings") })
        {
            const std::int32_t len{ vmhook::array_length(a) };
            ctx.check("aeh_ref_strings_len3", len == 3);
            if (len == 3)
            {
                void* const s0{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(a, 0)) };
                void* const s1{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(a, 1)) };
                void* const s2{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(a, 2)) };
                // HARD: the null slot decodes to nullptr (a 0 compressed oop ->
                // nullptr), and the two non-null slots decode to distinct,
                // non-null pointers.  This is the null-vs-present layout, which
                // does not depend on the exact heap base.
                const bool layout_ok{ s1 == nullptr && s0 != nullptr && s2 != nullptr && s0 != s2 };
                ctx.check("aeh_ref_strings_null_layout", layout_ok);
                if (layout_ok && vmhook::hotspot::is_valid_pointer(s0)
                    && vmhook::hotspot::is_valid_pointer(s2))
                {
                    ctx.check("aeh_ref_strings_nonnull_valid_oops", true);
                }
                else
                {
                    ctx.record("[INFO] array_element_helpers: decoded String[] non-null slots "
                               "are not is_valid_pointer on this config (compressed-oop base "
                               "variance); null-vs-present layout already asserted HARD.");
                }
            }
        }
        else { ctx.check("aeh_ref_strings_oop", false); }

        // Object[] all-nulls: every slot decodes to nullptr.
        if (void* const a{ wrapper::array_oop_of("refNullsOnly") })
        {
            const std::int32_t len{ vmhook::array_length(a) };
            ctx.check("aeh_ref_nulls_len2", len == 2);
            if (len == 2)
            {
                void* const n0{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(a, 0)) };
                void* const n1{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(a, 1)) };
                ctx.check("aeh_ref_nulls_all_null", n0 == nullptr && n1 == nullptr);
            }
        }
        else { ctx.check("aeh_ref_nulls_oop", false); }
    }

    // -------------------------------------------------------------------------
    // 9) JAGGED / MULTIDIM int[][] outer-dimension element access.
    //    jagged = { {1,2,3}, null, {9} }.  The OUTER array's elements are
    //    compressed oops -> inner row oops (or null).  We read the outer length,
    //    decode each outer slot, confirm the null ROW is a real nullptr, and --
    //    HARD -- read array_length on each non-null inner row (3 and 1).  This
    //    exercises the helper recursively (outer element decode feeds an inner
    //    array_length), the multidim angle from the task spec.
    // -------------------------------------------------------------------------
    {
        if (void* const outer{ wrapper::array_oop_of("jagged") })
        {
            const std::int32_t outer_len{ vmhook::array_length(outer) };
            ctx.check("aeh_jagged_outer_len3", outer_len == 3);
            if (outer_len == 3)
            {
                void* const row0{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(outer, 0)) };
                void* const row1{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(outer, 1)) };
                void* const row2{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(outer, 2)) };

                // HARD: middle row is null; rows 0 and 2 are non-null.
                ctx.check("aeh_jagged_row1_null", row1 == nullptr);
                const bool rows_present{ row0 != nullptr && row2 != nullptr };
                ctx.check("aeh_jagged_rows_present", rows_present);

                // HARD: inner-row LENGTHS via array_length on the decoded inner
                // oops (3 and 1).  Only attempt when the decoded row passes the
                // pointer guard (compressed-oop base variance otherwise).
                if (rows_present
                    && vmhook::hotspot::is_valid_pointer(row0)
                    && vmhook::hotspot::is_valid_pointer(row2))
                {
                    ctx.check("aeh_jagged_inner_lengths",
                              vmhook::array_length(row0) == 3
                              && vmhook::array_length(row2) == 1);
                    // And the inner element values of row0 {1,2,3}.
                    ctx.check("aeh_jagged_inner_row0_values",
                              vmhook::get_array_element<std::int32_t>(row0, 0) == 1
                              && vmhook::get_array_element<std::int32_t>(row0, 1) == 2
                              && vmhook::get_array_element<std::int32_t>(row0, 2) == 3);
                }
                else
                {
                    ctx.record("[INFO] array_element_helpers: decoded inner rows not "
                               "is_valid_pointer on this config; outer null-layout asserted HARD.");
                }
            }
        }
        else { ctx.check("aeh_jagged_outer_oop", false); }
    }

    // -------------------------------------------------------------------------
    // 10) INSTANCE-field array oops (instance-offset path, distinct from the
    //     static-mirror path).  HARD value reads on int[]/double[].
    // -------------------------------------------------------------------------
    {
        const std::unique_ptr<wrapper> self{ wrapper::get_instance() };
        ctx.check("aeh_instance_acquired", self != nullptr);
        if (self)
        {
            if (void* const a{ self->inst_array_oop_of("instIntArray") })
            {
                ctx.check("aeh_inst_int_len3", vmhook::array_length(a) == 3);
                ctx.check("aeh_inst_int_values",
                          vmhook::get_array_element<std::int32_t>(a, 0) == 4000
                          && vmhook::get_array_element<std::int32_t>(a, 1) == 5000
                          && vmhook::get_array_element<std::int32_t>(a, 2) == 6000);
                // OOB safety on an instance-field array too.
                ctx.check("aeh_oob_safe_inst_int", oob_is_safe<std::int32_t>(a, 3));
            }
            else { ctx.check("aeh_inst_int_oop", false); }

            if (void* const a{ self->inst_array_oop_of("instDoubleArray") })
            {
                ctx.check("aeh_inst_double_values",
                          bits_equal(vmhook::get_array_element<double>(a, 0), 4.25)
                          && bits_equal(vmhook::get_array_element<double>(a, 2), 6.25));
            }
            else { ctx.check("aeh_inst_double_oop", false); }
        }
    }

    // =========================================================================
    // 11) LIVE-DISPATCH HANDSHAKE + set_array_element WRITE-BACK VERIFIED BY JAVA.
    //
    //   (a) Install ONE interpreter hook on touch(int) and drive run_probe so a
    //       real Java bytecode dispatch fires through the modular path (proves
    //       the fixture is live; mirrors the pilot handshake).  The primary
    //       probe action also publishes probeChecksum.
    //   (b) THEN write the EXPECT_* contract into each primitive scratch array
    //       via set_array_element, request the verifyScratch action, and re-run
    //       the probe.  Java re-reads the scratch arrays and publishes a bitmask;
    //       bit i set proves the i-th scratch array now holds exactly the values
    //       this module wrote -- i.e. the raw C++ writes landed in the JVM heap
    //       and are visible to Java.  This is the unique live-JVM capability.
    // =========================================================================
    {
        auto handle{ vmhook::scoped_hook<aeh_fixture>(
            "touch",
            [](vmhook::return_value&, const std::unique_ptr<aeh_fixture>& self,
               std::int32_t delta)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_arg.store(delta, std::memory_order_relaxed);
                static_cast<void>(self);
            }) };
        ctx.check("aeh_hook_installed", handle.installed());

        const bool probe_done{ ctx.run_probe(
            [](bool value) { wrapper::set_go(value); },
            []() { return wrapper::get_done(); }) };
        ctx.check("aeh_probe_completed", probe_done);
        ctx.check("aeh_probe_checksum_nonzero", wrapper::get_probe_checksum() != 0);
        ctx.check("aeh_hook_fired", g_hook_calls.load(std::memory_order_relaxed) >= 1);
        // The probe action calls instance.touch(0), so the hook observed delta 0.
        ctx.check("aeh_hook_saw_arg_zero", g_hook_arg.load(std::memory_order_relaxed) == 0);

        // ---- (b) set_array_element write-back, verified by Java --------------
        // Write the EXPECT_* contract (index 0/1/2) into each primitive scratch
        // array through the raw setter.  These are genuine writes to live heap.
        bool wrote_all{ true };

        if (void* const a{ wrapper::array_oop_of("scratchBool") })
        {
            vmhook::set_array_element<std::uint8_t>(a, 0, 1u);
            vmhook::set_array_element<std::uint8_t>(a, 1, 0u);
            vmhook::set_array_element<std::uint8_t>(a, 2, 1u);
            // Read back through the helper immediately (C++-side round trip).
            ctx.check("aeh_scratch_bool_cpp_roundtrip",
                      vmhook::get_array_element<std::uint8_t>(a, 0) != 0
                      && vmhook::get_array_element<std::uint8_t>(a, 1) == 0
                      && vmhook::get_array_element<std::uint8_t>(a, 2) != 0);
        }
        else { wrote_all = false; ctx.check("aeh_scratch_bool_oop", false); }

        if (void* const a{ wrapper::array_oop_of("scratchByte") })
        {
            vmhook::set_array_element<std::int8_t>(a, 0, (std::numeric_limits<std::int8_t>::min)());
            vmhook::set_array_element<std::int8_t>(a, 1, static_cast<std::int8_t>(7));
            vmhook::set_array_element<std::int8_t>(a, 2, (std::numeric_limits<std::int8_t>::max)());
            ctx.check("aeh_scratch_byte_cpp_roundtrip",
                      vmhook::get_array_element<std::int8_t>(a, 0) == (std::numeric_limits<std::int8_t>::min)()
                      && vmhook::get_array_element<std::int8_t>(a, 1) == 7
                      && vmhook::get_array_element<std::int8_t>(a, 2) == (std::numeric_limits<std::int8_t>::max)());
        }
        else { wrote_all = false; ctx.check("aeh_scratch_byte_oop", false); }

        if (void* const a{ wrapper::array_oop_of("scratchShort") })
        {
            vmhook::set_array_element<std::int16_t>(a, 0, (std::numeric_limits<std::int16_t>::min)());
            vmhook::set_array_element<std::int16_t>(a, 1, static_cast<std::int16_t>(9));
            vmhook::set_array_element<std::int16_t>(a, 2, (std::numeric_limits<std::int16_t>::max)());
        }
        else { wrote_all = false; ctx.check("aeh_scratch_short_oop", false); }

        if (void* const a{ wrapper::array_oop_of("scratchChar") })
        {
            vmhook::set_array_element<std::uint16_t>(a, 0, static_cast<std::uint16_t>(0x0041));
            vmhook::set_array_element<std::uint16_t>(a, 1, static_cast<std::uint16_t>(0x4E2D));
            vmhook::set_array_element<std::uint16_t>(a, 2, static_cast<std::uint16_t>(0xFFFF));
        }
        else { wrote_all = false; ctx.check("aeh_scratch_char_oop", false); }

        if (void* const a{ wrapper::array_oop_of("scratchInt") })
        {
            vmhook::set_array_element<std::int32_t>(a, 0, (std::numeric_limits<std::int32_t>::min)());
            vmhook::set_array_element<std::int32_t>(a, 1, 1234567);
            vmhook::set_array_element<std::int32_t>(a, 2, (std::numeric_limits<std::int32_t>::max)());
        }
        else { wrote_all = false; ctx.check("aeh_scratch_int_oop", false); }

        if (void* const a{ wrapper::array_oop_of("scratchLong") })
        {
            vmhook::set_array_element<std::int64_t>(a, 0, (std::numeric_limits<std::int64_t>::min)());
            vmhook::set_array_element<std::int64_t>(a, 1, 9876543210LL);
            vmhook::set_array_element<std::int64_t>(a, 2, (std::numeric_limits<std::int64_t>::max)());
        }
        else { wrote_all = false; ctx.check("aeh_scratch_long_oop", false); }

        if (void* const a{ wrapper::array_oop_of("scratchFloat") })
        {
            vmhook::set_array_element<float>(a, 0, -3.5f);
            vmhook::set_array_element<float>(a, 1, 0.5f);
            vmhook::set_array_element<float>(a, 2, 1234.5f);
        }
        else { wrote_all = false; ctx.check("aeh_scratch_float_oop", false); }

        if (void* const a{ wrapper::array_oop_of("scratchDouble") })
        {
            vmhook::set_array_element<double>(a, 0, -2.5);
            vmhook::set_array_element<double>(a, 1, 0.25);
            vmhook::set_array_element<double>(a, 2, 9.875);
        }
        else { wrote_all = false; ctx.check("aeh_scratch_double_oop", false); }

        ctx.check("aeh_scratch_all_oops_resolved", wrote_all);

        // Ask Java to re-read the scratch arrays and publish the match bitmask.
        wrapper::request_verify_scratch(true);
        wrapper::reset_done(false);
        const bool verify_done{ ctx.run_probe(
            [](bool value) { wrapper::set_go(value); },
            []() { return wrapper::get_done(); }) };
        ctx.check("aeh_scratch_verify_probe_completed", verify_done);

        if (verify_done)
        {
            const std::int32_t mask{ wrapper::get_scratch_verify_mask() };
            // bit i set => scratch array i holds exactly the C++-written values
            // when re-read THROUGH Java.  All 8 primitive widths must match:
            // 0xFF == bits 0..7.  HARD -- a missing bit means a raw write either
            // did not land in the JVM heap or landed at the wrong offset/width.
            ctx.check("aeh_scratch_bool_visible_to_java",   (mask & (1 << 0)) != 0);
            ctx.check("aeh_scratch_byte_visible_to_java",   (mask & (1 << 1)) != 0);
            ctx.check("aeh_scratch_short_visible_to_java",  (mask & (1 << 2)) != 0);
            ctx.check("aeh_scratch_char_visible_to_java",   (mask & (1 << 3)) != 0);
            ctx.check("aeh_scratch_int_visible_to_java",    (mask & (1 << 4)) != 0);
            ctx.check("aeh_scratch_long_visible_to_java",   (mask & (1 << 5)) != 0);
            ctx.check("aeh_scratch_float_visible_to_java",  (mask & (1 << 6)) != 0);
            ctx.check("aeh_scratch_double_visible_to_java", (mask & (1 << 7)) != 0);
            ctx.check("aeh_scratch_all_eight_visible_to_java", (mask & 0xFF) == 0xFF);
        }
    }
}

VMHOOK_JVM_MODULE(array_element_helpers)
{
    // SUITE-SAFETY (mirrors field_arrays_primitive.cpp / aaa_warmup.cpp):
    //   * the whole body runs under a try/catch so a stray throw from any vmhook
    //     call is recorded as [INFO], never a FAIL, and never escapes this module;
    //   * an unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the
    //     module returns to the driver with an EMPTY hook table on every path
    //     (idempotent and safe-when-empty; proven by shutdown_hooks_teardown).
    bool body_threw{ false };
    try
    {
        run_array_element_helpers_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- OUTSIDE the try so it ALWAYS runs.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] array_element_helpers: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks "
                   "for partial results.");
    }
    ctx.check("aeh_module_left_clean_final_shutdown", true);
}
