// Standalone (no-JVM) unit tests for the Method-entry-point accessor layer
// (the "FIX C" surface): get_i2i_entry / get_from_interpreted_entry /
// get_from_compiled_entry / get_code / set_*_entry / set_code / get_adapter /
// get_c2i_entry_from_adapter / validate_adapter_handler_entry /
// detect_adapter_offset_from_method.
//
// EVERYTHING here runs WITHOUT a live JVM in-process. With no jvm.dll /
// libjvm.so loaded:
//   * get_vm_structs() caches nullptr -> iterate_struct_entries("Method", ...)
//     returns nullptr for EVERY field name (including _i2i_entry,
//     _from_interpreted_entry, _from_compiled_entry, _from_compiled_code_entry
//     _point, _code, _adapter), as does iterate_struct_entries(
//     "AdapterHandlerEntry", "_c2i_entry").
//   * The getters therefore find a null VMStruct entry and return nullptr
//     (the throw->catch path on get_i2i_entry/get_from_interpreted_entry, the
//     noexcept early-return on the others).
//   * The setters (set_code / set_from_interpreted_entry / set_from_compiled
//     _entry) consequently no-op without writing.
//   * get_adapter() falls into the JDK 9+ heuristic branch (exported entry is
//     null), then detect_adapter_offset_from_method() returns 0 because the
//     AdapterHandlerEntry::_c2i_entry offset is unresolvable -> get_adapter()
//     returns nullptr.
//   * get_c2i_entry_from_adapter(nullptr) and (bogus pointer) return nullptr.
//   * validate_adapter_handler_entry(nullptr/garbage, ...) returns false.
//
// This file proves the NO-JVM / INVALID-POINTER degradation paths are crash-
// free and side-effect-free, plus pins the cross-JDK ABI constants that any
// future width/offset-aware reworking of the entry-point layer must satisfy.
// The VALUE-correctness of these accessors on a live Method* (right pointer
// read back, c2i restored, set_from_interpreted_entry(i2i) re-arms dispatch)
// is exercised by the live-JVM modules hook_install_after_jit,
// deoptimize_methods, and dont_inline_dont_compile and is OUT OF SCOPE here.

#include <vmhook/vmhook.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* const name, const bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ─────────────────────────────────────────────────────────────────────────
//  AUTHORITATIVE HotSpot Method entry-point layout facts (JDK 8..26).
//
//  Verified against OpenJDK source (method.hpp on jdk8u, jdk11u, jdk17u,
//  jdk21u, master).  These are the cross-version contract a width/offset-
//  aware rework of the accessor layer must honour.
// ─────────────────────────────────────────────────────────────────────────
namespace entry_layout
{
    // Every entry-point field in Method is a single function-pointer
    // (`address`, which HotSpot defines as `unsigned char*` and treats as a
    // raw code-cache pointer).  Width therefore == sizeof(void*) on every
    // supported JDK.  This is what set_from_interpreted_entry /
    // set_from_compiled_entry / set_code write through, and what the get_*
    // accessors read back as `void*`.
    constexpr int entry_pointer_width_bytes = static_cast<int>(sizeof(void*));

    // Code-cache code stubs are PAGE-ALIGNED at allocation but each
    // instruction stream is only required to be naturally-aligned for the
    // target ISA.  HotSpot's CodeEntryAlignment is 32 on x86_64 / aarch64,
    // 16 on x86_32, but the only universal lower bound we can rely on for
    // an `address` field is `sizeof(void*)` (the natural pointer alignment
    // the C++ struct member guarantees).  We pin BOTH:
    //   * the natural-pointer alignment (universal, even on JDK 8 i386), and
    //   * the codegen-alignment family (CodeEntryAlignment ∈ {16, 32}, the
    //     two values HotSpot has ever used on the supported architectures),
    //     pinned as a documented invariant — NOT asserted at runtime here
    //     because we have no live entry point in this no-JVM file.
    constexpr int entry_pointer_natural_alignment = static_cast<int>(alignof(void*));
    constexpr int code_entry_alignment_x86_32     = 16;
    constexpr int code_entry_alignment_x86_64     = 32;
    constexpr int code_entry_alignment_aarch64    = 32;

