// Standalone (no-JVM) tests for vmhook::array_length / get_array_element<T> /
// set_array_element<T> against a hand-built fake Java array buffer, covering
// every primitive element width and the bounds/null guards.
//
// These three helpers are pure pointer arithmetic over the HotSpot array layout
// (x64, compressed OOPs):
//     +0   mark word        (8 B)
//     +8   klass narrow ptr (4 B)
//     +12  _length          (int32)
//     +16  _data[0]         (element stride = sizeof(T))
// No live oop or running JVM is required: array_length reads the int32 at +12,
// and get/set_array_element memcpy at +16 + index*sizeof(T) after a bounds check
// against [0, length).  A heap-allocated std::vector<uint8_t> backing buffer is
// at a canonical, >=2-byte-aligned, non-sentinel address, so it passes
// vmhook::hotspot::is_valid_pointer (the guard the helpers gate on).
//
// test_helpers.cpp::test_array_helpers already covers the int32 width plus a few
// boundary cases; this file extends that to EVERY width (uint8/int8/int16/uint16/
// int32/int64/float/double) with explicit round-trip (set-then-get) and
// out-of-bounds checks per width, plus stride-isolation and header-integrity
// checks that the int32-only test does not exercise.
//
// Out of scope here (needs a live JVM / real compressed-oop base):
//   * field_proxy::value_t::operator std::vector<T>() and read_array_value() —
//     the compressed-oop decode path; corrupted-_length safety belongs in a
//     dedicated value_t test (see audit field_proxy_array_primitives.md) and the
//     end-to-end round trip is covered by JVM integration in example.cpp.

#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// Fake-array buffer helpers.
//
// build_fake_array<T>(values) returns a byte buffer laid out exactly like a
// HotSpot primitive array: 16-byte header (mark + klass + length) followed by
// the element payload.  _length at +12 is set to values.size().  The buffer is
// heap-backed (std::vector) so its data() is a canonical address that clears
// is_valid_pointer, and so static analysers do not constant-fold the OOB
// indices we deliberately feed the helpers.
// ---------------------------------------------------------------------------
template<typename T>
static auto build_fake_array(const std::vector<T>& values) -> std::vector<std::uint8_t>
{
    const std::int32_t length{ static_cast<std::int32_t>(values.size()) };
    std::vector<std::uint8_t> buffer(16u + values.size() * sizeof(T), std::uint8_t{ 0 });
    std::memcpy(buffer.data() + 12, &length, sizeof(length));
    for (std::size_t i{ 0 }; i < values.size(); ++i)
    {
        std::memcpy(buffer.data() + 16 + i * sizeof(T), &values[i], sizeof(T));
    }
    return buffer;
}

// Route OOB indices through a volatile read so the compiler cannot statically
// prove they are out of range and refuse to compile the call (mirrors the
// opaque_index trick in test_helpers.cpp).
static auto opaque_index(std::int32_t i) noexcept -> std::int32_t
{
    volatile std::int32_t v{ i };
    return v;
}

// Bit-exact comparison helper so float/double round-trips are checked by their
// stored bit pattern, not by IEEE == (which would also pass on accidental
// re-rounding but fails to flag a stride bug that swaps in neighbouring bytes).
template<typename T>
static auto bits_equal(T a, T b) noexcept -> bool
{
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// ---------------------------------------------------------------------------
// Generic per-width round-trip + bounds harness.
//
// For an element type T and three sentinel values, this:
//   * confirms array_length reports the element count,
//   * reads every seeded element back,
//   * overwrites the middle element via set_array_element and reads it back,
//   * confirms the neighbouring elements are untouched (stride correctness),
//   * confirms negative / at-length / far OOB reads return T{},
//   * confirms OOB writes are no-ops that leave the buffer intact.
// label_prefix names the checks per width (e.g. "int16").
// ---------------------------------------------------------------------------
template<typename T>
static auto exercise_width(const char* label_prefix, T a, T b, T c) -> void
{
    auto tag{ [&](const char* suffix)
    {
        static thread_local char storage[96];
        std::snprintf(storage, sizeof(storage), "%s_%s", label_prefix, suffix);
        return storage;
    } };

    const std::vector<T> seed{ a, b, c };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const array_oop{ buffer.data() };

    // array_length reflects the seeded count.
    check(tag("array_length_is_3"), vmhook::array_length(array_oop) == 3);

    // Every seeded element reads back bit-exact.
    check(tag("get_index_0"), bits_equal(vmhook::get_array_element<T>(array_oop, 0), a));
    check(tag("get_index_1"), bits_equal(vmhook::get_array_element<T>(array_oop, 1), b));
    check(tag("get_index_2"), bits_equal(vmhook::get_array_element<T>(array_oop, 2), c));

    // Round-trip: overwrite the middle slot, read it back.
    vmhook::set_array_element<T>(array_oop, 1, a);
    check(tag("set_then_get_roundtrip"),
          bits_equal(vmhook::get_array_element<T>(array_oop, 1), a));

    // Stride correctness: writing index 1 must not disturb index 0 or index 2.
    check(tag("set_preserves_lower_neighbour"),
          bits_equal(vmhook::get_array_element<T>(array_oop, 0), a));
    check(tag("set_preserves_upper_neighbour"),
          bits_equal(vmhook::get_array_element<T>(array_oop, 2), c));

    // Header bytes (mark + klass, offsets 0..11) must never be touched by a
    // data write: a stride/offset bug that wrote before +16 would corrupt them.
    {
        bool header_zero{ true };
        for (std::size_t i{ 0 }; i < 12; ++i)
        {
            if (buffer[i] != 0) { header_zero = false; break; }
        }
        check(tag("header_untouched_by_writes"), header_zero);
    }

    // Out-of-bounds reads return a value-initialised element_type.
    check(tag("get_negative_index_returns_default"),
          bits_equal(vmhook::get_array_element<T>(array_oop, opaque_index(-1)), T{}));
    check(tag("get_at_length_index_returns_default"),
          bits_equal(vmhook::get_array_element<T>(array_oop, opaque_index(3)), T{}));
    check(tag("get_far_oob_index_returns_default"),
          bits_equal(vmhook::get_array_element<T>(array_oop, opaque_index(9999)), T{}));

    // Out-of-bounds writes are no-ops: capture the payload, attempt the writes,
    // confirm nothing in the buffer changed.
    {
        const std::vector<std::uint8_t> before{ buffer };
        vmhook::set_array_element<T>(array_oop, opaque_index(-1), b);
        vmhook::set_array_element<T>(array_oop, opaque_index(3), b);
        vmhook::set_array_element<T>(array_oop, opaque_index(9999), b);
        check(tag("oob_writes_are_noops"), buffer == before);
    }
}

// ---------------------------------------------------------------------------
// 1. Every primitive element width: round-trip + bounds.
//    Widths from the cluster focus: uint8/int8 (1B), int16/uint16 (2B),
//    int32 (4B), int64 (8B), float (4B), double (8B).  char is also a 1-byte
//    element width (the raw helper treats it as a plain byte; the "[C" -> char
//    narrowing only exists in the append path, which needs a JVM).
// ---------------------------------------------------------------------------
static auto test_all_widths() -> void
{
    exercise_width<std::uint8_t>("uint8",
        std::uint8_t{ 0x00 }, std::uint8_t{ 0x7F }, std::uint8_t{ 0xFF });
    exercise_width<std::int8_t>("int8",
        std::int8_t{ -128 }, std::int8_t{ 0 }, std::int8_t{ 127 });
    exercise_width<std::int16_t>("int16",
        std::int16_t{ -32768 }, std::int16_t{ 0 }, std::int16_t{ 32767 });
    exercise_width<std::uint16_t>("uint16",
        std::uint16_t{ 0x0000 }, std::uint16_t{ 0xABCD }, std::uint16_t{ 0xFFFF });
    exercise_width<std::int32_t>("int32",
        std::int32_t{ -2000000000 }, std::int32_t{ 0x12345678 }, std::int32_t{ 2000000000 });
    exercise_width<std::int64_t>("int64",
        std::int64_t{ -9000000000000000000ll },
        std::int64_t{ 0x1122334455667788ll },
        std::int64_t{ 9000000000000000000ll });
    exercise_width<float>("float", -3.5f, 0.0f, 1234.5f);
    exercise_width<double>("double", -2.718281828459045, 0.0, 1.7976931348623157e308);
}

// ---------------------------------------------------------------------------
// 2. char width — vmhook stores java char[] as uint16 (UTF-16), but the raw
//    get/set_array_element<char> helper operates on a 1-byte stride.  Verify
//    the 1-byte path round-trips so the dispatch in append_array_value (which
//    reads uint16 for "[C" and char otherwise) has a tested foundation.
// ---------------------------------------------------------------------------
static auto test_char_width() -> void
{
    const std::vector<char> seed{ 'A', 'z', '\x7F' };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const array_oop{ buffer.data() };

    check("char_array_length_is_3", vmhook::array_length(array_oop) == 3);
    check("char_get_index_0", vmhook::get_array_element<char>(array_oop, 0) == 'A');
    check("char_get_index_1", vmhook::get_array_element<char>(array_oop, 1) == 'z');
    check("char_get_index_2", vmhook::get_array_element<char>(array_oop, 2) == '\x7F');

    vmhook::set_array_element<char>(array_oop, 0, 'Q');
    check("char_set_then_get_roundtrip", vmhook::get_array_element<char>(array_oop, 0) == 'Q');
    check("char_set_preserves_neighbour", vmhook::get_array_element<char>(array_oop, 1) == 'z');
}

// ---------------------------------------------------------------------------
// 3. array_length boundary / null behaviour.
// ---------------------------------------------------------------------------
static auto test_array_length_edges() -> void
{
    // Null oop short-circuits to 0 without faulting.
    check("array_length_null_returns_zero", vmhook::array_length(nullptr) == 0);

    // A buffer whose _length field is 0 reports 0 (empty array, header only).
    {
        std::vector<std::uint8_t> empty_arr(16u, std::uint8_t{ 0 });
        check("array_length_zero_length_field",
              vmhook::array_length(empty_arr.data()) == 0);
    }

    // array_length reads exactly the int32 at +12 and nothing else: set a
    // distinctive length and a different value in the data region, confirm the
    // length is what was written at +12.
    {
        std::vector<std::uint8_t> buf(16u + 4u * sizeof(std::int32_t), std::uint8_t{ 0 });
        const std::int32_t len{ 4 };
        std::memcpy(buf.data() + 12, &len, sizeof(len));
        // Poison the mark/klass header to prove array_length ignores them.
        std::memset(buf.data(), 0xFF, 12);
        check("array_length_reads_offset_12_only",
              vmhook::array_length(buf.data()) == 4);
    }

    // A small low/sentinel pointer is rejected by is_valid_pointer -> length 0.
    check("array_length_small_sentinel_returns_zero",
          vmhook::array_length(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x100ull))) == 0);
}

// ---------------------------------------------------------------------------
// 4. get/set_array_element null & invalid-pointer guards (width-independent).
// ---------------------------------------------------------------------------
static auto test_element_null_guards() -> void
{
    // Null oop -> default-constructed element, no fault, for several widths.
    check("get_null_oop_uint8_default",
          vmhook::get_array_element<std::uint8_t>(nullptr, 0) == 0);
    check("get_null_oop_int32_default",
          vmhook::get_array_element<std::int32_t>(nullptr, 0) == 0);
    check("get_null_oop_int64_default",
          vmhook::get_array_element<std::int64_t>(nullptr, 0) == 0);
    check("get_null_oop_double_default",
          bits_equal(vmhook::get_array_element<double>(nullptr, 0), 0.0));

    // set on a null oop is a no-op that must not crash.
    vmhook::set_array_element<std::int32_t>(nullptr, 0, 42);
    check("set_null_oop_is_safe_noop", true);

    // A low sentinel pointer fails is_valid_pointer -> read returns default,
    // write is a no-op (both must short-circuit before any dereference).
    void* const sentinel{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x100ull)) };
    check("get_sentinel_oop_returns_default",
          vmhook::get_array_element<std::int32_t>(sentinel, 0) == 0);
    vmhook::set_array_element<std::int32_t>(sentinel, 0, 7);
    check("set_sentinel_oop_is_safe_noop", true);
}

// ---------------------------------------------------------------------------
// 5. Single-element and exact-boundary index arithmetic.
//
// With length 1 the only valid index is 0; index 1 (== length) must be the
// first rejected index.  This pins the half-open [0, length) contract that the
// helpers document, independent of element width.
// ---------------------------------------------------------------------------
static auto test_single_element_boundaries() -> void
{
    const std::vector<std::int32_t> seed{ 0x0BADF00D };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const array_oop{ buffer.data() };

    check("single_array_length_is_1", vmhook::array_length(array_oop) == 1);
    check("single_get_index_0_ok",
          vmhook::get_array_element<std::int32_t>(array_oop, 0) == 0x0BADF00D);

    // index == length is the first OOB index.
    check("single_get_index_1_is_oob_default",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(1)) == 0);

    // Writing the sole valid slot works; writing index 1 is a no-op.
    vmhook::set_array_element<std::int32_t>(array_oop, 0, 0x5A5A5A5A);
    check("single_set_index_0_ok",
          vmhook::get_array_element<std::int32_t>(array_oop, 0) == 0x5A5A5A5A);

    const std::vector<std::uint8_t> before{ buffer };
    vmhook::set_array_element<std::int32_t>(array_oop, opaque_index(1), 0x33333333);
    check("single_set_index_1_is_noop", buffer == before);
}

