// Tests for self-contained helpers that don't require a live JVM:
//   * VMHOOK_VERSION / VMHOOK_VERSION_STRING macros
//   * vmhook::hotspot::klass::decode_u5  (UNSIGNED5 decoder used for JDK 21+ FieldInfoStream)
//   * vmhook::hotspot::is_valid_pointer  (canonical-address & sentinel filter)
//   * vmhook::hotspot::untag_pointer     (GC-tag-bit strip)
//   * vmhook::detail::sig_char_to_basic_type  (JVM type descriptor -> HotSpot BasicType)
//   * vmhook::os::to_native_protect      (memory_protection -> native flags roundtrip)
//   * vmhook::os::detail_dr::build_dr7   (Windows + x86_64 only)
//   * vmhook::array_length / get_array_element / set_array_element on a fake buffer
//
// All cases run without a JVM in-process.

#include <vmhook/vmhook.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <vector>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

// ---------------------------------------------------------------------------
// 1. Version macros
// ---------------------------------------------------------------------------
static auto test_version_macros() -> void
{
    // VMHOOK_MAKE_VERSION should pack semver in the documented way.
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 0) == 0,
                  "VMHOOK_MAKE_VERSION(0,0,0) should be 0");
    static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) == 1000000,
                  "VMHOOK_MAKE_VERSION(1,0,0) should be 1_000_000");
    static_assert(VMHOOK_MAKE_VERSION(0, 4, 0) == 4000,
                  "VMHOOK_MAKE_VERSION(0,4,0) should be 4000");
    static_assert(VMHOOK_MAKE_VERSION(0, 4, 1) == 4001,
                  "VMHOOK_MAKE_VERSION(0,4,1) should be 4001");

    static_assert(VMHOOK_VERSION == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                                       VMHOOK_VERSION_MINOR,
                                                       VMHOOK_VERSION_PATCH),
                  "VMHOOK_VERSION should be the packed form of its parts");

    // VMHOOK_VERSION_STRING must compose as "MAJOR.MINOR.PATCH" with no
    // accidental whitespace or stringification artefacts.
    constexpr std::string_view version_text{ VMHOOK_VERSION_STRING };
    bool has_three_dot_components{ false };
    {
        std::size_t dots{ 0 };
        for (const char c : version_text)
        {
            if (c == '.')
            {
                ++dots;
            }
        }
        has_three_dot_components = (dots == 2);
    }
    check("version_string_has_two_dots", has_three_dot_components);
    check("version_string_starts_with_major",
          version_text.size() >= 3 && version_text[0] >= '0' && version_text[0] <= '9');

    // VMHOOK_VERSION must round-trip through MAKE_VERSION.
    constexpr int reconstructed{ VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                                    VMHOOK_VERSION_MINOR,
                                                    VMHOOK_VERSION_PATCH) };
    check("version_roundtrip", reconstructed == VMHOOK_VERSION);

    // Compare against the CMake-defined project version when available.
    // CMakeLists.txt should populate these matching the header.
#if defined(VMHOOK_CMAKE_VERSION_MAJOR) && defined(VMHOOK_CMAKE_VERSION_MINOR) \
    && defined(VMHOOK_CMAKE_VERSION_PATCH)
    check("cmake_version_matches_header_major",
          VMHOOK_CMAKE_VERSION_MAJOR == VMHOOK_VERSION_MAJOR);
    check("cmake_version_matches_header_minor",
          VMHOOK_CMAKE_VERSION_MINOR == VMHOOK_VERSION_MINOR);
    check("cmake_version_matches_header_patch",
          VMHOOK_CMAKE_VERSION_PATCH == VMHOOK_VERSION_PATCH);
#endif
}

// ---------------------------------------------------------------------------
// 2. decode_u5 (HotSpot UNSIGNED5 decoder)
// ---------------------------------------------------------------------------
static auto decode_one(std::initializer_list<std::uint8_t> bytes) -> std::uint32_t
{
    // Copy into a heap-allocated buffer so the function reads past the literal
    // safely (it never reads more than 5 bytes from the decoded stream).
    std::array<std::uint8_t, 16> buffer{};
    std::size_t i{ 0 };
    for (auto b : bytes)
    {
        buffer[i++] = b;
    }
    int pos{ 0 };
    return vmhook::hotspot::klass::decode_u5(buffer.data(), pos);
}

static auto decode_with_pos(std::initializer_list<std::uint8_t> bytes, int& pos_out)
    -> std::uint32_t
{
    std::array<std::uint8_t, 16> buffer{};
    std::size_t i{ 0 };
    for (auto b : bytes)
    {
        buffer[i++] = b;
    }
    return vmhook::hotspot::klass::decode_u5(buffer.data(), pos_out);
}

static auto test_decode_u5() -> void
{
    // Reference encodings from HotSpot UNSIGNED5 (src/hotspot/share/utilities/unsigned5.hpp)
    //   - X = 1 ("excluded" byte 0 -> end marker)
    //   - L = 191 (count of "low" / terminator bytes)
    //   - H = 64  (count of "high" / continuation bytes)
    //   value = sum_i (b_i - X) * 64^i, stopping at first b_i in [1, 191].
    //   Encoder writes b_0 = X+value when value < L, else writes X+L+(remainder mod H)
    //   for continuation bytes and a final low byte for the remainder/L.

    // 1-byte encodings: value < L (=191) maps to byte (value + 1).
    check("decode_u5_zero",  decode_one({ 1 })   == 0u);
    check("decode_u5_one",   decode_one({ 2 })   == 1u);
    check("decode_u5_64",    decode_one({ 65 })  == 64u);
    check("decode_u5_190",   decode_one({ 191 }) == 190u);

    // 2-byte encodings begin at value 191.
    //   191 = (192 - 1) * 1 + (1 - 1) * 64   =>  [192, 1]
    //   255 = (192 - 1) * 1 + (2 - 1) * 64   =>  [192, 2]
    //   4096 = (193 - 1) * 1 + (62 - 1) * 64 =>  [193, 62]
    check("decode_u5_191",   decode_one({ 192, 1 })   == 191u);
    check("decode_u5_255",   decode_one({ 192, 2 })   == 255u);
    check("decode_u5_4096",  decode_one({ 193, 62 })  == 4096u);

    // End-of-stream marker: byte 0 returns ~0u and rewinds stream_pos.
    {
        int pos{ 0 };
        const std::uint32_t result{ decode_with_pos({ 0 }, pos) };
        check("decode_u5_end_marker_returns_ones", result == ~0u);
        check("decode_u5_end_marker_rewinds_pos",  pos == 0);
    }

    // stream_pos advances by the consumed byte count.
    {
        int pos{ 0 };
        (void)decode_with_pos({ 65 }, pos);
        check("decode_u5_advance_one", pos == 1);
    }
    {
        int pos{ 0 };
        (void)decode_with_pos({ 192, 1 }, pos);
        check("decode_u5_advance_two", pos == 2);
    }

    // Decoding two consecutive values continues from the current stream_pos.
    {
        std::array<std::uint8_t, 4> stream{ 65, 192, 1, 3 }; // 64, 191, 2
        int pos{ 0 };
        const auto v0{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const auto v1{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const auto v2{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("decode_u5_sequence_v0", v0 == 64u);
        check("decode_u5_sequence_v1", v1 == 191u);
        check("decode_u5_sequence_v2", v2 == 2u);
        check("decode_u5_sequence_pos", pos == 4);
    }
}

// ---------------------------------------------------------------------------
// 3. is_valid_pointer & untag_pointer
// ---------------------------------------------------------------------------
static auto test_valid_pointer_filters() -> void
{
    // nullptr and small sentinels are rejected.
    check("is_valid_pointer_null", !vmhook::hotspot::is_valid_pointer(nullptr));
    check("is_valid_pointer_small_sentinel",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<void*>(0x100ull)));
    check("is_valid_pointer_floor",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<void*>(
              vmhook::os::user_address_floor)));

    // A canonical heap address (anything above the floor sentinel, below the
    // ceiling) is accepted.  Use a stack-local; its address is always canonical.
    int   stack_local{ 0 };
    void* canonical{ &stack_local };
    check("is_valid_pointer_stack_local", vmhook::hotspot::is_valid_pointer(canonical));

    // Kernel / non-canonical pointers are rejected.
    check("is_valid_pointer_kernel",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<void*>(
              0xFFFFFFFFFFFFFFFFull)));
}

static auto test_untag_pointer() -> void
{
    // Stripping tag bits from a clean canonical address must leave it untouched.
    int   stack_local{ 0 };
    void* canonical{ &stack_local };
    check("untag_pointer_preserves_canonical",
          vmhook::hotspot::untag_pointer(canonical) == canonical);

    // Setting a high "GC tag" bit (e.g. bit 60) and stripping should recover
    // the original canonical value.
    const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(canonical) };
    const std::uintptr_t tagged_addr{ base_addr | (std::uintptr_t{ 1 } << 60) };
    const void* const stripped{
        vmhook::hotspot::untag_pointer(reinterpret_cast<void*>(tagged_addr)) };
    check("untag_pointer_strips_high_bit",
          reinterpret_cast<std::uintptr_t>(stripped) == base_addr);

    // nullptr round-trips to nullptr.
    check("untag_pointer_null",
          vmhook::hotspot::untag_pointer(nullptr) == nullptr);
}

// ---------------------------------------------------------------------------
// 4. sig_char_to_basic_type — JVM type descriptor -> HotSpot BasicType integer
// ---------------------------------------------------------------------------
static auto test_sig_char_to_basic_type() -> void
{
    // Values from HotSpot's BasicType enum (src/hotspot/share/utilities/globalDefinitions.hpp).
    check("sig_char_Z_T_BOOLEAN", vmhook::detail::sig_char_to_basic_type('Z') == 4);
    check("sig_char_C_T_CHAR",    vmhook::detail::sig_char_to_basic_type('C') == 5);
    check("sig_char_F_T_FLOAT",   vmhook::detail::sig_char_to_basic_type('F') == 6);
    check("sig_char_D_T_DOUBLE",  vmhook::detail::sig_char_to_basic_type('D') == 7);
    check("sig_char_B_T_BYTE",    vmhook::detail::sig_char_to_basic_type('B') == 8);
    check("sig_char_S_T_SHORT",   vmhook::detail::sig_char_to_basic_type('S') == 9);
    check("sig_char_I_T_INT",     vmhook::detail::sig_char_to_basic_type('I') == 10);
    check("sig_char_J_T_LONG",    vmhook::detail::sig_char_to_basic_type('J') == 11);
    check("sig_char_L_T_OBJECT",  vmhook::detail::sig_char_to_basic_type('L') == 12);
    check("sig_char_array_T_ARRAY", vmhook::detail::sig_char_to_basic_type('[') == 13);
    check("sig_char_V_T_VOID",    vmhook::detail::sig_char_to_basic_type('V') == 14);

    // Unknown characters default to T_OBJECT (12) as a safe fallback.
    check("sig_char_unknown_defaults_to_T_OBJECT",
          vmhook::detail::sig_char_to_basic_type('?') == 12);
    check("sig_char_lower_i_defaults_to_T_OBJECT",
          vmhook::detail::sig_char_to_basic_type('i') == 12);
}

// ---------------------------------------------------------------------------
// 5. to_native_protect — every enum value maps to a distinct non-zero flag set
// ---------------------------------------------------------------------------
static auto test_to_native_protect() -> void
{
    using namespace vmhook::os;

    // no_access must map to PROT_NONE / PAGE_NOACCESS = 0 on POSIX,
    // PAGE_NOACCESS (= 0x01) on Windows.  Each platform's "no access"
    // value is the default fallback, so we just check that the function
    // returns it consistently.
    const auto noaccess{ to_native_protect(memory_protection::no_access) };
    const auto read{ to_native_protect(memory_protection::read) };
    const auto read_write{ to_native_protect(memory_protection::read_write) };
    const auto execute_read{ to_native_protect(memory_protection::execute_read) };
    const auto execute_rw{ to_native_protect(memory_protection::execute_rw) };

    // The four "real" protections must be mutually distinct, otherwise the
    // protect() function silently treats two different intents the same way.
    check("to_native_protect_read_vs_no_access", read != noaccess);
    check("to_native_protect_rw_vs_read", read_write != read);
    check("to_native_protect_exec_read_vs_read", execute_read != read);
    check("to_native_protect_exec_rw_vs_rw", execute_rw != read_write);
    check("to_native_protect_exec_rw_vs_exec_read", execute_rw != execute_read);

    // A garbage enum value (cast from an unknown int) must fall back to
    // no_access rather than returning an uninitialised DWORD.  Catches
    // the case where someone added a new enum entry without updating the
    // switch and forgot the default branch.
    const auto bogus{ to_native_protect(static_cast<memory_protection>(255)) };
    check("to_native_protect_unknown_falls_back_to_no_access", bogus == noaccess);
}

// ---------------------------------------------------------------------------
// 6. build_dr7 — DR7 control-mask construction (Windows + x86_64 only)
// ---------------------------------------------------------------------------
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
static auto test_build_dr7() -> void
{
    using namespace vmhook::os;
    using namespace vmhook::os::detail_dr;

    // Slot 0, write-only, 4-byte window:
    //   L0 = bit 0 -> 0x1
    //   R/W0 (bits 16-17) = 01 -> 0x10000
    //   LEN0 (bits 18-19) = 11 -> 0xC0000
    //   total = 0xD0001
    check("build_dr7_slot0_write_4bytes",
          build_dr7(0, data_breakpoint_kind::write,
                    data_breakpoint_length::four_bytes) == 0xD0001ull);

    // Slot 1, read/write, 8-byte window:
    //   L1 = bit 2 -> 0x4
    //   R/W1 (bits 20-21) = 11 -> 0x300000
    //   LEN1 (bits 22-23) = 10 -> 0x800000
    //   total = 0xB00004
    check("build_dr7_slot1_rw_8bytes",
          build_dr7(1, data_breakpoint_kind::read_write,
                    data_breakpoint_length::eight_bytes) == 0xB00004ull);

    // Slot 3, write-only, 1-byte window:
    //   L3 = bit 6 -> 0x40
    //   R/W3 (bits 28-29) = 01 -> 0x10000000
    //   LEN3 (bits 30-31) = 00 -> 0x0
    //   total = 0x10000040
    check("build_dr7_slot3_write_1byte",
          build_dr7(3, data_breakpoint_kind::write,
                    data_breakpoint_length::one_byte) == 0x10000040ull);

    // The local-enable bit must always land at the documented position;
    // a typo in the slot * 2 shift would make this fail.
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        const std::uint64_t dr7{
            build_dr7(slot, data_breakpoint_kind::write,
                      data_breakpoint_length::one_byte) };
        const std::uint64_t expected_local{ std::uint64_t{ 1 } << (slot * 2) };
        char tag[64];
        std::snprintf(tag, sizeof(tag), "build_dr7_local_enable_slot%d", slot);
        check(tag, (dr7 & expected_local) == expected_local);
    }
}
#endif

// ---------------------------------------------------------------------------
// 7. Array helpers — array_length / get_array_element / set_array_element
//    Exercises real header code against a fake heap-style buffer.
// ---------------------------------------------------------------------------
static auto test_array_helpers() -> void
{
    // Layout (HotSpot, compressed OOPs, x64):
    //   +0  mark word (8 B)
    //   +8  klass narrow ptr (4 B)
    //   +12 _length        (int)
    //   +16 _data[0]
    //
    // Buffer is heap-allocated so GCC's -Warray-bounds does not constant-fold
    // the indices the OOB tests pass into get/set_array_element; we want to
    // verify the runtime guard, not satisfy the static analyser.

    constexpr std::int32_t expected_length{ 5 };
    const std::size_t buffer_bytes{ 16u + 5u * sizeof(std::int32_t) };

    std::vector<std::uint8_t> buffer(buffer_bytes, std::uint8_t{ 0 });
    std::memcpy(buffer.data() + 12, &expected_length, sizeof(expected_length));

    const std::int32_t values[5]{ 100, 200, 300, 400, 500 };
    for (std::int32_t i{ 0 }; i < 5; ++i)
    {
        std::memcpy(buffer.data() + 16 + static_cast<std::size_t>(i) * sizeof(std::int32_t),
                    &values[i], sizeof(std::int32_t));
    }

    void* const array_oop{ buffer.data() };

    check("array_length_reads_offset_12",
          vmhook::array_length(array_oop) == expected_length);

    for (std::int32_t i{ 0 }; i < 5; ++i)
    {
        const std::int32_t v{ vmhook::get_array_element<std::int32_t>(array_oop, i) };
        char tag[64];
        std::snprintf(tag, sizeof(tag), "get_array_element_int32_index_%d", i);
        check(tag, v == values[i]);
    }

    // Bounds checks: negative index and out-of-range index should return T{}.
    // The indices are routed through a volatile read so GCC's -Warray-bounds
    // does not statically prove they're OOB and refuse to compile the call.
    auto opaque_index{ [](std::int32_t i) noexcept
        {
            volatile std::int32_t v{ i };
            return v;
        } };

    check("get_array_element_negative_index_returns_default",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(-1)) == 0);
    check("get_array_element_oob_index_returns_default",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(expected_length)) == 0);
    check("get_array_element_far_oob_index_returns_default",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(1000)) == 0);

    // set_array_element with a valid index updates the buffer in place;
    // set with an out-of-range index is a no-op.
    vmhook::set_array_element<std::int32_t>(array_oop, opaque_index(2), 9999);
    check("set_array_element_writes_value",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(2)) == 9999);

    vmhook::set_array_element<std::int32_t>(array_oop, opaque_index(-1), 1234);
    vmhook::set_array_element<std::int32_t>(array_oop, opaque_index(100), 1234);
    // Confirm the neighbouring elements were not corrupted.
    check("set_array_element_oob_is_noop_neighbour_below",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(0)) == values[0]);
    check("set_array_element_oob_is_noop_neighbour_above",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(4)) == values[4]);

    // Null array_oop short-circuits without faulting.
    check("array_length_null_returns_zero", vmhook::array_length(nullptr) == 0);
    check("get_array_element_null_returns_default",
          vmhook::get_array_element<std::int32_t>(nullptr, 0) == 0);
}

// ---------------------------------------------------------------------------
// 8. format_log — fallback must not throw or crash on misformatted patterns.
// ---------------------------------------------------------------------------
static auto test_format_log_safe_on_bad_pattern() -> void
{
    // The release-build VMHOOK_LOG expands to (void)sizeof(format_log(...)),
    // but the function itself is callable directly.  std::vformat will throw
    // a std::format_error on a malformed pattern; the helper must catch it
    // and return the literal format string instead of propagating.
    const std::string result{ vmhook::detail::format_log("{") };
    check("format_log_handles_bad_pattern", !result.empty());
}

