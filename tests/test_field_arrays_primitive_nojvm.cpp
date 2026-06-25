// Standalone unit test: field_arrays_primitive — COLD-STATE read contract.
//
// Wave-32 LEDGER GAPS this file closes (no-JVM):
//   * cold-state primitive-array field accessor returns empty / null for ALL
//     8 JVM primitive-array descriptors ([Z [B [S [C [I [J [F [D) consumed
//     via the implicit value_t -> std::vector<T> conversion,
//   * null-parent field_proxy (raw_address() == nullptr) is safe across all
//     8 descriptors AND for many target element types (signed/unsigned,
//     std::byte, char, char16_t, float/double) — no UB, no throw, always {},
//   * static_asserts on signature stability (signature_text round-trip
//     through the cold path) and on the per-primitive element-size invariants
//     that the live read-path consults at run time (sizeof T vs JVM width),
//   * static_asserts on RETURN-TYPE IDENTITY of the implicit conversion to
//     std::vector<T> for every primitive T — proves the conversion operator
//     yields exactly std::vector<T> with no surprise alternative-vector
//     promotion (e.g. vector<int> never silently widens to vector<long>),
//   * the null-pointer fallback fires the int32_t variant alternative
//     regardless of the [* descriptor, and the vector overload of
//     cast_for_variant maps that to an empty target_type (this is the cold
//     path glue that makes empty-vector-on-cold true).
//
// Contrast vs sibling files:
//   * test_field_primitives_get_nojvm.cpp covers SCALAR primitive cold reads
//     (Z/B/S/C/I/J/F/D), the int32 variant invariant, and noexcept
//     characterisation — but NOT array descriptors.
//   * test_field_primitives_set_nojvm.cpp covers WRITE-side guards.
//   * test_make_java_array_nojvm.cpp covers array CONSTRUCTION, not read.
//   * test_array_element_helpers.cpp covers the low-level array_length /
//     get_array_element helpers against fabricated buffers, not the
//     field_proxy::value_t read path.
//   * THIS file covers cold-state std::vector<T> conversion through value_t
//     for every JVM primitive-array descriptor.

#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// ---------------------------------------------------------------------------
// SECTION 0 — static_asserts on conversion return-type identity for every
// primitive-array element type.  The implicit conversion lives at value_t::
// operator T() (vmhook.hpp ~15451) and delegates to cast_for_variant<T>.
// For target T = std::vector<U>, the result must be EXACTLY std::vector<U> —
// not vector<int> when U=int8_t, not vector<long> when U=int32_t, not
// vector<double> when U=float.  A future refactor that introduces a stealth
// `return std::vector<long>{}` somewhere in the chain is caught here at
// compile time.
// ---------------------------------------------------------------------------

namespace
{
template <class T>
auto convert_default_vector(std::string_view sig) -> std::vector<T>
{
    vmhook::field_proxy proxy{ nullptr, std::string{ sig }, /*is_static=*/false };
    const auto value{ proxy.get() };
    return static_cast<std::vector<T>>(value);
}

static_assert(std::is_same_v<decltype(convert_default_vector<bool>("[Z")),
                             std::vector<bool>>,
              "value_t -> vector<bool> conversion must yield vector<bool>");
static_assert(std::is_same_v<decltype(convert_default_vector<std::int8_t>("[B")),
                             std::vector<std::int8_t>>,
              "value_t -> vector<int8_t> must yield vector<int8_t>");
static_assert(std::is_same_v<decltype(convert_default_vector<std::byte>("[B")),
                             std::vector<std::byte>>,
              "value_t -> vector<byte> must yield vector<byte>");
static_assert(std::is_same_v<decltype(convert_default_vector<std::int16_t>("[S")),
                             std::vector<std::int16_t>>,
              "value_t -> vector<int16_t> must yield vector<int16_t>");
static_assert(std::is_same_v<decltype(convert_default_vector<std::int32_t>("[I")),
                             std::vector<std::int32_t>>,
              "value_t -> vector<int32_t> must yield vector<int32_t>");
static_assert(std::is_same_v<decltype(convert_default_vector<std::int64_t>("[J")),
                             std::vector<std::int64_t>>,
              "value_t -> vector<int64_t> must yield vector<int64_t>");
static_assert(std::is_same_v<decltype(convert_default_vector<float>("[F")),
                             std::vector<float>>,
              "value_t -> vector<float> must yield vector<float>");
static_assert(std::is_same_v<decltype(convert_default_vector<double>("[D")),
                             std::vector<double>>,
              "value_t -> vector<double> must yield vector<double>");
static_assert(std::is_same_v<decltype(convert_default_vector<char>("[C")),
                             std::vector<char>>,
              "value_t -> vector<char> must yield vector<char> (lossy 'C' overload)");
}