// ---------------------------------------------------------------------------
// 6. Last-index access on a multi-element array.
//
// The final element (index length-1) is in bounds and lands at the very end of
// the payload; index length is OOB.  Confirms the upper boundary is inclusive
// of length-1 and exclusive of length, and that writing the last element does
// not run past the buffer.
// ---------------------------------------------------------------------------
static auto test_last_index_access() -> void
{
    const std::vector<std::int64_t> seed{ 10, 20, 30, 40 };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const array_oop{ buffer.data() };

    check("last_array_length_is_4", vmhook::array_length(array_oop) == 4);
    check("last_get_final_index_ok",
          vmhook::get_array_element<std::int64_t>(array_oop, 3) == 40);

    vmhook::set_array_element<std::int64_t>(array_oop, 3, 0x7FFFFFFFFFFFFFFFll);
    check("last_set_final_index_roundtrip",
          vmhook::get_array_element<std::int64_t>(array_oop, 3) == 0x7FFFFFFFFFFFFFFFll);

    // index == length (4) must be rejected for both read and write.
    check("last_get_at_length_is_default",
          vmhook::get_array_element<std::int64_t>(array_oop, opaque_index(4)) == 0);

    const std::vector<std::uint8_t> before{ buffer };
    vmhook::set_array_element<std::int64_t>(array_oop, opaque_index(4), 0x1234ll);
    check("last_set_at_length_is_noop", buffer == before);
}

// ---------------------------------------------------------------------------
// 7. EXPANDED: additional element widths/types not in test_all_widths.
//
// The helpers are templated on any trivially-copyable element_type, gating only
// on sizeof(T) for the stride.  Cover bool (1B), uint32_t (4B), and char16_t
// (the real Java `char` representation, 2B) end-to-end, mirroring the generic
// round-trip + bounds expectations derived from get/set_array_element:
//   in-bounds [0,length) -> memcpy at +16+index*sizeof(T); OOB -> default/no-op.
// ---------------------------------------------------------------------------
static auto test_extra_widths() -> void
{
    // bool: only canonical 0/1 are seeded (reading a non-canonical byte into
    // bool would be UB to observe, per the field_proxy notes); 1-byte stride.
    {
        const std::vector<std::uint8_t> seed{ 1u, 0u, 1u };  // backing bytes
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        check("bool_array_length_is_3", vmhook::array_length(oop) == 3);
        check("bool_get_index_0_true",  vmhook::get_array_element<bool>(oop, 0) == true);
        check("bool_get_index_1_false", vmhook::get_array_element<bool>(oop, 1) == false);
        check("bool_get_index_2_true",  vmhook::get_array_element<bool>(oop, 2) == true);
        vmhook::set_array_element<bool>(oop, 1, true);
        check("bool_set_then_get_roundtrip", vmhook::get_array_element<bool>(oop, 1) == true);
        check("bool_oob_get_is_false",
              vmhook::get_array_element<bool>(oop, opaque_index(3)) == false);
    }

    // uint32_t round-trip + bounds across the full unsigned range endpoints.
    exercise_width<std::uint32_t>("uint32",
        std::uint32_t{ 0u }, std::uint32_t{ 0x9ABCDEF0u }, std::uint32_t{ 0xFFFFFFFFu });

    // char16_t — the genuine UTF-16 code-unit type for Java char[].
    exercise_width<char16_t>("char16",
        char16_t{ 0x0000 }, char16_t{ 0x4E2D }, char16_t{ 0xFFFF });
}

// ---------------------------------------------------------------------------
// 8. EXPANDED: a NEGATIVE _length field makes every access safe-default.
//
// array_length returns the raw int32 at +12 with no clamping, so a negative
// length is returned verbatim.  get/set then compare `index >= length`; for a
// negative length EVERY non-negative index satisfies index >= length and is
// rejected, so reads return T{} and writes are no-ops.  This pins that a
// corrupted/negative length cannot be used to read or write any element.
// ---------------------------------------------------------------------------
static auto test_negative_length_field() -> void
{
    // Build a 3-element buffer, then overwrite _length with -5.
    const std::vector<std::int32_t> seed{ 111, 222, 333 };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };
    const std::int32_t neg{ -5 };
    std::memcpy(buffer.data() + 12, &neg, sizeof(neg));

    check("negative_length_returned_verbatim", vmhook::array_length(oop) == -5);

    // Index 0 (and any non-negative index) is rejected: 0 >= -5 is true.
    check("negative_length_get_index_0_is_default",
          vmhook::get_array_element<std::int32_t>(oop, 0) == 0);
    check("negative_length_get_index_2_is_default",
          vmhook::get_array_element<std::int32_t>(oop, opaque_index(2)) == 0);

    // Writes are no-ops: capture the payload, attempt writes, confirm unchanged.
    const std::vector<std::uint8_t> before{ buffer };
    vmhook::set_array_element<std::int32_t>(oop, 0, 0x44444444);
    vmhook::set_array_element<std::int32_t>(oop, opaque_index(1), 0x55555555);
    check("negative_length_writes_are_noops", buffer == before);
}

// ---------------------------------------------------------------------------
// 9. EXPANDED: zero-length array through get/set (not just array_length).
//
// With _length == 0 the only candidate index is 0, and 0 >= 0 (== length) is
// the first rejected index, so the half-open [0,0) range is empty: every read
// is default and every write a no-op.  (test_array_length_edges covers the
// array_length==0 readback; this exercises the element helpers on it.)
// ---------------------------------------------------------------------------
static auto test_zero_length_element_access() -> void
{
    // Header-only buffer plus a little slack so a (rejected) write target would
    // at least be inside the vector if the guard ever regressed.
    std::vector<std::uint8_t> buffer(16u + 4u * sizeof(std::int32_t), std::uint8_t{ 0 });
    // _length already 0 (zero-filled); be explicit.
    const std::int32_t zero{ 0 };
    std::memcpy(buffer.data() + 12, &zero, sizeof(zero));
    void* const oop{ buffer.data() };

    check("zero_length_array_length_is_0", vmhook::array_length(oop) == 0);
    check("zero_length_get_index_0_is_default",
          vmhook::get_array_element<std::int32_t>(oop, opaque_index(0)) == 0);
    check("zero_length_get_negative_is_default",
          vmhook::get_array_element<std::int32_t>(oop, opaque_index(-1)) == 0);

    const std::vector<std::uint8_t> before{ buffer };
    vmhook::set_array_element<std::int32_t>(oop, opaque_index(0), 0x66666666);
    check("zero_length_set_index_0_is_noop", buffer == before);
}

// ---------------------------------------------------------------------------
// 10. EXPANDED: extreme index sentinels are rejected for every access.
//
// INT32_MIN (negative -> first guard) and INT32_MAX (>= any real length ->
// second guard) must both be rejected for read and write, for several widths,
// without computing the would-be byte offset (the guards run BEFORE the
// index*sizeof multiply).
// ---------------------------------------------------------------------------
static auto test_extreme_index_sentinels() -> void
{
    const std::vector<std::int64_t> seed{ 1, 2, 3, 4, 5 };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    check("extreme_length_is_5", vmhook::array_length(oop) == 5);

    check("intmin_index_get_is_default_i64",
          vmhook::get_array_element<std::int64_t>(oop, opaque_index(-2147483647 - 1)) == 0);
    check("intmax_index_get_is_default_i64",
          vmhook::get_array_element<std::int64_t>(oop, opaque_index(2147483647)) == 0);
    check("intmin_index_get_is_default_i8",
          vmhook::get_array_element<std::int8_t>(oop, opaque_index(-2147483647 - 1)) == 0);
    check("intmax_index_get_is_default_i8",
          vmhook::get_array_element<std::int8_t>(oop, opaque_index(2147483647)) == 0);

    const std::vector<std::uint8_t> before{ buffer };
    vmhook::set_array_element<std::int64_t>(oop, opaque_index(-2147483647 - 1), 0x77ll);
    vmhook::set_array_element<std::int64_t>(oop, opaque_index(2147483647), 0x88ll);
    check("extreme_index_writes_are_noops", buffer == before);
}

// ---------------------------------------------------------------------------
// 11. EXPANDED: array_length returns the stored int32 SIGN-correctly.
//
// The field is read as a signed int32: 0xFFFFFFFF -> -1, 0x80000000 -> INT32_MIN,
// 0x7FFFFFFF -> INT32_MAX.  Pin the exact signed reinterpretation (no unsigned
// read), and that a -1 length makes get return default.  (We never index into a
// fabricated huge positive length — that would read past our real buffer.)
// ---------------------------------------------------------------------------
static auto test_array_length_sign() -> void
{
    auto length_from_bytes{ [](std::uint32_t raw) -> std::int32_t
    {
        std::vector<std::uint8_t> buf(16u, std::uint8_t{ 0 });
        std::memcpy(buf.data() + 12, &raw, sizeof(raw));
        return vmhook::array_length(buf.data());
    } };

    check("array_length_0xFFFFFFFF_is_minus_one",
          length_from_bytes(0xFFFFFFFFu) == -1);
    check("array_length_0x80000000_is_int32_min",
          length_from_bytes(0x80000000u) == (std::int32_t{ -2147483647 } - 1));
    check("array_length_0x7FFFFFFF_is_int32_max",
          length_from_bytes(0x7FFFFFFFu) == 2147483647);
    check("array_length_0x00000001_is_one",
          length_from_bytes(0x00000001u) == 1);

    // A -1 length rejects index 0 (0 >= -1) -> default read.
    {
        std::vector<std::uint8_t> buf(16u + sizeof(std::int32_t), std::uint8_t{ 0 });
        const std::uint32_t raw{ 0xFFFFFFFFu };
        std::memcpy(buf.data() + 12, &raw, sizeof(raw));
        check("minus_one_length_get_is_default",
              vmhook::get_array_element<std::int32_t>(buf.data(), opaque_index(0)) == 0);
    }
}

// ===========================================================================
// EXHAUSTIVE EXPANSION (12+).  The three helpers reduce to one piece of
// arithmetic — element address = base + 16 + index * sizeof(T) — gated by a
// half-open [0, length) bounds check and vmhook::hotspot::is_valid_pointer on
// the *header* pointer.  There is no BasicType size table or base-offset helper
// inside this feature: the per-element stride IS sizeof(T) (the C++ element
// type) and the base IS the constant +16.  So "element size for every JVM
// BasicType" maps here to "every element width T", and the sweeps below pin the
// exact byte offset the helper touches against an independent oracle.
//
// The oracle (offset_of) recomputes the address the SAME way the header does,
// then reads/writes the raw backing bytes at that offset and cross-checks
// against get/set_array_element.  Mirroring the header arithmetic exactly means
// a divergence (e.g. a base or stride change) fails loudly.
// ===========================================================================

// Independent oracle for the element byte offset within the buffer.  Mirrors
// vmhook.hpp: data starts at +16, stride is sizeof(T).  Uses size_t here so the
// oracle itself never overflows (the header uses an int32 multiply — see the
// documented overflow note in test_index_scale_overflow / the bug report).
template<typename T>
static auto offset_of(std::int32_t index) -> std::size_t
{
    return std::size_t{ 16u } + static_cast<std::size_t>(index) * sizeof(T);
}

// Read sizeof(T) raw bytes straight out of the backing buffer at the oracle
// offset (bypassing the helper) so we can prove the helper landed exactly there.
template<typename T>
static auto raw_peek(const std::vector<std::uint8_t>& buffer, std::int32_t index) -> T
{
    T value{};
    std::memcpy(&value, buffer.data() + offset_of<T>(index), sizeof(T));
    return value;
}

// ---------------------------------------------------------------------------
// 12. Per-width STRIDE / element-size proof.
//
// For every supported element width, build a fresh N-element array seeded with
// a per-index distinct pattern, then for EACH index assert:
//   * get_array_element<T>(oop, i) == raw bytes the oracle reads at +16+i*sizeof(T)
//   * after set_array_element<T>(oop, i, v), the oracle reads back v at that
//     same offset and the helper agrees.
// This is the exhaustive "element stride == sizeof(T), base == +16" check across
// all widths (the JVM BasicType size table, expressed as C++ widths):
//   boolean/byte = 1, char/short = 2, int/float = 4, long/double = 8,
//   compressed-oop = 4, uncompressed-oop = 8.
// ---------------------------------------------------------------------------
template<typename T>
static auto exercise_stride(const char* label_prefix, std::int32_t count) -> void
{
    auto tag{ [&](const char* suffix)
    {
        static thread_local char storage[112];
        std::snprintf(storage, sizeof(storage), "%s_%s", label_prefix, suffix);
        return storage;
    } };

    // Seed each element with a distinct, width-filling pattern derived from its
    // index so that a stride error (reading/writing a neighbour) is visible.
    std::vector<T> seed(static_cast<std::size_t>(count));
    for (std::int32_t i{ 0 }; i < count; ++i)
    {
        // Spread the index across all bytes of T so neighbours never alias.
        const std::uint64_t pattern{ (static_cast<std::uint64_t>(i) * 0x0101010101010101ull)
                                     ^ (static_cast<std::uint64_t>(i) << 3) ^ 0x55u };
        T element{};
        std::memcpy(&element, &pattern, sizeof(T));
        seed[static_cast<std::size_t>(i)] = element;
    }

    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    check(tag("length_matches_count"), vmhook::array_length(oop) == count);

    bool all_get_match{ true };
    for (std::int32_t i{ 0 }; i < count; ++i)
    {
        const T via_helper{ vmhook::get_array_element<T>(oop, i) };
        const T via_oracle{ raw_peek<T>(buffer, i) };
        if (!bits_equal(via_helper, via_oracle)) { all_get_match = false; break; }
    }
    check(tag("get_lands_at_base16_plus_index_stride"), all_get_match);

    // Overwrite every slot with a second distinct pattern and confirm both the
    // helper and the raw oracle see the new value at exactly the oracle offset,
    // and that no other slot moved (full re-read after the full rewrite).
    std::vector<T> rewritten(static_cast<std::size_t>(count));
    for (std::int32_t i{ 0 }; i < count; ++i)
    {
        const std::uint64_t pattern{ (static_cast<std::uint64_t>(count - i) * 0x8080808080808080ull)
                                     ^ 0xA5A5u ^ (static_cast<std::uint64_t>(i) << 1) };
        T element{};
        std::memcpy(&element, &pattern, sizeof(T));
        rewritten[static_cast<std::size_t>(i)] = element;
        vmhook::set_array_element<T>(oop, i, element);
    }

    bool all_set_match{ true };
    for (std::int32_t i{ 0 }; i < count; ++i)
    {
        const T expected{ rewritten[static_cast<std::size_t>(i)] };
        if (!bits_equal(vmhook::get_array_element<T>(oop, i), expected)) { all_set_match = false; break; }
        if (!bits_equal(raw_peek<T>(buffer, i), expected))               { all_set_match = false; break; }
    }
    check(tag("set_lands_at_base16_plus_index_stride"), all_set_match);

    // The 16-byte header must be byte-identical to its seeded state after the
    // full rewrite (length at +12 preserved; mark/klass zero).
    {
        bool header_ok{ vmhook::array_length(oop) == count };
        for (std::size_t i{ 0 }; i < 12u && header_ok; ++i)
        {
            if (buffer[i] != 0) { header_ok = false; }
        }
        check(tag("header_intact_after_full_rewrite"), header_ok);
    }
}