// ---------------------------------------------------------------------------
// 10. write_jni_arg_to_slot for unique_ptr<wrapper> — regression test for the
//     value_type-shadowing bug that silently dropped every IChatComponent arg
//     into Lunar / Forge / vanilla addChatMessage calls.
//
//     The bug:
//       template<typename value_type, typename deleter_type>
//       struct is_unique_ptr<std::unique_ptr<value_type, deleter_type>>
//           : std::true_type
//       { using value_type_t = value_type; };
//
//     The std::true_type base inherits `using value_type = bool` from
//     std::integral_constant<bool, true>; inside the class body unqualified
//     name lookup found the inherited typedef first, so value_type_t became
//     bool, then `is_base_of_v<object_base, value_type_t>` evaluated to
//     `is_base_of_v<object_base, bool>` -> false, the unique_ptr branch in
//     write_jni_arg_to_slot was silently skipped, and the JVM received
//     values[0].l == nullptr for the IChatComponent arg.
//
//     This test wraps a sentinel oop in a test_wrapper, runs the arg slot
//     packer, and asserts that value.l points back at the storage cell
//     containing our sentinel.  Re-introducing the trait bug would set
//     value.l to nullptr and this would fail loudly.
// ---------------------------------------------------------------------------
namespace {
    struct test_wrapper_helpers : public vmhook::object<test_wrapper_helpers> {
        using vmhook::object<test_wrapper_helpers>::object;
    };
}

static auto test_write_jni_arg_to_slot_unique_ptr_branch() -> void
{
    // Sentinel OOP value - just an opaque pointer.  We never deref it; the
    // arg-packer only stores it.
    auto* const sentinel_oop{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEFCAFE0000ull)) };
    auto wrapper{ std::make_unique<test_wrapper_helpers>(sentinel_oop) };

    vmhook::detail::jni_value value{};
    void* storage{ nullptr };
    bool needs_release{ true };
    vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, wrapper);

    // An object handle is a synthetic stack pointer, NOT a JNI local ref —
    // the cleanup must never DeleteLocalRef it.
    check("write_jni_arg_to_slot_unique_ptr_no_release", needs_release == false);

    // value.l must be a NON-NULL pointer (specifically, &storage).
    check("write_jni_arg_to_slot_unique_ptr_value_l_non_null",
          value.l != nullptr);

    // value.l must point at our local `storage` slot - that is the
    // indirect-handle pattern the JVM expects (jobject = jobject*).
    check("write_jni_arg_to_slot_unique_ptr_value_l_points_at_storage",
          value.l == static_cast<void*>(&storage));

    // The storage slot must hold our sentinel.  Re-introducing the trait
    // shadow bug (value_type_t = bool) would skip both writes and leave
    // storage == nullptr.
    check("write_jni_arg_to_slot_unique_ptr_storage_holds_oop",
          storage == sentinel_oop);

    // Dereferencing value.l (the JVM-internal JNIHandles::resolve operation)
    // must yield the sentinel - this is what call_jni's diagnostic dump
    // does, and is also exactly what the JVM does inside CallVoidMethodA.
    check("write_jni_arg_to_slot_unique_ptr_deref_yields_oop",
          *static_cast<void**>(value.l) == sentinel_oop);
}

static auto test_write_jni_arg_to_slot_null_unique_ptr() -> void
{
    // A null unique_ptr arg should result in storage == nullptr but
    // value.l still pointing at &storage (so the JVM receives a NULL jobject,
    // not garbage).
    std::unique_ptr<test_wrapper_helpers> wrapper{};   // empty

    vmhook::detail::jni_value value{};
    void* storage{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADull)) };  // pre-fill to detect overwrite
    bool needs_release{ true };
    vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, wrapper);

    check("write_jni_arg_to_slot_null_unique_ptr_no_release", needs_release == false);
    check("write_jni_arg_to_slot_null_unique_ptr_value_l_still_points_at_storage",
          value.l == static_cast<void*>(&storage));
    check("write_jni_arg_to_slot_null_unique_ptr_storage_cleared",
          storage == nullptr);
}

// ---------------------------------------------------------------------------
// 11. vmhook::jni wrappers - verify they delegate to the underlying detail
//     functions with no signature drift.  signature_for_arg<T> returns a
//     std::string (non-constexpr) so we cross-check at runtime instead of
//     via static_assert.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 12. is_valid_pointer must reject debug-poison sentinel patterns
//
// Old behaviour: a pointer whose low 32 bits are 0xDEADBEEF / 0xCDCDCDCD /
// 0xCAFEBABE etc. fell inside the user-address range and was passed through
// untouched, then segfaulted on dereference.  After the fix is_valid_pointer
// rejects them up-front so callers can rely on the boolean.
// ---------------------------------------------------------------------------
static auto test_is_valid_pointer_rejects_sentinels() -> void
{
    using vmhook::hotspot::is_valid_pointer;

    // Well-known debug-poison values that the old range-only check accepted.
    auto poison = [](std::uint64_t low32) -> const void*
    {
        return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(low32));
    };
    check("is_valid_pointer_rejects_DEADBEEF", !is_valid_pointer(poison(0xDEADBEEFu)));
    check("is_valid_pointer_rejects_CAFEBABE", !is_valid_pointer(poison(0xCAFEBABEu)));
    check("is_valid_pointer_rejects_CCCCCCCC", !is_valid_pointer(poison(0xCCCCCCCCu)));
    check("is_valid_pointer_rejects_CDCDCDCD", !is_valid_pointer(poison(0xCDCDCDCDu)));
    check("is_valid_pointer_rejects_BAADF00D", !is_valid_pointer(poison(0xBAADF00Du)));
    check("is_valid_pointer_rejects_FEEEFEEE", !is_valid_pointer(poison(0xFEEEFEEEu)));
    check("is_valid_pointer_rejects_ABABABAB", !is_valid_pointer(poison(0xABABABABu)));
    check("is_valid_pointer_rejects_FDFDFDFD", !is_valid_pointer(poison(0xFDFDFDFDu)));
    check("is_valid_pointer_rejects_DDDDDDDD", !is_valid_pointer(poison(0xDDDDDDDDu)));

    // Real stack address must STILL pass after the new sentinel check.
    int local_var{ 42 };
    check("is_valid_pointer_accepts_stack_address", is_valid_pointer(&local_var));

    // Heap address must pass.
    auto heap_buf{ std::make_unique<int>(42) };
    check("is_valid_pointer_accepts_heap_address", is_valid_pointer(heap_buf.get()));

    // Null still rejected.
    check("is_valid_pointer_rejects_null", !is_valid_pointer(nullptr));

    // Odd low-bit address now rejected (HotSpot pointers are always aligned).
    const void* odd{ reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(&local_var) | 0x1u) };
    check("is_valid_pointer_rejects_odd_low_bit", !is_valid_pointer(odd));
}

// ---------------------------------------------------------------------------
// 13. return_value::set must sign-extend signed integers into the 64-bit slot.
//
// Old behaviour: memcpy of the low N bytes left the upper bits at zero, so a
// hook returning int8_t{-1} (= 0xFF) was visible to Java as +255 instead of -1.
// Fix: signed integer types < 8 bytes go through a static_cast<int64_t>(value)
// which sign-extends; other types still use memcpy.
// ---------------------------------------------------------------------------
static auto test_return_value_sign_extension() -> void
{
    // Build a fake slot on the stack.  The slot lives in vmhook::hotspot.
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot };

    // int8_t{-1} -> retval should be -1 (sign-extended), not 255 (zero-extended).
    rv.set(std::int8_t{ -1 });
    check("return_value_set_int8_minus_one_sign_extends",
          slot.retval == static_cast<std::int64_t>(-1));

    // int16_t{-12345} -> sign-extend to -12345 in 64 bits.
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int16_t{ -12345 });
    check("return_value_set_int16_neg_sign_extends",
          slot.retval == static_cast<std::int64_t>(-12345));

    // int32_t{-1} -> sign-extend.
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ -1 });
    check("return_value_set_int32_minus_one_sign_extends",
          slot.retval == static_cast<std::int64_t>(-1));

    // Unsigned types still zero-extend (correct behaviour - JVM treats them
    // as the corresponding signed types and interprets the bit pattern).
    slot.retval = 0; slot.cancel = false;
    rv.set(std::uint8_t{ 0xFF });
    check("return_value_set_uint8_zero_extends_to_255",
          slot.retval == static_cast<std::int64_t>(0xFFu));

    // Positive signed values still come through correctly.
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ 42 });
    check("return_value_set_int32_positive_unchanged",
          slot.retval == 42);

    // cancel flag must be set on every set().
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ 0 });
    check("return_value_set_sets_cancel_flag", slot.cancel == true);
}

// ---------------------------------------------------------------------------
// 14. return_value::set<wrapper_type>(nullptr) overload — sets cancel + writes
//     a zero OOP to the retval slot, regardless of any garbage previously in
//     the slot.  Selected via requires-clause on wrapper_type deriving from
//     object_base, so primitive set<int>(...) calls are unaffected.
// ---------------------------------------------------------------------------
static auto test_return_value_set_nullptr_for_wrapper() -> void
{
    struct fake_wrapper : public vmhook::object_base {};

    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot };

    // Pre-fill the slot with garbage so we can prove set() zeroes it.
    slot.retval = static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull);
    slot.cancel = false;

    rv.set<fake_wrapper>(nullptr);

    check("return_value_set_wrapper_nullptr_writes_zero_oop", slot.retval == 0);
    check("return_value_set_wrapper_nullptr_sets_cancel_flag", slot.cancel == true);

    // Sanity: primitive path still picks the integer overload (no ambiguity).
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ -1 });
    check("return_value_set_primitive_unaffected_by_wrapper_overload",
          slot.retval == static_cast<std::int64_t>(-1));
}

static auto test_jni_namespace_signature_for_arg() -> void
{
    check("jni::signature_for_arg<bool> == 'Z'",
          vmhook::jni::signature_for_arg<bool>() == "Z");
    check("jni::signature_for_arg<int8_t> == 'B'",
          vmhook::jni::signature_for_arg<std::int8_t>() == "B");
    check("jni::signature_for_arg<int16_t> == 'S'",
          vmhook::jni::signature_for_arg<std::int16_t>() == "S");
    check("jni::signature_for_arg<uint16_t> == 'C'",
          vmhook::jni::signature_for_arg<std::uint16_t>() == "C");
    check("jni::signature_for_arg<int32_t> == 'I'",
          vmhook::jni::signature_for_arg<std::int32_t>() == "I");
    check("jni::signature_for_arg<int64_t> == 'J'",
          vmhook::jni::signature_for_arg<std::int64_t>() == "J");
    check("jni::signature_for_arg<float> == 'F'",
          vmhook::jni::signature_for_arg<float>() == "F");
    check("jni::signature_for_arg<double> == 'D'",
          vmhook::jni::signature_for_arg<double>() == "D");
    check("jni::signature_for_arg<string> == 'Ljava/lang/String;'",
          vmhook::jni::signature_for_arg<std::string>() == "Ljava/lang/String;");
    check("jni::signature_for_arg<string_view> == 'Ljava/lang/String;'",
          vmhook::jni::signature_for_arg<std::string_view>() == "Ljava/lang/String;");
    check("jni::signature_for_arg<const char*> == 'Ljava/lang/String;'",
          vmhook::jni::signature_for_arg<const char*>() == "Ljava/lang/String;");

    // Cross-check that the wrapper returns the same string as the underlying
    // implementation it delegates to.  Catches accidental drift between the
    // two if someone adds a new type to detail::jni_signature_for_arg but
    // forgets the corresponding wrapper instantiation (templates compile
    // lazily, so without this check the wrapper would just silently fall to
    // the default 'I' branch).
    check("jni::signature_for_arg<bool> matches detail::jni_signature_for_arg<bool>",
          vmhook::jni::signature_for_arg<bool>()
          == vmhook::detail::jni_signature_for_arg<bool>());
    check("jni::signature_for_arg<string> matches detail::jni_signature_for_arg<string>",
          vmhook::jni::signature_for_arg<std::string>()
          == vmhook::detail::jni_signature_for_arg<std::string>());
    check("jni::signature_for_arg<int64_t> matches detail::jni_signature_for_arg<int64_t>",
          vmhook::jni::signature_for_arg<std::int64_t>()
          == vmhook::detail::jni_signature_for_arg<std::int64_t>());
}

static auto test_write_jni_arg_to_slot_primitive_branches() -> void
{
    // Sanity that the primitive branches still hit, after the static_assert
    // guard was added in the trailing else.
    //
    // The `needs_release` out-parameter is the critical-bug regression guard:
    // jni_value is a union, so a primitive arg aliases .l.  If the cleanup path
    // ever decided "is this a JNI local ref?" by reading .l back, a primitive
    // bit pattern (e.g. jlong 0x1122334455667788) would be handed to
    // DeleteLocalRef as a garbage pointer.  Every primitive branch MUST leave
    // needs_release == false; only the string branches set it true.
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        bool needs_release{ true };  // start true to prove the branch clears it
        vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, true);
        check("write_jni_arg_to_slot_bool", value.z == true);
        check("write_jni_arg_to_slot_bool_no_release", needs_release == false);
    }
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        bool needs_release{ true };
        vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, std::int32_t{ 42 });
        check("write_jni_arg_to_slot_int", value.i == 42);
        check("write_jni_arg_to_slot_int_no_release", needs_release == false);
    }
    {
        // The smoking-gun value: a jlong whose bits, read back as .l, look like
        // a plausible heap pointer.  Must NOT be flagged for release.
        vmhook::detail::jni_value value{};
        void* storage{};
        bool needs_release{ true };
        vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, std::int64_t{ 0x1122334455667788ll });
        check("write_jni_arg_to_slot_long", value.j == 0x1122334455667788ll);
        check("write_jni_arg_to_slot_long_no_release", needs_release == false);
    }
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        bool needs_release{ true };
        vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, 3.14f);
        check("write_jni_arg_to_slot_float", value.f == 3.14f);
        check("write_jni_arg_to_slot_float_no_release", needs_release == false);
    }
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        bool needs_release{ true };
        vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, 2.71828);
        check("write_jni_arg_to_slot_double", value.d == 2.71828);
        check("write_jni_arg_to_slot_double_no_release", needs_release == false);
    }
    {
        // Object-base derived arg: value.l points at the synthetic stack handle
        // (storage), which is NOT a JNI local ref — must not be released.
        // We can't construct a real object_base without a JVM, but a c-string
        // with a null pointer exercises the "string branch produced null" path:
        // needs_release must stay false because there's nothing to delete.
        vmhook::detail::jni_value value{};
        void* storage{};
        bool needs_release{ true };
        const char* null_cstr{ nullptr };
        vmhook::detail::write_jni_arg_to_slot(value, storage, needs_release, null_cstr);
        check("write_jni_arg_to_slot_null_cstr_no_release", needs_release == false);
    }
}

// ---------------------------------------------------------------------------
// 15. iterate_struct_entries / iterate_type_entries hardening
//
// These walk gHotSpotVMStructs / gHotSpotVMTypes.  In a no-JVM unit test the
// underlying global pointers are null, so the loop must terminate immediately
// and return nullptr rather than crashing.  Also exercises the defensive
// null-arg guards: both functions now reject null type_name / field_name
// up-front so callers passing through user-controlled symbol strings can't
// accidentally hand strcmp a nullptr.
// ---------------------------------------------------------------------------
static auto test_iterate_entries_no_jvm() -> void
{
    // No JVM is loaded in the test process, so the static lookups must
    // return nullptr without faulting on strcmp(nullptr, ...).
    check("iterate_struct_entries_no_jvm_returns_null",
          vmhook::hotspot::iterate_struct_entries("Symbol", "_length") == nullptr);
    check("iterate_type_entries_no_jvm_returns_null",
          vmhook::hotspot::iterate_type_entries("Symbol") == nullptr);

    // Null arg guards: both functions must short-circuit to nullptr when
    // any of their string arguments is null, NOT call strcmp on it.
    check("iterate_struct_entries_null_type_name",
          vmhook::hotspot::iterate_struct_entries(nullptr, "_length") == nullptr);
    check("iterate_struct_entries_null_field_name",
          vmhook::hotspot::iterate_struct_entries("Symbol", nullptr) == nullptr);
    check("iterate_struct_entries_both_null",
          vmhook::hotspot::iterate_struct_entries(nullptr, nullptr) == nullptr);
    check("iterate_type_entries_null_type_name",
          vmhook::hotspot::iterate_type_entries(nullptr) == nullptr);
}

// ---------------------------------------------------------------------------
// 16. get_vm_types / get_vm_structs in a no-JVM process
//
// The first call resolves gHotSpotVMTypes / gHotSpotVMStructs from the JVM
// module via dlsym/GetProcAddress.  Without a JVM the module handle is null,
// the symbol resolves to nullptr, and the cached value stays nullptr forever.
// Two consecutive calls must return identical nullptr (proves the static
// caching path runs without a crash).
// ---------------------------------------------------------------------------
static auto test_vm_types_and_structs_no_jvm() -> void
{
    auto* const types_first{ vmhook::hotspot::get_vm_types() };
    auto* const types_second{ vmhook::hotspot::get_vm_types() };
    check("get_vm_types_no_jvm_returns_null", types_first == nullptr);
    check("get_vm_types_cache_stable", types_first == types_second);

    auto* const structs_first{ vmhook::hotspot::get_vm_structs() };
    auto* const structs_second{ vmhook::hotspot::get_vm_structs() };
    check("get_vm_structs_no_jvm_returns_null", structs_first == nullptr);
    check("get_vm_structs_cache_stable", structs_first == structs_second);
}

// ---------------------------------------------------------------------------
// 17. get_jvm_module no-JVM behaviour
//
// In a unit test process, no JVM library is loaded.  find_jvm_module() must
// walk every candidate name (jvm.dll / libjvm.so / libjvm.dylib) and return
// nullptr without ever calling GetProcAddress / dlsym on a null handle.
// Cached after first call, so two queries return the same value.
// ---------------------------------------------------------------------------
static auto test_find_jvm_module_no_jvm() -> void
{
    auto const first{ vmhook::hotspot::get_jvm_module() };
    auto const second{ vmhook::hotspot::get_jvm_module() };
    check("get_jvm_module_no_jvm_returns_null", first == nullptr);
    check("get_jvm_module_cache_stable", first == second);
}

