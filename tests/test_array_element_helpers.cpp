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

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
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