static auto test_element_size_strides() -> void
{
    // 1-byte widths: boolean / byte.
    exercise_stride<std::uint8_t>("stride_u8",  37);
    exercise_stride<std::int8_t>("stride_i8",   37);
    // 2-byte widths: char / short.
    exercise_stride<std::int16_t>("stride_i16", 29);
    exercise_stride<std::uint16_t>("stride_u16",29);
    exercise_stride<char16_t>("stride_c16",     29);
    // 4-byte widths: int / float / compressed-oop.
    exercise_stride<std::int32_t>("stride_i32", 23);
    exercise_stride<std::uint32_t>("stride_u32",23);
    exercise_stride<float>("stride_f32",        23);
    // 8-byte widths: long / double / uncompressed-oop.
    exercise_stride<std::int64_t>("stride_i64", 19);
    exercise_stride<std::uint64_t>("stride_u64",19);
    exercise_stride<double>("stride_f64",       19);
}

// ---------------------------------------------------------------------------
// 13. Dense address sweep across every element scale.
//
// For scales 1/2/4/8, sweep EVERY index in a sizeable array and assert the
// address the helper uses equals base + 16 + index*scale.  We prove the address
// indirectly but exactly: write a unique value through the helper at index i,
// then read the raw bytes at the independently-computed oracle offset and
// require equality (and vice-versa).  A dense sweep (not just 0/1/mid/last)
// catches any non-linear stride bug.
// ---------------------------------------------------------------------------
template<typename T>
static auto sweep_scale(const char* label, std::int32_t count) -> void
{
    std::vector<T> seed(static_cast<std::size_t>(count), T{});
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    bool ok{ true };
    for (std::int32_t i{ 0 }; i < count && ok; ++i)
    {
        // Unique per-index value across the full width of T.
        const std::uint64_t bits{ 0xC0FFEE0000000000ull ^ (static_cast<std::uint64_t>(i) * 2654435761ull) };
        T value{};
        std::memcpy(&value, &bits, sizeof(T));

        // set via helper -> raw oracle must see it at +16+i*scale.
        vmhook::set_array_element<T>(oop, i, value);
        if (!bits_equal(raw_peek<T>(buffer, i), value)) { ok = false; break; }

        // get via helper must read the same bytes back.
        if (!bits_equal(vmhook::get_array_element<T>(oop, i), value)) { ok = false; break; }

        // And the helper must NOT have touched the next slot's first byte
        // (stride exactness): that byte should still be zero for i < count-1.
        if (i + 1 < count)
        {
            if (buffer[offset_of<T>(i + 1)] != 0) { ok = false; break; }
        }
    }
    check(label, ok);
}

static auto test_address_computation_sweep() -> void
{
    sweep_scale<std::uint8_t>("sweep_scale1_dense", 100);
    sweep_scale<std::uint16_t>("sweep_scale2_dense", 100);
    sweep_scale<std::uint32_t>("sweep_scale4_dense", 100);
    sweep_scale<std::uint64_t>("sweep_scale8_dense", 100);

    // Explicit index 0 / 1 / mid / last per scale, asserting the exact oracle
    // offset value (16, 16+scale, ...), so the offsets are pinned numerically
    // and not only relatively.
    {
        const std::vector<std::uint32_t> seed(50u, 0u);
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        vmhook::set_array_element<std::uint32_t>(oop, 0,  0x11111111u);
        vmhook::set_array_element<std::uint32_t>(oop, 1,  0x22222222u);
        vmhook::set_array_element<std::uint32_t>(oop, 25, 0x33333333u);
        vmhook::set_array_element<std::uint32_t>(oop, 49, 0x44444444u);
        // Oracle offsets: 16, 20, 16+25*4=116, 16+49*4=212.
        check("explicit_offset_index0_is_16",
              raw_peek<std::uint32_t>(buffer, 0) == 0x11111111u && offset_of<std::uint32_t>(0) == 16u);
        check("explicit_offset_index1_is_20",
              raw_peek<std::uint32_t>(buffer, 1) == 0x22222222u && offset_of<std::uint32_t>(1) == 20u);
        check("explicit_offset_index25_is_116",
              raw_peek<std::uint32_t>(buffer, 25) == 0x33333333u && offset_of<std::uint32_t>(25) == 116u);
        check("explicit_offset_index49_is_212",
              raw_peek<std::uint32_t>(buffer, 49) == 0x44444444u && offset_of<std::uint32_t>(49) == 212u);
    }
}

// ---------------------------------------------------------------------------
// 14. The COMPLETE boundary set, read AND write, across widths and lengths.
//
// For a given length L the half-open contract is: valid iff 0 <= index < L.
// Enumerate the full boundary neighbourhood:
//   -1, INT_MIN, 0, L-1 (last valid), L (first invalid), L+1, INT_MAX.
// On a length-L array: 0 and L-1 succeed (when L>=1); the rest are rejected.
// Empty array (L==0): 0 itself is the first invalid index.
// ---------------------------------------------------------------------------
template<typename T>
static auto check_boundary(const char* label_prefix, std::int32_t length, T probe) -> void
{
    auto tag{ [&](const char* suffix)
    {
        static thread_local char storage[112];
        std::snprintf(storage, sizeof(storage), "%s_len%d_%s", label_prefix, length, suffix);
        return storage;
    } };

    std::vector<T> seed(static_cast<std::size_t>(length), T{});
    // Seed a recognisable value so a successful read returns something specific.
    for (std::int32_t i{ 0 }; i < length; ++i)
    {
        T v{};
        const std::uint64_t bits{ 0xABCDEF12u + static_cast<std::uint64_t>(i) };
        std::memcpy(&v, &bits, sizeof(T));
        seed[static_cast<std::size_t>(i)] = v;
    }
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    constexpr std::int32_t int_min{ -2147483647 - 1 };
    constexpr std::int32_t int_max{ 2147483647 };

    // Invalid indices: must read default and be no-op on write, regardless of L.
    const std::int32_t invalid[]{ opaque_index(-1), opaque_index(int_min),
                                  opaque_index(length), opaque_index(length + 1),
                                  opaque_index(int_max) };
    bool all_invalid_default{ true };
    for (const std::int32_t idx : invalid)
    {
        if (!bits_equal(vmhook::get_array_element<T>(oop, idx), T{})) { all_invalid_default = false; break; }
    }
    check(tag("invalid_reads_default"), all_invalid_default);

    {
        const std::vector<std::uint8_t> before{ buffer };
        for (const std::int32_t idx : invalid)
        {
            vmhook::set_array_element<T>(oop, idx, probe);
        }
        check(tag("invalid_writes_noop"), buffer == before);
    }

    // Valid indices: 0 and L-1 (only meaningful when L >= 1).
    if (length >= 1)
    {
        check(tag("index0_valid_read"),
              bits_equal(vmhook::get_array_element<T>(oop, 0), seed[0]));
        check(tag("last_valid_read"),
              bits_equal(vmhook::get_array_element<T>(oop, length - 1),
                         seed[static_cast<std::size_t>(length - 1)]));

        vmhook::set_array_element<T>(oop, 0, probe);
        check(tag("index0_valid_write"),
              bits_equal(vmhook::get_array_element<T>(oop, 0), probe));
        vmhook::set_array_element<T>(oop, length - 1, probe);
        check(tag("last_valid_write"),
              bits_equal(vmhook::get_array_element<T>(oop, length - 1), probe));
    }
    else
    {
        // Empty array: index 0 is itself the first invalid index.
        check(tag("empty_index0_read_default"),
              bits_equal(vmhook::get_array_element<T>(oop, opaque_index(0)), T{}));
        const std::vector<std::uint8_t> before{ buffer };
        vmhook::set_array_element<T>(oop, opaque_index(0), probe);
        check(tag("empty_index0_write_noop"), buffer == before);
    }
}

static auto test_full_boundary_set() -> void
{
    // Lengths 0,1,2,3,8 across a 1/4/8-byte width.
    check_boundary<std::uint8_t>("bnd_u8", 0, std::uint8_t{ 0x7E });
    check_boundary<std::uint8_t>("bnd_u8", 1, std::uint8_t{ 0x7E });
    check_boundary<std::uint8_t>("bnd_u8", 2, std::uint8_t{ 0x7E });
    check_boundary<std::int32_t>("bnd_i32", 1, std::int32_t{ 0x5EED5EED });
    check_boundary<std::int32_t>("bnd_i32", 3, std::int32_t{ 0x5EED5EED });
    check_boundary<std::int64_t>("bnd_i64", 1, std::int64_t{ 0x0123456789ABCDEFll });
    check_boundary<std::int64_t>("bnd_i64", 8, std::int64_t{ 0x0123456789ABCDEFll });
    check_boundary<double>("bnd_f64", 2, 3.141592653589793);
}

// ---------------------------------------------------------------------------
// 15. OOP element widths: compressed (4-byte) and uncompressed (8-byte).
//
// Reference arrays decode through get_array_element<std::uint32_t> (compressed
// OOP, 4-byte stride) across the codebase, and would be 8-byte (uintptr_t /
// void*) under -XX:-UseCompressedOops.  The raw helpers are width-agnostic; pin
// both element sizes round-trip exactly so the documented compressed/uncompressed
// oop element size is covered as an input to the stride arithmetic.
// ---------------------------------------------------------------------------
static auto test_oop_element_widths() -> void
{
    // Compressed OOP element = uint32 (4-byte stride).
    {
        const std::vector<std::uint32_t> seed{ 0x00000001u, 0x0000ABCDu, 0xFFFFFFF8u, 0x80000000u };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        check("oop_narrow_length_4", vmhook::array_length(oop) == 4);
        bool ok{ true };
        for (std::int32_t i{ 0 }; i < 4; ++i)
        {
            if (vmhook::get_array_element<std::uint32_t>(oop, i)
                != seed[static_cast<std::size_t>(i)]) { ok = false; }
        }
        check("oop_narrow_roundtrip_4byte_stride", ok);
        // Stride proof: each slot is exactly 4 bytes apart.
        check("oop_narrow_stride_is_4",
              offset_of<std::uint32_t>(0) == 16u && offset_of<std::uint32_t>(1) == 20u
              && offset_of<std::uint32_t>(3) == 28u);
    }

    // Uncompressed OOP element = uintptr_t / void* (8-byte stride on x64).
    {
        const std::vector<std::uintptr_t> seed{ 0x1ull, 0x00007FFF12345678ull, 0xDEADBEEFCAFEull };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        check("oop_wide_length_3", vmhook::array_length(oop) == 3);
        bool ok{ true };
        for (std::int32_t i{ 0 }; i < 3; ++i)
        {
            if (vmhook::get_array_element<std::uintptr_t>(oop, i)
                != seed[static_cast<std::size_t>(i)]) { ok = false; }
        }
        check("oop_wide_roundtrip_8byte_stride", ok);
        check("oop_wide_stride_is_8",
              offset_of<std::uintptr_t>(0) == 16u && offset_of<std::uintptr_t>(1) == 24u
              && offset_of<std::uintptr_t>(2) == 32u);

        // void* element type also compiles (trivially copyable) and round-trips.
        std::vector<void*> seedp{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x2ull)),
                                  reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x00007FFFAABBCCDEull)) };
        std::vector<std::uint8_t> bufp{ build_fake_array(seedp) };
        void* const oopp{ bufp.data() };
        check("oop_voidptr_roundtrip",
              vmhook::get_array_element<void*>(oopp, 0) == seedp[0]
              && vmhook::get_array_element<void*>(oopp, 1) == seedp[1]);
    }
}