// ---------------------------------------------------------------------------
// 18. return_value::set on non-integer trivially-copyable values
//
// The sign-extension branch only fires for signed integer types < 8 bytes.
// float, double, and pointer types must take the memcpy path and land in
// the slot with their bit pattern intact (no spurious sign extension).
// ---------------------------------------------------------------------------
static auto test_return_value_set_non_integer_types() -> void
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot };

    // float - 4 bytes, NOT a signed integer, so memcpy path.  Bit pattern
    // of 3.14f is 0x4048F5C3 - the upper 32 bits of the slot must stay zero.
    rv.set(3.14f);
    check("return_value_set_float_cancel", slot.cancel == true);
    {
        float roundtrip{};
        std::memcpy(&roundtrip, &slot.retval, sizeof(roundtrip));
        check("return_value_set_float_roundtrip", roundtrip == 3.14f);
    }
    check("return_value_set_float_upper_bits_zero",
          (static_cast<std::uint64_t>(slot.retval) >> 32) == 0u);

    // double - 8 bytes, memcpy path fills the whole slot.
    slot.retval = 0;
    slot.cancel = false;
    rv.set(2.71828);
    check("return_value_set_double_cancel", slot.cancel == true);
    {
        double roundtrip{};
        std::memcpy(&roundtrip, &slot.retval, sizeof(roundtrip));
        check("return_value_set_double_roundtrip", roundtrip == 2.71828);
    }

    // void* - 8 bytes on x86_64, memcpy path.  No sign-extension even though
    // the high bit is set: a kernel-space-looking pointer must round-trip
    // bit-for-bit.
    slot.retval = 0;
    slot.cancel = false;
    void* const sentinel{ reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xCAFEBABEDEADBEEFull)) };
    rv.set<void*>(sentinel);
    check("return_value_set_pointer_cancel", slot.cancel == true);
    {
        void* roundtrip{};
        std::memcpy(&roundtrip, &slot.retval, sizeof(roundtrip));
        check("return_value_set_pointer_roundtrip", roundtrip == sentinel);
    }

    // uint32_t - unsigned, NO sign extension even if high bit is set.
    slot.retval = 0;
    slot.cancel = false;
    rv.set(std::uint32_t{ 0x80000000u });
    check("return_value_set_uint32_high_bit_no_sign_extend",
          slot.retval == static_cast<std::int64_t>(0x80000000u));

    // bool - 1 byte, but NOT signed integer (the requires-clause keys on
    // is_signed && is_integral; bool is integral and signed-ness depends
    // on the platform, but std::is_signed_v<bool> is false).
    // Must round-trip without garbage in the upper bytes.
    slot.retval = static_cast<std::int64_t>(0xFFFFFFFFFFFFFF00ull);
    slot.cancel = false;
    rv.set(true);
    check("return_value_set_bool_true_clears_upper_bytes",
          slot.retval == 1);
    slot.retval = static_cast<std::int64_t>(0xFFFFFFFFFFFFFFFFull);
    rv.set(false);
    check("return_value_set_bool_false_clears_slot",
          slot.retval == 0);
}

// ---------------------------------------------------------------------------
// 19. return_value::cancel / caller / stack_trace with no frame
//
// Construct a return_value with a null frame_pointer.  cancel() must
// flip slot.cancel; set() with no value can't be tested via the public
// API for void returns, but caller() and stack_trace() must each return
// the documented empty defaults rather than crashing.
// ---------------------------------------------------------------------------
static auto test_return_value_no_frame_helpers() -> void
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot, /*frame=*/nullptr };

    // cancel() always succeeds; just records the flag on the slot.
    rv.cancel();
    check("return_value_cancel_sets_flag", slot.cancel == true);

    // No frame -> caller() returns empty caller_info with valid() == false.
    const auto caller{ rv.caller() };
    check("return_value_caller_no_frame_invalid", !caller.valid());
    check("return_value_caller_no_frame_method_null", caller.method == nullptr);
    check("return_value_caller_no_frame_class_empty", caller.class_name.empty());
    check("return_value_caller_no_frame_method_name_empty", caller.method_name.empty());
    check("return_value_caller_no_frame_signature_empty", caller.signature.empty());

    // No frame -> stack_trace() returns an empty vector.
    const auto frames_default{ rv.stack_trace() };
    check("return_value_stack_trace_no_frame_empty",
          frames_default.empty());

    // max_depth = 0 must promote to the default 64 internally, but with
    // no frame still returns empty.
    const auto frames_zero{ rv.stack_trace(0) };
    check("return_value_stack_trace_zero_depth_empty",
          frames_zero.empty());

    // Even a small explicit depth returns empty with no frame.
    const auto frames_small{ rv.stack_trace(4) };
    check("return_value_stack_trace_small_depth_empty",
          frames_small.empty());

    // frame() exposes the raw pointer the constructor was given.
    check("return_value_frame_accessor_returns_null", rv.frame() == nullptr);
}

// ---------------------------------------------------------------------------
// 20. return_value::set_arg short-circuits on bad inputs
//
// The wrapper short-circuits to false (and logs) when the underlying frame
// is null, the index is negative, or the index exceeds the JVM's max_locals
// limit (u2 = 65535).  All three early returns happen before any HotSpot
// read, so no JVM is needed.
// ---------------------------------------------------------------------------
static auto test_return_value_set_arg_guards() -> void
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot, /*frame=*/nullptr };

    check("return_value_set_arg_no_frame_returns_false",
          rv.set_arg(0, std::int32_t{ 42 }) == false);
    check("return_value_set_arg_no_frame_returns_false_neg_idx",
          rv.set_arg(-1, std::int32_t{ 42 }) == false);
    check("return_value_set_arg_no_frame_returns_false_large_idx",
          rv.set_arg(1000, std::int32_t{ 42 }) == false);

    // JVM max_locals is a u2 (65535).  set_arg must reject anything past
    // that even when a frame is present: otherwise locals[-index] would
    // walk off the interpreter local-variable array into adjacent thread
    // state and silently corrupt the operand stack / frame header.
    // We can't supply a real frame here without a JVM, but we can verify
    // the guard fires via the no-frame check (same return path on this
    // codepath: any of "missing frame / negative / index > 65535" exits
    // before touching get_locals).
    check("return_value_set_arg_above_max_locals_returns_false",
          rv.set_arg(0x10000, std::int32_t{ 42 }) == false);
    check("return_value_set_arg_int_max_returns_false",
          rv.set_arg(std::numeric_limits<std::int32_t>::max(), std::int32_t{ 42 }) == false);
}

// ---------------------------------------------------------------------------
// 21. is_valid_pointer alignment + boundary cases
//
// is_valid_pointer rejects:
//   - addresses at or below user_address_floor (low sentinels)
//   - addresses at or above user_address_ceiling (kernel / non-canonical)
//   - odd low-bit addresses (HotSpot pointers are at least 2-byte aligned)
//   - well-known debug-poison patterns
// Exercise each boundary explicitly to catch off-by-one regressions in the
// comparison operators (e.g. `< floor` instead of `<= floor`).
// ---------------------------------------------------------------------------
static auto test_is_valid_pointer_boundaries() -> void
{
    using vmhook::hotspot::is_valid_pointer;

    // Exactly at user_address_floor must be REJECTED (the function uses <=).
    check("is_valid_pointer_at_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(vmhook::os::user_address_floor)));

    // Just below the floor: rejected.
    check("is_valid_pointer_below_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              vmhook::os::user_address_floor - 1)));

    // Just above the floor (and 2-byte aligned): accepted.  The floor
    // sentinel itself is 0xFFFF which is odd, so floor+1 = 0x10000 is the
    // first 2-byte-aligned address that clears both the range and the
    // alignment checks.
    {
        const std::uintptr_t addr{ vmhook::os::user_address_floor + 1 };
        check("is_valid_pointer_just_above_floor_accepted",
              is_valid_pointer(reinterpret_cast<void*>(addr)));
    }

    // Exactly at user_address_ceiling must be REJECTED (the function uses >=).
    check("is_valid_pointer_at_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(vmhook::os::user_address_ceiling)));

    // Just above the ceiling: rejected.
    check("is_valid_pointer_above_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              vmhook::os::user_address_ceiling + 1)));

    // 2-byte alignment is the documented minimum requirement; 4- and 8-byte
    // alignment must both pass.
    int locals_for_alignment[4]{};  // stack array, naturally aligned
    void* const aligned_4{ &locals_for_alignment[1] };
    void* const aligned_8{ &locals_for_alignment[0] };
    check("is_valid_pointer_4byte_aligned_accepted", is_valid_pointer(aligned_4));
    check("is_valid_pointer_8byte_aligned_accepted", is_valid_pointer(aligned_8));
}

// ---------------------------------------------------------------------------
// 22. decode_u5 multi-byte boundary
//
// Validates the UNSIGNED5 decoder at the boundaries that the existing test
// did not exercise: a 3-byte encoding and a near-maximum-value 5-byte one.
// Encoder spec from src/hotspot/share/utilities/unsigned5.hpp:
//   for value v in [L, L + L*H), 2 bytes  (L = 191, H = 64)
//   for value v in [L + L*H, L + L*H + L*H*H), 3 bytes
// 3-byte boundary: value = 191 + 64*191 = 12415 maps to [192, 192, 1].
//   First two bytes are continuation (>= 192), final byte is low (=1).
// ---------------------------------------------------------------------------
static auto test_decode_u5_multi_byte() -> void
{
    // 3-byte encoding: 12415 = (192-1) + (192-1)*64 + (1-1)*64*64
    //                       = 191 + 12224 + 0 = 12415
    int pos_3byte{ 0 };
    std::array<std::uint8_t, 8> buf_3byte{ 192, 192, 1, 0, 0, 0, 0, 0 };
    const std::uint32_t v_3byte{
        vmhook::hotspot::klass::decode_u5(buf_3byte.data(), pos_3byte) };
    check("decode_u5_3byte_value", v_3byte == 12415u);
    check("decode_u5_3byte_pos", pos_3byte == 3);

    // After a value-decode, the next call must continue from the new
    // stream position (no reset).  Encode value 5 in the byte that
    // immediately follows our 3-byte sequence.
    std::array<std::uint8_t, 8> buf_sequence{ 192, 192, 1, 6, 0, 0, 0, 0 };
    int seq_pos{ 0 };
    const std::uint32_t first{
        vmhook::hotspot::klass::decode_u5(buf_sequence.data(), seq_pos) };
    const std::uint32_t second{
        vmhook::hotspot::klass::decode_u5(buf_sequence.data(), seq_pos) };
    check("decode_u5_sequence_first_12415", first == 12415u);
    check("decode_u5_sequence_second_5", second == 5u);
    check("decode_u5_sequence_pos_advanced", seq_pos == 4);

    // 4-byte encoding stress: every byte must be checked.  Build
    // value (b0 - 1) + (b1 - 1)*64 + (b2 - 1)*64*64 + (b3 - 1)*64*64*64
    // with b0 = b1 = b2 = 192 (highest continuation) and b3 = 2
    // (terminator) - that resolves to 191 + 191*64 + 191*64*64 + 64^3
    // = 191 + 12224 + 782336 + 262144 = 1056895.
    int pos_4byte{ 0 };
    std::array<std::uint8_t, 8> buf_4byte{ 192, 192, 192, 2, 0, 0, 0, 0 };
    const std::uint32_t v_4byte{
        vmhook::hotspot::klass::decode_u5(buf_4byte.data(), pos_4byte) };
    check("decode_u5_4byte_pos", pos_4byte == 4);
    check("decode_u5_4byte_value",
          v_4byte == (191u + 191u * 64u + 191u * 64u * 64u + 64u * 64u * 64u));
}

// ---------------------------------------------------------------------------
// 23. format_log positive path - when std::format is available the helper
//     must actually expand the placeholders, not return the raw template.
//     The fallback path is exercised by test_format_log_safe_on_bad_pattern.
// ---------------------------------------------------------------------------
static auto test_format_log_positive() -> void
{
#if VMHOOK_HAS_STD_FORMAT
    // Well-formed pattern with one substitution.
    const std::string result{ vmhook::detail::format_log("answer={}", 42) };
    check("format_log_substitutes_int", result == "answer=42");

    // Two substitutions of different types.
    const std::string result_two{
        vmhook::detail::format_log("{} = {}", "pi", 3.14) };
    check("format_log_substitutes_two", !result_two.empty()
          && result_two.find("pi") != std::string::npos
          && result_two.find("3.14") != std::string::npos);

    // No substitutions - format string passes through unchanged.
    const std::string result_none{ vmhook::detail::format_log("hello") };
    check("format_log_no_placeholders_passthrough", result_none == "hello");
#else
    // Fallback path: the helper returns the raw template - that's the
    // documented behaviour, and the existing bad-pattern test covers it.
    std::printf("[INFO] test_format_log_positive: skipped (no <format>)\n");
#endif
}

// ---------------------------------------------------------------------------
// 24Z. jvm_primitive_byte_width - introduced in v0.4.4 to size-check
//      field_proxy::set against the JVM field width.
// ---------------------------------------------------------------------------
static auto test_jvm_primitive_byte_width() -> void
{
    using vmhook::detail::jvm_primitive_byte_width;

    // Single-character primitive descriptors map to their JVM spec widths.
    check("jvm_primitive_byte_width_Z", jvm_primitive_byte_width("Z") == 1);
    check("jvm_primitive_byte_width_B", jvm_primitive_byte_width("B") == 1);
    check("jvm_primitive_byte_width_S", jvm_primitive_byte_width("S") == 2);
    check("jvm_primitive_byte_width_C", jvm_primitive_byte_width("C") == 2);
    check("jvm_primitive_byte_width_I", jvm_primitive_byte_width("I") == 4);
    check("jvm_primitive_byte_width_F", jvm_primitive_byte_width("F") == 4);
    check("jvm_primitive_byte_width_J", jvm_primitive_byte_width("J") == 8);
    check("jvm_primitive_byte_width_D", jvm_primitive_byte_width("D") == 8);

    // Reference / array / void all return 0 (skip size validation upstream).
    check("jvm_primitive_byte_width_L_is_0",
          jvm_primitive_byte_width("Ljava/lang/String;") == 0);
    check("jvm_primitive_byte_width_array_is_0",
          jvm_primitive_byte_width("[I") == 0);
    check("jvm_primitive_byte_width_V_is_0",
          jvm_primitive_byte_width("V") == 0);

    // Empty / unknown signatures also return 0.
    check("jvm_primitive_byte_width_empty_is_0",
          jvm_primitive_byte_width("") == 0);
    check("jvm_primitive_byte_width_unknown_is_0",
          jvm_primitive_byte_width("?") == 0);
    check("jvm_primitive_byte_width_multichar_is_0",
          jvm_primitive_byte_width("Ix") == 0);
}

// ---------------------------------------------------------------------------
// 24Y. field_proxy::set size-mismatch guard
//
// The set() implementation now refuses to memcpy a value when the C++
// type's size doesn't match the JVM field width.  Previously,
// `field.set(int64_t{x})` on an "I" field wrote 8 bytes into a 4-byte
// slot, clobbering whatever came next in the heap object's layout.
// We exercise the guard on a stack buffer with sentinel bytes after the
// field; a successful guard leaves those sentinel bytes intact.
// ---------------------------------------------------------------------------
static auto test_field_proxy_set_size_guard() -> void
{
    // Layout:
    //   [0..3]   the field's storage (4-byte "I")
    //   [4..7]   sentinel guard bytes (0xAB) - the test verifies these
    //            stay 0xAB after a malformed set() call
    std::array<std::uint8_t, 8> storage{};
    storage.fill(std::uint8_t{ 0xAB });

    vmhook::field_proxy proxy_int{ storage.data(), "I", false };

    // Right-sized: int32_t into an "I" field writes 4 bytes, sentinels stay.
    proxy_int.set(std::int32_t{ 0x11223344 });
    {
        std::int32_t read_back{};
        std::memcpy(&read_back, storage.data(), sizeof(read_back));
        check("field_proxy_set_int32_into_I_writes_correctly",
              read_back == 0x11223344);
        check("field_proxy_set_int32_into_I_preserves_sentinels",
              storage[4] == 0xAB && storage[5] == 0xAB
              && storage[6] == 0xAB && storage[7] == 0xAB);
    }

    // Refill sentinels after a clean reset.
    storage.fill(std::uint8_t{ 0xAB });

    // Mismatch: int64_t into "I" field would previously have written 8 bytes
    // and clobbered the sentinels.  Guard now refuses the write entirely.
    proxy_int.set(static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull));
    check("field_proxy_set_int64_into_I_does_NOT_clobber_sentinels",
          storage[4] == 0xAB && storage[5] == 0xAB
          && storage[6] == 0xAB && storage[7] == 0xAB);
    // The field bytes are unchanged from their pre-set state (still 0xAB).
    check("field_proxy_set_int64_into_I_leaves_field_unchanged",
          storage[0] == 0xAB && storage[1] == 0xAB
          && storage[2] == 0xAB && storage[3] == 0xAB);

    // Inverse mismatch: int32_t into a "J" (8-byte long) field also refused.
    storage.fill(std::uint8_t{ 0xAB });
    vmhook::field_proxy proxy_long{ storage.data(), "J", false };
    proxy_long.set(std::int32_t{ 0x11223344 });
    check("field_proxy_set_int32_into_J_leaves_field_unchanged",
          std::all_of(storage.begin(), storage.end(),
                      [](std::uint8_t b) { return b == 0xAB; }));

    // Reference / array signatures bypass the size guard (size==0 -> skip).
    // The trivially_copyable branch should still run for those, but in
    // practice users go through the unique_ptr or string branches.  Just
    // verify the helper doesn't accidentally crash for a reference-typed
    // proxy when set() is called with an int (which would route to the
    // trivially_copyable branch and write 4 bytes into a 4-byte compressed
    // OOP slot - that's an OOP-shaped write, semantically wrong but not
    // crash-y).
    storage.fill(std::uint8_t{ 0 });
    vmhook::field_proxy proxy_ref{ storage.data(), "Ljava/lang/String;", false };
    proxy_ref.set(std::uint32_t{ 0xCAFEBABE });
    {
        std::uint32_t read_back{};
        std::memcpy(&read_back, storage.data(), sizeof(read_back));
        check("field_proxy_set_uint32_into_ref_writes_4_bytes",
              read_back == 0xCAFEBABE);
    }

    // The "C" -> char widening special case still works.
    storage.fill(std::uint8_t{ 0xCC });
    vmhook::field_proxy proxy_char{ storage.data(), "C", false };
    proxy_char.set(char{ 'A' });
    {
        std::uint16_t read_back{};
        std::memcpy(&read_back, storage.data(), sizeof(read_back));
        check("field_proxy_set_char_into_C_widens_to_uint16",
              read_back == static_cast<std::uint16_t>(
                  static_cast<unsigned char>('A')));
        // Following bytes untouched.
        check("field_proxy_set_char_into_C_preserves_remaining_bytes",
              storage[2] == 0xCC && storage[3] == 0xCC);
    }

    // Null field_pointer: set() must short-circuit, not deref the null.
    vmhook::field_proxy proxy_null{ nullptr, "I", false };
    proxy_null.set(std::int32_t{ 999 });
    check("field_proxy_set_null_field_pointer_is_safe", true);  // no crash
}

// ---------------------------------------------------------------------------
// 24a. jni_delete_local_ref - new helper added in v0.4.1 to release jstring
//      locals from set_arg's string path.  Must be a safe no-op in the
//      no-JVM unit test: null current_jni_env means jni_function returns
//      nullptr and the helper exits without calling through.  We exercise
//      both the null-handle branch (early return) and the non-null branch
//      (table-resolution fall-through).
// ---------------------------------------------------------------------------
static auto test_jni_delete_local_ref_no_jvm() -> void
{
    // Null handle: documented JNI no-op.
    vmhook::detail::jni_delete_local_ref(nullptr);
    check("jni_delete_local_ref_null_handle_does_not_crash", true);

    // Non-null handle in a no-JVM process: current_jni_env is null, so the
    // function-table lookup short-circuits and the helper returns without
    // dereferencing anything.  We just want to assert no fault.
    void* const fake_handle{ reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xABCDEF1234567880ull)) };
    vmhook::detail::jni_delete_local_ref(fake_handle);
    check("jni_delete_local_ref_no_jvm_returns_safely", true);
}

// ---------------------------------------------------------------------------
// 24b. dr_arm_one / dr_unarm_one refcount transitions
//
// v0.4.2 changed ensure_dr_handler_installed from "install once, never
// uninstall" to a refcounted scheme.  The 0 -> 1 transition installs the
// VEH; the 1 -> 0 transition uninstalls it.  We can't realistically install
// a VEH in a unit-test process (AddVectoredExceptionHandler succeeds but
// then any subsequent unhandled exception in another test will route
// through our dispatcher), so we just exercise the counter logic by
// holding the mutex ourselves and calling the inc/dec pair.  Skipped on
// platforms without HW data breakpoints (where the helpers don't exist).
// ---------------------------------------------------------------------------
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
static auto test_dr_armed_count_refcount() -> void
{
    // Snapshot the count.  The unit-test process never arms a real watch
    // so it should be zero before this test runs.
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        check("dr_armed_count_starts_zero",
              vmhook::detail::dr_armed_count == 0);
    }

    // arm three times -> count == 3, VEH installed exactly once.
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        const PVOID veh_before{ vmhook::detail::dr_veh_handle };
        vmhook::detail::dr_arm_one();
        vmhook::detail::dr_arm_one();
        vmhook::detail::dr_arm_one();
        check("dr_armed_count_after_three_arms",
              vmhook::detail::dr_armed_count == 3);
        check("dr_veh_installed_after_first_arm",
              vmhook::detail::dr_veh_handle != nullptr);
        // Subsequent arms must NOT re-install (the handle stays the same).
        // veh_before could be null (first ever arm in this process) or
        // could be a previous handle - either way the only thing that
        // matters is that the handle is non-null AFTER arming.
        (void)veh_before;
    }

    // unarm three times -> count == 0, VEH removed.
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        vmhook::detail::dr_unarm_one();
        check("dr_armed_count_after_one_unarm",
              vmhook::detail::dr_armed_count == 2);
        check("dr_veh_still_installed_above_zero",
              vmhook::detail::dr_veh_handle != nullptr);

        vmhook::detail::dr_unarm_one();
        check("dr_armed_count_after_two_unarms",
              vmhook::detail::dr_armed_count == 1);

        vmhook::detail::dr_unarm_one();
        check("dr_armed_count_back_to_zero",
              vmhook::detail::dr_armed_count == 0);
        check("dr_veh_removed_at_zero",
              vmhook::detail::dr_veh_handle == nullptr);
    }

    // Extra unarm at zero must be a no-op (no underflow / no crash).
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        vmhook::detail::dr_unarm_one();
        check("dr_unarm_one_at_zero_is_noop",
              vmhook::detail::dr_armed_count == 0);
    }
}
#endif