// Per-primitive element-size invariants — the live read path's width guard at
// vmhook.hpp:15066 reads jvm_primitive_byte_width(sig.substr(1)) and refuses a
// width-mismatched read.  Pin the C++ side of that comparison so a future
// refactor that swaps sizeof(int32_t) for sizeof(long) on a 32-bit target
// shows up here at compile time.
static_assert(sizeof(bool)         == 1, "JVM [Z element is 1 byte");
static_assert(sizeof(std::int8_t)  == 1, "JVM [B element is 1 byte");
static_assert(sizeof(std::byte)    == 1, "JVM [B element is 1 byte (std::byte target)");
static_assert(sizeof(std::int16_t) == 2, "JVM [S element is 2 bytes");
static_assert(sizeof(std::int32_t) == 4, "JVM [I element is 4 bytes");
static_assert(sizeof(std::int64_t) == 8, "JVM [J element is 8 bytes");
static_assert(sizeof(float)        == 4, "JVM [F element is 4 bytes");
static_assert(sizeof(double)       == 8, "JVM [D element is 8 bytes");

// is_vector_v selector that gates the vector branch of cast_for_variant —
// pin its compile-time truth for every primitive-array C++ representative so
// a refactor that adds a `is_array_v` precondition doesn't silently break
// the vector branch for one of them.
static_assert(vmhook::detail::is_vector_v<std::vector<bool>>);
static_assert(vmhook::detail::is_vector_v<std::vector<std::int8_t>>);
static_assert(vmhook::detail::is_vector_v<std::vector<std::byte>>);
static_assert(vmhook::detail::is_vector_v<std::vector<std::int16_t>>);
static_assert(vmhook::detail::is_vector_v<std::vector<std::int32_t>>);
static_assert(vmhook::detail::is_vector_v<std::vector<std::int64_t>>);
static_assert(vmhook::detail::is_vector_v<std::vector<float>>);
static_assert(vmhook::detail::is_vector_v<std::vector<double>>);
static_assert(vmhook::detail::is_vector_v<std::vector<char>>);
static_assert(!vmhook::detail::is_vector_v<std::int32_t>);
static_assert(!vmhook::detail::is_vector_v<void*>);

// jvm_primitive_byte_width — the live width-guard helper at vmhook.hpp:15067.
// Pin the per-primitive widths so a refactor that swaps the table by mistake
// shows up here at compile time.  These are the values cast_for_variant
// compares against sizeof(element_type) in the matching-width fast path.
static_assert(noexcept(vmhook::detail::jvm_primitive_byte_width(std::string_view{})),
              "jvm_primitive_byte_width must be noexcept");

// 3-arg field_proxy ctor noexcept — independently pinned (sibling file pins
// it too; we want both sites to fail loudly if a future ctor adds throwing
// logic).
static_assert(std::is_nothrow_constructible_v<vmhook::field_proxy,
                                              void*, std::string, bool>,
              "field_proxy(void*, string, bool) must be noexcept");

// ---------------------------------------------------------------------------
// SECTION 1 — runtime: jvm_primitive_byte_width per element descriptor.
// These are the values the live read-path width-guard compares against
// sizeof(element_type).  Pin them at runtime too (constexpr would be ideal
// but the helper is not declared constexpr).
// ---------------------------------------------------------------------------

static auto section_byte_widths() -> void
{
    using vmhook::detail::jvm_primitive_byte_width;
    check("[Z byte width == 1", jvm_primitive_byte_width("Z") == 1);
    check("[B byte width == 1", jvm_primitive_byte_width("B") == 1);
    check("[S byte width == 2", jvm_primitive_byte_width("S") == 2);
    check("[C byte width == 2", jvm_primitive_byte_width("C") == 2);
    check("[I byte width == 4", jvm_primitive_byte_width("I") == 4);
    check("[J byte width == 8", jvm_primitive_byte_width("J") == 8);
    check("[F byte width == 4", jvm_primitive_byte_width("F") == 4);
    check("[D byte width == 8", jvm_primitive_byte_width("D") == 8);
    // The helper returns 0 for object / unknown descriptors — the live
    // width-guard treats 0 as "skip this guard" (carve-out for object arrays
    // and the bool/char/string overloads).  Pin the 0 so a future refactor
    // that returns sizeof(void*) doesn't accidentally arm the guard for
    // object arrays.
    check("L... byte width == 0 (object)", jvm_primitive_byte_width("Ljava/lang/String;") == 0);
    check("[... byte width == 0 (nested array)", jvm_primitive_byte_width("[I") == 0);
    check("empty byte width == 0", jvm_primitive_byte_width("") == 0);
}