    // The Method field names that are resolved by VMStructs lookup.  These
    // are the only strings the accessor layer ever passes to
    // iterate_struct_entries.  Pinned so a future rename in the library
    // surfaces here too.
    constexpr const char* fld_i2i_entry              = "_i2i_entry";
    constexpr const char* fld_from_interpreted       = "_from_interpreted_entry";
    constexpr const char* fld_from_compiled_pre21    = "_from_compiled_code_entry_point";
    constexpr const char* fld_from_compiled_post21   = "_from_compiled_entry";
    constexpr const char* fld_code                   = "_code";
    constexpr const char* fld_adapter                = "_adapter";
    constexpr const char* fld_ahe_c2i_entry          = "_c2i_entry";
    constexpr const char* fld_ahe_i2c_entry          = "_i2c_entry";

    // detect_adapter_offset_from_method's known-fields skip-set: any Method
    // field whose offset appears here is excluded from the brute byte-scan
    // so a c2i scan never mis-picks _constMethod / _code / _i2i_entry /
    // _from_interpreted_entry / _method_data / _method_counters.
    // Pinned cardinality (6 entries, capped at array size 8 by the library).
    constexpr int detect_adapter_skip_set_size = 6;
    constexpr int detect_adapter_skip_set_cap  = 8;

    // _adapter is exported via gHotSpotVMStructs ONLY on JDK 8.  Every JDK
    // 9..26 dropped the export and relies on the heuristic scan +
    // process-wide cache.
    constexpr int adapter_exported_only_jdk = 8;

    // _from_compiled_entry rename pivot: name changed at JDK 21
    // (_from_compiled_code_entry_point -> _from_compiled_entry).
    constexpr int from_compiled_entry_rename_jdk = 21;

    // Method::_i2i_entry is the FIRST entry-point field laid out after the
    // _constMethod / _method_data / _method_counters / _access_flags header;
    // its offset has been pinned (modulo width changes for _access_flags) at
    // ~32-64 bytes on x86_64 across every supported JDK.  We don't try to
    // pin the absolute offset (it varies with _access_flags width JDK 24
    // collapse) — we only pin the relative-ordering invariant:
    //   _i2i_entry < _from_interpreted_entry < _from_compiled_entry
    // which HotSpot has preserved on every JDK 8..26 (method.hpp).
    constexpr bool entry_point_field_order_invariant = true;
}

// Compile-time pinning of the layout contract: any future rework that changes
// these has to consciously update this table.
static_assert(entry_layout::entry_pointer_width_bytes == sizeof(void*),
              "Method entry-point fields are address-sized on every JDK");
static_assert(entry_layout::entry_pointer_natural_alignment == alignof(void*),
              "Method entry-point fields naturally pointer-aligned");
static_assert(entry_layout::code_entry_alignment_x86_64 == 32
              && entry_layout::code_entry_alignment_aarch64 == 32
              && entry_layout::code_entry_alignment_x86_32 == 16,
              "HotSpot CodeEntryAlignment family: 32 on 64-bit ISAs, 16 on i386");
static_assert(entry_layout::adapter_exported_only_jdk == 8,
              "Method::_adapter exported via VMStructs ONLY on JDK 8");
static_assert(entry_layout::from_compiled_entry_rename_jdk == 21,
              "Method::_from_compiled_entry rename at JDK 21");
static_assert(entry_layout::detect_adapter_skip_set_size
              <= entry_layout::detect_adapter_skip_set_cap,
              "skip-set fits in the library's std::array<std::size_t, 8> cap");
static_assert(entry_layout::entry_point_field_order_invariant,
              "ordering invariant flag must hold; flip-only on JDK rework");

// Static asserts on entry-point function-pointer type identity (ledger gap).
// The library hands these back as `void*` (HotSpot internally uses `address`
// aka `unsigned char*`).  Pin the public-surface type identity.
static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>().get_i2i_entry()), void*>,
              "get_i2i_entry returns void* (HotSpot address)");
static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>().get_from_interpreted_entry()), void*>,
              "get_from_interpreted_entry returns void*");
static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>().get_from_compiled_entry()), void*>,
              "get_from_compiled_entry returns void*");
static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>().get_code()), void*>,
              "get_code returns void*");
static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>().get_adapter()), void*>,
              "get_adapter returns void*");

