// Standalone (no-JVM) unit test: compile-time platform / arch / capability
// macros are defined and mutually consistent.
//
// Scope: pure compile-time macro logic plus the small pure-logic helpers that
// the capability macros gate (vmhook::os::detail_dr::build_dr7, the portable
// os:: page/address constants).  No live oop or running HotSpot is required or
// touched here.  Anything needing a JVM (actually arming a hardware data
// breakpoint, installing a runtime hook) is covered by JVM integration in
// example.cpp and is intentionally out of scope for this file.
//
// The macros under test are *compile-time constants*, so the strongest check
// is a static_assert (the file would not compile if a relationship were
// violated).  Each relationship is ALSO surfaced as a runtime check() so the
// executable reports a per-item PASS/FAIL line rather than just failing the
// build; both layers must agree.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <iterator>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// Compile-time invariants.  If any of these are wrong the translation unit
// fails to compile, which is the hardest possible guarantee.  We mirror each
// as a runtime check() below so a passing binary documents the relationship.
// ---------------------------------------------------------------------------

// Every OS macro must be *defined* (an undefined macro expands to 0 in #if,
// which would silently hide a typo).  `defined()` is a preprocessor-only
// operator, so the "is it defined?" guard must live in an #if, not a
// static_assert.
#if !defined(VMHOOK_OS_WINDOWS) || !defined(VMHOOK_OS_LINUX)      \
 || !defined(VMHOOK_OS_MACOS)   || !defined(VMHOOK_OS_IOS)        \
 || !defined(VMHOOK_OS_ANDROID)
#error "all five VMHOOK_OS_* macros must be defined"
#endif

// Each OS macro is strictly 0 or 1.
static_assert((VMHOOK_OS_WINDOWS == 0 || VMHOOK_OS_WINDOWS == 1)
                  && (VMHOOK_OS_LINUX == 0 || VMHOOK_OS_LINUX == 1)
                  && (VMHOOK_OS_MACOS == 0 || VMHOOK_OS_MACOS == 1)
                  && (VMHOOK_OS_IOS == 0 || VMHOOK_OS_IOS == 1)
                  && (VMHOOK_OS_ANDROID == 0 || VMHOOK_OS_ANDROID == 1),
              "each VMHOOK_OS_* macro must be 0 or 1");

// Exactly one base OS is selected.
static_assert(VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS
                  + VMHOOK_OS_IOS + VMHOOK_OS_ANDROID == 1,
              "exactly one VMHOOK_OS_* base macro must be 1");

// Aggregates are defined exactly as documented.
static_assert(VMHOOK_OS_POSIX
                  == (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID),
              "VMHOOK_OS_POSIX must equal Linux|macOS|iOS|Android");
static_assert(VMHOOK_OS_APPLE == (VMHOOK_OS_MACOS | VMHOOK_OS_IOS),
              "VMHOOK_OS_APPLE must equal macOS|iOS");

// Windows and POSIX partition the supported OS set: never both, always one.
static_assert((VMHOOK_OS_WINDOWS & VMHOOK_OS_POSIX) == 0,
              "Windows and POSIX are mutually exclusive");
static_assert((VMHOOK_OS_WINDOWS | VMHOOK_OS_POSIX) == 1,
              "every supported OS is either Windows or POSIX");

// Arch macros: both defined, each 0/1, exactly one selected (x86_64 xor arm64).
#if !defined(VMHOOK_ARCH_X86_64) || !defined(VMHOOK_ARCH_ARM64)
#error "both VMHOOK_ARCH_* macros must be defined"
#endif
static_assert((VMHOOK_ARCH_X86_64 == 0 || VMHOOK_ARCH_X86_64 == 1)
                  && (VMHOOK_ARCH_ARM64 == 0 || VMHOOK_ARCH_ARM64 == 1),
              "each VMHOOK_ARCH_* macro must be 0 or 1");
static_assert(VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64 == 1,
              "exactly one architecture (x86_64 xor arm64) must be selected");

// Runtime-hooking availability is defined 0/1 and is exactly the documented
// predicate: x86_64 AND not iOS.
#if !defined(VMHOOK_RUNTIME_HOOKING_AVAILABLE)
#error "VMHOOK_RUNTIME_HOOKING_AVAILABLE must be defined"
#endif
static_assert(VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0
                  || VMHOOK_RUNTIME_HOOKING_AVAILABLE == 1,
              "VMHOOK_RUNTIME_HOOKING_AVAILABLE must be 0 or 1");