// ---------------------------------------------------------------------------
// 16. index * sizeof(T) offset-overflow behaviour (documents library FLAW #1).
//
// vmhook.hpp computes the byte offset as
//     16 + index * static_cast<std::int32_t>(sizeof(element_type))
// i.e. a 32-bit (int) multiply.  For a *legitimate* array the offset can never
// overflow, because every index that passes the bounds check satisfies
// index < length and length is bounded by the real backing allocation — so the
// helper's behaviour on every reachable input is deterministic and correct.
//
// We pin that here without reading wild memory: on a buffer whose real,
// allocation-backed length is small, every in-bounds index lands at a byte
// offset that fits comfortably in int32 (16 + (length-1)*sizeof(T) << INT_MAX),
// so there is no overflow on any index the bounds check admits.  The DANGER
// case (a fabricated huge _length that admits a large index whose
// index*sizeof(T) wraps int32) requires a corrupted length AND would dereference
// unmapped memory; it is documented in the bug report and intentionally NOT
// executed here (it is a genuine library hazard, not test-observable safely).
// ---------------------------------------------------------------------------
static auto test_index_scale_overflow() -> void
{
    // The int32 multiply wraps when index * sizeof(T) >= 2^31.  For sizeof==8
    // that threshold index is 0x10000000 (268435456); for sizeof==1 it is
    // 0x80000000 which is already < 0 as int32 and rejected by the negative
    // guard.  Document the threshold arithmetic as a compile-time-style check so
    // a reader sees exactly where the wrap is, without performing it.
    constexpr std::int64_t wrap_threshold_stride8{ static_cast<std::int64_t>(1) << 31 };
    check("overflow_threshold_stride8_is_2pow31",
          wrap_threshold_stride8 == 2147483648ll
          && (0x10000000ll * 8ll) == wrap_threshold_stride8);

    // Reachability proof: with a real (small) backing length, the maximum
    // in-bounds byte offset is tiny and cannot overflow int32.  We assert the
    // helper reads the genuine last element (no wrap) for an 8-byte stride.
    const std::vector<std::int64_t> seed{ 0x1111111111111111ll, 0x2222222222222222ll,
                                          0x3333333333333333ll, 0x4444444444444444ll };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };
    const std::int32_t last{ vmhook::array_length(oop) - 1 };
    const std::int64_t max_offset{ 16ll + static_cast<std::int64_t>(last) * 8ll };
    check("reachable_max_offset_fits_int32", max_offset < 2147483647ll);
    check("reachable_last_index_no_wrap",
          vmhook::get_array_element<std::int64_t>(oop, last) == 0x4444444444444444ll);

    // A fabricated negative-via-wrap index is still caught by the FRONT guard:
    // 0x10000000 with the bounds check fails because length (4) <= index, so the
    // multiply is never reached.  This shows the bounds check shields the wrap on
    // any array whose length is honest (the supported precondition).
    check("large_index_rejected_before_multiply",
          vmhook::get_array_element<std::int64_t>(oop, opaque_index(0x10000000)) == 0);
    const std::vector<std::uint8_t> before{ buffer };
    vmhook::set_array_element<std::int64_t>(oop, opaque_index(0x10000000), 0x9999999999999999ll);
    check("large_index_write_rejected_before_multiply", buffer == before);
}

// ---------------------------------------------------------------------------
// 17. is_valid_pointer guard matrix on the array oop (header pointer).
//
// The helpers gate on vmhook::hotspot::is_valid_pointer(array_oop).  Exhaustively
// feed each rejection class as the oop and require length 0 / default read /
// no-op write:
//   * floor (0xFFFF) and at/below it,
//   * the canonical ceiling (>= 0x00007FFFFFFFFFFF),
//   * an odd (unaligned) address,
//   * each of the 9 debug/sentinel low-32 patterns.
// A valid heap pointer (our buffer) is the positive control.
// ---------------------------------------------------------------------------
static auto test_pointer_guard_matrix() -> void
{
    auto rejected{ [](std::uintptr_t addr) -> bool
    {
        void* const p{ reinterpret_cast<void*>(addr) };
        const bool len0{ vmhook::array_length(p) == 0 };
        const bool read_default{ vmhook::get_array_element<std::int32_t>(p, 0) == 0 };
        // A write must be a no-op; we cannot observe a no-op on an arbitrary
        // address directly, but it must not crash — reaching the next line is
        // the observable evidence the guard short-circuited before dereference.
        vmhook::set_array_element<std::int32_t>(p, 0, 0x1234);
        return len0 && read_default;
    } };

    // Range floor: 0xFFFF and below are rejected (addr <= floor).  We do NOT
    // probe just-above-floor (e.g. 0x10000): that address PASSES is_valid_pointer
    // (even, > floor, not a sentinel) and the helper would dereference unmapped
    // memory — a real fault, not a guard-observable rejection.  The guard
    // validates address range/shape only, never OS page state (see flaw #3).
    check("guard_floor_exact_rejected", rejected(0xFFFFull));
    check("guard_below_floor_rejected", rejected(0x1000ull));

    // Range ceiling: at/above 0x00007FFFFFFFFFFF rejected (addr >= ceiling).
    check("guard_ceiling_exact_rejected", rejected(0x00007FFFFFFFFFFFull));
    check("guard_above_ceiling_rejected", rejected(0x0000800000000000ull));

    // Odd / unaligned addresses rejected by the (addr & 1) check.
    check("guard_odd_address_rejected", rejected(0x0000000000400001ull));

    // Each of the nine debug/sentinel low-32 patterns must be rejected when it
    // forms the low 32 bits of the oop address.  We place the sentinel in the
    // low 32 bits and a small in-range high word, WITHOUT altering the low bits.
    // is_valid_pointer rejects via either the odd-address check (for the odd
    // sentinels: 0xDEADBEEF, 0xCDCDCDCD, 0xABABABAB, 0xFDFDFDFD) or the sentinel
    // switch (for the even ones).  Both are rejections, which is all the helper
    // contract requires — so we assert rejection, not the specific reason.
    const std::uint32_t sentinels[]{
        0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
        0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu };
    bool all_sentinels_rejected{ true };
    for (const std::uint32_t s : sentinels)
    {
        // High word 0x00000001 keeps the address inside (floor, ceiling) and the
        // low 32 bits exactly equal to the sentinel.
        const std::uintptr_t hi_addr{ (std::uintptr_t{ 1ull } << 32) | s };
        if (!rejected(hi_addr)) { all_sentinels_rejected = false; }
        // Canonical low-only form (high word zero): the low-32 switch matches
        // regardless of the high bits, and the even sentinels are caught by it;
        // the odd ones are caught by the odd-address check.  Either way rejected.
        if (!rejected(static_cast<std::uintptr_t>(s))) { all_sentinels_rejected = false; }
    }
    check("guard_all_nine_sentinels_rejected", all_sentinels_rejected);

    // Positive control: a real heap buffer passes the guard and reads back.
    {
        const std::vector<std::int32_t> seed{ 0x0ABCDEF0 };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        check("guard_valid_heap_pointer_accepted",
              vmhook::array_length(buffer.data()) == 1
              && vmhook::get_array_element<std::int32_t>(buffer.data(), 0) == 0x0ABCDEF0);
    }
}

// ---------------------------------------------------------------------------
// 18. Mixed-width aliasing over the same buffer (endianness / no-padding pin).
//
// Writing two int32s at logical indices 0 and 1 lays down 8 contiguous bytes at
// +16..+23; reading a single int64 at index 0 over the same bytes must compose
// them little-endian (low int32 in the low half).  This proves there is no
// hidden per-element padding beyond sizeof and pins the byte order the codebase
// assumes (x64 LE).
// ---------------------------------------------------------------------------
static auto test_mixed_width_aliasing() -> void
{
    // Buffer big enough for two int64 slots (16 data bytes).
    std::vector<std::uint8_t> buffer(16u + 2u * sizeof(std::int64_t), std::uint8_t{ 0 });
    const std::int32_t len{ 4 }; // 4 int32 slots == 2 int64 slots
    std::memcpy(buffer.data() + 12, &len, sizeof(len));
    void* const oop{ buffer.data() };

    // Write low and high halves as int32.
    vmhook::set_array_element<std::int32_t>(oop, 0, static_cast<std::int32_t>(0x89ABCDEF));
    vmhook::set_array_element<std::int32_t>(oop, 1, static_cast<std::int32_t>(0x01234567));

    // Read back as a single int64 at index 0 (8-byte stride from +16).
    const std::int64_t composed{ vmhook::get_array_element<std::int64_t>(oop, 0) };
    check("alias_two_i32_compose_le_i64",
          composed == static_cast<std::int64_t>(0x0123456789ABCDEFll));

    // And the reverse: write an int64, read the two int32 halves.
    vmhook::set_array_element<std::int64_t>(oop, 1, static_cast<std::int64_t>(0x1122334455667788ll));
    check("alias_i64_low_half_is_i32_index2",
          vmhook::get_array_element<std::uint32_t>(oop, 2) == 0x55667788u);
    check("alias_i64_high_half_is_i32_index3",
          vmhook::get_array_element<std::uint32_t>(oop, 3) == 0x11223344u);
}

// ---------------------------------------------------------------------------
// 19. Dense round-trip at non-trivial indices on a large 8-byte array.
//
// Round-tripping only indices 0..3 (as the original tests do) cannot reveal a
// stride error whose magnitude is a multiple that only diverges at larger
// indices.  Use a 64-element int64 array and round-trip at 0,1,17,31,32,63,
// each verified against the oracle offset, so a 32-bit-vs-64-bit stride
// discrepancy would surface as a wrong neighbour.
// ---------------------------------------------------------------------------
static auto test_dense_nontrivial_indices() -> void
{
    constexpr std::int32_t n{ 64 };
    std::vector<std::int64_t> seed(static_cast<std::size_t>(n));
    for (std::int32_t i{ 0 }; i < n; ++i)
    {
        seed[static_cast<std::size_t>(i)] = 0x1000000000000000ll + i;
    }
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    check("dense64_length_is_64", vmhook::array_length(oop) == n);

    const std::int32_t probes[]{ 0, 1, 17, 31, 32, 63 };
    bool ok{ true };
    for (const std::int32_t i : probes)
    {
        const std::int64_t want{ 0x1000000000000000ll + i };
        if (vmhook::get_array_element<std::int64_t>(oop, i) != want) { ok = false; break; }
        if (raw_peek<std::int64_t>(buffer, i) != want)               { ok = false; break; }

        const std::int64_t newv{ 0x2000000000000000ll + i };
        vmhook::set_array_element<std::int64_t>(oop, i, newv);
        if (vmhook::get_array_element<std::int64_t>(oop, i) != newv) { ok = false; break; }
        if (raw_peek<std::int64_t>(buffer, i) != newv)               { ok = false; break; }
    }
    check("dense64_nontrivial_index_roundtrip", ok);

    // Neighbours of a touched index must be unchanged: rewrite index 32 and
    // confirm 31 and 33 keep their values.
    vmhook::set_array_element<std::int64_t>(oop, 31, 0x3100000000000031ll);
    vmhook::set_array_element<std::int64_t>(oop, 33, 0x3300000000000033ll);
    vmhook::set_array_element<std::int64_t>(oop, 32, 0x3200000000000032ll);
    check("dense64_neighbours_preserved",
          vmhook::get_array_element<std::int64_t>(oop, 31) == 0x3100000000000031ll
          && vmhook::get_array_element<std::int64_t>(oop, 32) == 0x3200000000000032ll
          && vmhook::get_array_element<std::int64_t>(oop, 33) == 0x3300000000000033ll);
}

// ---------------------------------------------------------------------------
// 20. array_length reads EXACTLY +12 — not +0 (mark) or +8 (klass).
//
// Poison the mark word (+0..+7) and the narrow-klass slot (+8..+11) with values
// that, if mis-read as the length, would give a wrong answer; confirm the length
// still equals the int32 actually written at +12 for several distinct lengths.
// Also confirm element writes never alter +0..+15 for an interior index.
// ---------------------------------------------------------------------------
static auto test_length_reads_only_offset12() -> void
{
    for (const std::int32_t len : { 1, 2, 7, 100 })
    {
        std::vector<std::uint8_t> buffer(16u + static_cast<std::size_t>(len) * sizeof(std::int32_t),
                                         std::uint8_t{ 0 });
        // Poison mark (+0..+7) and narrow klass (+8..+11) with 0xEE.
        std::memset(buffer.data(), 0xEE, 12u);
        std::memcpy(buffer.data() + 12, &len, sizeof(len));
        char name[64];
        std::snprintf(name, sizeof(name), "length_only_off12_len%d", len);
        check(name, vmhook::array_length(buffer.data()) == len);
    }

    // Element write to an interior index leaves the entire 16-byte header
    // (mark+klass+length) byte-identical.
    {
        const std::vector<std::int32_t> seed{ 10, 20, 30, 40, 50 };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        // Snapshot the header (note _length lives in here too, at +12).
        std::uint8_t header_before[16];
        std::memcpy(header_before, buffer.data(), 16u);
        vmhook::set_array_element<std::int32_t>(buffer.data(), 2, 0x6B6B6B6B);
        const bool header_same{ std::memcmp(header_before, buffer.data(), 16u) == 0 };
        check("interior_write_leaves_full_header", header_same
              && vmhook::get_array_element<std::int32_t>(buffer.data(), 2) == 0x6B6B6B6B);
    }
}