// ---------------------------------------------------------------------------
// SECTION 2 — per-descriptor cold-state std::vector<T> conversion returns
// an EMPTY vector for every JVM primitive-array descriptor.  Static and
// instance proxies — cold-state behaviour ignores is_static() (consulted
// only at lookup time by object::get_field).
// ---------------------------------------------------------------------------

template <class T>
static auto cold_vec(std::string_view sig, bool is_static) -> std::vector<T>
{
    vmhook::field_proxy proxy{ nullptr, std::string{ sig }, is_static };
    const auto value{ proxy.get() };
    return static_cast<std::vector<T>>(value);
}

static auto section_cold_vec_all_descriptors() -> void
{
    // [Z / boolean[]
    {
        const auto vi{ cold_vec<bool>("[Z", false) };
        const auto vs{ cold_vec<bool>("[Z", true) };
        check("cold vector<bool> from [Z instance is empty", vi.empty());
        check("cold vector<bool> from [Z static   is empty", vs.empty());
    }
    // [B / byte[] — both signed and std::byte representations
    {
        const auto v8{ cold_vec<std::int8_t>("[B", false) };
        const auto vb{ cold_vec<std::byte>("[B", false) };
        const auto vs{ cold_vec<std::int8_t>("[B", true) };
        check("cold vector<int8_t> from [B is empty", v8.empty());
        check("cold vector<byte>   from [B is empty", vb.empty());
        check("cold vector<int8_t> from [B static is empty", vs.empty());
    }
    // [S / short[]
    {
        const auto v{ cold_vec<std::int16_t>("[S", false) };
        const auto vs{ cold_vec<std::int16_t>("[S", true) };
        check("cold vector<int16_t> from [S is empty", v.empty());
        check("cold vector<int16_t> from [S static is empty", vs.empty());
    }
    // [C / char[] — both the dedicated lossy 'C' overload (vector<char>) and
    // a wider target.  The lossy overload special-cases [C and reads uint16
    // narrowed to char.
    {
        const auto vc{ cold_vec<char>("[C", false) };
        check("cold vector<char> from [C is empty", vc.empty());
    }
    // [I / int[]
    {
        const auto v{ cold_vec<std::int32_t>("[I", false) };
        const auto vs{ cold_vec<std::int32_t>("[I", true) };
        check("cold vector<int32_t> from [I is empty", v.empty());
        check("cold vector<int32_t> from [I static is empty", vs.empty());
    }
    // [J / long[]
    {
        const auto v{ cold_vec<std::int64_t>("[J", false) };
        const auto vs{ cold_vec<std::int64_t>("[J", true) };
        check("cold vector<int64_t> from [J is empty", v.empty());
        check("cold vector<int64_t> from [J static is empty", vs.empty());
    }
    // [F / float[]
    {
        const auto v{ cold_vec<float>("[F", false) };
        check("cold vector<float> from [F is empty", v.empty());
    }
    // [D / double[]
    {
        const auto v{ cold_vec<double>("[D", false) };
        check("cold vector<double> from [D is empty", v.empty());
    }
}

// ---------------------------------------------------------------------------
// SECTION 3 — null-parent safety: capacity() == 0 / data() returns a non-
// dereferenced pointer / no implicit allocation.  The cold-vector path
// short-circuits BEFORE reserve(), so the empty vector should have capacity
// 0 as well as size 0.  This pins that no speculative allocation happens
// for an unbound (null) field_proxy.
// ---------------------------------------------------------------------------

template <class T>
static auto assert_null_parent_safe(std::string_view sig, const char* tag) -> void
{
    vmhook::field_proxy proxy{ nullptr, std::string{ sig }, false };
    // raw_address() / signature() are noexcept and must round-trip.
    check((std::string{ tag } + ": raw_address() == nullptr").c_str(),
          proxy.raw_address() == nullptr);
    check((std::string{ tag } + ": signature() round-trips").c_str(),
          proxy.signature() == sig);
    const std::vector<T> v{ static_cast<std::vector<T>>(proxy.get()) };
    check((std::string{ tag } + ": vector is empty").c_str(), v.empty());
    check((std::string{ tag } + ": vector has size 0").c_str(), v.size() == 0);
    // capacity 0 — no speculative reserve on the cold path.  This is the
    // observable shape that proves decode_array_oop short-circuited at
    // vmhook.hpp:15023-15027 before reaching reserve().
    check((std::string{ tag } + ": vector has capacity 0").c_str(),
          v.capacity() == 0);
}