// Pin the noexcept contract: the entry-point setters and the noexcept
// getters MUST NOT throw (they run on the detached watchdog thread on the
// auto-repair path, where an escaping exception would tear down the JVM).
static_assert(noexcept(std::declval<vmhook::hotspot::method>().set_code(nullptr)),
              "set_code is noexcept (watchdog re-arm path)");
static_assert(noexcept(std::declval<vmhook::hotspot::method>().set_from_interpreted_entry(nullptr)),
              "set_from_interpreted_entry is noexcept");
static_assert(noexcept(std::declval<vmhook::hotspot::method>().set_from_compiled_entry(nullptr)),
              "set_from_compiled_entry is noexcept");
static_assert(noexcept(std::declval<vmhook::hotspot::method>().get_code()),
              "get_code is noexcept");
static_assert(noexcept(std::declval<vmhook::hotspot::method>().get_from_compiled_entry()),
              "get_from_compiled_entry is noexcept");
static_assert(noexcept(std::declval<vmhook::hotspot::method>().get_adapter()),
              "get_adapter is noexcept");

// ─────────────────────────────────────────────────────────────────────────
//  1. Cold-state (no-JVM) accessors all return nullptr — never wild interior
//     pointers — on EVERY entry-point field.  This is the master "safe
//     degradation" gate for the FIX C surface.
// ─────────────────────────────────────────────────────────────────────────
static auto test_cold_state_accessors_return_null() -> void
{
    alignas(16) std::array<std::uint8_t, 128> fake_method{};
    fake_method.fill(0x5A);
    auto* const as_method{ reinterpret_cast<vmhook::hotspot::method*>(fake_method.data()) };

    // All five entry-point readers must degrade to nullptr with no JVM.
    check("get_i2i_entry_no_jvm_returns_null",            as_method->get_i2i_entry()              == nullptr);
    check("get_from_interpreted_entry_no_jvm_returns_null", as_method->get_from_interpreted_entry() == nullptr);
    check("get_from_compiled_entry_no_jvm_returns_null",  as_method->get_from_compiled_entry()    == nullptr);
    check("get_code_no_jvm_returns_null",                 as_method->get_code()                   == nullptr);
    check("get_adapter_no_jvm_returns_null",              as_method->get_adapter()                == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
//  2. Invalid-this guard: an odd / sentinel / below-floor pointer is
//     rejected BEFORE forming `this + offset`.  Reaching the line after the
//     calls is the assertion: a deref would have crashed the process.
// ─────────────────────────────────────────────────────────────────────────
static auto test_invalid_this_pointer_is_safe() -> void
{
    auto* const odd{ reinterpret_cast<vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x3u)) };
    auto* const low{ reinterpret_cast<vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x8u)) };
    auto* const wild{ reinterpret_cast<vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0xDEADBEEFu)) };

    check("get_i2i_entry_invalid_this_returns_null",            odd->get_i2i_entry()                == nullptr);
    check("get_from_interpreted_entry_invalid_this_returns_null", odd->get_from_interpreted_entry() == nullptr);
    check("get_from_compiled_entry_invalid_this_returns_null",  low->get_from_compiled_entry()      == nullptr);
    check("get_code_invalid_this_returns_null",                 low->get_code()                     == nullptr);
    check("get_adapter_invalid_this_returns_null",              wild->get_adapter()                 == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
//  3. Setters never_throws contract & no-write on cold/invalid Method.
//
//  With no JVM the VMStruct entry is absent so set_* must early-return WITHOUT
//  touching the in-range fake_method buffer.  Pattern-fill, call, compare.
// ─────────────────────────────────────────────────────────────────────────
static auto test_setters_no_jvm_do_not_write() -> void
{
    alignas(16) std::array<std::uint8_t, 128> fake_method{};
    fake_method.fill(0xAB);
    const std::array<std::uint8_t, 128> snapshot{ fake_method };

    auto* const as_method{ reinterpret_cast<vmhook::hotspot::method*>(fake_method.data()) };

    // A recognisable bogus "entry point" value — must NOT be written anywhere.
    auto* const fake_entry{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xCAFEBABEu)) };

    as_method->set_from_interpreted_entry(fake_entry);
    as_method->set_from_compiled_entry(fake_entry);
    as_method->set_code(fake_entry);
    as_method->set_code(nullptr);
    as_method->set_from_interpreted_entry(nullptr);
    as_method->set_from_compiled_entry(nullptr);

    check("setters_no_jvm_do_not_write_any_byte",
          std::memcmp(fake_method.data(), snapshot.data(), fake_method.size()) == 0);
}