// ---------------------------------------------------------------------------
// 21. clamp_safe_container_count — the defensive count clamp shared by every
//     reader that derives an element count from a live oop (array_length() or a
//     Java size / size() field).  Pure arithmetic, no oop needed: it maps a raw
//     (possibly negative or absurdly large) int32 to [0, k_max_safe_container_elems]
//     and is used for BOTH the pre-reservation and the read-loop bound so a
//     corrupted count degrades to a bounded, non-terminating partial read while
//     an honest count (raw <= cap) passes through byte-identically.
//
// We assert the boundary behaviour both at runtime AND in a constexpr context
// (the helper is constexpr, so a wrong constant-folded result is a compile error).
// ---------------------------------------------------------------------------
static auto test_clamp_safe_container_count() -> void
{
    constexpr std::int32_t cap{ static_cast<std::int32_t>(vmhook::k_max_safe_container_elems) };

    // The cap must be the documented 16,777,216 (1<<24): large enough to never
    // truncate a realistic introspected container, small enough that
    // cap * sizeof(largest reserved element) cannot plausibly bad_alloc.
    check("clamp_cap_is_1_shl_24", vmhook::k_max_safe_container_elems == (1ull << 24));
    check("clamp_cap_value_16777216", vmhook::k_max_safe_container_elems == 16777216ull);

    // The cap fits int32 with room to spare (the helper returns int32; its own
    // static_assert guarantees this, but assert it here too for documentation).
    check("clamp_cap_fits_int32",
          vmhook::k_max_safe_container_elems
              <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));

    // cap * the widest element this header ever reserves (a std::pair of two
    // unique_ptrs == 16 bytes on x64) stays well under any allocator's hard
    // ceiling, so the post-clamp reserve cannot throw length_error.
    check("clamp_cap_times_16_no_overflow",
          vmhook::k_max_safe_container_elems
              < (std::numeric_limits<std::size_t>::max() / 16u));

    // Negatives (a torn / sign-flipped _length read) collapse to 0.
    check("clamp_intmin_is_0",
          vmhook::clamp_safe_container_count(std::numeric_limits<std::int32_t>::min()) == 0);
    check("clamp_neg_one_is_0", vmhook::clamp_safe_container_count(-1) == 0);
    check("clamp_neg_large_is_0", vmhook::clamp_safe_container_count(-123456789) == 0);

    // Zero stays zero (empty container -> empty vector, no reserve, no loop).
    check("clamp_zero_is_0", vmhook::clamp_safe_container_count(0) == 0);

    // In-range values pass through UNCHANGED — the honest-container guarantee.
    check("clamp_one_is_1", vmhook::clamp_safe_container_count(1) == 1);
    check("clamp_small_passthrough", vmhook::clamp_safe_container_count(5000) == 5000);
    check("clamp_below_cap_passthrough", vmhook::clamp_safe_container_count(cap - 1) == cap - 1);

    // At and above the cap, the result saturates to exactly the cap.
    check("clamp_at_cap_is_cap", vmhook::clamp_safe_container_count(cap) == cap);
    check("clamp_above_cap_is_cap", vmhook::clamp_safe_container_count(cap + 1) == cap);
    check("clamp_intmax_is_cap",
          vmhook::clamp_safe_container_count(std::numeric_limits<std::int32_t>::max()) == cap);

    // The result is ALWAYS a valid, non-negative reservation/loop bound.
    bool all_in_range{ true };
    for (const std::int32_t raw : { std::numeric_limits<std::int32_t>::min(), -7, -1, 0,
                                    1, 63, 5000, cap - 1, cap, cap + 1,
                                    std::numeric_limits<std::int32_t>::max() })
    {
        const std::int32_t got{ vmhook::clamp_safe_container_count(raw) };
        if (got < 0 || got > cap) { all_in_range = false; break; }
    }
    check("clamp_result_always_in_0_cap", all_in_range);

    // constexpr usability: the helper folds at compile time, so these are
    // compile-time-checked clamps (a regression would fail to build).
    static_assert(vmhook::clamp_safe_container_count(-1) == 0,
                  "clamp(-1) must be 0 in constexpr context");
    static_assert(vmhook::clamp_safe_container_count(0) == 0,
                  "clamp(0) must be 0 in constexpr context");
    static_assert(vmhook::clamp_safe_container_count(42) == 42,
                  "clamp(42) must pass through in constexpr context");
    static_assert(vmhook::clamp_safe_container_count(
                      std::numeric_limits<std::int32_t>::max())
                      == static_cast<std::int32_t>(vmhook::k_max_safe_container_elems),
                  "clamp(INT_MAX) must saturate to the cap in constexpr context");
    check("clamp_constexpr_static_asserts_compiled", true);
}

// ===========================================================================
// COMPILE-TIME ARITHMETIC SWEEP (22-25).  The prompt asks, where possible, for
// compile-time static_assert proofs of the PURE width / offset / address
// arithmetic, with runtime reserved for anything needing a synthetic buffer.
//
// The three helpers contain no BasicType size table and no base-offset helper:
// the element stride IS sizeof(T) (the C++ element type chosen per BasicType)
// and the data base IS the constant +16, with _length at +12 (see
// vmhook.hpp::array_length / get_array_element / set_array_element).  The
// sections below pin EXACTLY those three numbers — the JVM BasicType -> C++
// width mapping, the +12 length slot, the +16 data base, and the
// base + index*stride address — as constant expressions, so any divergence is a
// COMPILE error (a hard regression gate the runtime checks cannot give) rather
// than a runtime [FAIL].  These mirror the header's own constants independently.
// ===========================================================================

// The documented HotSpot array layout constants the helpers hard-code.  Kept as
// named constants here so the static_asserts read as "the helper's +12 / +16",
// and a single edit re-points every proof if the layout assumption ever moves.
namespace layout_oracle
{
    inline constexpr std::size_t length_offset{ 12u };  // _length (int32) slot
    inline constexpr std::size_t data_base{ 16u };      // _data[0] byte offset
    inline constexpr std::size_t narrow_oop_size{ 4u }; // compressed reference
    inline constexpr std::size_t wide_oop_size{ 8u };   // -XX:-UseCompressedOops

    // The element byte offset the helpers compute: +16 + index * sizeof(T).
    // size_t arithmetic so the ORACLE never wraps (the documented int->ptrdiff_t
    // widening inside the helper is what keeps the helper itself honest).
    template<typename T>
    constexpr auto element_offset(std::size_t index) noexcept -> std::size_t
    {
        return data_base + index * sizeof(T);
    }

    // The helper's half-open bounds predicate, as a pure function: an access at
    // `index` into a length-`length` array is in range iff 0 <= index < length.
    constexpr auto index_in_range(std::int32_t index, std::int32_t length) noexcept -> bool
    {
        return index >= 0 && index < length;
    }
}

// ---------------------------------------------------------------------------
// 22. COMPILE-TIME element width for EVERY JVM BasicType.
//
// HotSpot BasicType -> element stride (== sizeof of the C++ type the helpers are
// instantiated with for that BasicType, the value used as `index * sizeof(T)`):
//     T_BOOLEAN  boolean -> 1   T_BYTE   byte   -> 1
//     T_CHAR     char    -> 2   T_SHORT  short  -> 2
//     T_INT      int     -> 4   T_FLOAT  float  -> 4
//     T_LONG     long    -> 8   T_DOUBLE double -> 8
//     T_OBJECT/T_ARRAY (reference): narrow oop -> 4, wide oop -> 8
// Every one is asserted at compile time against the exact C++ element type the
// codebase passes for that BasicType.  A platform whose `float`/`double` or
// fixed-width type drifted from the JVM contract would fail to BUILD.
// ---------------------------------------------------------------------------
// Primitive widths (the eight Java primitives).
static_assert(sizeof(bool)          == 1, "T_BOOLEAN element stride must be 1 byte");
static_assert(sizeof(std::uint8_t)  == 1, "boolean[] backing byte stride must be 1");
static_assert(sizeof(std::int8_t)   == 1, "T_BYTE element stride must be 1 byte");
static_assert(sizeof(char)          == 1, "raw char element stride must be 1 byte");
static_assert(sizeof(char16_t)      == 2, "T_CHAR (UTF-16) element stride must be 2 bytes");
static_assert(sizeof(std::int16_t)  == 2, "T_SHORT element stride must be 2 bytes");
static_assert(sizeof(std::uint16_t) == 2, "char[]/short[] unsigned stride must be 2 bytes");
static_assert(sizeof(std::int32_t)  == 4, "T_INT element stride must be 4 bytes");
static_assert(sizeof(std::uint32_t) == 4, "compressed-oop / int unsigned stride must be 4 bytes");
static_assert(sizeof(float)         == 4, "T_FLOAT element stride must be 4 bytes (IEEE-754 single)");
static_assert(sizeof(std::int64_t)  == 8, "T_LONG element stride must be 8 bytes");
static_assert(sizeof(std::uint64_t) == 8, "long unsigned stride must be 8 bytes");
static_assert(sizeof(double)        == 8, "T_DOUBLE element stride must be 8 bytes (IEEE-754 double)");
// Reference (T_OBJECT / T_ARRAY) element widths: compressed vs uncompressed oop.
static_assert(layout_oracle::narrow_oop_size == sizeof(std::uint32_t),
              "compressed reference element must be a 4-byte narrow oop");
static_assert(layout_oracle::wide_oop_size == sizeof(std::uintptr_t),
              "uncompressed reference element must be a pointer-width (8-byte) oop on x64");
static_assert(sizeof(void*) == layout_oracle::wide_oop_size,
              "void* reference element width must equal the wide-oop size on this target");
// The helpers only accept trivially-copyable element types (their static_assert):
// confirm every BasicType-mapped C++ type the codebase uses qualifies, at compile time.
static_assert(std::is_trivially_copyable_v<bool>
              && std::is_trivially_copyable_v<std::int8_t>
              && std::is_trivially_copyable_v<std::uint8_t>
              && std::is_trivially_copyable_v<char>
              && std::is_trivially_copyable_v<char16_t>
              && std::is_trivially_copyable_v<std::int16_t>
              && std::is_trivially_copyable_v<std::uint16_t>
              && std::is_trivially_copyable_v<std::int32_t>
              && std::is_trivially_copyable_v<std::uint32_t>
              && std::is_trivially_copyable_v<float>
              && std::is_trivially_copyable_v<std::int64_t>
              && std::is_trivially_copyable_v<std::uint64_t>
              && std::is_trivially_copyable_v<double>
              && std::is_trivially_copyable_v<std::uintptr_t>
              && std::is_trivially_copyable_v<void*>,
              "every BasicType-mapped element type must satisfy the helpers' trivially-copyable contract");

// ---------------------------------------------------------------------------
// 23. COMPILE-TIME header/layout constants: length slot +12, data base +16.
//
// array_length reads the int32 at +12; get/set read/write at +16 + i*stride.
// Pin those two offsets, their relationship (length slot sits in the last 4
// bytes of the 16-byte header, immediately before the data), and that the data
// base equals the make_java_array header size (16) as constant expressions.
// ---------------------------------------------------------------------------
static_assert(layout_oracle::length_offset == 12u, "_length must live at byte +12");
static_assert(layout_oracle::data_base == 16u, "_data[0] must start at byte +16");
static_assert(layout_oracle::length_offset + sizeof(std::int32_t) == layout_oracle::data_base,
              "the int32 _length slot must end exactly where _data begins (no gap, no overlap)");
static_assert(layout_oracle::data_base - layout_oracle::length_offset == sizeof(std::int32_t),
              "_length occupies exactly the 4 bytes between +12 and the data base");
// index 0's element offset is the data base for every width (no per-type bias).
static_assert(layout_oracle::element_offset<bool>(0)          == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<std::int8_t>(0)   == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<char16_t>(0)      == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<std::int16_t>(0)  == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<std::int32_t>(0)  == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<float>(0)         == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<std::int64_t>(0)  == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<double>(0)        == layout_oracle::data_base, "");
static_assert(layout_oracle::element_offset<std::uintptr_t>(0)== layout_oracle::data_base, "");

// ---------------------------------------------------------------------------
// 24. COMPILE-TIME element-address matrix: +16 + index*stride, every (width x
//     index) combination, with exact numeric offsets.
//
// For each element width and a spread of indices (0, 1, mid, a last-valid, and
// a large index whose 64-bit offset would differ from a 32-bit-truncated one),
// assert the helper's address formula yields the exact byte offset.  This is the
// "(type x index) address matrix" the prompt calls for, decided at compile time
// so a base or stride regression cannot link.
// ---------------------------------------------------------------------------
// Helper: stringize the per-width offset assertions compactly.  Each line is
// element_offset<T>(index) == 16 + index*sizeof(T) with the literal RHS spelled
// out, so a wrong base OR a wrong stride both fail (they are independent terms).

// 1-byte stride: offset == 16 + index.
static_assert(layout_oracle::element_offset<std::uint8_t>(0)   == 16u,  "u8[0]");
static_assert(layout_oracle::element_offset<std::uint8_t>(1)   == 17u,  "u8[1]");
static_assert(layout_oracle::element_offset<std::uint8_t>(7)   == 23u,  "u8[7]");
static_assert(layout_oracle::element_offset<std::uint8_t>(255) == 271u, "u8[255]");

// 2-byte stride: offset == 16 + 2*index.
static_assert(layout_oracle::element_offset<char16_t>(0)    == 16u,  "c16[0]");
static_assert(layout_oracle::element_offset<char16_t>(1)    == 18u,  "c16[1]");
static_assert(layout_oracle::element_offset<char16_t>(10)   == 36u,  "c16[10]");
static_assert(layout_oracle::element_offset<std::int16_t>(100) == 216u, "i16[100]");

// 4-byte stride: offset == 16 + 4*index (int / float / narrow oop).
static_assert(layout_oracle::element_offset<std::int32_t>(0)  == 16u,  "i32[0]");
static_assert(layout_oracle::element_offset<std::int32_t>(1)  == 20u,  "i32[1]");
static_assert(layout_oracle::element_offset<std::int32_t>(25) == 116u, "i32[25]");
static_assert(layout_oracle::element_offset<std::int32_t>(49) == 212u, "i32[49]");
static_assert(layout_oracle::element_offset<float>(49)        == 212u, "f32[49] matches i32 stride");
static_assert(layout_oracle::element_offset<std::uint32_t>(7) == 44u,  "narrow-oop[7]");

// 8-byte stride: offset == 16 + 8*index (long / double / wide oop).
static_assert(layout_oracle::element_offset<std::int64_t>(0)  == 16u,  "i64[0]");
static_assert(layout_oracle::element_offset<std::int64_t>(1)  == 24u,  "i64[1]");
static_assert(layout_oracle::element_offset<std::int64_t>(3)  == 40u,  "i64[3]");
static_assert(layout_oracle::element_offset<std::int64_t>(63) == 520u, "i64[63]");
static_assert(layout_oracle::element_offset<double>(63)       == 520u, "f64[63] matches i64 stride");
static_assert(layout_oracle::element_offset<std::uintptr_t>(2)== 32u,  "wide-oop[2]");

// 32-bit-vs-64-bit divergence pin: index 0x10000000 at stride 8 wraps a 32-bit
// `index * (int)sizeof` to INT_MIN (-2147483648); the honest 64-bit offset is
// 16 + 0x10000000*8 = 0x80000010.  The oracle (size_t) yields the honest value,
// documenting the magnitude the helper's ptrdiff_t widening must preserve.
static_assert(layout_oracle::element_offset<std::int64_t>(0x10000000u) == 0x80000010ull,
              "index 0x10000000 stride 8 must scale to 0x80000010 in honest (non-wrapping) arithmetic");