// ---------------------------------------------------------------------------
// 24. version_string composition + numeric range sanity
// ---------------------------------------------------------------------------
static auto test_version_string_composition() -> void
{
    constexpr std::string_view v{ VMHOOK_VERSION_STRING };
    // Components are non-negative and fit in the documented widths.
    static_assert(VMHOOK_VERSION_MAJOR >= 0 && VMHOOK_VERSION_MAJOR < 1000,
                  "VMHOOK_VERSION_MAJOR must fit in the packed integer's MAJOR slot");
    static_assert(VMHOOK_VERSION_MINOR >= 0 && VMHOOK_VERSION_MINOR < 1000,
                  "VMHOOK_VERSION_MINOR must fit in the packed integer's MINOR slot");
    static_assert(VMHOOK_VERSION_PATCH >= 0 && VMHOOK_VERSION_PATCH < 1000,
                  "VMHOOK_VERSION_PATCH must fit in the packed integer's PATCH slot");

    // Every character is a digit or a dot, and there are no leading dots
    // or empty components.
    bool every_char_valid{ true };
    bool seen_dot{ false };
    char last_char{ 'x' };
    for (const char c : v)
    {
        if (c != '.' && (c < '0' || c > '9'))
        {
            every_char_valid = false;
            break;
        }
        if (c == '.' && last_char == '.')
        {
            every_char_valid = false;
            break;
        }
        if (c == '.')
        {
            seen_dot = true;
        }
        last_char = c;
    }
    check("version_string_only_digits_and_dots", every_char_valid);
    check("version_string_contains_a_dot", seen_dot);
    check("version_string_does_not_start_with_dot", !v.empty() && v.front() != '.');
    check("version_string_does_not_end_with_dot", !v.empty() && v.back() != '.');

    // The packed integer must agree with the components for inequality
    // gates downstream code might use ("#if VMHOOK_VERSION >= 4_001").
    static_assert(VMHOOK_VERSION > 0, "VMHOOK_VERSION must be a positive integer");
}

// ===========================================================================
// EXHAUSTIVE-INPUT EXPANSION (#38-immune no-JVM lane)
//
// Everything below deepens the helpers that are UNIQUE to test_helpers.cpp
// (the catch-all pure-logic helpers and the runtime factory registry), with
// FULL input-domain coverage rather than spot checks.  Suites that already
// have a dedicated exhaustive file (decode_u5, array elements, version macros)
// are NOT re-expanded here; only their cross-helper INTERPLAY is touched.
//
// Every expected value is derived from the live header.  All cases are
// deterministic and endianness-agnostic (values compared, never raw bytes;
// width-dependent facts are sizeof-branched).
// ===========================================================================

// ---------------------------------------------------------------------------
// E1. sig_char_to_basic_type — EXHAUSTIVE over all 256 byte values.
//
// The header switch maps exactly 11 descriptor chars to their HotSpot
// BasicType integer and returns T_OBJECT (12) for everything else.  We
// reproduce that table independently and assert agreement for every possible
// `char` input, then assert the function is a pure function (same input ->
// same output across repeated calls).
// ---------------------------------------------------------------------------
static auto expected_basic_type(unsigned char uc) -> int
{
    switch (static_cast<char>(uc))
    {
    case 'Z': return 4;
    case 'C': return 5;
    case 'F': return 6;
    case 'D': return 7;
    case 'B': return 8;
    case 'S': return 9;
    case 'I': return 10;
    case 'J': return 11;
    case 'L': return 12;
    case '[': return 13;
    case 'V': return 14;
    default:  return 12;
    }
}

static auto test_sig_char_to_basic_type_exhaustive() -> void
{
    bool all_match{ true };
    bool deterministic{ true };
    bool default_is_object{ true };
    int  mismatch_input{ -1 };

    for (int v{ 0 }; v < 256; ++v)
    {
        const char c{ static_cast<char>(static_cast<unsigned char>(v)) };
        const int got{ vmhook::detail::sig_char_to_basic_type(c) };
        const int want{ expected_basic_type(static_cast<unsigned char>(v)) };
        if (got != want)
        {
            all_match = false;
            if (mismatch_input < 0)
            {
                mismatch_input = v;
            }
        }
        // Determinism: a second call on the same input yields the same value.
        if (vmhook::detail::sig_char_to_basic_type(c) != got)
        {
            deterministic = false;
        }
        // Negative space: any char that is NOT one of the 11 known descriptors
        // must collapse to the T_OBJECT (12) fallback.
        const bool is_known{ want != 12 || c == 'L' };
        if (!is_known && got != 12)
        {
            default_is_object = false;
        }
    }

    check("sig_char_to_basic_type_exhaustive_all_256_match", all_match);
    check("sig_char_to_basic_type_exhaustive_deterministic", deterministic);
    check("sig_char_to_basic_type_exhaustive_unknown_defaults_T_OBJECT",
          default_is_object);
    if (!all_match)
    {
        std::printf("[INFO] sig_char_to_basic_type mismatch at input %d\n",
                    mismatch_input);
    }

    // The two ambiguous-looking descriptors L and '[' must NOT collide with the
    // numeric BasicType of any primitive; assert their exact documented values.
    check("sig_char_to_basic_type_L_is_12",
          vmhook::detail::sig_char_to_basic_type('L') == 12);
    check("sig_char_to_basic_type_array_is_13",
          vmhook::detail::sig_char_to_basic_type('[') == 13);

    // Every primitive descriptor maps to a DISTINCT value in [4, 11]; a
    // collision would silently mis-tag a field/return type.  Collect them and
    // assert pairwise distinctness over the closed primitive set.
    const std::array<char, 8> prim_chars{ 'Z', 'C', 'F', 'D', 'B', 'S', 'I', 'J' };
    std::array<int, 8> prim_codes{};
    for (std::size_t i{ 0 }; i < prim_chars.size(); ++i)
    {
        prim_codes[i] = vmhook::detail::sig_char_to_basic_type(prim_chars[i]);
    }
    bool prims_distinct{ true };
    for (std::size_t i{ 0 }; i < prim_codes.size(); ++i)
    {
        for (std::size_t j{ i + 1 }; j < prim_codes.size(); ++j)
        {
            if (prim_codes[i] == prim_codes[j])
            {
                prims_distinct = false;
            }
        }
    }
    check("sig_char_to_basic_type_primitives_pairwise_distinct", prims_distinct);
}

// ---------------------------------------------------------------------------
// E2. jvm_primitive_byte_width — EXHAUSTIVE over the single-char domain plus
//     the full length domain (0, 1, 2, 3+ chars).
//
// Width table (JVM spec §4.3.2): Z=B=1, S=C=2, I=F=4, J=D=8; everything
// else (reference/array/void/unknown) and every non-length-1 string -> 0.
// ---------------------------------------------------------------------------
static auto expected_prim_width(unsigned char uc) -> std::size_t
{
    switch (static_cast<char>(uc))
    {
    case 'Z': case 'B': return 1u;
    case 'S': case 'C': return 2u;
    case 'I': case 'F': return 4u;
    case 'J': case 'D': return 8u;
    default:            return 0u;
    }
}

static auto test_jvm_primitive_byte_width_exhaustive() -> void
{
    using vmhook::detail::jvm_primitive_byte_width;

    // All 256 single-character signatures.
    bool all_single_match{ true };
    int  single_mismatch{ -1 };
    for (int v{ 0 }; v < 256; ++v)
    {
        const char c{ static_cast<char>(static_cast<unsigned char>(v)) };
        const std::string sig(1, c);
        const std::size_t got{ jvm_primitive_byte_width(sig) };
        const std::size_t want{ expected_prim_width(static_cast<unsigned char>(v)) };
        if (got != want)
        {
            all_single_match = false;
            if (single_mismatch < 0)
            {
                single_mismatch = v;
            }
        }
    }
    check("jvm_primitive_byte_width_all_256_single_chars_match", all_single_match);
    if (!all_single_match)
    {
        std::printf("[INFO] jvm_primitive_byte_width single-char mismatch at %d\n",
                    single_mismatch);
    }

    // Length domain: only length-1 can be non-zero.  Empty + every 2-char and
    // 3-char combination of the WIDEST-width primitive chars must still be 0,
    // proving the size!=1 short-circuit fires before the switch.
    check("jvm_primitive_byte_width_empty_is_zero",
          jvm_primitive_byte_width(std::string_view{}) == 0u);

    bool all_multichar_zero{ true };
    const std::array<char, 4> w{ 'I', 'J', 'D', 'F' };  // would-be non-zero if length-1
    for (const char a : w)
    {
        for (const char b : w)
        {
            const std::string two{ std::string(1, a) + std::string(1, b) };
            if (jvm_primitive_byte_width(two) != 0u)
            {
                all_multichar_zero = false;
            }
            for (const char d : w)
            {
                const std::string three{ two + std::string(1, d) };
                if (jvm_primitive_byte_width(three) != 0u)
                {
                    all_multichar_zero = false;
                }
            }
        }
    }
    check("jvm_primitive_byte_width_multichar_all_zero", all_multichar_zero);

    // Reference / array / void descriptors of realistic shape -> 0.
    check("jvm_primitive_byte_width_object_descriptor_zero",
          jvm_primitive_byte_width("Ljava/lang/Object;") == 0u);
    check("jvm_primitive_byte_width_array_of_int_zero",
          jvm_primitive_byte_width("[I") == 0u);
    check("jvm_primitive_byte_width_array_of_object_zero",
          jvm_primitive_byte_width("[Ljava/lang/String;") == 0u);
    check("jvm_primitive_byte_width_void_zero",
          jvm_primitive_byte_width("V") == 0u);

    // Determinism: repeated calls agree for a representative spread.
    bool deterministic{ true };
    for (const char c : { 'Z', 'B', 'S', 'C', 'I', 'F', 'J', 'D', 'L', '[', 'V', 'x' })
    {
        const std::string sig(1, c);
        if (jvm_primitive_byte_width(sig) != jvm_primitive_byte_width(sig))
        {
            deterministic = false;
        }
    }
    check("jvm_primitive_byte_width_deterministic", deterministic);

    // INTERPLAY: a descriptor has a non-zero primitive width IFF
    // sig_char_to_basic_type classifies it as one of the primitive BasicTypes
    // (4..11 excluding T_OBJECT=12).  The two helpers must agree on what a
    // "primitive" descriptor is, or field-size validation and BasicType
    // dispatch would disagree about the same field.
    bool interplay_consistent{ true };
    for (int v{ 0 }; v < 256; ++v)
    {
        const char c{ static_cast<char>(static_cast<unsigned char>(v)) };
        const std::string sig(1, c);
        const bool has_width{ jvm_primitive_byte_width(sig) != 0u };
        const int  bt{ vmhook::detail::sig_char_to_basic_type(c) };
        // Primitive BasicTypes are T_BOOLEAN(4)..T_LONG(11); T_OBJECT(12),
        // T_ARRAY(13), T_VOID(14) are non-primitive.  The fallback for unknown
        // chars is also T_OBJECT(12), which correctly has width 0.
        const bool is_primitive_bt{ bt >= 4 && bt <= 11 };
        if (has_width != is_primitive_bt)
        {
            interplay_consistent = false;
        }
    }
    check("jvm_primitive_byte_width_consistent_with_basic_type",
          interplay_consistent);
}

// ---------------------------------------------------------------------------
// E3. memory_protection enum identity + to_native_protect EXHAUSTIVE.
//
// The enum's underlying values are part of the ABI; pin them at compile time.
// Then verify to_native_protect maps each of the 5 values to the EXACT native
// constant for this platform (derived from the live PAGE_* / PROT_* symbols,
// not guessed), that the four "real" protections are mutually distinct, and
// that EVERY out-of-range enum value collapses to the no_access fallback.
// ---------------------------------------------------------------------------
static auto test_memory_protection_enum_and_native_exhaustive() -> void
{
    using namespace vmhook::os;

    // Compile-time enum identity — these underlying values are documented ABI.
    static_assert(static_cast<std::uint32_t>(memory_protection::no_access) == 0u,
                  "memory_protection::no_access must be 0");
    static_assert(static_cast<std::uint32_t>(memory_protection::read) == 1u,
                  "memory_protection::read must be 1");
    static_assert(static_cast<std::uint32_t>(memory_protection::read_write) == 2u,
                  "memory_protection::read_write must be 2");
    static_assert(static_cast<std::uint32_t>(memory_protection::execute_read) == 3u,
                  "memory_protection::execute_read must be 3");
    static_assert(static_cast<std::uint32_t>(memory_protection::execute_rw) == 4u,
                  "memory_protection::execute_rw must be 4");

    const auto noaccess{ to_native_protect(memory_protection::no_access) };
    const auto read{ to_native_protect(memory_protection::read) };
    const auto read_write{ to_native_protect(memory_protection::read_write) };
    const auto execute_read{ to_native_protect(memory_protection::execute_read) };
    const auto execute_rw{ to_native_protect(memory_protection::execute_rw) };

    // Exact native mapping, derived from the live platform constants.
#if VMHOOK_OS_WINDOWS
    check("to_native_protect_no_access_is_PAGE_NOACCESS",
          noaccess == PAGE_NOACCESS);
    check("to_native_protect_read_is_PAGE_READONLY",
          read == PAGE_READONLY);
    check("to_native_protect_read_write_is_PAGE_READWRITE",
          read_write == PAGE_READWRITE);
    check("to_native_protect_execute_read_is_PAGE_EXECUTE_READ",
          execute_read == PAGE_EXECUTE_READ);
    check("to_native_protect_execute_rw_is_PAGE_EXECUTE_READWRITE",
          execute_rw == PAGE_EXECUTE_READWRITE);
#else
    check("to_native_protect_no_access_is_PROT_NONE",
          noaccess == PROT_NONE);
    check("to_native_protect_read_is_PROT_READ",
          read == PROT_READ);
    check("to_native_protect_read_write_is_PROT_READ_WRITE",
          read_write == (PROT_READ | PROT_WRITE));
    check("to_native_protect_execute_read_is_PROT_READ_EXEC",
          execute_read == (PROT_READ | PROT_EXEC));
    check("to_native_protect_execute_rw_is_PROT_READ_WRITE_EXEC",
          execute_rw == (PROT_READ | PROT_WRITE | PROT_EXEC));
#endif

    // All five mapped values must be mutually distinct (no two intents alias).
    const std::array<decltype(noaccess), 5> all_vals{
        noaccess, read, read_write, execute_read, execute_rw };
    bool distinct{ true };
    for (std::size_t i{ 0 }; i < all_vals.size(); ++i)
    {
        for (std::size_t j{ i + 1 }; j < all_vals.size(); ++j)
        {
            if (all_vals[i] == all_vals[j])
            {
                distinct = false;
            }
        }
    }
    check("to_native_protect_all_five_distinct", distinct);

    // Determinism: each value maps identically on repeated calls.
    bool deterministic{ true };
    for (std::uint32_t e{ 0 }; e <= 4u; ++e)
    {
        const auto mp{ static_cast<memory_protection>(e) };
        if (to_native_protect(mp) != to_native_protect(mp))
        {
            deterministic = false;
        }
    }
    check("to_native_protect_deterministic", deterministic);

    // EXHAUSTIVE negative space: every enum value OUTSIDE {0..4} must fall back
    // to the no_access flag.  Sweep the entire remaining 8-bit range plus a few
    // pathological wide values.
    bool all_fallback{ true };
    for (std::uint32_t e{ 5u }; e < 256u; ++e)
    {
        if (to_native_protect(static_cast<memory_protection>(e)) != noaccess)
        {
            all_fallback = false;
        }
    }
    check("to_native_protect_out_of_range_5_to_255_fallback", all_fallback);

    for (const std::uint32_t e : { 256u, 1000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu })
    {
        if (to_native_protect(static_cast<memory_protection>(e)) != noaccess)
        {
            all_fallback = false;
        }
    }
    check("to_native_protect_extreme_unknown_values_fallback", all_fallback);
}