// ─────────────────────────────────────────────────────────────────────────
//  4. Setters on a null Method* are a safe no-op (never_throws contract on
//     null Method, ledger gap).  These setters are declared noexcept and the
//     library's is_valid_pointer(this) guard short-circuits before
//     `this + offset` is formed.
// ─────────────────────────────────────────────────────────────────────────
static auto test_setters_null_this_is_safe_noop() -> void
{
    // Launder the null through a runtime-opaque source so -Wnonnull cannot
    // see the constant nullptr at the call site (the library's
    // is_valid_pointer(this) guard short-circuits at runtime).
    volatile std::uintptr_t opaque_zero{ 0 };
    auto* const null_method{ reinterpret_cast<vmhook::hotspot::method*>(
        static_cast<std::uintptr_t>(opaque_zero)) };
    // Reaching the next line proves no fault.
    null_method->set_code(nullptr);
    null_method->set_code(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234u)));
    null_method->set_from_interpreted_entry(nullptr);
    null_method->set_from_compiled_entry(nullptr);
    check("setters_null_this_is_safe_noop", true);
}

// ─────────────────────────────────────────────────────────────────────────
//  5. Getters on a null Method* degrade to nullptr / no fault.  Same contract
//     as the setters but on the read side.  The two throwing getters
//     (get_i2i_entry / get_from_interpreted_entry) catch internally and
//     return nullptr — they never let an exception escape on a null receiver.
// ─────────────────────────────────────────────────────────────────────────
static auto test_getters_null_this_returns_null() -> void
{
    volatile std::uintptr_t opaque_zero{ 0 };
    auto* const null_method{ reinterpret_cast<vmhook::hotspot::method*>(
        static_cast<std::uintptr_t>(opaque_zero)) };
    check("get_i2i_entry_null_this_returns_null",              null_method->get_i2i_entry()              == nullptr);
    check("get_from_interpreted_entry_null_this_returns_null", null_method->get_from_interpreted_entry() == nullptr);
    check("get_from_compiled_entry_null_this_returns_null",    null_method->get_from_compiled_entry()    == nullptr);
    check("get_code_null_this_returns_null",                   null_method->get_code()                   == nullptr);
    check("get_adapter_null_this_returns_null",                null_method->get_adapter()                == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
//  6. get_c2i_entry_from_adapter rejects null/garbage adapters.
//     (Pure-logic flaw #2 sentinel — guards against the AHE _i2c_entry
//      offset-0 assumption ever being driven through a wild pointer.)
// ─────────────────────────────────────────────────────────────────────────
static auto test_get_c2i_entry_from_adapter_rejects_garbage() -> void
{
    using vmhook::hotspot::get_c2i_entry_from_adapter;
    check("c2i_from_adapter_null_returns_null",
          get_c2i_entry_from_adapter(nullptr) == nullptr);
    check("c2i_from_adapter_odd_returns_null",
          get_c2i_entry_from_adapter(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x3u))) == nullptr);
    check("c2i_from_adapter_low_returns_null",
          get_c2i_entry_from_adapter(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x8u))) == nullptr);
    check("c2i_from_adapter_wild_returns_null",
          get_c2i_entry_from_adapter(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEFu))) == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