// Document the 32-bit wrap the helper's ptrdiff_t widening avoids, WITHOUT
// invoking signed overflow (which is UB and not a constant expression): the
// product is formed in unsigned (well-defined modular wrap) to the bit pattern
// 0x80000000, which reinterpreted as int32 is INT_MIN.  This is the value a
// naive `index * (int)sizeof` would have produced before sign-extension.
static_assert(0x10000000u * 8u == 0x80000000u,
              "0x10000000 * 8 wraps (mod 2^32) to the bit pattern 0x80000000");
static_assert(static_cast<std::int32_t>(0x80000000u) == (std::numeric_limits<std::int32_t>::min)(),
              "bit pattern 0x80000000 reinterpreted as int32 is INT_MIN — the wild offset avoided");

// ---------------------------------------------------------------------------
// 25. COMPILE-TIME bounds predicate over a (length x index) boundary matrix.
//
// The helpers accept an access iff 0 <= index < length (half-open).  Enumerate
// the full boundary neighbourhood for representative lengths as constant
// expressions: -1 and INT_MIN always rejected; 0 valid iff length>=1; length-1
// valid iff length>=1; length / length+1 / INT_MAX always rejected; empty array
// (length 0) rejects index 0.  A change to the comparison (e.g. <= vs <) fails
// to build.
// ---------------------------------------------------------------------------
namespace
{
    constexpr std::int32_t k_int_min{ (std::numeric_limits<std::int32_t>::min)() };
    constexpr std::int32_t k_int_max{ (std::numeric_limits<std::int32_t>::max)() };
}
// Universally-invalid indices, independent of length:
static_assert(!layout_oracle::index_in_range(-1, 0),        "neg index rejected (len 0)");
static_assert(!layout_oracle::index_in_range(-1, 5),        "neg index rejected (len 5)");
static_assert(!layout_oracle::index_in_range(k_int_min, 5), "INT_MIN index rejected");
static_assert(!layout_oracle::index_in_range(k_int_max, 5), "INT_MAX index rejected (len 5)");
// Empty array: every index, including 0, is out of range.
static_assert(!layout_oracle::index_in_range(0, 0), "len 0: index 0 is the first OOB index");
static_assert(!layout_oracle::index_in_range(1, 0), "len 0: index 1 OOB");
// Length 1: only index 0 is valid; index 1 (== length) is the first OOB.
static_assert(layout_oracle::index_in_range(0, 1),  "len 1: index 0 valid");
static_assert(!layout_oracle::index_in_range(1, 1), "len 1: index 1 (==len) OOB");
// Length 5: 0 and 4 valid; 5 and 6 invalid (the half-open upper edge).
static_assert(layout_oracle::index_in_range(0, 5),  "len 5: index 0 valid");
static_assert(layout_oracle::index_in_range(4, 5),  "len 5: last index 4 valid");
static_assert(!layout_oracle::index_in_range(5, 5), "len 5: index 5 (==len) OOB");
static_assert(!layout_oracle::index_in_range(6, 5), "len 5: index 6 (len+1) OOB");
// A negative length (corrupted/torn _length) rejects EVERY non-negative index,
// because index >= length whenever length < 0 <= index — pins flaw-#2 containment
// at compile time (no index can pass the bounds check on a negative length).
static_assert(!layout_oracle::index_in_range(0, -1),         "neg length: index 0 rejected");
static_assert(!layout_oracle::index_in_range(0, -5),         "neg length: index 0 rejected");
static_assert(!layout_oracle::index_in_range(k_int_max, -1), "neg length: INT_MAX rejected");
static_assert(!layout_oracle::index_in_range(1000000, k_int_min), "neg length INT_MIN: index rejected");

// ---------------------------------------------------------------------------
// Runtime cross-check that the SAME oracle the static_asserts use agrees with
// what the live helpers actually do on a synthetic buffer at the exhaustive
// (width x index) matrix.  The static_asserts prove the formula; this proves the
// helpers obey the formula (offset + bounds) on real bytes, closing the loop.
// ---------------------------------------------------------------------------
template<typename T>
static auto crosscheck_oracle_offset(const char* label) -> void
{
    // A 70-element buffer: indices below span 0, 1, mid, last-valid for the
    // widths used, all comfortably inside the allocation.
    constexpr std::int32_t n{ 70 };
    std::vector<T> seed(static_cast<std::size_t>(n), T{});
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    bool ok{ vmhook::array_length(oop) == n };
    const std::int32_t probes[]{ 0, 1, 2, 9, 31, 35, 69 };
    for (const std::int32_t i : probes)
    {
        if (!ok) { break; }
        // The byte offset the test oracle predicts must equal +16+i*sizeof(T)
        // AND must be exactly where the helper deposits a written value.
        const std::size_t predicted{ layout_oracle::element_offset<T>(static_cast<std::size_t>(i)) };
        if (predicted != 16u + static_cast<std::size_t>(i) * sizeof(T)) { ok = false; break; }

        const std::uint64_t bits{ 0xF00DCAFE00000000ull ^ (static_cast<std::uint64_t>(i) * 1099511628211ull) };
        T value{};
        std::memcpy(&value, &bits, sizeof(T));
        vmhook::set_array_element<T>(oop, i, value);

        // Raw bytes at the predicted offset must equal what we wrote, and the
        // helper's own read must agree — i.e. the helper used `predicted`.
        T raw{};
        std::memcpy(&raw, buffer.data() + predicted, sizeof(T));
        if (!bits_equal(raw, value)) { ok = false; break; }
        if (!bits_equal(vmhook::get_array_element<T>(oop, i), value)) { ok = false; break; }
    }
    check(label, ok);
}

static auto test_compile_time_arithmetic() -> void
{
    // The static_asserts above already fired at compile time; this check records
    // that the TU embedding them built (a regression would have failed the build).
    check("ct_width_offset_bounds_static_asserts_compiled", true);

    // Runtime confirmation that the helpers honour the same oracle, per width.
    crosscheck_oracle_offset<std::uint8_t>("ct_crosscheck_offset_u8");
    crosscheck_oracle_offset<std::int16_t>("ct_crosscheck_offset_i16");
    crosscheck_oracle_offset<char16_t>("ct_crosscheck_offset_c16");
    crosscheck_oracle_offset<std::int32_t>("ct_crosscheck_offset_i32");
    crosscheck_oracle_offset<float>("ct_crosscheck_offset_f32");
    crosscheck_oracle_offset<std::int64_t>("ct_crosscheck_offset_i64");
    crosscheck_oracle_offset<double>("ct_crosscheck_offset_f64");
    crosscheck_oracle_offset<std::uint32_t>("ct_crosscheck_offset_narrow_oop");
    crosscheck_oracle_offset<std::uintptr_t>("ct_crosscheck_offset_wide_oop");

    // Runtime spot-checks that the live bounds guard matches index_in_range at
    // the exact boundary, for a concrete length (the static matrix proves the
    // predicate; this proves get/set use it).  Length 5 int32 array.
    {
        const std::vector<std::int32_t> seed{ 10, 11, 12, 13, 14 };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        bool agree{ true };
        for (std::int32_t i{ -2 }; i <= 7 && agree; ++i)
        {
            const bool predicate{ layout_oracle::index_in_range(i, 5) };
            const std::int32_t got{ vmhook::get_array_element<std::int32_t>(oop, opaque_index(i)) };
            // In range -> the seeded value (10+i); out of range -> default 0.
            const bool helper_in_range{ got == 10 + i };
            const bool helper_out_range{ got == 0 };
            if (predicate && !helper_in_range)  { agree = false; }
            if (!predicate && !helper_out_range){ agree = false; }
        }
        check("ct_live_bounds_match_predicate_len5", agree);
    }
}

// ===========================================================================
// EXHAUSTIVE VALUE-DIMENSION EXPANSION (26-33).  Sections 1-25 exhaust the
// INDEX / offset / bounds / guard dimensions; the element VALUE that travels
// through the memcpy was only spot-sampled (a few ordinary numbers per width).
// The helpers copy sizeof(T) bytes verbatim, so the value contract is "every
// bit pattern of T survives a set->get round trip byte-identically, and is
// deposited at exactly the oracle offset".  The sections below pin that across
// the FULL value space: literally all 256 1-byte and all 65536 2-byte patterns,
// every single-bit and boundary pattern for 4/8-byte widths, and the complete
// IEEE-754 special-value set (NaN payloads, +/-0, +/-inf, subnormals) compared
// by bits (never by ==, which mis-handles NaN and +/-0 — proven below).
// ===========================================================================

// ---------------------------------------------------------------------------
// 26. IEEE-754 float/double special bit patterns: NaN (quiet/signalling/payload),
//     +/-0, +/-inf, smallest/largest subnormal, smallest normal, max, lowest,
//     epsilon.  Each must round-trip BIT-EXACT through set->get (the helper does
//     a raw sizeof(T) memcpy, so every pattern — including non-canonical NaNs —
//     must survive unchanged).  Comparison is by raw bits, because:
//       * NaN == NaN is FALSE, so an == round-trip check would wrongly FAIL even
//         on a perfectly preserved NaN;
//       * (-0.0) == (+0.0) is TRUE, so an == check would wrongly PASS even if a
//         stride bug swapped -0 for +0 (different sign bit).
//     We assert both of those facts explicitly so the rationale for bits_equal
//     (used throughout this file) is itself tested, then prove the helpers
//     preserve the sign bit of zero and the exact NaN payload bits.
// ---------------------------------------------------------------------------
static_assert(std::numeric_limits<float>::is_iec559,
              "section 26 assumes IEEE-754 binary32 for deterministic special-value bit patterns");
static_assert(std::numeric_limits<double>::is_iec559,
              "section 26 assumes IEEE-754 binary64 for deterministic special-value bit patterns");
static_assert(sizeof(float) == sizeof(std::uint32_t), "binary32 must alias uint32 for bit construction");
static_assert(sizeof(double) == sizeof(std::uint64_t), "binary64 must alias uint64 for bit construction");