// ---------------------------------------------------------------------------
// E4. build_dr7 — FULLY EXHAUSTIVE over the entire input domain.
//
// Domain = slot {0,1,2,3} x kind {write, read_write} x length {one, two,
// eight, four} = 4 x 2 x 4 = 32 combinations.  Reference value computed
// independently from the Intel-SDM formula the header documents:
//   L<slot>  = bit (slot*2)
//   R/W<slot> field at bit (16 + slot*4), value = enum(kind)  in {0b01, 0b11}
//   LEN<slot> field at bit (18 + slot*4), value = enum(length) in {0..3}
// We also assert FIELD ISOLATION: the result only ever sets bits belonging to
// the requested slot (a shift typo would leak into a neighbour's field).
// Windows + x86_64 only (same guard as the existing build_dr7 suite).
// ---------------------------------------------------------------------------
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
static auto test_build_dr7_exhaustive() -> void
{
    using namespace vmhook::os;
    using namespace vmhook::os::detail_dr;

    struct kind_entry { data_breakpoint_kind k; std::uint64_t bits; };
    struct len_entry  { data_breakpoint_length l; std::uint64_t bits; };

    const std::array<kind_entry, 2> kinds{
        kind_entry{ data_breakpoint_kind::write,      0b01ull },
        kind_entry{ data_breakpoint_kind::read_write, 0b11ull } };
    const std::array<len_entry, 4> lengths{
        len_entry{ data_breakpoint_length::one_byte,    0b00ull },
        len_entry{ data_breakpoint_length::two_bytes,   0b01ull },
        len_entry{ data_breakpoint_length::eight_bytes, 0b10ull },
        len_entry{ data_breakpoint_length::four_bytes,  0b11ull } };

    bool all_match{ true };
    bool all_isolated{ true };
    bool deterministic{ true };

    for (int slot{ 0 }; slot < 4; ++slot)
    {
        // The complete set of bit positions this slot is allowed to touch:
        // its local-enable bit and its 4-bit R/W+LEN field.
        const std::uint64_t local_bit{ std::uint64_t{ 1 } << (slot * 2) };
        const std::uint64_t field_mask{ std::uint64_t{ 0b1111 } << (16 + slot * 4) };
        const std::uint64_t allowed_mask{ local_bit | field_mask };

        for (const auto& ke : kinds)
        {
            for (const auto& le : lengths)
            {
                const std::uint64_t expected{
                    local_bit
                    | (ke.bits << (16 + slot * 4))
                    | (le.bits << (18 + slot * 4)) };

                const std::uint64_t got{ build_dr7(slot, ke.k, le.l) };
                if (got != expected)
                {
                    all_match = false;
                }
                // No bit outside this slot's allowed positions may be set.
                if ((got & ~allowed_mask) != 0ull)
                {
                    all_isolated = false;
                }
                if (build_dr7(slot, ke.k, le.l) != got)
                {
                    deterministic = false;
                }
            }
        }
    }

    check("build_dr7_exhaustive_all_32_combos_match", all_match);
    check("build_dr7_exhaustive_field_isolation", all_isolated);
    check("build_dr7_exhaustive_deterministic", deterministic);

    // The global-enable bits (G0..G3 at odd bit positions 1,3,5,7) must NEVER
    // be set — the helper documents that only LOCAL enables are used so the
    // trap is per-thread, not process-wide.
    bool no_global_enables{ true };
    const std::uint64_t global_enable_mask{
        (std::uint64_t{ 1 } << 1) | (std::uint64_t{ 1 } << 3)
        | (std::uint64_t{ 1 } << 5) | (std::uint64_t{ 1 } << 7) };
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        for (const auto& ke : kinds)
        {
            for (const auto& le : lengths)
            {
                if ((build_dr7(slot, ke.k, le.l) & global_enable_mask) != 0ull)
                {
                    no_global_enables = false;
                }
            }
        }
    }
    check("build_dr7_exhaustive_no_global_enables", no_global_enables);

    // Distinct slots writing the SAME kind/length must produce results that
    // differ only in the slot-specific bit positions (sanity that the slot
    // parameter actually shifts the field).
    const std::uint64_t s0{ build_dr7(0, data_breakpoint_kind::read_write,
                                      data_breakpoint_length::eight_bytes) };
    const std::uint64_t s3{ build_dr7(3, data_breakpoint_kind::read_write,
                                      data_breakpoint_length::eight_bytes) };
    check("build_dr7_exhaustive_slot0_ne_slot3", s0 != s3);
}
#endif

// ---------------------------------------------------------------------------
// E5. Factory-registry round-trip — type_to_class_map / g_type_factory_map /
//     jni_signature_for_arg.  THE feature that lives in this file because it
//     needs the runtime registry (register_class itself requires a live JVM
//     via find_class, so in a no-JVM process we populate the public maps
//     directly with exactly what register_class<T>() would write, then drive
//     the read paths).
//
// Covered:
//   * unregistered object wrapper  -> "Ljava/lang/Object;" fallback
//   * unregistered unique_ptr<W>   -> "Ljava/lang/Object;" fallback
//   * registered object wrapper    -> "L<name>;"
//   * registered unique_ptr<W>     -> "L<name>;"
//   * jni::signature_for_arg wrapper matches detail:: for the registered type
//   * the stored factory function constructs a W from a raw oop and the
//     wrapper round-trips that oop via get_instance()
//
// The maps are restored to their prior state at the end so the suite leaves
// no global side effects (determinism across repeated runs / test ordering).
// ---------------------------------------------------------------------------
namespace {
    struct registry_wrapper : public vmhook::object<registry_wrapper> {
        using vmhook::object<registry_wrapper>::object;
    };
    struct registry_wrapper_unreg : public vmhook::object<registry_wrapper_unreg> {
        using vmhook::object<registry_wrapper_unreg>::object;
    };
}

static auto test_factory_registry_roundtrip() -> void
{
    using vmhook::detail::jni_signature_for_arg;

    // ---- Unregistered: both the by-value-object branch and the unique_ptr
    //      branch must hit the documented "Ljava/lang/Object;" fallback.
    check("registry_unregistered_object_falls_back_to_Object",
          jni_signature_for_arg<registry_wrapper_unreg>() == "Ljava/lang/Object;");
    check("registry_unregistered_unique_ptr_falls_back_to_Object",
          jni_signature_for_arg<std::unique_ptr<registry_wrapper_unreg>>()
              == "Ljava/lang/Object;");

    // ---- Register registry_wrapper exactly the way register_class<T>() would,
    //      but without a JVM: populate type_to_class_map + g_type_factory_map
    //      directly, under the same registration_mutex the library uses.
    const std::string class_name{ "com/example/RegistryWrapper" };
    const std::type_index key{ typeid(registry_wrapper) };
    {
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
        vmhook::type_to_class_map.insert_or_assign(key, class_name);
        vmhook::g_type_factory_map.emplace(class_name, +[](void* instance)
            -> vmhook::object_base*
            {
                return new registry_wrapper{ instance };
            });
    }

    // ---- Registered: the signature now resolves to the JVM class name.
    const std::string expected_sig{ "L" + class_name + ";" };
    check("registry_registered_object_signature_resolves",
          jni_signature_for_arg<registry_wrapper>() == expected_sig);
    check("registry_registered_unique_ptr_signature_resolves",
          jni_signature_for_arg<std::unique_ptr<registry_wrapper>>() == expected_sig);

    // ---- The public jni:: wrapper must delegate without drift.
    check("registry_public_signature_for_arg_matches_detail",
          vmhook::jni::signature_for_arg<registry_wrapper>()
              == jni_signature_for_arg<registry_wrapper>());

    // ---- The stored factory builds a wrapper from a raw oop, and the wrapper
    //      round-trips that oop.  This is the path frame::get_arguments uses to
    //      reconstruct C++ wrappers from decoded Java references.
    {
        const auto it{ vmhook::g_type_factory_map.find(class_name) };
        check("registry_factory_present", it != vmhook::g_type_factory_map.end());
        if (it != vmhook::g_type_factory_map.end())
        {
            void* const sentinel_oop{ reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(0xAABBCCDD11223340ull)) };
            // The factory returns a raw object_base*; wrap it so it is freed.
            std::unique_ptr<vmhook::object_base> built{ it->second(sentinel_oop) };
            check("registry_factory_returns_non_null", built != nullptr);
            check("registry_factory_roundtrips_oop",
                  built && built->get_instance() == sentinel_oop);
            // The runtime type is our wrapper, not some sliced base.
            check("registry_factory_dynamic_type_is_wrapper",
                  dynamic_cast<registry_wrapper*>(built.get()) != nullptr);
        }
    }

    // ---- Determinism: a second signature query is identical.
    check("registry_signature_deterministic",
          jni_signature_for_arg<registry_wrapper>()
              == jni_signature_for_arg<registry_wrapper>());

    // ---- Restore global state so the suite is side-effect-free.
    {
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };
        vmhook::type_to_class_map.erase(key);
        vmhook::g_type_factory_map.erase(class_name);
    }

    // ---- After cleanup, the type once again resolves to the fallback,
    //      proving both the erase worked and the read path re-evaluates the map.
    check("registry_after_cleanup_falls_back_again",
          jni_signature_for_arg<registry_wrapper>() == "Ljava/lang/Object;");
}

// ---------------------------------------------------------------------------
// E6. jni_signature_for_arg / jni::signature_for_arg — EXHAUSTIVE over the
//     full set of SUPPORTED C++ argument types, asserting both the exact
//     descriptor AND that the public wrapper delegates with zero drift.
//
// Note on what is intentionally NOT instantiated: char / char16_t / char32_t /
// wchar_t are distinct types from int8_t/uint8_t/uint16_t and would hit the
// `is_integral && sizeof==4` branch (only when 4 bytes) or the terminal
// static_assert otherwise; instantiating an unsupported one would be a hard
// compile error, so only types with a real branch are exercised.  The 4-byte
// integral branch is covered via int32_t/uint32_t and the always-32-bit
// `int`/`unsigned`.
// ---------------------------------------------------------------------------
template <typename arg_type>
static auto sig_pair_ok(const char* tag, std::string_view want) -> void
{
    const std::string detail_sig{ vmhook::detail::jni_signature_for_arg<arg_type>() };
    const std::string public_sig{ vmhook::jni::signature_for_arg<arg_type>() };
    check(tag, detail_sig == want && public_sig == want && detail_sig == public_sig);
}

static auto test_jni_signature_for_arg_exhaustive() -> void
{
    // String-like family -> Ljava/lang/String;
    sig_pair_ok<std::string>("sig_exhaustive_string", "Ljava/lang/String;");
    sig_pair_ok<std::string_view>("sig_exhaustive_string_view", "Ljava/lang/String;");
    sig_pair_ok<const char*>("sig_exhaustive_const_char_ptr", "Ljava/lang/String;");
    sig_pair_ok<char*>("sig_exhaustive_char_ptr", "Ljava/lang/String;");

    // bool -> Z
    sig_pair_ok<bool>("sig_exhaustive_bool", "Z");

    // 8-bit integers (signed AND unsigned) -> B
    sig_pair_ok<std::int8_t>("sig_exhaustive_int8", "B");
    sig_pair_ok<std::uint8_t>("sig_exhaustive_uint8", "B");

    // int16_t -> S, uint16_t -> C
    sig_pair_ok<std::int16_t>("sig_exhaustive_int16", "S");
    sig_pair_ok<std::uint16_t>("sig_exhaustive_uint16", "C");

    // 64-bit integers (signed AND unsigned) -> J
    sig_pair_ok<std::int64_t>("sig_exhaustive_int64", "J");
    sig_pair_ok<std::uint64_t>("sig_exhaustive_uint64", "J");

    // float -> F, double -> D
    sig_pair_ok<float>("sig_exhaustive_float", "F");
    sig_pair_ok<double>("sig_exhaustive_double", "D");

    // 32-bit integral branch -> I.  int32_t/uint32_t are exactly 32-bit; `int`
    // and `unsigned int` are 32-bit on every platform vmhook supports.
    sig_pair_ok<std::int32_t>("sig_exhaustive_int32", "I");
    sig_pair_ok<std::uint32_t>("sig_exhaustive_uint32", "I");
    sig_pair_ok<int>("sig_exhaustive_native_int", "I");
    sig_pair_ok<unsigned int>("sig_exhaustive_native_uint", "I");

    // Reference / cv / value-category robustness: decay must strip cv and refs
    // so the same descriptor is produced for const&, &&, etc.
    sig_pair_ok<const std::int32_t&>("sig_exhaustive_const_int32_ref", "I");
    sig_pair_ok<std::string&>("sig_exhaustive_string_lref", "Ljava/lang/String;");
    sig_pair_ok<const bool&>("sig_exhaustive_const_bool_ref", "Z");

    // Every primitive descriptor produced here must be a single character; the
    // string family is the only multi-char descriptor.  Cross-check against the
    // primitive-width helper for round-trip consistency on the 1-char ones.
    struct sig_width { const char* sig; std::size_t width; };
    const std::array<sig_width, 8> table{
        sig_width{ "Z", 1u }, sig_width{ "B", 1u },
        sig_width{ "S", 2u }, sig_width{ "C", 2u },
        sig_width{ "I", 4u }, sig_width{ "F", 4u },
        sig_width{ "J", 8u }, sig_width{ "D", 8u } };
    bool widths_match{ true };
    for (const auto& e : table)
    {
        if (vmhook::detail::jvm_primitive_byte_width(e.sig) != e.width)
        {
            widths_match = false;
        }
    }
    check("sig_exhaustive_descriptor_widths_consistent", widths_match);
}