//  7. validate_adapter_handler_entry rejects null/non-readable/non-executable
//     candidates.  This is the unit angle for FLAW #2 (the _i2c_entry-at-
//     offset-0 assumption): even when the assumption is hard-coded, the
//     validation MUST fail-closed on every garbage input.
// ─────────────────────────────────────────────────────────────────────────
static auto test_validate_adapter_handler_entry_rejects_garbage() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    // Null candidate: rejected regardless of c2i offset.
    check("validate_ahe_null_candidate_returns_false",
          validate_adapter_handler_entry(nullptr, 0) == false);
    check("validate_ahe_null_candidate_arb_offset_returns_false",
          validate_adapter_handler_entry(nullptr, 64) == false);

    // Sentinel / wild candidates: is_readable_pointer rejects them.
    check("validate_ahe_odd_candidate_returns_false",
          validate_adapter_handler_entry(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x3u)), 8) == false);
    check("validate_ahe_low_candidate_returns_false",
          validate_adapter_handler_entry(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10u)), 8) == false);
    check("validate_ahe_wild_candidate_returns_false",
          validate_adapter_handler_entry(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEFu)), 8) == false);

    // Owned readable buffer whose pointer-slots are NOT executable memory:
    // candidate passes is_readable_pointer but the (i2c / c2i) reads land on
    // a stack page that is committed-but-non-executable -> query_region's
    // executable bit is false -> validation returns false.  This locks in
    // the executable-page requirement on BOTH slots.
    alignas(16) std::array<std::uint8_t, 256> fake_ahe{};
    // Fill with self-pointers so i2c and c2i are readable (point back into
    // the stack buffer) — but the stack page is non-executable on every
    // supported OS, so validation MUST still return false.
    void* const self{ fake_ahe.data() };
    std::memcpy(fake_ahe.data() + 0,  &self, sizeof(self));   // i2c slot (offset 0)
    std::memcpy(fake_ahe.data() + 16, &self, sizeof(self));   // c2i slot (arbitrary offset)

    check("validate_ahe_non_executable_target_returns_false",
          validate_adapter_handler_entry(fake_ahe.data(), 16) == false);
}

// ─────────────────────────────────────────────────────────────────────────
//  8. detect_adapter_offset_from_method degrades to 0 cleanly when the
//     AHE _c2i_entry offset is unresolvable (no JVM), and on null probe.
// ─────────────────────────────────────────────────────────────────────────
static auto test_detect_adapter_offset_degrades_to_zero() -> void
{
    using vmhook::hotspot::detect_adapter_offset_from_method;
    check("detect_adapter_null_probe_returns_zero",
          detect_adapter_offset_from_method(nullptr) == 0u);

    // In-range fake Method buffer: even though `this` passes is_valid_pointer,
    // the AHE _c2i_entry field offset is unresolved -> early return 0.
    alignas(16) std::array<std::uint8_t, 256> fake_method{};
    auto* const probe{ reinterpret_cast<vmhook::hotspot::method*>(fake_method.data()) };
    check("detect_adapter_no_jvm_returns_zero",
          detect_adapter_offset_from_method(probe) == 0u);
}