static_assert(VMHOOK_RUNTIME_HOOKING_AVAILABLE
                  == (VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS),
              "runtime hooking available iff x86_64 && !iOS");
// Consequences spelled out in the cluster focus: 0 on arm64, 0 on iOS.
static_assert(!(VMHOOK_ARCH_ARM64 && VMHOOK_RUNTIME_HOOKING_AVAILABLE),
              "runtime hooking must be unavailable on arm64");
static_assert(!(VMHOOK_OS_IOS && VMHOOK_RUNTIME_HOOKING_AVAILABLE),
              "runtime hooking must be unavailable on iOS");
// Availability implies x86_64.
static_assert(!VMHOOK_RUNTIME_HOOKING_AVAILABLE || VMHOOK_ARCH_X86_64,
              "runtime hooking availability implies x86_64");

// Hardware data breakpoints: defined 0/1, exactly Windows AND x86_64.
#if !defined(VMHOOK_HAS_HW_DATA_BREAKPOINTS)
#error "VMHOOK_HAS_HW_DATA_BREAKPOINTS must be defined"
#endif
static_assert(VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0
                  || VMHOOK_HAS_HW_DATA_BREAKPOINTS == 1,
              "VMHOOK_HAS_HW_DATA_BREAKPOINTS must be 0 or 1");
static_assert(VMHOOK_HAS_HW_DATA_BREAKPOINTS
                  == (VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64),
              "HW data breakpoints iff Windows && x86_64");
// Capability implies Windows, implies x86_64, and (since it needs x86_64)
// implies runtime hooking is also available.
static_assert(!VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_OS_WINDOWS,
              "HW data breakpoints imply Windows");
static_assert(!VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_ARCH_X86_64,
              "HW data breakpoints imply x86_64");
static_assert(!VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_RUNTIME_HOOKING_AVAILABLE,
              "HW data breakpoints imply runtime hooking is available");