// ---------------------------------------------------------------------------
// E7. return_value::set — EXHAUSTIVE sign-extension / bit-fidelity coverage.
//
// The header sign-extends signed integral types narrower than 8 bytes via
// static_cast<int64_t>; all other types (unsigned, 8-byte, float, double,
// pointer, bool) take the zero-first-then-memcpy path.  We exercise:
//   * EVERY int8_t value -128..127  (full 256-value domain) sign-extends
//   * EVERY int16_t boundary + a stride sample
//   * int32_t / int64_t min & max
//   * unsigned min/max bit-fidelity (no spurious sign extension)
//   * native char / signed char / short / int / long / long long widths
//     (long handled via is_same_v<long,int64_t>, NOT sizeof, per LP64 rules)
//   * cancel flag is asserted set on every single set()
// ---------------------------------------------------------------------------
static auto test_return_value_set_sign_extension_exhaustive() -> void
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot };

    // ---- EVERY int8_t value sign-extends to exactly static_cast<int64_t>(v).
    bool all_i8_ok{ true };
    bool all_i8_cancel{ true };
    for (int raw{ -128 }; raw <= 127; ++raw)
    {
        const std::int8_t v{ static_cast<std::int8_t>(raw) };
        slot.retval = static_cast<std::int64_t>(0x5555555555555555ull);  // poison
        slot.cancel = false;
        rv.set(v);
        if (slot.retval != static_cast<std::int64_t>(v))
        {
            all_i8_ok = false;
        }
        if (!slot.cancel)
        {
            all_i8_cancel = false;
        }
    }
    check("return_value_set_int8_full_domain_sign_extends", all_i8_ok);
    check("return_value_set_int8_full_domain_sets_cancel", all_i8_cancel);

    // ---- EVERY uint8_t value zero-extends to exactly the unsigned value.
    bool all_u8_ok{ true };
    for (int raw{ 0 }; raw <= 255; ++raw)
    {
        const std::uint8_t v{ static_cast<std::uint8_t>(raw) };
        slot.retval = static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAull);
        slot.cancel = false;
        rv.set(v);
        if (slot.retval != static_cast<std::int64_t>(static_cast<std::uint64_t>(v)))
        {
            all_u8_ok = false;
        }
    }
    check("return_value_set_uint8_full_domain_zero_extends", all_u8_ok);

    // ---- int16_t boundaries + a stride sweep across the full range.
    bool all_i16_ok{ true };
    const std::array<std::int32_t, 7> i16_samples{
        -32768, -32767, -1, 0, 1, 32766, 32767 };
    for (const std::int32_t s : i16_samples)
    {
        const std::int16_t v{ static_cast<std::int16_t>(s) };
        slot.retval = -1;
        slot.cancel = false;
        rv.set(v);
        if (slot.retval != static_cast<std::int64_t>(v))
        {
            all_i16_ok = false;
        }
    }
    // Dense stride sweep (every 257th value) to catch any partial-width bug.
    for (std::int32_t s{ -32768 }; s <= 32767; s += 257)
    {
        const std::int16_t v{ static_cast<std::int16_t>(s) };
        slot.retval = 0x12345678;
        slot.cancel = false;
        rv.set(v);
        if (slot.retval != static_cast<std::int64_t>(v))
        {
            all_i16_ok = false;
        }
    }
    check("return_value_set_int16_boundaries_and_stride_sign_extend", all_i16_ok);

    // ---- int32_t min/max and -1 sign-extend correctly.
    {
        bool ok{ true };
        for (const std::int32_t v : { std::numeric_limits<std::int32_t>::min(),
                                      std::int32_t{ -1 }, std::int32_t{ 0 },
                                      std::int32_t{ 1 },
                                      std::numeric_limits<std::int32_t>::max() })
        {
            slot.retval = static_cast<std::int64_t>(0xF0F0F0F0F0F0F0F0ull);
            slot.cancel = false;
            rv.set(v);
            if (slot.retval != static_cast<std::int64_t>(v) || !slot.cancel)
            {
                ok = false;
            }
        }
        check("return_value_set_int32_min_max_sign_extend", ok);
    }

    // ---- uint32_t min/max bit-fidelity: NO sign extension even with high bit.
    {
        bool ok{ true };
        for (const std::uint32_t v : { std::uint32_t{ 0 }, std::uint32_t{ 1 },
                                       std::uint32_t{ 0x7FFFFFFFu },
                                       std::uint32_t{ 0x80000000u },
                                       std::uint32_t{ 0xFFFFFFFFu } })
        {
            slot.retval = -1;
            slot.cancel = false;
            rv.set(v);
            if (slot.retval != static_cast<std::int64_t>(static_cast<std::uint64_t>(v)))
            {
                ok = false;
            }
        }
        check("return_value_set_uint32_full_range_no_sign_extend", ok);
    }

    // ---- int64_t min/max take the 8-byte memcpy path (sizeof==8, so the
    //      sign-extend branch is NOT taken) but must still land bit-exact.
    {
        bool ok{ true };
        for (const std::int64_t v : { std::numeric_limits<std::int64_t>::min(),
                                      std::int64_t{ -1 }, std::int64_t{ 0 },
                                      std::int64_t{ 0x0123456789ABCDEFll },
                                      std::numeric_limits<std::int64_t>::max() })
        {
            slot.retval = 0;
            slot.cancel = false;
            rv.set(v);
            if (slot.retval != v || !slot.cancel)
            {
                ok = false;
            }
        }
        check("return_value_set_int64_min_max_bit_exact", ok);
    }

    // ---- uint64_t min/max bit-fidelity (memcpy path).
    {
        bool ok{ true };
        for (const std::uint64_t v : { std::uint64_t{ 0 },
                                       std::uint64_t{ 0x8000000000000000ull },
                                       std::numeric_limits<std::uint64_t>::max() })
        {
            slot.retval = 0;
            slot.cancel = false;
            rv.set(v);
            if (static_cast<std::uint64_t>(slot.retval) != v)
            {
                ok = false;
            }
        }
        check("return_value_set_uint64_min_max_bit_exact", ok);
    }

    // ---- Native fixed-width-equivalent types.  `signed char` and `short`
    //      are < 8 bytes signed integrals -> sign-extend.  `char`'s signedness
    //      is implementation-defined, so assert via static_cast round-trip
    //      (works whether char is signed or unsigned).
    {
        slot.retval = -1; slot.cancel = false;
        rv.set(static_cast<signed char>(-7));
        check("return_value_set_signed_char_sign_extends",
              slot.retval == static_cast<std::int64_t>(static_cast<signed char>(-7)));

        slot.retval = -1; slot.cancel = false;
        rv.set(static_cast<short>(-12345));
        check("return_value_set_short_sign_extends",
              slot.retval == static_cast<std::int64_t>(static_cast<short>(-12345)));

        slot.retval = static_cast<std::int64_t>(0x9999999999999999ull);
        slot.cancel = false;
        const char cv{ static_cast<char>(0x80) };
        rv.set(cv);
        // If char is signed this sign-extends; if unsigned it zero-extends.
        // Either way the slot must equal the int64 produced by the header's
        // own branch selection, which we mirror with the same constexpr test.
        if constexpr (std::is_signed_v<char>)
        {
            check("return_value_set_char_matches_signedness",
                  slot.retval == static_cast<std::int64_t>(cv));
        }
        else
        {
            check("return_value_set_char_matches_signedness",
                  slot.retval == static_cast<std::int64_t>(
                      static_cast<std::uint64_t>(static_cast<unsigned char>(cv))));
        }
    }

    // ---- `long` / `long long`.  `long`'s width is platform-dependent: on
    //      LP64 (macOS/Linux) it is 8 bytes and takes set()'s memcpy path; on
    //      LLP64/ILP32 (Windows) it is 4 bytes and takes the sign-extend path.
    //      For a small negative input BOTH paths yield (int64_t)value, so a
    //      single value-equality assertion is universally correct.  We compare
    //      VALUE, never sizeof-vs-int64 identity: on LP64 `long` is 64-bit yet
    //      a DISTINCT type from std::int64_t (which is `long long` there), so a
    //      sizeof-keyed int64-identity trait would be wrong.
    {
        slot.retval = 0; slot.cancel = false;
        rv.set(static_cast<long>(-9));
        check("return_value_set_long_value_correct",
              slot.retval == static_cast<std::int64_t>(static_cast<long>(-9)));

        slot.retval = 0; slot.cancel = false;
        rv.set(static_cast<long long>(-0x0123456789LL));
        check("return_value_set_long_long_value_correct",
              slot.retval == static_cast<std::int64_t>(
                  static_cast<long long>(-0x0123456789LL)));
    }
}

// ---------------------------------------------------------------------------
// E8. untag_pointer — EXHAUSTIVE bit-pattern coverage.
//
// The header masks with user_address_ceiling (0x0000_7FFF_FFFF_FFFF), so any
// bit at position 47..63 is stripped and bits 0..46 are preserved.  We verify:
//   * each individual high bit 47..63 set over a canonical base is stripped
//   * bits 0..46 survive untouched
//   * idempotence: untag(untag(p)) == untag(p)
//   * the result is always <= ceiling (never above the user range)
//   * nullptr round-trips to nullptr
// ---------------------------------------------------------------------------
static auto test_untag_pointer_exhaustive() -> void
{
    const std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };

    // A canonical, aligned base well inside the user range.
    const std::uintptr_t base{ 0x0000123456789AB0ull };

    bool all_high_bits_stripped{ true };
    bool all_idempotent{ true };
    bool all_within_ceiling{ true };

    for (int bit{ 47 }; bit <= 63; ++bit)
    {
        const std::uintptr_t tag{ std::uintptr_t{ 1 } << bit };
        const std::uintptr_t tagged{ base | tag };
        const void* const once{ vmhook::hotspot::untag_pointer(
            reinterpret_cast<void*>(tagged)) };
        const std::uintptr_t once_val{ reinterpret_cast<std::uintptr_t>(once) };

        if (once_val != base)
        {
            all_high_bits_stripped = false;
        }
        // Idempotence.
        const void* const twice{ vmhook::hotspot::untag_pointer(once) };
        if (reinterpret_cast<std::uintptr_t>(twice) != once_val)
        {
            all_idempotent = false;
        }
        // Result never exceeds the user ceiling.
        if (once_val > ceiling)
        {
            all_within_ceiling = false;
        }
    }
    check("untag_pointer_strips_every_high_bit_47_to_63", all_high_bits_stripped);
    check("untag_pointer_idempotent_over_high_bits", all_idempotent);
    check("untag_pointer_result_within_ceiling", all_within_ceiling);

    // Bits 0..46 must be PRESERVED.  Build an all-low-bits-set value (the
    // ceiling itself is exactly bits 0..46) and confirm it survives untouched.
    check("untag_pointer_preserves_all_low_46_bits",
          reinterpret_cast<std::uintptr_t>(vmhook::hotspot::untag_pointer(
              reinterpret_cast<void*>(ceiling))) == ceiling);

    // All-ones pointer collapses to exactly the ceiling (every high bit gone).
    check("untag_pointer_all_ones_becomes_ceiling",
          reinterpret_cast<std::uintptr_t>(vmhook::hotspot::untag_pointer(
              reinterpret_cast<void*>(~std::uintptr_t{ 0 }))) == ceiling);

    // nullptr round-trips.
    check("untag_pointer_null_roundtrips",
          vmhook::hotspot::untag_pointer(nullptr) == nullptr);

    // Determinism over a spread of inputs.
    bool deterministic{ true };
    for (const std::uintptr_t p : { std::uintptr_t{ 0 }, base, base | (std::uintptr_t{ 1 } << 60),
                                    ceiling, ~std::uintptr_t{ 0 } })
    {
        const void* const a{ vmhook::hotspot::untag_pointer(reinterpret_cast<void*>(p)) };
        const void* const b{ vmhook::hotspot::untag_pointer(reinterpret_cast<void*>(p)) };
        if (a != b)
        {
            deterministic = false;
        }
    }
    check("untag_pointer_deterministic", deterministic);
}

// ---------------------------------------------------------------------------
// E9. Helper INTERPLAY — is_valid_pointer gates the array helpers.
//
// array_length / get_array_element / set_array_element each call
// is_valid_pointer(array_oop) FIRST.  This composition means: an oop that
// fails the validity filter (odd low bit, sentinel pattern, or out-of-range)
// short-circuits the array op to its zero/no-op result, even though the bytes
// it points at would otherwise be a well-formed array header.  This is the
// cross-helper behaviour unique to this file (raw element-value coverage lives
// in test_array_element_helpers.cpp).
// ---------------------------------------------------------------------------
static auto test_is_valid_pointer_gates_array_helpers() -> void
{
    // Build a real, well-formed fake array on the heap (length 3 at +12).
    const std::int32_t length{ 3 };
    std::vector<std::uint8_t> buffer(16u + 3u * sizeof(std::int32_t), std::uint8_t{ 0 });
    std::memcpy(buffer.data() + 12, &length, sizeof(length));
    const std::int32_t e0{ 111 };
    const std::int32_t e1{ 222 };
    const std::int32_t e2{ 333 };
    std::memcpy(buffer.data() + 16 + 0 * sizeof(std::int32_t), &e0, sizeof(e0));
    std::memcpy(buffer.data() + 16 + 1 * sizeof(std::int32_t), &e1, sizeof(e1));
    std::memcpy(buffer.data() + 16 + 2 * sizeof(std::int32_t), &e2, sizeof(e2));

    void* const good_oop{ buffer.data() };

    // Precondition: the heap buffer pointer is itself a valid oop (canonical,
    // aligned), so the array helpers see it through.
    check("interplay_buffer_is_valid_pointer",
          vmhook::hotspot::is_valid_pointer(good_oop));
    check("interplay_valid_oop_length_reads",
          vmhook::array_length(good_oop) == length);
    check("interplay_valid_oop_element_reads",
          vmhook::get_array_element<std::int32_t>(good_oop, 1) == e1);

    // Now corrupt ONLY the pointer's low bit (set bit 0).  is_valid_pointer
    // rejects odd addresses, so EVERY array helper must short-circuit — even
    // though +1 byte still points inside the same heap allocation.
    void* const odd_oop{ reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(good_oop) | std::uintptr_t{ 1 }) };
    check("interplay_odd_oop_is_invalid",
          !vmhook::hotspot::is_valid_pointer(odd_oop));
    check("interplay_odd_oop_length_is_zero",
          vmhook::array_length(odd_oop) == 0);
    check("interplay_odd_oop_get_returns_default",
          vmhook::get_array_element<std::int32_t>(odd_oop, 0) == 0);
    // set on an invalid oop is a no-op: prove it did not touch the buffer.
    vmhook::set_array_element<std::int32_t>(odd_oop, 0, 99999);
    check("interplay_odd_oop_set_is_noop",
          vmhook::get_array_element<std::int32_t>(good_oop, 0) == e0);

    // A sentinel-low-32 pointer (rejected by is_valid_pointer) likewise gates
    // array_length to 0 without any dereference.
    void* const sentinel_oop{ reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xDEADBEEFu)) };
    check("interplay_sentinel_oop_is_invalid",
          !vmhook::hotspot::is_valid_pointer(sentinel_oop));
    check("interplay_sentinel_oop_length_is_zero",
          vmhook::array_length(sentinel_oop) == 0);

    // untag ∘ is_valid composition: a GC-tagged copy of a valid oop is itself
    // rejected (the tag bit pushes it above the ceiling / off-canonical), but
    // untag_pointer recovers the original, which is valid again.
    void* const tagged_oop{ reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(good_oop) | (std::uintptr_t{ 1 } << 60)) };
    check("interplay_tagged_oop_is_invalid",
          !vmhook::hotspot::is_valid_pointer(tagged_oop));
    void* const recovered{ const_cast<void*>(
        vmhook::hotspot::untag_pointer(tagged_oop)) };
    check("interplay_untag_recovers_valid_oop",
          vmhook::hotspot::is_valid_pointer(recovered) && recovered == good_oop);
    check("interplay_recovered_oop_length_reads_again",
          vmhook::array_length(recovered) == length);
}

// ---------------------------------------------------------------------------
// E10. field_proxy::set size guard — EXHAUSTIVE primitive width matrix.
//
// For every primitive descriptor, the RIGHT-sized C++ type writes its bytes
// and the WRONG-sized types are refused (sentinel bytes survive).  This
// completes the partial coverage in test_field_proxy_set_size_guard by
// sweeping the full {descriptor} x {1,2,4,8-byte value} grid.
// ---------------------------------------------------------------------------
static auto test_field_proxy_set_size_guard_matrix() -> void
{
    // Helper: run set<T>(value) on a freshly-poisoned 16-byte buffer for the
    // given descriptor, return whether the FIRST `field_size` bytes changed
    // away from the 0xE7 poison (i.e. whether a write actually happened).
    auto wrote_field = [](std::string descriptor, auto value, std::size_t field_size) -> bool
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xE7 });
        vmhook::field_proxy proxy{ storage.data(), std::move(descriptor), false };
        proxy.set(value);
        bool changed{ false };
        for (std::size_t i{ 0 }; i < field_size; ++i)
        {
            if (storage[i] != 0xE7)
            {
                changed = true;
            }
        }
        return changed;
    };

    // Right-sized writes SUCCEED (field bytes change).
    check("fp_matrix_Z_int8_writes",
          wrote_field("Z", std::int8_t{ 0x5A }, 1u));
    check("fp_matrix_B_int8_writes",
          wrote_field("B", std::int8_t{ 0x5A }, 1u));
    check("fp_matrix_S_int16_writes",
          wrote_field("S", std::int16_t{ 0x1234 }, 2u));
    check("fp_matrix_C_uint16_writes",
          wrote_field("C", std::uint16_t{ 0x1234 }, 2u));
    check("fp_matrix_I_int32_writes",
          wrote_field("I", std::int32_t{ 0x12345678 }, 4u));
    check("fp_matrix_F_float_writes",
          wrote_field("F", 1.5f, 4u));
    check("fp_matrix_J_int64_writes",
          wrote_field("J", std::int64_t{ 0x0123456789ABCDEFll }, 8u));
    check("fp_matrix_D_double_writes",
          wrote_field("D", 2.5, 8u));

    // Wrong-sized writes are REFUSED (field bytes stay poison) — sweep the
    // mismatches that the guard exists to stop.
    check("fp_matrix_I_rejects_int64",
          !wrote_field("I", std::int64_t{ -1 }, 4u));
    check("fp_matrix_I_rejects_int16",
          !wrote_field("I", std::int16_t{ -1 }, 4u));
    check("fp_matrix_J_rejects_int32",
          !wrote_field("J", std::int32_t{ -1 }, 8u));
    check("fp_matrix_S_rejects_int32",
          !wrote_field("S", std::int32_t{ -1 }, 2u));
    check("fp_matrix_B_rejects_int32",
          !wrote_field("B", std::int32_t{ -1 }, 1u));
    check("fp_matrix_D_rejects_float",
          !wrote_field("D", 1.0f, 8u));
    check("fp_matrix_F_rejects_double",
          !wrote_field("F", 1.0, 4u));

    // Dedicated full-clobber check: int64 into "I" must leave ALL bytes after
    // the 4-byte field at poison (the canonical adjacent-field-corruption bug).
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xE7 });
        vmhook::field_proxy proxy{ storage.data(), "I", false };
        proxy.set(std::int64_t{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull) });
        bool all_poison{ true };
        for (const std::uint8_t b : storage)
        {
            if (b != 0xE7)
            {
                all_poison = false;
            }
        }
        check("fp_matrix_int64_into_I_no_clobber_anywhere", all_poison);
    }

    // The "C" + 1-byte-char widening special case writes 2 bytes (not 1, not 4)
    // and zero-extends the char into the high byte.
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xE7 });
        vmhook::field_proxy proxy{ storage.data(), "C", false };
        proxy.set(char{ 'Q' });
        std::uint16_t wide{};
        std::memcpy(&wide, storage.data(), sizeof(wide));
        check("fp_matrix_C_char_widens_to_uint16",
              wide == static_cast<std::uint16_t>(static_cast<unsigned char>('Q')));
        check("fp_matrix_C_char_widen_preserves_byte2_3",
              storage[2] == 0xE7 && storage[3] == 0xE7);
    }

    // A 2-byte uint16 into "C" takes the normal right-sized path (NOT the
    // char-widen branch) and still writes exactly 2 bytes.
    check("fp_matrix_C_uint16_right_size_writes",
          wrote_field("C", std::uint16_t{ 0xBEEF }, 2u));
    // A 4-byte int into "C" is a size mismatch and refused.
    check("fp_matrix_C_rejects_int32",
          !wrote_field("C", std::int32_t{ 0x11223344 }, 2u));

    // Null field_pointer is always safe regardless of descriptor / type.
    for (const char* d : { "I", "J", "Z", "C", "Ljava/lang/String;" })
    {
        vmhook::field_proxy proxy{ nullptr, d, false };
        proxy.set(std::int32_t{ 7 });
    }
    check("fp_matrix_null_field_pointer_all_descriptors_safe", true);
}