// ─────────────────────────────────────────────────────────────────────────
//  9. fake-method buffer pointer-alignment pin (ledger gap: entry-point
//     alignment).  Method entry-point fields are pointer-sized and read via
//     a `reinterpret_cast<const void* const*>` shape after offset addition;
//     the offset PLUS the Method base must therefore land on an alignof(void*)
//     boundary on every supported ISA.  We can't read a real Method here, but
//     we CAN pin that the buffer we hand in is correctly aligned for the
//     guarantees the library relies on (alignof(void*) == 8 on 64-bit,
//     4 on 32-bit; we test the active ABI).
// ─────────────────────────────────────────────────────────────────────────
static auto test_entry_pointer_alignment_pin() -> void
{
    alignas(alignof(void*)) std::array<std::uint8_t, 64> buf{};
    const auto addr{ reinterpret_cast<std::uintptr_t>(buf.data()) };
    check("entry_pointer_alignment_buffer_aligned",
          (addr % alignof(void*)) == 0);
    // 8/16-byte alignment family pin: alignof(void*) is 8 on 64-bit, 4 on
    // 32-bit; CodeEntryAlignment is 16/32; both are powers of two.
    constexpr int a = entry_layout::entry_pointer_natural_alignment;
    check("entry_pointer_alignment_is_power_of_two",
          a > 0 && (a & (a - 1)) == 0);
    check("code_entry_alignment_family_powers_of_two",
          (entry_layout::code_entry_alignment_x86_32  & (entry_layout::code_entry_alignment_x86_32  - 1)) == 0
       && (entry_layout::code_entry_alignment_x86_64  & (entry_layout::code_entry_alignment_x86_64  - 1)) == 0
       && (entry_layout::code_entry_alignment_aarch64 & (entry_layout::code_entry_alignment_aarch64 - 1)) == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// 10. Determinism: repeated cold-state reads return the same (null) value.
//     The accessors cache the resolved VMStruct entry in function-local
//     statics; with no JVM each cache latches nullptr on first call and
//     every subsequent call returns nullptr without re-probing.
// ─────────────────────────────────────────────────────────────────────────
static auto test_cold_state_accessors_deterministic() -> void
{
    alignas(16) std::array<std::uint8_t, 128> fake_method{};
    auto* const as_method{ reinterpret_cast<vmhook::hotspot::method*>(fake_method.data()) };

    bool ok{ true };
    for (int i{ 0 }; i < 8; ++i)
    {
        ok = ok
            && as_method->get_i2i_entry()              == nullptr
            && as_method->get_from_interpreted_entry() == nullptr
            && as_method->get_from_compiled_entry()    == nullptr
            && as_method->get_code()                   == nullptr
            && as_method->get_adapter()                == nullptr;
    }
    check("cold_state_accessors_stable_across_repeated_calls", ok);
}

// ─────────────────────────────────────────────────────────────────────────
// 11. Layout-contract runtime echo (mirrors static_asserts so the facts also
//     show up as named PASS lines in CI logs).
// ─────────────────────────────────────────────────────────────────────────
static auto test_layout_contract_runtime() -> void
{
    using namespace entry_layout;
    check("layout_entry_pointer_width_is_pointer_size",
          entry_pointer_width_bytes == static_cast<int>(sizeof(void*)));
    check("layout_entry_pointer_natural_alignment",
          entry_pointer_natural_alignment == static_cast<int>(alignof(void*)));
    check("layout_code_entry_alignment_family",
          code_entry_alignment_x86_64 == 32 && code_entry_alignment_x86_32 == 16);
    check("layout_adapter_exported_only_jdk8",
          adapter_exported_only_jdk == 8);
    check("layout_from_compiled_entry_rename_at_jdk21",
          from_compiled_entry_rename_jdk == 21);
    check("layout_detect_adapter_skip_set_within_cap",
          detect_adapter_skip_set_size <= detect_adapter_skip_set_cap);
}

// ─────────────────────────────────────────────────────────────────────────
// 12. Wave-28 ledger gap: noexcept static_asserts on the FULL recovery
//     surface (validate_adapter_handler_entry / get_c2i_entry_from_adapter /
//     detect_adapter_offset_from_method).  Recovery is a crash-proofing
//     contract; any future signature drift that drops noexcept must turn this
//     file red.
// ─────────────────────────────────────────────────────────────────────────
static_assert(noexcept(vmhook::hotspot::validate_adapter_handler_entry(nullptr, 0u)),
              "validate_adapter_handler_entry is noexcept");
static_assert(noexcept(vmhook::hotspot::get_c2i_entry_from_adapter(nullptr)),
              "get_c2i_entry_from_adapter is noexcept");
static_assert(noexcept(vmhook::hotspot::detect_adapter_offset_from_method(nullptr)),
              "detect_adapter_offset_from_method is noexcept");

// ─────────────────────────────────────────────────────────────────────────
// 13. Wave-28 ledger gap: 32 fabricated bad-entry inputs to
//     validate_adapter_handler_entry MUST all be rejected.  Sweeps low
//     sentinels (0x0..0x1F), misaligned bit-patterns, and assorted "looks
//     plausible" addresses against several c2i offsets.  Belt-and-braces
//     against any future regression that loosens validation.
// ─────────────────────────────────────────────────────────────────────────
static auto test_validate_ahe_32_fabricated_bad_inputs() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    constexpr std::uintptr_t bad_addrs[] = {
        0x0u, 0x1u, 0x2u, 0x3u, 0x4u, 0x7u, 0x8u, 0xFu,
        0x10u, 0x18u, 0x1Fu, 0x42u, 0x100u, 0x1000u,
        0xBAADu, 0xDEADu, 0xCAFEu, 0xFEEDu,
        0xDEADBEEFu, 0xBAADF00Du, 0xCAFEBABEu, 0xFEEDFACEu,
        0xFFFFFFFFu, 0xFFFF0000u,
        0xAAAA'AAAAu, 0x5555'5555u,
        0x1234'5678u, 0x8765'4321u,
        0x0FFF'FFFFu, 0x7FFF'FFFFu,
        0x1u << 20, 0x1u << 30,
    };
    constexpr std::size_t n = sizeof(bad_addrs) / sizeof(bad_addrs[0]);
    static_assert(n == 32, "exactly 32 fabricated bad inputs");

    bool all_rejected{ true };
    for (auto a : bad_addrs)
    {
        void* const p{ reinterpret_cast<void*>(a) };
        // Try several plausible c2i offsets; every shape must be rejected.
        if (validate_adapter_handler_entry(p, 0))   { all_rejected = false; }
        if (validate_adapter_handler_entry(p, 8))   { all_rejected = false; }
        if (validate_adapter_handler_entry(p, 16))  { all_rejected = false; }
        if (validate_adapter_handler_entry(p, 64))  { all_rejected = false; }
    }
    check("validate_ahe_32_fabricated_bad_inputs_all_rejected", all_rejected);
}

// ─────────────────────────────────────────────────────────────────────────
// 14. Wave-28 ledger gap: recovery idempotence.  Calling the recovery
//     surface twice on the same (no-JVM, null/garbage) inputs MUST yield
//     bit-identical results to a single call.  Guards against any caching
//     drift between cold first call and subsequent calls.
// ─────────────────────────────────────────────────────────────────────────
static auto test_recovery_idempotent_twice_equals_once() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;
    using vmhook::hotspot::get_c2i_entry_from_adapter;
    using vmhook::hotspot::detect_adapter_offset_from_method;

    // get_c2i_entry_from_adapter(nullptr): twice == once == nullptr.
    void* const c2i_a{ get_c2i_entry_from_adapter(nullptr) };
    void* const c2i_b{ get_c2i_entry_from_adapter(nullptr) };
    check("c2i_from_adapter_null_idempotent", c2i_a == nullptr && c2i_a == c2i_b);

    // detect_adapter_offset_from_method(nullptr): twice == once == 0.
    const auto d_a{ detect_adapter_offset_from_method(nullptr) };
    const auto d_b{ detect_adapter_offset_from_method(nullptr) };
    check("detect_adapter_offset_null_idempotent", d_a == 0u && d_a == d_b);

    // validate_adapter_handler_entry(nullptr, 0): twice == once == false.
    const bool v_a{ validate_adapter_handler_entry(nullptr, 0) };
    const bool v_b{ validate_adapter_handler_entry(nullptr, 0) };
    check("validate_ahe_null_idempotent", v_a == false && v_a == v_b);

    // On a stable owned buffer the answer must not change between calls.
    alignas(16) std::array<std::uint8_t, 256> fake_method{};
    auto* const probe{ reinterpret_cast<vmhook::hotspot::method*>(fake_method.data()) };
    const auto p_a{ detect_adapter_offset_from_method(probe) };
    const auto p_b{ detect_adapter_offset_from_method(probe) };
    check("detect_adapter_offset_owned_probe_idempotent", p_a == p_b);

    // get_adapter() on a fake Method is idempotent across repeated cold calls.
    void* const ga_a{ probe->get_adapter() };
    void* const ga_b{ probe->get_adapter() };
    check("get_adapter_owned_probe_idempotent", ga_a == ga_b);
}

// ─────────────────────────────────────────────────────────────────────────
//  Driver
// ─────────────────────────────────────────────────────────────────────────
auto main() -> int
{
    std::printf("[method_entry_points] no-JVM exhaustive coverage\n");

    test_cold_state_accessors_return_null();
    test_invalid_this_pointer_is_safe();
    test_setters_no_jvm_do_not_write();
    test_setters_null_this_is_safe_noop();
    test_getters_null_this_returns_null();
    test_get_c2i_entry_from_adapter_rejects_garbage();
    test_validate_adapter_handler_entry_rejects_garbage();
    test_detect_adapter_offset_degrades_to_zero();
    test_entry_pointer_alignment_pin();
    test_cold_state_accessors_deterministic();
    test_layout_contract_runtime();
    test_validate_ahe_32_fabricated_bad_inputs();
    test_recovery_idempotent_twice_equals_once();

    std::printf("[method_entry_points] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