// Build a float/double from an explicit IEEE-754 bit pattern (deterministic and
// cross-platform under is_iec559; std::nan() payloads are impl-defined, so we
// lay the bits down ourselves instead).
static auto f32_from_bits(std::uint32_t bits) noexcept -> float
{
    float f{};
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
static auto f64_from_bits(std::uint64_t bits) noexcept -> double
{
    double d{};
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}
static auto bits_of_f32(float f) noexcept -> std::uint32_t
{
    std::uint32_t bits{};
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}
static auto bits_of_f64(double d) noexcept -> std::uint64_t
{
    std::uint64_t bits{};
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static auto test_float_special_values() -> void
{
    // ---- Rationale assertions: why this file compares floats by bits. --------
    // A signalling-vs-quiet distinction and a custom payload are only observable
    // bit-wise; == collapses them all to "unordered".
    {
        const float qnan{ f32_from_bits(0x7FC00000u) };       // canonical quiet NaN
        // NaN != NaN under IEEE ordering, yet its bits equal themselves.
        check("f32_nan_not_eq_self_under_operator_eq", !(qnan == qnan)); // NOLINT
        check("f32_nan_bits_equal_self", bits_equal(qnan, qnan));

        const float neg_zero{ f32_from_bits(0x80000000u) };
        const float pos_zero{ f32_from_bits(0x00000000u) };
        // +0 == -0 is TRUE, but their bits differ — bits_equal must distinguish.
        check("f32_neg_zero_eq_pos_zero_under_operator_eq", neg_zero == pos_zero);
        check("f32_neg_zero_bits_differ_from_pos_zero", !bits_equal(neg_zero, pos_zero));
        check("f32_neg_zero_sign_bit_set", bits_of_f32(neg_zero) == 0x80000000u);
    }

    // ---- float (binary32) special-value round-trip table. --------------------
    // {label, bit pattern}.  Covers: +/-0, +/-inf, quiet NaN, signalling NaN,
    // NaN with a non-zero low payload, NaN with sign bit, smallest positive
    // subnormal, largest subnormal, smallest positive normal, largest finite,
    // and an all-ones pattern (a negative NaN).
    struct f32_case { const char* label; std::uint32_t bits; };
    const f32_case f32_cases[]{
        { "pos_zero",        0x00000000u },
        { "neg_zero",        0x80000000u },
        { "pos_inf",         0x7F800000u },
        { "neg_inf",         0xFF800000u },
        { "qnan",            0x7FC00000u },
        { "snan",            0x7F800001u },  // exponent all-ones, payload nonzero, quiet bit clear
        { "nan_payload",     0x7FABCDEFu },
        { "neg_nan",         0xFFC00000u },
        { "min_subnormal",   0x00000001u },
        { "max_subnormal",   0x007FFFFFu },
        { "min_normal",      0x00800000u },
        { "max_finite",      0x7F7FFFFFu },
        { "all_ones",        0xFFFFFFFFu },
    };
    bool all_f32_roundtrip{ true };
    bool all_f32_at_oracle{ true };
    for (const f32_case& c : f32_cases)
    {
        const std::vector<float> seed{ 0.0f, 0.0f, 0.0f };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        const float v{ f32_from_bits(c.bits) };

        vmhook::set_array_element<float>(oop, 1, v);
        // Round-trips bit-exact.
        if (!bits_equal(vmhook::get_array_element<float>(oop, 1), v)) { all_f32_roundtrip = false; }
        // Lands at exactly +16 + 1*4, and the raw 4 bytes equal the pattern.
        // (compare by bits, not float ==: raw_peek<float> == v mis-handles NaN.)
        if (bits_of_f32(raw_peek<float>(buffer, 1)) != c.bits) { all_f32_at_oracle = false; }
        // Neighbours (still +0.0) untouched.
        if (bits_of_f32(vmhook::get_array_element<float>(oop, 0)) != 0u) { all_f32_roundtrip = false; }
        if (bits_of_f32(vmhook::get_array_element<float>(oop, 2)) != 0u) { all_f32_roundtrip = false; }
    }
    check("f32_special_values_roundtrip_bit_exact", all_f32_roundtrip);
    check("f32_special_values_land_at_oracle_offset", all_f32_at_oracle);

    // Cross-check the constructed specials against <limits> (independent oracle):
    // the bit patterns we laid down must equal the library's own constants.
    check("f32_pos_inf_matches_limits",
          bits_of_f32(f32_from_bits(0x7F800000u)) == bits_of_f32(std::numeric_limits<float>::infinity()));
    check("f32_max_finite_matches_limits",
          bits_of_f32(f32_from_bits(0x7F7FFFFFu)) == bits_of_f32((std::numeric_limits<float>::max)()));
    check("f32_min_normal_matches_limits",
          bits_of_f32(f32_from_bits(0x00800000u)) == bits_of_f32((std::numeric_limits<float>::min)()));
    check("f32_min_subnormal_matches_limits",
          bits_of_f32(f32_from_bits(0x00000001u)) == bits_of_f32(std::numeric_limits<float>::denorm_min()));
    check("f32_lowest_matches_neg_max_finite",
          bits_of_f32(std::numeric_limits<float>::lowest()) == 0xFF7FFFFFu);

    // ---- double (binary64) special-value round-trip table. -------------------
    {
        const double qnan{ f64_from_bits(0x7FF8000000000000ull) };
        check("f64_nan_not_eq_self_under_operator_eq", !(qnan == qnan)); // NOLINT
        check("f64_nan_bits_equal_self", bits_equal(qnan, qnan));
        const double neg_zero{ f64_from_bits(0x8000000000000000ull) };
        const double pos_zero{ f64_from_bits(0x0000000000000000ull) };
        check("f64_neg_zero_eq_pos_zero_under_operator_eq", neg_zero == pos_zero);
        check("f64_neg_zero_bits_differ_from_pos_zero", !bits_equal(neg_zero, pos_zero));
    }

    struct f64_case { const char* label; std::uint64_t bits; };
    const f64_case f64_cases[]{
        { "pos_zero",        0x0000000000000000ull },
        { "neg_zero",        0x8000000000000000ull },
        { "pos_inf",         0x7FF0000000000000ull },
        { "neg_inf",         0xFFF0000000000000ull },
        { "qnan",            0x7FF8000000000000ull },
        { "snan",            0x7FF0000000000001ull },
        { "nan_payload",     0x7FF123456789ABCDull },
        { "neg_nan",         0xFFF8000000000000ull },
        { "min_subnormal",   0x0000000000000001ull },
        { "max_subnormal",   0x000FFFFFFFFFFFFFull },
        { "min_normal",      0x0010000000000000ull },
        { "max_finite",      0x7FEFFFFFFFFFFFFFull },
        { "all_ones",        0xFFFFFFFFFFFFFFFFull },
    };
    bool all_f64_roundtrip{ true };
    bool all_f64_at_oracle{ true };
    for (const f64_case& c : f64_cases)
    {
        const std::vector<double> seed{ 0.0, 0.0, 0.0 };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        const double v{ f64_from_bits(c.bits) };

        vmhook::set_array_element<double>(oop, 1, v);
        if (!bits_equal(vmhook::get_array_element<double>(oop, 1), v)) { all_f64_roundtrip = false; }
        if (bits_of_f64(raw_peek<double>(buffer, 1)) != c.bits) { all_f64_at_oracle = false; }
        if (bits_of_f64(vmhook::get_array_element<double>(oop, 0)) != 0ull) { all_f64_roundtrip = false; }
        if (bits_of_f64(vmhook::get_array_element<double>(oop, 2)) != 0ull) { all_f64_roundtrip = false; }
    }
    check("f64_special_values_roundtrip_bit_exact", all_f64_roundtrip);
    check("f64_special_values_land_at_oracle_offset", all_f64_at_oracle);

    check("f64_pos_inf_matches_limits",
          bits_of_f64(f64_from_bits(0x7FF0000000000000ull)) == bits_of_f64(std::numeric_limits<double>::infinity()));
    check("f64_max_finite_matches_limits",
          bits_of_f64(f64_from_bits(0x7FEFFFFFFFFFFFFFull)) == bits_of_f64((std::numeric_limits<double>::max)()));
    check("f64_min_normal_matches_limits",
          bits_of_f64(f64_from_bits(0x0010000000000000ull)) == bits_of_f64((std::numeric_limits<double>::min)()));
    check("f64_min_subnormal_matches_limits",
          bits_of_f64(f64_from_bits(0x0000000000000001ull)) == bits_of_f64(std::numeric_limits<double>::denorm_min()));
    check("f64_lowest_matches_neg_max_finite",
          bits_of_f64(std::numeric_limits<double>::lowest()) == 0xFFEFFFFFFFFFFFFFull);
}

// ---------------------------------------------------------------------------
// 27. Per-width integer VALUE-pattern table: 0, all-ones, sign-bit-only,
//     INT/LONG min & max, alternating 0x55../0xAA.., 0x01/0x80, and a walking
//     single-bit across EVERY bit position of the width.  Each pattern must
//     round-trip bit-exact and land at the oracle offset.  Aggregated into one
//     pass/width so the (pattern x width) matrix is exhaustive without flooding
//     output.  Independent oracle: raw_peek at +16+i*sizeof(T).
// ---------------------------------------------------------------------------
template<typename T>
static auto exercise_value_patterns(const char* label_prefix) -> void
{
    static_assert(std::is_integral_v<T>, "value-pattern sweep is for integer widths");
    auto tag{ [&](const char* suffix)
    {
        static thread_local char storage[112];
        std::snprintf(storage, sizeof(storage), "%s_%s", label_prefix, suffix);
        return storage;
    } };

    using U = std::make_unsigned_t<T>;
    constexpr int width_bits{ static_cast<int>(sizeof(T) * 8u) };

    // Fixed canonical patterns (built in the unsigned domain, then bit-cast to T).
    std::vector<U> patterns{
        U{ 0 },
        static_cast<U>(~U{ 0 }),                                   // all ones
        static_cast<U>(U{ 1 } << (width_bits - 1)),               // sign bit only (== 0x80..)
        static_cast<U>((U{ 1 } << (width_bits - 1)) - U{ 1 }),    // max positive magnitude (0x7F..)
        U{ 1 },                                                   // low bit only (0x..01)
    };
    // Alternating 0x55.. and 0xAA.. spanning the full width.
    {
        U a{ 0 };
        U b{ 0 };
        for (int byte{ 0 }; byte < static_cast<int>(sizeof(T)); ++byte)
        {
            a = static_cast<U>(a | (static_cast<U>(0x55u) << (byte * 8)));
            b = static_cast<U>(b | (static_cast<U>(0xAAu) << (byte * 8)));
        }
        patterns.push_back(a);
        patterns.push_back(b);
    }
    // Walking single bit across every bit position.
    for (int bit{ 0 }; bit < width_bits; ++bit)
    {
        patterns.push_back(static_cast<U>(U{ 1 } << bit));
    }

    bool all_roundtrip{ true };
    bool all_at_oracle{ true };
    for (const U raw : patterns)
    {
        T value{};
        std::memcpy(&value, &raw, sizeof(T));   // bit-cast unsigned pattern -> T

        const std::vector<T> seed(3u, T{});
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };

        vmhook::set_array_element<T>(oop, 1, value);
        if (!bits_equal(vmhook::get_array_element<T>(oop, 1), value)) { all_roundtrip = false; break; }
        if (!bits_equal(raw_peek<T>(buffer, 1), value))               { all_at_oracle = false; break; }
        // Neighbours stay zero (stride exactness for this value).
        if (!bits_equal(vmhook::get_array_element<T>(oop, 0), T{}))   { all_roundtrip = false; break; }
        if (!bits_equal(vmhook::get_array_element<T>(oop, 2), T{}))   { all_roundtrip = false; break; }
    }
    check(tag("value_patterns_roundtrip_bit_exact"), all_roundtrip);
    check(tag("value_patterns_land_at_oracle"), all_at_oracle);

    // Explicit numeric_limits endpoints as a cross-checked independent oracle:
    // min() and max() of T must round-trip to exactly those values.
    {
        const std::vector<T> seed(2u, T{});
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        const T tmin{ (std::numeric_limits<T>::min)() };
        const T tmax{ (std::numeric_limits<T>::max)() };
        vmhook::set_array_element<T>(oop, 0, tmin);
        vmhook::set_array_element<T>(oop, 1, tmax);
        check(tag("limits_min_max_roundtrip"),
              vmhook::get_array_element<T>(oop, 0) == tmin
              && vmhook::get_array_element<T>(oop, 1) == tmax);
    }
}

static auto test_integer_value_patterns() -> void
{
    exercise_value_patterns<std::uint8_t>("vp_u8");
    exercise_value_patterns<std::int8_t>("vp_i8");
    exercise_value_patterns<std::uint16_t>("vp_u16");
    exercise_value_patterns<std::int16_t>("vp_i16");
    exercise_value_patterns<std::uint32_t>("vp_u32");
    exercise_value_patterns<std::int32_t>("vp_i32");
    exercise_value_patterns<std::uint64_t>("vp_u64");
    exercise_value_patterns<std::int64_t>("vp_i64");
}

// ---------------------------------------------------------------------------
// 28. EXHAUSTIVE 1-byte value space: ALL 256 byte values round-trip through the
//     1-byte element types.  This is literally "every possible input" for the
//     value dimension at a 1-byte stride.  For each value we set->get and also
//     confirm the raw backing byte equals the low 8 bits, and that the helper
//     write touched ONLY the target byte (the slack neighbour stays zero).
//     char's signedness is platform-defined, so we compare round-tripped values
//     of the SAME type (signedness-agnostic) and verify the stored byte equals
//     the value reinterpreted to its 8 raw bits.
// ---------------------------------------------------------------------------
template<typename T>
static auto exhaustive_byte_values(const char* label) -> void
{
    static_assert(sizeof(T) == 1, "exhaustive_byte_values is for 1-byte element types");
    // Two element slots so a write to index 0 can be checked against an
    // untouched index 1.
    const std::vector<T> seed{ T{}, T{} };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    bool ok{ true };
    for (int v{ 0 }; v <= 255 && ok; ++v)
    {
        const std::uint8_t raw_byte{ static_cast<std::uint8_t>(v) };
        T value{};
        std::memcpy(&value, &raw_byte, 1u);

        // reset the neighbour so we can detect any spill.
        vmhook::set_array_element<T>(oop, 1, T{});
        vmhook::set_array_element<T>(oop, 0, value);

        if (!bits_equal(vmhook::get_array_element<T>(oop, 0), value)) { ok = false; break; }
        // Raw backing byte at +16 equals the value's 8 bits.
        std::uint8_t stored{};
        std::memcpy(&stored, buffer.data() + 16, 1u);
        if (stored != raw_byte) { ok = false; break; }
        // Neighbour byte at +17 untouched (still zero).
        std::uint8_t neighbour{};
        std::memcpy(&neighbour, buffer.data() + 17, 1u);
        if (neighbour != 0u) { ok = false; break; }
    }
    check(label, ok);
}

static auto test_exhaustive_byte_values() -> void
{
    exhaustive_byte_values<std::uint8_t>("exhaustive_all_256_u8");
    exhaustive_byte_values<std::int8_t>("exhaustive_all_256_i8");
    exhaustive_byte_values<char>("exhaustive_all_256_char");
    exhaustive_byte_values<signed char>("exhaustive_all_256_schar");
    exhaustive_byte_values<unsigned char>("exhaustive_all_256_uchar");

    // bool: only the two canonical states are well-defined to observe; cover both.
    {
        const std::vector<std::uint8_t> seed{ 0u, 0u };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        vmhook::set_array_element<bool>(oop, 0, false);
        const bool got_false{ vmhook::get_array_element<bool>(oop, 0) == false };
        vmhook::set_array_element<bool>(oop, 0, true);
        const bool got_true{ vmhook::get_array_element<bool>(oop, 0) == true };
        check("exhaustive_bool_both_states", got_false && got_true);
    }
}

// ---------------------------------------------------------------------------
// 29. EXHAUSTIVE 2-byte value space: ALL 65536 values round-trip through the
//     2-byte element types (uint16/int16/char16_t).  Feasible and fast; this is
//     "every possible input" for the value dimension at a 2-byte stride.  Each
//     value is verified bit-exact via the helper AND against the raw little-/
//     big-endian-agnostic typed read at the oracle offset (we compare the
//     correctly-typed read, never raw byte order).
// ---------------------------------------------------------------------------
template<typename T>
static auto exhaustive_u16_values(const char* label) -> void
{
    static_assert(sizeof(T) == 2, "exhaustive_u16_values is for 2-byte element types");
    const std::vector<T> seed{ T{}, T{} };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    bool ok{ true };
    for (int v{ 0 }; v <= 0xFFFF && ok; ++v)
    {
        const std::uint16_t raw16{ static_cast<std::uint16_t>(v) };
        T value{};
        std::memcpy(&value, &raw16, 2u);

        vmhook::set_array_element<T>(oop, 1, T{});      // clear neighbour
        vmhook::set_array_element<T>(oop, 0, value);

        if (!bits_equal(vmhook::get_array_element<T>(oop, 0), value)) { ok = false; break; }
        // Typed oracle read at +16 (value-level, endianness-agnostic).
        if (!bits_equal(raw_peek<T>(buffer, 0), value)) { ok = false; break; }
        // Neighbour slot (index 1, +18) still zero — no 2-byte spill.
        if (!bits_equal(vmhook::get_array_element<T>(oop, 1), T{})) { ok = false; break; }
    }
    check(label, ok);
}

static auto test_exhaustive_u16_values() -> void
{
    exhaustive_u16_values<std::uint16_t>("exhaustive_all_65536_u16");
    exhaustive_u16_values<std::int16_t>("exhaustive_all_65536_i16");
    exhaustive_u16_values<char16_t>("exhaustive_all_65536_c16");
}

// ---------------------------------------------------------------------------
// 30. 4-byte and 8-byte value space: the full 2^32 / 2^64 spaces are too large
//     to enumerate, so cover the structurally-complete subset — every single-bit
//     pattern (32 / 64 of them), every "all bits below position k" run, the
//     alternating patterns, and the min/max/zero/all-ones endpoints — each
//     round-tripped bit-exact and verified at the oracle offset.  A stride or
//     truncation bug at these widths surfaces as a wrong byte in some bit.
// ---------------------------------------------------------------------------
template<typename UInt, typename T>
static auto wide_bit_patterns(const char* label) -> void
{
    static_assert(sizeof(UInt) == sizeof(T), "unsigned shadow must match element width");
    const std::vector<T> seed{ T{}, T{}, T{} };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    constexpr int width_bits{ static_cast<int>(sizeof(UInt) * 8u) };
    std::vector<UInt> patterns;
    patterns.push_back(UInt{ 0 });
    patterns.push_back(static_cast<UInt>(~UInt{ 0 }));
    for (int bit{ 0 }; bit < width_bits; ++bit)
    {
        patterns.push_back(static_cast<UInt>(UInt{ 1 } << bit));                 // single bit
        patterns.push_back(static_cast<UInt>((UInt{ 1 } << bit) - UInt{ 1 }));   // low run of 'bit' ones
    }
    // Alternating nibble patterns spanning the full width.
    {
        UInt a{ 0 };
        UInt b{ 0 };
        for (int byte{ 0 }; byte < static_cast<int>(sizeof(UInt)); ++byte)
        {
            a = static_cast<UInt>(a | (static_cast<UInt>(0x55u) << (byte * 8)));
            b = static_cast<UInt>(b | (static_cast<UInt>(0xAAu) << (byte * 8)));
        }
        patterns.push_back(a);
        patterns.push_back(b);
    }

    bool ok{ true };
    for (const UInt raw : patterns)
    {
        T value{};
        std::memcpy(&value, &raw, sizeof(T));
        vmhook::set_array_element<T>(oop, 1, value);
        if (!bits_equal(vmhook::get_array_element<T>(oop, 1), value)) { ok = false; break; }
        if (!bits_equal(raw_peek<T>(buffer, 1), value))               { ok = false; break; }
        // Both neighbours independent of this slot.
        if (!bits_equal(vmhook::get_array_element<T>(oop, 0), T{}))   { ok = false; break; }
        if (!bits_equal(vmhook::get_array_element<T>(oop, 2), T{}))   { ok = false; break; }
        // Restore neighbour-free state for the next pattern.
        vmhook::set_array_element<T>(oop, 1, T{});
    }
    check(label, ok);
}

static auto test_wide_bit_patterns() -> void
{
    wide_bit_patterns<std::uint32_t, std::uint32_t>("wide_bits_u32");
    wide_bit_patterns<std::uint32_t, std::int32_t>("wide_bits_i32");
    wide_bit_patterns<std::uint32_t, float>("wide_bits_f32");
    wide_bit_patterns<std::uint64_t, std::uint64_t>("wide_bits_u64");
    wide_bit_patterns<std::uint64_t, std::int64_t>("wide_bits_i64");
    wide_bit_patterns<std::uint64_t, double>("wide_bits_f64");
}

// ---------------------------------------------------------------------------
// 31. FULL (length x index) runtime truth table for lengths 0..8.
//
// Section 25 proves the half-open predicate at compile time and section 14 hits
// representative lengths.  This enumerates EVERY (length, index) cell at runtime
// for lengths 0..8 and indices -2..length+2, asserting the live helper agrees
// with the half-open oracle for BOTH read (in-range -> seeded value, else
// default) and write (in-range -> persists, else no-op), across a 1/4/8-byte
// width.  A single off-by-one in the bounds check fails some cell here.
// ---------------------------------------------------------------------------
template<typename T>
static auto length_index_truth_table(const char* label) -> void
{
    bool ok{ true };
    for (std::int32_t length{ 0 }; length <= 8 && ok; ++length)
    {
        // Seed each slot with a recognisable, distinct value per index.
        std::vector<T> seed(static_cast<std::size_t>(length), T{});
        for (std::int32_t i{ 0 }; i < length; ++i)
        {
            T v{};
            const std::uint64_t bits{ 0x0102030405060700ull + static_cast<std::uint64_t>(i) };
            std::memcpy(&v, &bits, sizeof(T));
            seed[static_cast<std::size_t>(i)] = v;
        }
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };

        if (vmhook::array_length(oop) != length) { ok = false; break; }

        for (std::int32_t idx{ -2 }; idx <= length + 2 && ok; ++idx)
        {
            const bool in_range{ layout_oracle::index_in_range(idx, length) };

            // READ: in-range -> the seeded value; out-of-range -> default.
            const T got{ vmhook::get_array_element<T>(oop, opaque_index(idx)) };
            if (in_range)
            {
                if (!bits_equal(got, seed[static_cast<std::size_t>(idx)])) { ok = false; break; }
            }
            else if (!bits_equal(got, T{})) { ok = false; break; }

            // WRITE: snapshot, write a probe, verify persist-iff-in-range.
            T probe{};
            const std::uint64_t pbits{ 0xF1F2F3F4F5F6F7F8ull };
            std::memcpy(&probe, &pbits, sizeof(T));
            const std::vector<std::uint8_t> before{ buffer };
            vmhook::set_array_element<T>(oop, opaque_index(idx), probe);
            if (in_range)
            {
                if (!bits_equal(vmhook::get_array_element<T>(oop, idx), probe)) { ok = false; break; }
                // Restore the seeded value so subsequent reads of this slot are
                // still predictable for the rest of the row.
                vmhook::set_array_element<T>(oop, idx, seed[static_cast<std::size_t>(idx)]);
            }
            else if (buffer != before) { ok = false; break; } // OOB write must be a no-op
        }
    }
    check(label, ok);
}