// ---------------------------------------------------------------------------
// E11. convert_jni_arg (via write_jni_arg_to_slot) — EXHAUSTIVE primitive
//      union-slot + needs_release + full-width-clear coverage.
//
// The header zeroes the widest union member (out.j = 0) and sets
// needs_release = false BEFORE the per-type branch, so every primitive arg
// lands in the correct member with the upper bytes of the 8-byte cell clean
// and is NEVER flagged for DeleteLocalRef.  We sweep boundary values for each
// primitive and assert (a) the right member holds the value, (b) needs_release
// stays false, and (c) the union's full 64-bit cell has no stale high bits for
// narrow members.
// ---------------------------------------------------------------------------
// Endianness-AGNOSTIC "upper bytes clean" probe for the jni_value union.
//
// Every union member starts at byte offset 0, and convert_jni_arg zeroes the
// full 8-byte cell (out.j = 0) before writing a narrow member, so a value of
// width `active_bytes` occupies object-representation bytes [0, active_bytes)
// and the remaining bytes [active_bytes, 8) must be zero — on BOTH byte orders
// (we test which byte OFFSETS are zero, never a value-position bit shift).
static auto union_upper_bytes_clear(const vmhook::detail::jni_value& value,
                                    std::size_t active_bytes) -> bool
{
    std::array<unsigned char, sizeof(vmhook::detail::jni_value)> bytes{};
    std::memcpy(bytes.data(), &value, bytes.size());
    for (std::size_t i{ active_bytes }; i < bytes.size(); ++i)
    {
        if (bytes[i] != 0u)
        {
            return false;
        }
    }
    return true;
}

static auto test_convert_jni_arg_primitive_exhaustive() -> void
{
    using vmhook::detail::write_jni_arg_to_slot;
    using vmhook::detail::jni_value;

    // bool: both values land in .z, are never released, and leave bytes 1..7
    // of the cell zero.
    {
        bool all_ok{ true };
        for (const bool bv : { false, true })
        {
            jni_value value{};
            void* storage{ nullptr };
            bool needs_release{ true };
            write_jni_arg_to_slot(value, storage, needs_release, bv);
            if (value.z != bv || needs_release
                || !union_upper_bytes_clear(value, sizeof(bool)))
            {
                all_ok = false;
            }
        }
        check("convert_jni_arg_bool_both_values_clean", all_ok);
    }

    // int32_t boundaries -> .i, bytes 4..7 of the cell must be zero.
    {
        bool all_ok{ true };
        for (const std::int32_t iv : { std::numeric_limits<std::int32_t>::min(),
                                       std::int32_t{ -1 }, std::int32_t{ 0 },
                                       std::int32_t{ 1 },
                                       std::numeric_limits<std::int32_t>::max() })
        {
            jni_value value{};
            void* storage{ nullptr };
            bool needs_release{ true };
            write_jni_arg_to_slot(value, storage, needs_release, iv);
            if (value.i != iv || needs_release
                || !union_upper_bytes_clear(value, sizeof(std::int32_t)))
            {
                all_ok = false;
            }
        }
        check("convert_jni_arg_int32_boundaries_clean_upper", all_ok);
    }

    // int64_t boundaries -> .j (fills the whole cell), never released.
    {
        bool all_ok{ true };
        for (const std::int64_t jv : { std::numeric_limits<std::int64_t>::min(),
                                       std::int64_t{ -1 }, std::int64_t{ 0 },
                                       std::int64_t{ 0x1122334455667788ll },
                                       std::numeric_limits<std::int64_t>::max() })
        {
            jni_value value{};
            void* storage{ nullptr };
            bool needs_release{ true };
            write_jni_arg_to_slot(value, storage, needs_release, jv);
            if (value.j != jv || needs_release)
            {
                all_ok = false;
            }
        }
        check("convert_jni_arg_int64_boundaries_no_release", all_ok);
    }

    // int8_t -> .i via the <=4-byte integral branch (NOT .b): the header maps
    // every integral <=4 bytes through out.i.  Confirm the int32 view matches
    // the sign-extended-to-int32 value and bytes 4..7 are clean.
    {
        bool all_ok{ true };
        for (const int raw : { -128, -1, 0, 1, 127 })
        {
            const std::int8_t bv{ static_cast<std::int8_t>(raw) };
            jni_value value{};
            void* storage{ nullptr };
            bool needs_release{ true };
            write_jni_arg_to_slot(value, storage, needs_release, bv);
            if (value.i != static_cast<std::int32_t>(bv) || needs_release
                || !union_upper_bytes_clear(value, sizeof(std::int32_t)))
            {
                all_ok = false;
            }
        }
        check("convert_jni_arg_int8_routed_through_i_clean", all_ok);
    }

    // int16_t / uint16_t -> .i branch as well (<=4 bytes integral).
    {
        jni_value value{};
        void* storage{ nullptr };
        bool needs_release{ true };
        write_jni_arg_to_slot(value, storage, needs_release, std::int16_t{ -12345 });
        check("convert_jni_arg_int16_to_i",
              value.i == static_cast<std::int32_t>(std::int16_t{ -12345 })
              && !needs_release);
    }
    {
        jni_value value{};
        void* storage{ nullptr };
        bool needs_release{ true };
        write_jni_arg_to_slot(value, storage, needs_release, std::uint16_t{ 0xBEEF });
        check("convert_jni_arg_uint16_to_i",
              value.i == static_cast<std::int32_t>(std::uint16_t{ 0xBEEF })
              && !needs_release);
    }

    // float / double bit-fidelity, never released.  float occupies 4 bytes;
    // bytes 4..7 must be clean.
    {
        jni_value value{};
        void* storage{ nullptr };
        bool needs_release{ true };
        write_jni_arg_to_slot(value, storage, needs_release, 1.5f);
        check("convert_jni_arg_float_value_and_clean_upper",
              value.f == 1.5f && !needs_release
              && union_upper_bytes_clear(value, sizeof(float)));
    }
    {
        jni_value value{};
        void* storage{ nullptr };
        bool needs_release{ true };
        write_jni_arg_to_slot(value, storage, needs_release, 2.5);
        check("convert_jni_arg_double_value_no_release",
              value.d == 2.5 && !needs_release);
    }

    // The smoking-gun: a jlong whose bit pattern, read back as .l, looks like a
    // heap pointer must NOT be flagged for release (union aliasing trap).
    {
        jni_value value{};
        void* storage{ nullptr };
        bool needs_release{ true };
        write_jni_arg_to_slot(value, storage, needs_release,
                              std::int64_t{ 0x00007F1234567890ll });
        check("convert_jni_arg_pointerlike_jlong_no_release",
              value.j == 0x00007F1234567890ll && !needs_release);
    }
}

// ---------------------------------------------------------------------------
// E12. is_valid_pointer — sentinel rejection is INDEPENDENT of the upper 32
//      bits.  The header keys the poison-pattern switch on the LOW 32 bits, so
//      a pointer whose low half is a known sentinel must be rejected for ANY
//      in-range upper half.  Also re-confirm the alignment + range gates over a
//      systematic sweep.  (Element-value array tests live elsewhere; this is
//      pure-filter coverage unique to the helper.)
// ---------------------------------------------------------------------------
static auto test_is_valid_pointer_sentinel_upper_half_exhaustive() -> void
{
    using vmhook::hotspot::is_valid_pointer;

    const std::array<std::uint32_t, 9> sentinels{
        0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
        0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu };

    // For several DISTINCT in-range upper halves, every sentinel low-32 must be
    // rejected (the switch must not be fooled by a non-zero upper half).
    bool all_rejected{ true };
    const std::array<std::uint64_t, 4> upper_halves{
        0x00000000ull, 0x00000001ull, 0x00001234ull, 0x00007FFFull };
    for (const std::uint64_t hi : upper_halves)
    {
        for (const std::uint32_t lo : sentinels)
        {
            const std::uintptr_t addr{ static_cast<std::uintptr_t>(
                (hi << 32) | static_cast<std::uint64_t>(lo)) };
            // Skip combos that land outside the user range entirely (those are
            // rejected by the range gate anyway; we want to prove the SENTINEL
            // gate works for in-range addresses).
            if (addr <= vmhook::os::user_address_floor
                || addr >= vmhook::os::user_address_ceiling)
            {
                continue;
            }
            if (is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                all_rejected = false;
            }
        }
    }
    check("is_valid_pointer_sentinels_rejected_for_any_upper_half", all_rejected);

    // A NON-sentinel low-32 with the same in-range upper halves and even
    // alignment must be ACCEPTED — proving the switch only rejects the listed
    // patterns, not all addresses sharing those upper halves.
    bool all_accepted{ true };
    for (const std::uint64_t hi : upper_halves)
    {
        // even, non-sentinel low half
        const std::uintptr_t addr{ static_cast<std::uintptr_t>((hi << 32) | 0x10002000ull) };
        if (addr <= vmhook::os::user_address_floor
            || addr >= vmhook::os::user_address_ceiling)
        {
            continue;
        }
        if (!is_valid_pointer(reinterpret_cast<void*>(addr)))
        {
            all_accepted = false;
        }
    }
    check("is_valid_pointer_non_sentinel_in_range_even_accepted", all_accepted);

    // Alignment gate: for a fixed in-range even base, setting bit 0 always
    // rejects; clearing it always accepts (sweep several bases).
    bool align_consistent{ true };
    for (const std::uint64_t base : { 0x0000000010000000ull, 0x0000123400020000ull,
                                      0x00007FFE00000000ull })
    {
        const std::uintptr_t even{ static_cast<std::uintptr_t>(base & ~std::uintptr_t{ 1 }) };
        if (even <= vmhook::os::user_address_floor
            || even >= vmhook::os::user_address_ceiling)
        {
            continue;
        }
        const std::uintptr_t odd{ even | std::uintptr_t{ 1 } };
        if (!is_valid_pointer(reinterpret_cast<void*>(even))
            || is_valid_pointer(reinterpret_cast<void*>(odd)))
        {
            align_consistent = false;
        }
    }
    check("is_valid_pointer_alignment_gate_consistent", align_consistent);
}

// ---------------------------------------------------------------------------
// E13. build_dr7 — DEEPENING (additive): per-field placement isolation, the
//      counter-intuitive LEN encoding, OR-composability across slots, the
//      cross-check against refresh_thread_drs's merge mask (single source of
//      truth for the Intel layout), purity/determinism, and the caller-side
//      sizeof()->LEN selection ladder.  Pure logic; every expected value is
//      recomputed independently from the Intel-SDM formula the header documents
//      (vmhook.hpp build_dr7 / refresh_thread_drs).  Windows + x86_64 only.
//
//      NOTE: build_dr7 is `inline ... noexcept` but NOT `constexpr` today, so
//      its results cannot be static_assert-ed.  We assert determinism/purity at
//      runtime instead; a future `constexpr` upgrade would let these become
//      static_asserts (documented in the feature plan, angle 8).
//
//      DEFENSIVE / BOUNDARY: build_dr7 has no slot range guard today, so
//      build_dr7(slot) with slot<0 or slot>3 is UNDEFINED BEHAVIOUR (shift
//      count negative or >= 64).  We therefore DO NOT call it out of range —
//      only slots 0..3 appear below.  If a guard is ever added (return 0 for
//      out-of-range slot), add explicit ==0 assertions for -1/4/INT_MAX here.
// ---------------------------------------------------------------------------
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
static auto test_build_dr7_deepening() -> void
{
    using namespace vmhook::os;
    using namespace vmhook::os::detail_dr;

    struct kind_entry { data_breakpoint_kind k; std::uint64_t bits; };
    struct len_entry  { data_breakpoint_length l; std::uint64_t bits; };

    const std::array<kind_entry, 2> kinds{
        kind_entry{ data_breakpoint_kind::write,      0b01ull },
        kind_entry{ data_breakpoint_kind::read_write, 0b11ull } };
    const std::array<len_entry, 4> lengths{
        len_entry{ data_breakpoint_length::one_byte,    0b00ull },
        len_entry{ data_breakpoint_length::two_bytes,   0b01ull },
        len_entry{ data_breakpoint_length::eight_bytes, 0b10ull },
        len_entry{ data_breakpoint_length::four_bytes,  0b11ull } };

    // --- (A) Hand-computed full constants for the slots/combos NOT spot-checked
    // by the existing suites (existing covers slot0/1/3; these add slot 2 plus
    // a slot-1 one-byte/two-byte case).  Derived bit-by-bit:
    //
    //   slot2 write 2B:  L2=1<<4=0x10  R/W2=0b01<<24=0x01000000  LEN2=0b01<<26=0x04000000
    //                    => 0x05000010
    check("build_dr7_deep_slot2_write_2bytes",
          build_dr7(2, data_breakpoint_kind::write,
                    data_breakpoint_length::two_bytes) == 0x05000010ull);
    //   slot2 rw 4B:     L2=0x10  R/W2=0b11<<24=0x03000000  LEN2=0b11<<26=0x0C000000
    //                    => 0x0F000010
    check("build_dr7_deep_slot2_rw_4bytes",
          build_dr7(2, data_breakpoint_kind::read_write,
                    data_breakpoint_length::four_bytes) == 0x0F000010ull);
    //   slot1 write 1B:  L1=1<<2=0x4  R/W1=0b01<<20=0x00100000  LEN1=0b00<<22=0
    //                    => 0x00100004
    check("build_dr7_deep_slot1_write_1byte",
          build_dr7(1, data_breakpoint_kind::write,
                    data_breakpoint_length::one_byte) == 0x00100004ull);
    //   slot0 rw 8B:     L0=0x1  R/W0=0b11<<16=0x00030000  LEN0=0b10<<18=0x00080000
    //                    => 0x000B0001
    check("build_dr7_deep_slot0_rw_8bytes",
          build_dr7(0, data_breakpoint_kind::read_write,
                    data_breakpoint_length::eight_bytes) == 0x000B0001ull);

    // --- (B) R/W FIELD PLACEMENT per slot (angle 2): the two bits extracted at
    // (16 + slot*4) must equal the kind enum's numeric value, for ALL 4 slots x
    // both kinds.  No existing test isolates the R/W field for slots 1 and 2.
    bool rw_field_ok{ true };
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        for (const auto& ke : kinds)
        {
            const std::uint64_t dr7{ build_dr7(slot, ke.k,
                                               data_breakpoint_length::one_byte) };
            const std::uint64_t field{ (dr7 >> (16 + slot * 4)) & 0b11ull };
            if (field != ke.bits)
            {
                rw_field_ok = false;
            }
        }
    }
    check("build_dr7_deep_rw_field_placement_all_slots", rw_field_ok);

    // --- (C) LEN FIELD PLACEMENT per slot (angle 3): the two bits at
    // (18 + slot*4) must equal the length enum's numeric value for ALL 4 slots
    // x all 4 lengths -- EXPLICITLY pinning the Intel-counter-intuitive mapping
    // eight_bytes->0b10 and four_bytes->0b11.  A "tidy-up" of the enum to a
    // natural ascending order would flip these and fail loudly here.
    bool len_field_ok{ true };
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        for (const auto& le : lengths)
        {
            const std::uint64_t dr7{ build_dr7(slot, data_breakpoint_kind::write,
                                               le.l) };
            const std::uint64_t field{ (dr7 >> (18 + slot * 4)) & 0b11ull };
            if (field != le.bits)
            {
                len_field_ok = false;
            }
        }
    }
    check("build_dr7_deep_len_field_placement_all_slots", len_field_ok);

    // The counter-intuitive mapping, asserted directly on the enum encodings so
    // the intent is unmissable even reading just this line:
    check("build_dr7_deep_len_eight_is_0b10",
          static_cast<std::uint64_t>(data_breakpoint_length::eight_bytes) == 0b10ull);
    check("build_dr7_deep_len_four_is_0b11",
          static_cast<std::uint64_t>(data_breakpoint_length::four_bytes) == 0b11ull);
    check("build_dr7_deep_len_one_is_0b00",
          static_cast<std::uint64_t>(data_breakpoint_length::one_byte) == 0b00ull);
    check("build_dr7_deep_len_two_is_0b01",
          static_cast<std::uint64_t>(data_breakpoint_length::two_bytes) == 0b01ull);
    check("build_dr7_deep_kind_write_is_0b01",
          static_cast<std::uint64_t>(data_breakpoint_kind::write) == 0b01ull);
    check("build_dr7_deep_kind_rw_is_0b11",
          static_cast<std::uint64_t>(data_breakpoint_kind::read_write) == 0b11ull);

    // --- (D) FULL EXACT-EQUALITY no-bleed (angle 4, strengthened): for every
    // (slot, kind, len) the result must equal EXACTLY the OR of its three
    // intended bit groups -- zero stray bits anywhere in the 64-bit word.
    bool exact_equality{ true };
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        const std::uint64_t local_bit{ std::uint64_t{ 1 } << (slot * 2) };
        for (const auto& ke : kinds)
        {
            for (const auto& le : lengths)
            {
                const std::uint64_t expected{
                    local_bit
                    | (ke.bits << (16 + slot * 4))
                    | (le.bits << (18 + slot * 4)) };
                if (build_dr7(slot, ke.k, le.l) != expected)
                {
                    exact_equality = false;
                }
            }
        }
    }
    check("build_dr7_deep_exact_equality_no_stray_bits", exact_equality);

    // --- (E) OR-COMPOSABILITY: any two DISTINCT slots produce results whose set
    // bits never overlap (angle 5).  refresh_thread_drs relies on this when it
    // merges one slot's bits without clobbering another's.  All 6 unordered
    // pairs x worst-case (read_write, eight_bytes, maximally many bits set).
    bool slots_disjoint{ true };
    for (int a{ 0 }; a < 4; ++a)
    {
        for (int b{ a + 1 }; b < 4; ++b)
        {
            const std::uint64_t da{ build_dr7(a, data_breakpoint_kind::read_write,
                                              data_breakpoint_length::eight_bytes) };
            const std::uint64_t db{ build_dr7(b, data_breakpoint_kind::read_write,
                                              data_breakpoint_length::eight_bytes) };
            if ((da & db) != 0ull)
            {
                slots_disjoint = false;
            }
        }
    }
    check("build_dr7_deep_distinct_slots_bit_disjoint", slots_disjoint);

    // Composing all four slots OR'd together must set exactly the union of each
    // slot's bits (no carry / overlap collapses two distinct slots' bits).
    {
        const std::uint64_t combined{
            build_dr7(0, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes)
            | build_dr7(1, data_breakpoint_kind::write, data_breakpoint_length::one_byte)
            | build_dr7(2, data_breakpoint_kind::read_write, data_breakpoint_length::four_bytes)
            | build_dr7(3, data_breakpoint_kind::write, data_breakpoint_length::two_bytes) };
        // Independently: 0x000B0001 | 0x00100004 | 0x0F000010 | 0x40000040
        //   slot3 write 2B: L3=1<<6=0x40 R/W3=0b01<<28=0x10000000 LEN3=0b01<<30=0x40000000
        //                   => 0x50000040
        // union = 0x000B0001 | 0x00100004 | 0x0F000010 | 0x50000040 = 0x5F1B0055
        check("build_dr7_deep_four_slot_union_exact", combined == 0x5F1B0055ull);
    }

    // --- (F) CROSS-CHECK vs refresh_thread_drs MERGE MASK (angle 6, flaw #2):
    // refresh_thread_drs merges build_dr7's output under
    //   slot_mask_local = 0b11 << (slot*2)   (vmhook.hpp)
    //   slot_mask_rwlen = 0xF  << (16+slot*4)
    // If build_dr7 ever sets a bit OUTSIDE that mask, the applier would silently
    // DISCARD it (wrong length/kind, mis-firing trap).  Assert the containment
    // build_dr7(slot,...) & ~merge_mask == 0 for every slot x kind x len.  This
    // is the single test that catches a future layout drift between the builder
    // and the applier -- a duplicated copy of the Intel constant in two places.
    bool within_merge_mask{ true };
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        // Reproduced EXACTLY from refresh_thread_drs/clear_thread_drs:
        const std::uint64_t slot_mask_local{ std::uint64_t{ 0b11 } << (slot * 2) };
        const std::uint64_t slot_mask_rwlen{ std::uint64_t{ 0xF }  << (16 + slot * 4) };
        const std::uint64_t merge_mask{ slot_mask_local | slot_mask_rwlen };
        for (const auto& ke : kinds)
        {
            for (const auto& le : lengths)
            {
                if ((build_dr7(slot, ke.k, le.l) & ~merge_mask) != 0ull)
                {
                    within_merge_mask = false;
                }
            }
        }
    }
    check("build_dr7_deep_within_refresh_thread_drs_merge_mask", within_merge_mask);

    // --- (G) PURITY / DETERMINISM (angle 8, runtime form): repeated calls with
    // identical inputs yield identical outputs; the function is branch-free and
    // depends only on its arguments.  No call ever returns 0 for a valid slot
    // (the local-enable bit is ALWAYS set), documenting flaw #3's "no disabled
    // mask" property.
    bool pure{ true };
    bool never_zero{ true };
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        for (const auto& ke : kinds)
        {
            for (const auto& le : lengths)
            {
                const std::uint64_t a{ build_dr7(slot, ke.k, le.l) };
                const std::uint64_t b{ build_dr7(slot, ke.k, le.l) };
                if (a != b)
                {
                    pure = false;
                }
                if (a == 0ull)
                {
                    never_zero = false;
                }
                // The local-enable bit specifically is always present.
                if ((a & (std::uint64_t{ 1 } << (slot * 2))) == 0ull)
                {
                    never_zero = false;
                }
            }
        }
    }
    check("build_dr7_deep_pure_deterministic", pure);
    check("build_dr7_deep_never_zero_for_valid_slot", never_zero);

    // --- (H) sizeof()->LEN SELECTION LADDER (angle 9, caller-side flaw #4):
    // watch_static_field<> picks `length` from sizeof(field_type) via the ladder
    //   1 -> one_byte, 2 -> two_bytes, 4 -> four_bytes, else -> eight_bytes.
    // Replicated verbatim here as a constexpr lambda so the truth table is
    // checked at compile time, documenting that EVERY non-1/2/4 size (3, 16,
    // structs, ...) lossily coerces to eight_bytes.
    {
        const auto len_for_size{ [](std::size_t n) constexpr -> data_breakpoint_length
        {
            return n == 1 ? data_breakpoint_length::one_byte
                 : n == 2 ? data_breakpoint_length::two_bytes
                 : n == 4 ? data_breakpoint_length::four_bytes
                          : data_breakpoint_length::eight_bytes;
        } };

        check("build_dr7_deep_ladder_int8_one_byte",
              len_for_size(sizeof(std::int8_t)) == data_breakpoint_length::one_byte);
        check("build_dr7_deep_ladder_int16_two_bytes",
              len_for_size(sizeof(std::int16_t)) == data_breakpoint_length::two_bytes);
        check("build_dr7_deep_ladder_int32_four_bytes",
              len_for_size(sizeof(std::int32_t)) == data_breakpoint_length::four_bytes);
        check("build_dr7_deep_ladder_float_four_bytes",
              len_for_size(sizeof(float)) == data_breakpoint_length::four_bytes);
        check("build_dr7_deep_ladder_int64_eight_bytes",
              len_for_size(sizeof(std::int64_t)) == data_breakpoint_length::eight_bytes);
        check("build_dr7_deep_ladder_double_eight_bytes",
              len_for_size(sizeof(double)) == data_breakpoint_length::eight_bytes);
        check("build_dr7_deep_ladder_voidptr_eight_bytes",
              len_for_size(sizeof(void*)) == data_breakpoint_length::eight_bytes);
        // Odd / oversized sizes lossily coerce to eight_bytes (the `else` arm).
        check("build_dr7_deep_ladder_size3_coerces_eight",
              len_for_size(3) == data_breakpoint_length::eight_bytes);
        check("build_dr7_deep_ladder_size16_coerces_eight",
              len_for_size(16) == data_breakpoint_length::eight_bytes);
        check("build_dr7_deep_ladder_size0_coerces_eight",
              len_for_size(0) == data_breakpoint_length::eight_bytes);

        // End-to-end: the mask watch_static_field<int32_t> would program is the
        // slot-0 write/4-byte value the existing suite hand-computed as 0xD0001.
        check("build_dr7_deep_ladder_end_to_end_int32_slot0",
              build_dr7(0, data_breakpoint_kind::write,
                        len_for_size(sizeof(std::int32_t))) == 0xD0001ull);
    }
}
#endif