int main()
{
    // -- OS macro consistency (runtime mirror of the static_asserts) --------
    const int os_sum{ VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS
                      + VMHOOK_OS_IOS + VMHOOK_OS_ANDROID };
    check("exactly_one_os_macro_is_one", os_sum == 1);

    const bool os_each_binary{
        (VMHOOK_OS_WINDOWS | 1) == 1 && (VMHOOK_OS_LINUX | 1) == 1
        && (VMHOOK_OS_MACOS | 1) == 1 && (VMHOOK_OS_IOS | 1) == 1
        && (VMHOOK_OS_ANDROID | 1) == 1
        && VMHOOK_OS_WINDOWS >= 0 && VMHOOK_OS_WINDOWS <= 1
        && VMHOOK_OS_LINUX >= 0 && VMHOOK_OS_LINUX <= 1
        && VMHOOK_OS_MACOS >= 0 && VMHOOK_OS_MACOS <= 1
        && VMHOOK_OS_IOS >= 0 && VMHOOK_OS_IOS <= 1
        && VMHOOK_OS_ANDROID >= 0 && VMHOOK_OS_ANDROID <= 1 };
    check("every_os_macro_is_zero_or_one", os_each_binary);

    check("os_posix_equals_documented_union",
          VMHOOK_OS_POSIX
              == (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID));
    check("os_apple_equals_macos_or_ios",
          VMHOOK_OS_APPLE == (VMHOOK_OS_MACOS | VMHOOK_OS_IOS));

    check("windows_and_posix_mutually_exclusive",
          (VMHOOK_OS_WINDOWS & VMHOOK_OS_POSIX) == 0);
    check("windows_xor_posix_covers_all_supported",
          (VMHOOK_OS_WINDOWS | VMHOOK_OS_POSIX) == 1);

    // macOS and iOS are the only Apple targets; Apple implies POSIX and never
    // Windows.
    check("apple_implies_posix_and_not_windows",
          (!VMHOOK_OS_APPLE) || (VMHOOK_OS_POSIX == 1 && VMHOOK_OS_WINDOWS == 0));
    // Android implies POSIX (its backend is the Linux/Android shared path).
    check("android_implies_posix",
          (!VMHOOK_OS_ANDROID) || VMHOOK_OS_POSIX == 1);

    // -- Arch macro consistency --------------------------------------------
    check("exactly_one_arch_macro_is_one",
          VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64 == 1);
    check("arch_macros_are_zero_or_one",
          VMHOOK_ARCH_X86_64 >= 0 && VMHOOK_ARCH_X86_64 <= 1
              && VMHOOK_ARCH_ARM64 >= 0 && VMHOOK_ARCH_ARM64 <= 1);
    check("arch_x86_64_xor_arm64",
          (VMHOOK_ARCH_X86_64 ^ VMHOOK_ARCH_ARM64) == 1);

    // -- Runtime hooking availability flag ---------------------------------
    check("runtime_hooking_flag_is_zero_or_one",
          VMHOOK_RUNTIME_HOOKING_AVAILABLE >= 0
              && VMHOOK_RUNTIME_HOOKING_AVAILABLE <= 1);
    check("runtime_hooking_equals_x86_64_and_not_ios",
          VMHOOK_RUNTIME_HOOKING_AVAILABLE
              == (VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS));
    check("runtime_hooking_unavailable_on_arm64",
          !(VMHOOK_ARCH_ARM64 && VMHOOK_RUNTIME_HOOKING_AVAILABLE));
    check("runtime_hooking_unavailable_on_ios",
          !(VMHOOK_OS_IOS && VMHOOK_RUNTIME_HOOKING_AVAILABLE));
    check("runtime_hooking_implies_x86_64",
          !VMHOOK_RUNTIME_HOOKING_AVAILABLE || VMHOOK_ARCH_X86_64);

    // -- Hardware data-breakpoint capability flag --------------------------
    check("hw_data_breakpoints_flag_is_zero_or_one",
          VMHOOK_HAS_HW_DATA_BREAKPOINTS >= 0
              && VMHOOK_HAS_HW_DATA_BREAKPOINTS <= 1);
    check("hw_data_breakpoints_equals_windows_and_x86_64",
          VMHOOK_HAS_HW_DATA_BREAKPOINTS
              == (VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64));
    check("hw_data_breakpoints_imply_windows_x86_64",
          !VMHOOK_HAS_HW_DATA_BREAKPOINTS
              || (VMHOOK_OS_WINDOWS == 1 && VMHOOK_ARCH_X86_64 == 1));
    check("hw_data_breakpoints_imply_runtime_hooking_available",
          !VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_RUNTIME_HOOKING_AVAILABLE);
    // No non-Windows / non-x86_64 platform may advertise the capability.
    check("hw_data_breakpoints_off_on_arm64_and_posix",
          (VMHOOK_ARCH_ARM64 || VMHOOK_OS_POSIX)
              ? (VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0)
              : true);

    // -- Additional cross-macro implications (runtime mirror). --------------
    // POSIX is the exact complement of Windows within the supported set, so
    // each implies the negation of the other.
    check("posix_implies_not_windows",
          !VMHOOK_OS_POSIX || (VMHOOK_OS_WINDOWS == 0));
    check("windows_implies_not_posix",
          !VMHOOK_OS_WINDOWS || (VMHOOK_OS_POSIX == 0));
    // The five base-OS flags XOR to 1 (an even number of 1s would mean either
    // zero or two selected — both are bugs the "exactly one" sum also catches,
    // but XOR pins the parity independently).
    check("os_flags_xor_to_one",
          (VMHOOK_OS_WINDOWS ^ VMHOOK_OS_LINUX ^ VMHOOK_OS_MACOS
           ^ VMHOOK_OS_IOS ^ VMHOOK_OS_ANDROID) == 1);
    // iOS is an Apple target, so iOS implies APPLE implies POSIX; and iOS is the
    // one POSIX/Apple platform where runtime hooking is forced off.
    check("ios_implies_apple_and_posix",
          !VMHOOK_OS_IOS || (VMHOOK_OS_APPLE == 1 && VMHOOK_OS_POSIX == 1));
    check("ios_forces_runtime_hooking_off",
          !VMHOOK_OS_IOS || (VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0));
    // arm64 forces BOTH higher capabilities off (no runtime hooking, no HW
    // breakpoints) since both require x86_64.
    check("arm64_forces_both_capabilities_off",
          !VMHOOK_ARCH_ARM64
              || (VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0
                  && VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0));
    // HW data breakpoints are the strongest capability: having them implies
    // runtime hooking is also available (monotone capability ladder).
    check("hw_breakpoints_is_subset_of_runtime_hooking",
          !VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_RUNTIME_HOOKING_AVAILABLE);

    // -- Portable os:: address-range constants (defined on every platform) --
    // These are the documented user-space bounds the hook/scan code relies on;
    // they are plain constexpr values, no JVM needed.
    check("user_address_floor_below_ceiling",
          vmhook::os::user_address_floor < vmhook::os::user_address_ceiling);
    check("user_address_ceiling_is_canonical_low_half_top",
          vmhook::os::user_address_ceiling == std::uintptr_t{ 0x00007FFFFFFFFFFFull });
    check("user_address_floor_is_64k",
          vmhook::os::user_address_floor == std::uintptr_t{ 0xFFFFull });

    // -- Portable os:: page geometry (pure syscalls, no JVM) ----------------
    const std::size_t ps{ vmhook::os::page_size() };
    check("page_size_is_nonzero", ps != 0);
    check("page_size_is_power_of_two", ps != 0 && (ps & (ps - 1)) == 0);
    check("page_size_at_least_4096", ps >= 4096);
    const std::size_t gran{ vmhook::os::allocation_granularity() };
    check("allocation_granularity_is_nonzero", gran != 0);
    check("allocation_granularity_multiple_of_page_size",
          ps != 0 && (gran % ps) == 0);

    // memory_protection is a portable enum present on all platforms; confirm
    // the documented stable ordinal values the OS-protection mapping relies on.
    check("memory_protection_enum_ordinals_stable",
          static_cast<std::uint32_t>(vmhook::os::memory_protection::no_access) == 0
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::read) == 1
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::read_write) == 2
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::execute_read) == 3
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::execute_rw) == 4);

    // memory_protection underlying type is std::uint32_t (vmhook.hpp:447); a
    // change of width would silently alter ABI of any struct holding it.
    check("memory_protection_underlying_is_uint32",
          std::is_same_v<std::underlying_type_t<vmhook::os::memory_protection>,
                         std::uint32_t>);

    // The five ordinals are CONTIGUOUS [0..4] and STRICTLY INCREASING in the
    // documented declaration order, with no gaps — the OS-protection lookup
    // tables index by these values, so a reorder or gap would mis-map.
    {
        using mp = vmhook::os::memory_protection;
        const std::uint32_t ord[]{
            static_cast<std::uint32_t>(mp::no_access),
            static_cast<std::uint32_t>(mp::read),
            static_cast<std::uint32_t>(mp::read_write),
            static_cast<std::uint32_t>(mp::execute_read),
            static_cast<std::uint32_t>(mp::execute_rw),
        };
        bool strictly_increasing_contiguous{ true };
        for (std::size_t i{ 0 }; i < std::size(ord); ++i)
        {
            if (ord[i] != static_cast<std::uint32_t>(i)) { strictly_increasing_contiguous = false; }
        }
        check("memory_protection_ordinals_contiguous_0_to_4",
              strictly_increasing_contiguous);
        // All five are pairwise distinct (a duplicate would collapse two
        // protection semantics onto one value).
        bool all_distinct{ true };
        for (std::size_t i{ 0 }; i < std::size(ord); ++i)
        {
            for (std::size_t j{ i + 1 }; j < std::size(ord); ++j)
            {
                if (ord[i] == ord[j]) { all_distinct = false; }
            }
        }
        check("memory_protection_ordinals_all_distinct", all_distinct);
        // The max ordinal is exactly 4 (execute_rw); anchors "exactly five".
        check("memory_protection_max_ordinal_is_4",
              static_cast<std::uint32_t>(mp::execute_rw) == 4);
        // execute_read and execute_rw are the only two with the "execute" bit
        // of meaning — they are the two highest ordinals and strictly above the
        // non-executable trio.
        check("memory_protection_execute_variants_are_highest",
              static_cast<std::uint32_t>(mp::execute_read) > static_cast<std::uint32_t>(mp::read_write)
              && static_cast<std::uint32_t>(mp::execute_rw) > static_cast<std::uint32_t>(mp::execute_read));
    }

    // region_info default-constructs to an all-empty/unset region (the scan
    // allocator depends on these defaults).
    {
        const vmhook::os::region_info ri{};
        check("region_info_default_is_empty_unset",
              ri.base == nullptr && ri.size == 0 && !ri.committed && !ri.free
                  && !ri.readable && !ri.executable && !ri.guarded);
    }

    // -- DR7 builder: only exists when the capability is compiled in --------
    // build_dr7 is a pure Intel-SDM bit-mask helper (no JVM, no live thread);
    // it is only declared on Windows/x86_64 where VMHOOK_HAS_HW_DATA_BREAKPOINTS
    // is 1.  Gate the checks on the macro so the file still compiles (and the
    // remaining checks still run) on platforms where the symbol is absent.
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    {
        using vmhook::os::data_breakpoint_kind;
        using vmhook::os::data_breakpoint_length;

        // Enum bit-patterns documented against the Intel DR7 R/W and LEN
        // fields.  build_dr7 shifts these raw values into place, so their
        // numeric values are load-bearing.
        check("dr_kind_write_is_0b01",
              static_cast<std::uint8_t>(data_breakpoint_kind::write) == 0b01);
        check("dr_kind_read_write_is_0b11",
              static_cast<std::uint8_t>(data_breakpoint_kind::read_write) == 0b11);
        check("dr_length_one_byte_is_0b00",
              static_cast<std::uint8_t>(data_breakpoint_length::one_byte) == 0b00);
        check("dr_length_eight_bytes_is_0b10",
              static_cast<std::uint8_t>(data_breakpoint_length::eight_bytes) == 0b10);

        // Slot 0, write, one-byte:
        //   L0  bit 0          -> 0x1
        //   R/W field bits16-17 = 01 -> 0x1 << 16
        //   LEN field bits18-19 = 00 -> 0
        const std::uint64_t dr7_s0{ vmhook::os::detail_dr::build_dr7(
            0, data_breakpoint_kind::write, data_breakpoint_length::one_byte) };
        check("build_dr7_slot0_write_one_byte",
              dr7_s0 == ((std::uint64_t{ 1 } << 0)
                         | (std::uint64_t{ 0b01 } << 16)
                         | (std::uint64_t{ 0b00 } << 18)));

        // Slot 3, read_write, eight-byte:
        //   L3  bit 6          -> 1 << 6
        //   R/W field at 16+4*3 = 28, value 0b11
        //   LEN field at 18+4*3 = 30, value 0b10
        const std::uint64_t dr7_s3{ vmhook::os::detail_dr::build_dr7(
            3, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes) };
        check("build_dr7_slot3_read_write_eight_byte",
              dr7_s3 == ((std::uint64_t{ 1 } << 6)
                         | (std::uint64_t{ 0b11 } << 28)
                         | (std::uint64_t{ 0b10 } << 30)));

        // The local-enable bit for slot N sits at bit 2*N; verify each slot's
        // enable bit is distinct and lands where the SDM places it.
        bool enable_bits_ok{ true };
        for (int slot{ 0 }; slot < 4; ++slot)
        {
            const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                slot, data_breakpoint_kind::write, data_breakpoint_length::one_byte) };
            if ((v & (std::uint64_t{ 1 } << (slot * 2))) == 0) { enable_bits_ok = false; }
        }
        check("build_dr7_local_enable_bit_per_slot", enable_bits_ok);

        // The four global-enable bits (G0..G3 at odd bits 1,3,5,7) must stay
        // clear: the watch is per-thread, never process-global.
        bool no_global_enable{ true };
        for (int slot{ 0 }; slot < 4; ++slot)
        {
            const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                slot, data_breakpoint_kind::read_write,
                data_breakpoint_length::four_bytes) };
            if ((v & (std::uint64_t{ 1 } << (slot * 2 + 1))) != 0) { no_global_enable = false; }
        }
        check("build_dr7_never_sets_global_enable", no_global_enable);

        // -- The remaining enum ordinals (vmhook.hpp:1004-1019). --------------
        // data_breakpoint_kind underlying type is uint8_t; only write(0b01) and
        // read_write(0b11) exist — both have bit 0 set (the "enabled R/W" low
        // bit), and execute (0b00) is deliberately absent (DR can't trap exec
        // via the data-watch path).
        check("dr_kind_underlying_is_uint8",
              std::is_same_v<std::underlying_type_t<data_breakpoint_kind>, std::uint8_t>);
        check("dr_kind_write_and_read_write_distinct",
              static_cast<std::uint8_t>(data_breakpoint_kind::write)
                  != static_cast<std::uint8_t>(data_breakpoint_kind::read_write));
        check("dr_kind_values_have_low_bit_set",
              (static_cast<std::uint8_t>(data_breakpoint_kind::write) & 0b01u) != 0
              && (static_cast<std::uint8_t>(data_breakpoint_kind::read_write) & 0b01u) != 0);

        // data_breakpoint_length: the Intel-SDM LEN encoding is NOT in numeric
        // byte order — one_byte=0b00, two_bytes=0b01, eight_bytes=0b10,
        // four_bytes=0b11.  Pin the two the existing suite omits (two_bytes,
        // four_bytes) and that all four are distinct 2-bit codes.
        check("dr_length_two_bytes_is_0b01",
              static_cast<std::uint8_t>(data_breakpoint_length::two_bytes) == 0b01);
        check("dr_length_four_bytes_is_0b11",
              static_cast<std::uint8_t>(data_breakpoint_length::four_bytes) == 0b11);
        check("dr_length_underlying_is_uint8",
              std::is_same_v<std::underlying_type_t<data_breakpoint_length>, std::uint8_t>);
        {
            const std::uint8_t lens[]{
                static_cast<std::uint8_t>(data_breakpoint_length::one_byte),
                static_cast<std::uint8_t>(data_breakpoint_length::two_bytes),
                static_cast<std::uint8_t>(data_breakpoint_length::eight_bytes),
                static_cast<std::uint8_t>(data_breakpoint_length::four_bytes),
            };
            bool all_two_bit_and_distinct{ true };
            for (std::size_t i{ 0 }; i < std::size(lens); ++i)
            {
                if ((lens[i] & ~0b11u) != 0) { all_two_bit_and_distinct = false; }
                for (std::size_t j{ i + 1 }; j < std::size(lens); ++j)
                {
                    if (lens[i] == lens[j]) { all_two_bit_and_distinct = false; }
                }
            }
            check("dr_length_all_codes_two_bit_and_distinct", all_two_bit_and_distinct);
        }

        // -- build_dr7 exhaustive field placement across all 4 slots. ---------
        // For every slot the R/W field sits at bit (16 + 4*slot) and the LEN
        // field at bit (18 + 4*slot); verify the raw enum codes land exactly
        // there for a read_write/two_bytes watch, and that the only L-enable bit
        // set is the slot's own (bit 2*slot).
        {
            bool fields_placed_ok{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::read_write,
                    data_breakpoint_length::two_bytes) };
                const std::uint64_t rw_field{ (v >> (16 + 4 * slot)) & 0b11u };
                const std::uint64_t len_field{ (v >> (18 + 4 * slot)) & 0b11u };
                if (rw_field != 0b11u) { fields_placed_ok = false; }    // read_write
                if (len_field != 0b01u) { fields_placed_ok = false; }   // two_bytes
                // Exactly one L-enable bit (the slot's own) among bits 0,2,4,6.
                const std::uint64_t l_enable_bits{ v & 0b01010101u };
                if (l_enable_bits != (std::uint64_t{ 1 } << (slot * 2))) { fields_placed_ok = false; }
            }
            check("build_dr7_rw_len_fields_placed_per_slot", fields_placed_ok);
        }

        // Two distinct slots produce distinct DR7 words (different L-enable
        // bit), and the same (slot, kind, length) is deterministic.
        check("build_dr7_distinct_slots_distinct_words",
              vmhook::os::detail_dr::build_dr7(0, data_breakpoint_kind::write,
                                               data_breakpoint_length::one_byte)
                  != vmhook::os::detail_dr::build_dr7(1, data_breakpoint_kind::write,
                                                      data_breakpoint_length::one_byte));
        check("build_dr7_is_deterministic",
              vmhook::os::detail_dr::build_dr7(2, data_breakpoint_kind::read_write,
                                               data_breakpoint_length::four_bytes)
                  == vmhook::os::detail_dr::build_dr7(2, data_breakpoint_kind::read_write,
                                                      data_breakpoint_length::four_bytes));
    }
#else
    // Symbol vmhook::os::detail_dr::build_dr7 / data_breakpoint_* are not
    // declared on this platform (capability flag is 0); the bit-mask logic is
    // therefore unreachable here and is exercised on Windows/x86_64 builds.
    check("build_dr7_absent_when_capability_disabled",
          VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0);
#endif

    return failures == 0 ? 0 : 1;
}