static auto test_length_index_truth_table() -> void
{
    length_index_truth_table<std::uint8_t>("truth_table_len0to8_u8");
    length_index_truth_table<std::int32_t>("truth_table_len0to8_i32");
    length_index_truth_table<std::int64_t>("truth_table_len0to8_i64");
}

// ---------------------------------------------------------------------------
// 32. Exhaustive INDEX bit-walk: every single-bit-set index (1,2,4,...,2^30)
//     plus the sign bit (INT_MIN) must be REJECTED on a small honest array, and
//     two's-complement "negative as a raw bit pattern" indices must also be
//     rejected.  No index bit pattern above the real length may slip past the
//     [0,length) guard.  We pick length 3 so every power-of-two index >= 4 is
//     OOB, indices 1 and 2 are in-range (positive control), and 0 is in-range.
// ---------------------------------------------------------------------------
static auto test_index_bit_walk() -> void
{
    const std::vector<std::int32_t> seed{ 0x11111111, 0x22222222, 0x33333333 };
    std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
    void* const oop{ buffer.data() };

    // Positive control: the three in-range indices read their seeded values.
    check("bitwalk_inrange_controls",
          vmhook::get_array_element<std::int32_t>(oop, 0) == 0x11111111
          && vmhook::get_array_element<std::int32_t>(oop, 1) == 0x22222222
          && vmhook::get_array_element<std::int32_t>(oop, 2) == 0x33333333);

    // Every single-bit index from 2^2 (==4) up through 2^30 is >= length(3),
    // so must be rejected for read (default) and write (no-op).
    bool all_high_bits_rejected{ true };
    const std::vector<std::uint8_t> before{ buffer };
    for (int bit{ 2 }; bit <= 30; ++bit)
    {
        const std::int32_t idx{ static_cast<std::int32_t>(1) << bit };
        if (vmhook::get_array_element<std::int32_t>(oop, opaque_index(idx)) != 0)
        {
            all_high_bits_rejected = false; break;
        }
        vmhook::set_array_element<std::int32_t>(oop, opaque_index(idx), 0x7E7E7E7E);
    }
    check("bitwalk_high_single_bit_indices_read_default", all_high_bits_rejected);
    check("bitwalk_high_single_bit_writes_are_noops", buffer == before);

    // The sign bit set (INT_MIN) and several "negative bit patterns" are < 0 and
    // rejected by the first guard clause.
    const std::int32_t negatives[]{
        (std::numeric_limits<std::int32_t>::min)(),   // 0x80000000
        static_cast<std::int32_t>(0xFFFFFFFFu),       // -1
        static_cast<std::int32_t>(0xC0000000u),       // large negative
        static_cast<std::int32_t>(0x80000001u),       // INT_MIN+1
    };
    bool all_neg_rejected{ true };
    const std::vector<std::uint8_t> before2{ buffer };
    for (const std::int32_t idx : negatives)
    {
        if (vmhook::get_array_element<std::int32_t>(oop, opaque_index(idx)) != 0)
        {
            all_neg_rejected = false; break;
        }
        vmhook::set_array_element<std::int32_t>(oop, opaque_index(idx), 0x5C5C5C5C);
    }
    check("bitwalk_negative_indices_read_default", all_neg_rejected);
    check("bitwalk_negative_writes_are_noops", buffer == before2);
}

// ---------------------------------------------------------------------------
// 33. array_length over a SWEEP of stored _length bit patterns, read back as a
//     signed int32 with no clamping (section 11 covers a few; this sweeps every
//     single-bit length value and the alternating patterns, cross-checked
//     against a from-first-principles signed reinterpretation).  Then confirm
//     the SIGN of the returned length governs element access: a length whose
//     stored pattern is negative rejects index 0; a small positive length admits
//     [0,length).  We never index past our real buffer for positive patterns.
// ---------------------------------------------------------------------------
static auto test_array_length_bit_sweep() -> void
{
    auto length_from_raw{ [](std::uint32_t raw) -> std::int32_t
    {
        std::vector<std::uint8_t> buf(16u, std::uint8_t{ 0 });
        std::memcpy(buf.data() + 12, &raw, sizeof(raw));
        return vmhook::array_length(buf.data());
    } };

    // Every single-bit pattern of the 32-bit length slot must read back as the
    // signed reinterpretation of that exact bit pattern (independent oracle:
    // static_cast<int32>(raw)).
    bool all_single_bit_ok{ true };
    for (int bit{ 0 }; bit < 32; ++bit)
    {
        const std::uint32_t raw{ static_cast<std::uint32_t>(1u) << bit };
        const std::int32_t expected{ static_cast<std::int32_t>(raw) };
        if (length_from_raw(raw) != expected) { all_single_bit_ok = false; break; }
    }
    check("length_every_single_bit_signed_readback", all_single_bit_ok);

    // Alternating and endpoint patterns, each vs the signed reinterpretation.
    const std::uint32_t raws[]{
        0x00000000u, 0xFFFFFFFFu, 0x55555555u, 0xAAAAAAAAu,
        0x7FFFFFFFu, 0x80000000u, 0x0000FFFFu, 0xFFFF0000u, 0x00FF00FFu };
    bool all_patterns_ok{ true };
    for (const std::uint32_t raw : raws)
    {
        if (length_from_raw(raw) != static_cast<std::int32_t>(raw)) { all_patterns_ok = false; break; }
    }
    check("length_alternating_patterns_signed_readback", all_patterns_ok);

    // Sign governs access: a negative stored length (0xAAAAAAAA -> large negative)
    // rejects index 0; a small positive length (e.g. 0x00000003 with a matching
    // real buffer) admits [0,3).
    {
        // Negative-length case: header-only buffer + tiny data slack.
        std::vector<std::uint8_t> buf(16u + sizeof(std::int32_t), std::uint8_t{ 0 });
        const std::uint32_t neg_raw{ 0xAAAAAAAAu };
        std::memcpy(buf.data() + 12, &neg_raw, sizeof(neg_raw));
        check("negative_pattern_length_rejects_index0",
              static_cast<std::int32_t>(neg_raw) < 0
              && vmhook::get_array_element<std::int32_t>(buf.data(), opaque_index(0)) == 0);
    }
    {
        // Small positive length, fully backed: [0,3) all read their seeds.
        const std::vector<std::int32_t> seed{ 0x0A0A0A0A, 0x0B0B0B0B, 0x0C0C0C0C };
        std::vector<std::uint8_t> buffer{ build_fake_array(seed) };
        void* const oop{ buffer.data() };
        check("small_positive_length_admits_full_range",
              vmhook::array_length(oop) == 3
              && vmhook::get_array_element<std::int32_t>(oop, 0) == 0x0A0A0A0A
              && vmhook::get_array_element<std::int32_t>(oop, 2) == 0x0C0C0C0C
              && vmhook::get_array_element<std::int32_t>(oop, opaque_index(3)) == 0);
    }
}

int main()
{
    test_all_widths();
    test_char_width();
    test_array_length_edges();
    test_element_null_guards();
    test_single_element_boundaries();
    test_last_index_access();
    test_extra_widths();
    test_negative_length_field();
    test_zero_length_element_access();
    test_extreme_index_sentinels();
    test_array_length_sign();
    test_element_size_strides();
    test_address_computation_sweep();
    test_full_boundary_set();
    test_oop_element_widths();
    test_index_scale_overflow();
    test_pointer_guard_matrix();
    test_mixed_width_aliasing();
    test_dense_nontrivial_indices();
    test_length_reads_only_offset12();
    test_clamp_safe_container_count();
    test_compile_time_arithmetic();
    test_float_special_values();
    test_integer_value_patterns();
    test_exhaustive_byte_values();
    test_exhaustive_u16_values();
    test_wide_bit_patterns();
    test_length_index_truth_table();
    test_index_bit_walk();
    test_array_length_bit_sweep();

    if (failures == 0)
    {
        std::printf("vmhook array element helpers: OK\n");
    }
    else
    {
        std::printf("vmhook array element helpers: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