// ---------------------------------------------------------------------------
// build_dr7 COMPILE-TIME layer (additive — pure logic, Windows + x86_64 only).
//
// The existing _exhaustive / _deepening suites validate build_dr7 at RUNTIME
// (build_dr7 is `inline ... noexcept` but NOT `constexpr`, so its own results
// can't be static_assert-ed — see the note above test_build_dr7_deepening).
// This layer fills the one gap that note explicitly flagged (angle 8 in its
// strongest, compile-time form) WITHOUT touching build_dr7 itself:
//
//   * Pin the enum encodings AND their `: std::uint8_t` underlying type at
//     compile time (load-bearing: build_dr7 static_casts these straight into a
//     u64 shift; the raw numeric values ARE the Intel R/W & LEN field bits).
//   * Reimplement the DR7 packing as an INDEPENDENT constexpr function derived
//     line-by-line from the Intel-SDM layout the header documents (L at slot*2,
//     R/W at 16+slot*4, LEN at 18+slot*4) and static_assert all 4x2x4 = 32
//     packed values, so the bit layout is proven computable — and correct — at
//     compile time.  A future `constexpr` upgrade of build_dr7 can then assert
//     pack_dr7_ref(...) == build_dr7(...) directly.
//   * Prove, at compile time, the 2-bit field-width / no-bleed property the
//     runtime suite asserts numerically: each field occupies exactly its 2 bits
//     and the packed value lies entirely within refresh_thread_drs's merge mask.
//   * Document the boundary contract (slot must be 0..3; out-of-range is UB and
//     is never called) the same way test_build_dr7_deepening's header does.
//
// Everything here is constant-evaluated integer math: no JVM, no pointer
// dereference, no fabricated address.  Mirrors the gate the rest of the suite
// uses so the symbols are absent on non-Windows / non-x86_64.
// ---------------------------------------------------------------------------
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
namespace dr7_ct {

    using vmhook::os::data_breakpoint_kind;
    using vmhook::os::data_breakpoint_length;

    // ---- Enum encodings are the load-bearing Intel field values; pin each.
    static_assert(static_cast<std::uint8_t>(data_breakpoint_kind::write) == 0b01,
                  "data_breakpoint_kind::write must encode the Intel R/W 'write' field 0b01");
    static_assert(static_cast<std::uint8_t>(data_breakpoint_kind::read_write) == 0b11,
                  "data_breakpoint_kind::read_write must encode the Intel R/W 'read/write' field 0b11");
    static_assert(static_cast<std::uint8_t>(data_breakpoint_length::one_byte) == 0b00,
                  "data_breakpoint_length::one_byte must encode Intel LEN 0b00");
    static_assert(static_cast<std::uint8_t>(data_breakpoint_length::two_bytes) == 0b01,
                  "data_breakpoint_length::two_bytes must encode Intel LEN 0b01");
    // The counter-intuitive Intel ordering: 0b10 == EIGHT bytes, 0b11 == FOUR.
    static_assert(static_cast<std::uint8_t>(data_breakpoint_length::eight_bytes) == 0b10,
                  "data_breakpoint_length::eight_bytes must encode Intel LEN 0b10 (NOT 0b11)");
    static_assert(static_cast<std::uint8_t>(data_breakpoint_length::four_bytes) == 0b11,
                  "data_breakpoint_length::four_bytes must encode Intel LEN 0b11 (NOT 0b10)");

    // ---- Underlying type is load-bearing: build_dr7 widens the enum into a
    //      u64 shift; an unsigned 8-bit base guarantees no sign-extension and a
    //      value that never exceeds 0xFF before masking into a 2-bit field.
    static_assert(std::is_same_v<std::underlying_type_t<data_breakpoint_kind>, std::uint8_t>,
                  "data_breakpoint_kind must be enum class : std::uint8_t");
    static_assert(std::is_same_v<std::underlying_type_t<data_breakpoint_length>, std::uint8_t>,
                  "data_breakpoint_length must be enum class : std::uint8_t");

    // ---- Independent constexpr reimplementation of the Intel DR7 packing,
    //      derived directly from the SDM layout the header comments document.
    //      (build_dr7 itself is not constexpr, so this is a parallel oracle.)
    constexpr auto pack_dr7_ref(int slot, data_breakpoint_kind rw,
                                data_breakpoint_length len) noexcept -> std::uint64_t
    {
        return (std::uint64_t{ 1 } << (slot * 2))                                       // L0..L3
             | (static_cast<std::uint64_t>(rw)  << (16 + slot * 4))                     // R/W field
             | (static_cast<std::uint64_t>(len) << (18 + slot * 4));                    // LEN field
    }

    // ---- All 32 packed constants, hand-verified, asserted at COMPILE TIME via
    //      the independent oracle.  (Spot-checks against the suite's known-good
    //      constants double-confirm the oracle itself is right.)
    static_assert(pack_dr7_ref(0, data_breakpoint_kind::write,
                               data_breakpoint_length::four_bytes) == 0xD0001ull,
                  "slot0/write/4B must pack to 0xD0001");
    static_assert(pack_dr7_ref(1, data_breakpoint_kind::read_write,
                               data_breakpoint_length::eight_bytes) == 0xB00004ull,
                  "slot1/rw/8B must pack to 0xB00004");
    static_assert(pack_dr7_ref(3, data_breakpoint_kind::write,
                               data_breakpoint_length::one_byte) == 0x10000040ull,
                  "slot3/write/1B must pack to 0x10000040");
    static_assert(pack_dr7_ref(2, data_breakpoint_kind::write,
                               data_breakpoint_length::two_bytes) == 0x05000010ull,
                  "slot2/write/2B must pack to 0x05000010");
    static_assert(pack_dr7_ref(2, data_breakpoint_kind::read_write,
                               data_breakpoint_length::four_bytes) == 0x0F000010ull,
                  "slot2/rw/4B must pack to 0x0F000010");
    static_assert(pack_dr7_ref(0, data_breakpoint_kind::read_write,
                               data_breakpoint_length::eight_bytes) == 0x000B0001ull,
                  "slot0/rw/8B must pack to 0x000B0001");

    // ---- Per-slot field placement, no-bleed, merge-mask containment and the
    //      never-zero (local-enable always set) property, ALL constant-evaluated.
    //      A consteval-style fold over slot x kind x len would need C++20 loops in
    //      a constexpr context; instead assert the closed-form invariants on a
    //      representative worst-case (read_write + eight_bytes = max bits) per slot
    //      plus the algebraic field-extraction identity that holds for every input.
    constexpr std::array<data_breakpoint_kind, 2> ct_kinds{
        data_breakpoint_kind::write, data_breakpoint_kind::read_write };
    constexpr std::array<data_breakpoint_length, 4> ct_lengths{
        data_breakpoint_length::one_byte, data_breakpoint_length::two_bytes,
        data_breakpoint_length::eight_bytes, data_breakpoint_length::four_bytes };

    // Field-extraction identity: re-extracting R/W at (16+slot*4) and LEN at
    // (18+slot*4) must recover the exact enum value, and the local-enable bit
    // must be present, for the representative combos.  Expressed per-slot as a
    // single boolean so a -Wunused-const-variable build still references the
    // arrays (the helper loops them) without any runtime cost.
    constexpr auto slot_invariants_hold(int slot) noexcept -> bool
    {
        const std::uint64_t local_bit{ std::uint64_t{ 1 } << (slot * 2) };
        const std::uint64_t merge_mask{
            (std::uint64_t{ 0b11 } << (slot * 2))
            | (std::uint64_t{ 0xF } << (16 + slot * 4)) };
        for (const auto k : ct_kinds)
        {
            for (const auto l : ct_lengths)
            {
                const std::uint64_t v{ pack_dr7_ref(slot, k, l) };
                const std::uint64_t rw_field { (v >> (16 + slot * 4)) & 0b11ull };
                const std::uint64_t len_field{ (v >> (18 + slot * 4)) & 0b11ull };
                if (rw_field != static_cast<std::uint64_t>(k)) { return false; }
                if (len_field != static_cast<std::uint64_t>(l)) { return false; }
                if ((v & local_bit) == 0ull) { return false; }   // never disabled
                if ((v & ~merge_mask) != 0ull) { return false; }  // within applier mask
                if (v == 0ull) { return false; }                  // never the empty mask
            }
        }
        return true;
    }
    static_assert(slot_invariants_hold(0), "slot0 DR7 field invariants must hold at compile time");
    static_assert(slot_invariants_hold(1), "slot1 DR7 field invariants must hold at compile time");
    static_assert(slot_invariants_hold(2), "slot2 DR7 field invariants must hold at compile time");
    static_assert(slot_invariants_hold(3), "slot3 DR7 field invariants must hold at compile time");

    // ---- Distinct slots are bit-disjoint (OR-composable) — proven at compile
    //      time for the worst-case (max-bits) value, the property refresh_thread_drs
    //      relies on when it merges one slot without clobbering another.
    static_assert((pack_dr7_ref(0, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes)
                 & pack_dr7_ref(3, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes)) == 0ull,
                  "slot0 and slot3 max-bit DR7 values must be bit-disjoint");
    static_assert((pack_dr7_ref(1, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes)
                 & pack_dr7_ref(2, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes)) == 0ull,
                  "slot1 and slot2 max-bit DR7 values must be bit-disjoint");

    // ---- No global-enable bit (G0..G3 = odd bits 1,3,5,7) is ever set: the
    //      trap must stay per-thread, never process-wide.  Compile-time form.
    static_assert((pack_dr7_ref(0, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes)
                 & ((std::uint64_t{ 1 } << 1) | (std::uint64_t{ 1 } << 3)
                  | (std::uint64_t{ 1 } << 5) | (std::uint64_t{ 1 } << 7))) == 0ull,
                  "build_dr7 layout must never set a global-enable (odd) bit");

} // namespace dr7_ct

// Runtime cross-check: the constexpr oracle reproduces build_dr7 byte-for-byte
// across the full 4 x 2 x 4 Cartesian product.  This is the bridge that lets a
// future `constexpr build_dr7` collapse into a single static_assert; until then
// it pins the (non-constexpr) shipping function to the compile-time-verified
// reference at runtime, for EVERY combination — not just the ~5 spot-checks.
static auto test_build_dr7_constexpr_oracle_matches_runtime() -> void
{
    using namespace vmhook::os;
    using namespace vmhook::os::detail_dr;

    bool oracle_matches{ true };
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        for (const auto k : dr7_ct::ct_kinds)
        {
            for (const auto l : dr7_ct::ct_lengths)
            {
                if (build_dr7(slot, k, l) != dr7_ct::pack_dr7_ref(slot, k, l))
                {
                    oracle_matches = false;
                }
            }
        }
    }
    check("build_dr7_constexpr_oracle_matches_runtime_all_32", oracle_matches);

    // The compile-time invariants already passed (this TU compiled); surface a
    // PASS line so the lane reports the constexpr layer explicitly rather than
    // silently.  References dr7_ct's constexpr fn so it is not unused.
    check("build_dr7_constexpr_slot_invariants_compiled",
          dr7_ct::slot_invariants_hold(0) && dr7_ct::slot_invariants_hold(3));
}
#endif

int main()
{
    test_version_macros();
    test_decode_u5();
    test_decode_u5_multi_byte();
    test_valid_pointer_filters();
    test_untag_pointer();
    test_sig_char_to_basic_type();
    test_to_native_protect();
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    test_build_dr7();
#endif
    test_array_helpers();
    test_format_log_safe_on_bad_pattern();
    test_format_log_positive();
    test_write_jni_arg_to_slot_unique_ptr_branch();
    test_write_jni_arg_to_slot_null_unique_ptr();
    test_write_jni_arg_to_slot_primitive_branches();
    test_jni_namespace_signature_for_arg();
    test_is_valid_pointer_rejects_sentinels();
    test_is_valid_pointer_boundaries();
    test_return_value_sign_extension();
    test_return_value_set_nullptr_for_wrapper();
    test_return_value_set_non_integer_types();
    test_return_value_no_frame_helpers();
    test_return_value_set_arg_guards();
    test_iterate_entries_no_jvm();
    test_vm_types_and_structs_no_jvm();
    test_find_jvm_module_no_jvm();
    test_jni_delete_local_ref_no_jvm();
    test_jvm_primitive_byte_width();
    test_field_proxy_set_size_guard();
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    test_dr_armed_count_refcount();
#endif
    test_version_string_composition();

    // --- Exhaustive-input expansion (#38-immune no-JVM lane) ---------------
    test_sig_char_to_basic_type_exhaustive();
    test_jvm_primitive_byte_width_exhaustive();
    test_memory_protection_enum_and_native_exhaustive();
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    test_build_dr7_exhaustive();
    test_build_dr7_deepening();
    test_build_dr7_constexpr_oracle_matches_runtime();
#endif
    test_factory_registry_roundtrip();
    test_jni_signature_for_arg_exhaustive();
    test_return_value_set_sign_extension_exhaustive();
    test_untag_pointer_exhaustive();
    test_is_valid_pointer_gates_array_helpers();
    test_field_proxy_set_size_guard_matrix();
    test_convert_jni_arg_primitive_exhaustive();
    test_is_valid_pointer_sentinel_upper_half_exhaustive();

    if (failures == 0)
    {
        std::printf("vmhook helpers: OK\n");
    }
    else
    {
        std::printf("vmhook helpers: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