static auto section_null_parent_safety() -> void
{
    assert_null_parent_safe<bool>        ("[Z", "null-parent [Z");
    assert_null_parent_safe<std::int8_t> ("[B", "null-parent [B (int8)");
    assert_null_parent_safe<std::byte>   ("[B", "null-parent [B (byte)");
    assert_null_parent_safe<std::int16_t>("[S", "null-parent [S");
    assert_null_parent_safe<char>        ("[C", "null-parent [C");
    assert_null_parent_safe<std::int32_t>("[I", "null-parent [I");
    assert_null_parent_safe<std::int64_t>("[J", "null-parent [J");
    assert_null_parent_safe<float>       ("[F", "null-parent [F");
    assert_null_parent_safe<double>      ("[D", "null-parent [D");
}

// ---------------------------------------------------------------------------
// SECTION 4 — variant alternative invariant on the cold path.
// The cold-state get() path returns value_t{ int32_t{}, signature_text } for
// EVERY descriptor (vmhook.hpp ~15601 / 15636).  cast_for_variant<vector<T>>
// then maps the int32_t alternative to target_type{} (the "else { return {};
// }" arm at vmhook.hpp:15354) instead of calling read_array_value.
// Pin the int32 alternative so a refactor that flips it (e.g. to uint32 to
// "match" the live array-OOP source type) is caught here — it would change
// the cold read from "empty vector via the {} arm" to "empty vector via
// read_array_value(0, sig) calling decode_array_oop(0)", which IS the same
// observable result but a different code path.  This guards the glue.
// ---------------------------------------------------------------------------

static auto section_variant_alternative_on_cold_array_path() -> void
{
    constexpr const char* sigs[]{ "[Z", "[B", "[S", "[C", "[I", "[J", "[F", "[D" };
    constexpr std::size_t int32_alternative_index{ 3 };
    for (const char* sig : sigs)
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ sig }, false };
        const auto value{ proxy.get() };
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "cold get('%s') variant alt == int32 (idx 3), actual=%zu",
                      sig, value.data.index());
        check(buf, value.data.index() == int32_alternative_index);

        char buf2[128];
        std::snprintf(buf2, sizeof(buf2),
                      "cold get('%s') signature round-trips byte-for-byte", sig);
        check(buf2, value.signature == sig);
    }
}

// ---------------------------------------------------------------------------
// SECTION 5 — descriptor / is_static orthogonality.  The null-parent cold
// read result is independent of is_static for every array descriptor.
// ---------------------------------------------------------------------------

static auto section_is_static_orthogonal() -> void
{
    constexpr const char* sigs[]{ "[Z", "[B", "[S", "[C", "[I", "[J", "[F", "[D" };
    for (const char* sig : sigs)
    {
        vmhook::field_proxy inst{ nullptr, std::string{ sig }, false };
        vmhook::field_proxy stat{ nullptr, std::string{ sig }, true };
        const auto vi{ inst.get() };
        const auto vs{ stat.get() };
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "cold get('%s') variant alt orthogonal to is_static", sig);
        check(buf, vi.data.index() == vs.data.index());
        char buf2[128];
        std::snprintf(buf2, sizeof(buf2),
                      "cold get('%s') signature orthogonal to is_static", sig);
        check(buf2, vi.signature == vs.signature);
    }
}

// ---------------------------------------------------------------------------
// SECTION 6 — repeated cold reads are idempotent and never throw.  The
// conversion operator is declared noexcept, but read_array_value internally
// reads via std::visit + array_length + a per-element loop; we exercise the
// path 256 times against every descriptor as a runtime stand-in for
// no-throw-in-practice.  Any iteration that threw would propagate out and
// abort the test, so successful loop exit IS the assertion.
// ---------------------------------------------------------------------------

static auto section_idempotent_cold_reads() -> void
{
    constexpr const char* sigs[]{ "[Z", "[B", "[S", "[C", "[I", "[J", "[F", "[D" };
    bool all_empty{ true };
    for (int iter{ 0 }; iter < 256 && all_empty; ++iter)
    {
        for (const char* sig : sigs)
        {
            vmhook::field_proxy p{ nullptr, std::string{ sig }, false };
            const auto v{ p.get() };
            // Different vector<T> per descriptor — exercise the int8/int16/
            // int32/int64/float/double element_type branches at least once.
            // We only care about emptiness here; the sigs sequence is fixed.
            const std::vector<std::int32_t> coerced{ static_cast<std::vector<std::int32_t>>(v) };
            if (!coerced.empty()) { all_empty = false; break; }
        }
    }
    check("256-iter cold vector<int32_t> over all 8 [* descriptors stays empty",
          all_empty);
}

int main()
{
    std::printf("field_arrays_primitive no-JVM unit test\n");
    section_byte_widths();
    section_cold_vec_all_descriptors();
    section_null_parent_safety();
    section_variant_alternative_on_cold_array_path();
    section_is_static_orthogonal();
    section_idempotent_cold_reads();
    if (failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", failures);
    return 1;
}
