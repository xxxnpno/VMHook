// Standalone (no-JVM) unit tests for the Method-flags accessors that back the
// JIT-inhibitor (set_dont_inline / get_flags) and the static-dispatch decision
// (method_proxy::is_static -> get_access_flags).  This is the "FIX E" feature
// surface: the width / offset / null-safety of Method::_flags and
// Method::_access_flags reads and writes.
//
// EVERYTHING here runs WITHOUT a live JVM in-process.  With no jvm.dll /
// libjvm.so loaded:
//   * get_jvm_module() -> nullptr, so get_vm_structs() caches nullptr and
//     iterate_struct_entries("Method", ...) returns nullptr for EVERY field.
//   * get_flags() and get_access_flags() therefore find a null VMStruct entry
//     and return nullptr (get_flags additionally guards is_valid_pointer(this)).
//   * set_dont_inline() consequently no-ops, and method_proxy::is_static()
//     returns false (its access-flags slot is unresolvable).
// This file proves the NULL / INVALID-POINTER / no-JVM degradation paths are
// crash-free and side-effect-free — the standalone half of the FIX-E coverage
// gap.  The VALUE-correctness of the reads/writes (the right bytes toggle, the
// adjacent field is never clobbered) requires a populated gHotSpotVMStructs and
// a live Method, so it is exercised by the live-JVM modules
// (dont_inline_dont_compile, method_static_portability, method_call_jni_fallback)
// and is OUT OF SCOPE for this pure no-JVM file — a fabricated in-process
// VMStructs array cannot be driven through the real accessors because they read
// the JVM-exported global symbol, not an array parameter.
//
// In addition this file pins, as compile-time + runtime assertions, the
// AUTHORITATIVE HotSpot Method-flags layout facts across JDK 8..26 (verified
// against the OpenJDK source: method.hpp / accessFlags.hpp / methodFlags.hpp /
// vmStructs.cpp on jdk8u, jdk17u, jdk21u and master).  These constants document
// the width / bit-position contract that any future width-aware get_flags /
// set_dont_inline (the deferred FACET A) must satisfy, and give that change a
// standalone regression anchor.

#include <vmhook/vmhook.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ─────────────────────────────────────────────────────────────────────────
//  AUTHORITATIVE HotSpot Method-flags layout facts (JDK 8..26).
//
//  These are the ground truth the FIX-E feature is about.  They are NOT read
//  from the header (the header hard-codes the JDK 11..20 u2 / bit-2 case); they
//  are the cross-version contract, verified against OpenJDK source, that a
//  width-aware accessor must honour.  Keeping them here as a typed table makes
//  the per-version expectations explicit and greppable.
// ─────────────────────────────────────────────────────────────────────────
namespace flags_layout
{
    // Method::_flags ("the internal HotSpot method flags", home of _dont_inline).
    //   JDK 8        : no `_flags` member at all (u1 bitfield group, NOT a named
    //                  field; gHotSpotVMStructs exports nothing) -> width 0.
    //   JDK 11..20   : `mutable u2 _flags`, exported as nonstatic_field(Method,
    //                  _flags, u2); _dont_inline == bit 2.
    //   JDK 21..23   : `MethodFlags _flags` (u4 _status), NOT exported;
    //                  _dont_inline == bit 12.
    //   JDK 24..26   : `MethodFlags _flags` (u4 _status), NOT exported;
    //                  _dont_inline == bit 12.  (AccessFlags shrank to u2 in 24,
    //                  but that is _access_flags, not _flags.)
    struct band
    {
        const char* name;
        int         flags_width_bytes;   // 0 == no exported _flags member
        bool        flags_exported;      // present in gHotSpotVMStructs?
        int         dont_inline_bit;     // bit position of _dont_inline
        int         access_flags_width;  // sizeof(AccessFlags): 4 (<=23) or 2 (24+)
    };

    constexpr band jdk8     { "jdk8",      0, false,  2, 4 };
    constexpr band jdk11_20 { "jdk11..20", 2, true,   2, 4 };
    constexpr band jdk21_23 { "jdk21..23", 4, false, 12, 4 };
    constexpr band jdk24_26 { "jdk24..26", 4, false, 12, 2 };

    // JVM_ACC_STATIC is a class-file flag; it lives in _access_flags on EVERY
    // JDK 8..26 and never moved.  Bit 3 / value 0x0008.  This is exactly why
    // reading the flags word as u4 and masking 0x0008 is width-independent and
    // remains correct on JDK 24+ where AccessFlags itself became u2.
    constexpr std::uint32_t jvm_acc_static = 0x0008u;

    // The hard-coded mask the library uses for _dont_inline on the supported
    // (JDK 11..20) band.  1u << 2 fits inside u1/u2/u4 alike — only the ACCESS
    // WIDTH must vary per band, never the mask's representability.
    constexpr std::uint32_t dont_inline_mask_supported = (1u << 2);
}

// Compile-time pinning of the layout contract (a future width-aware accessor
// that changes any of these has to consciously update this table).
static_assert(flags_layout::jdk11_20.dont_inline_bit == 2,
              "JDK 11..20 _dont_inline is bit 2 (matches set_dont_inline's 1<<2)");
static_assert(flags_layout::jdk21_23.dont_inline_bit == 12
              && flags_layout::jdk24_26.dont_inline_bit == 12,
              "JDK 21+ relocated _dont_inline to bit 12 (MethodFlags _status)");
static_assert(flags_layout::jdk11_20.flags_width_bytes == 2,
              "JDK 11..20 Method::_flags is u2 — the only width the library reads");
static_assert(flags_layout::jdk24_26.access_flags_width == 2
              && flags_layout::jdk21_23.access_flags_width == 4,
              "AccessFlags shrank u4 -> u2 in JDK 24 (JDK-8339113)");
static_assert((flags_layout::dont_inline_mask_supported & 0xFFFFu) == flags_layout::dont_inline_mask_supported,
              "the _dont_inline mask fits in the low 16 bits (u2-safe)");

// ─────────────────────────────────────────────────────────────────────────
//  1. set_dont_inline(nullptr, ...) is a crash-free no-op.
// ─────────────────────────────────────────────────────────────────────────
static auto test_set_dont_inline_null() -> void
{
    // The guard added for FIX E (flaw #3) makes this return before forming
    // `this + offset` from a null base.  Reaching the line after the calls is
    // the assertion: a deref would have crashed the process.
    vmhook::hotspot::set_dont_inline(nullptr, true);
    vmhook::hotspot::set_dont_inline(nullptr, false);
    check("set_dont_inline_null_pointer_is_safe_noop", true);
}

// ─────────────────────────────────────────────────────────────────────────
//  2. set_dont_inline(<invalid / sentinel pointer>, ...) writes NOTHING.
//
//  A freed/garbage Method* (sentinel low-32 patterns, an odd address, or an
//  out-of-user-range address) must be rejected by is_valid_pointer() BEFORE any
//  read-modify-write.  We can only positively prove "no write happened" for a
//  pointer we own, so case (b) below uses a real stack buffer whose address is
//  in range but whose VMStruct entry is absent (no JVM) — the RMW is suppressed
//  at the get_flags() null-entry stage, leaving the buffer byte-for-byte intact.
// ─────────────────────────────────────────────────────────────────────────
static auto test_set_dont_inline_invalid_pointer() -> void
{
    // (a) Classic sentinel / wild addresses: rejected by is_valid_pointer().
    //     There is nothing to clobber-check (we don't own them); the assertion
    //     is simply that the call returns without faulting.  Route the integer
    //     literals through std::uintptr_t so the cast is width-correct on LLP64
    //     (a bare `unsigned int` -> 64-bit pointer trips MSVC C4312 under /WX).
    vmhook::hotspot::set_dont_inline(
        reinterpret_cast<const vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0xDEADBEEFu)), true);
    vmhook::hotspot::set_dont_inline(
        reinterpret_cast<const vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x1u)), true);   // odd -> rejected
    vmhook::hotspot::set_dont_inline(
        reinterpret_cast<const vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x8u)), false);  // below floor
    check("set_dont_inline_sentinel_pointers_are_safe_noop", true);

    // (b) An in-range, owned buffer standing in for a Method.  With no JVM the
    //     Method::_flags VMStruct entry is absent, so get_flags() returns nullptr
    //     and set_dont_inline must touch NONE of the buffer.  Fill with a
    //     recognizable pattern and assert it survives both set and clear.
    alignas(16) std::array<std::uint8_t, 64> fake_method{};
    fake_method.fill(0xAB);
    const std::array<std::uint8_t, 64> snapshot{ fake_method };

    auto* const as_method{ reinterpret_cast<const vmhook::hotspot::method*>(fake_method.data()) };
    vmhook::hotspot::set_dont_inline(as_method, true);
    vmhook::hotspot::set_dont_inline(as_method, false);

    check("set_dont_inline_no_jvm_does_not_write_any_byte",
          std::memcmp(fake_method.data(), snapshot.data(), fake_method.size()) == 0);
}

// ─────────────────────────────────────────────────────────────────────────
//  3. get_flags() / get_access_flags() return nullptr with no JVM, and
//     get_flags() additionally rejects an invalid `this` without faulting.
// ─────────────────────────────────────────────────────────────────────────
static auto test_flag_accessors_no_jvm_null() -> void
{
    alignas(16) std::array<std::uint8_t, 64> fake_method{};
    fake_method.fill(0x5A);
    auto* const as_method{ reinterpret_cast<vmhook::hotspot::method*>(fake_method.data()) };

    // No JVM -> Method::_flags / Method::_access_flags VMStruct entries are
    // absent -> both accessors return nullptr (never a wild interior pointer).
    check("get_flags_no_jvm_returns_null", as_method->get_flags() == nullptr);
    check("get_access_flags_no_jvm_returns_null", as_method->get_access_flags() == nullptr);

    // get_flags() on a deliberately-invalid `this` (odd address) must short
    // out on the is_valid_pointer(this) guard rather than computing this+offset.
    auto* const bogus{ reinterpret_cast<vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x3u)) };
    check("get_flags_invalid_this_returns_null", bogus->get_flags() == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
//  4. method_proxy::is_static() degrades to false with no JVM and never derefs
//     a bad Method*.  This is the access-flags analogue that backs the
//     static-vs-instance dispatch decision (the Facet-B disjunct
//     is_static_call = object==nullptr || is_static()).  Its VALUE correctness
//     for real static vs instance methods is pinned on every JDK by the green
//     method_static_portability module; here we only prove the no-JVM/no-deref
//     contract that makes the disjunct safe to evaluate unconditionally.
// ─────────────────────────────────────────────────────────────────────────
static auto test_method_proxy_is_static_no_jvm() -> void
{
    // Null Method* -> is_static() short-circuits to false (no get_access_flags).
    vmhook::method_proxy null_method_proxy{ nullptr, nullptr, std::string{ "(I)I" } };
    check("is_static_null_method_returns_false", null_method_proxy.is_static() == false);

    // In-range fake Method* but no JVM -> get_access_flags() returns nullptr ->
    // is_static() returns false (the documented "slot can't be resolved" path).
    alignas(16) std::array<std::uint8_t, 64> fake_method{};
    fake_method.fill(0x10);  // low byte has bit 3 CLEAR; even if it were read, the
                             // point is the entry is absent so it is never read.
    auto* const as_method{ reinterpret_cast<vmhook::hotspot::method*>(fake_method.data()) };

    // A null-receiver (static-style) proxy and an instance-style proxy (non-null
    // owning object) must BOTH report is_static()==false here, because the JVM
    // truth source (the access-flags word) is unreachable without a JVM.
    vmhook::method_proxy static_style_proxy{ nullptr, as_method, std::string{ "()V" } };
    std::uint8_t pseudo_receiver{ 0 };
    vmhook::method_proxy instance_style_proxy{ &pseudo_receiver, as_method, std::string{ "(I)I" } };

    check("is_static_no_jvm_static_style_false", static_style_proxy.is_static() == false);
    check("is_static_no_jvm_instance_style_false", instance_style_proxy.is_static() == false);
}

// ─────────────────────────────────────────────────────────────────────────
//  5. Runtime echo of the layout contract (mirrors the static_asserts so the
//     facts also show up as named PASS lines in CI logs, and so a stray macro
//     redefinition of the constants is caught at runtime too).
// ─────────────────────────────────────────────────────────────────────────
static auto test_layout_contract_runtime() -> void
{
    using namespace flags_layout;

    check("layout_jdk8_no_exported_flags",
          jdk8.flags_exported == false && jdk8.flags_width_bytes == 0);
    check("layout_jdk11_20_u2_flags_exported_bit2",
          jdk11_20.flags_exported && jdk11_20.flags_width_bytes == 2 && jdk11_20.dont_inline_bit == 2);
    check("layout_jdk21_23_methodflags_u4_unexported_bit12",
          jdk21_23.flags_exported == false && jdk21_23.flags_width_bytes == 4 && jdk21_23.dont_inline_bit == 12);
    check("layout_jdk24_26_methodflags_u4_unexported_bit12",
          jdk24_26.flags_exported == false && jdk24_26.flags_width_bytes == 4 && jdk24_26.dont_inline_bit == 12);

    // _access_flags width: u4 through JDK 23, u2 from JDK 24 (JDK-8339113).
    check("layout_access_flags_u4_through_jdk23",
          jdk8.access_flags_width == 4 && jdk11_20.access_flags_width == 4 && jdk21_23.access_flags_width == 4);
    check("layout_access_flags_u2_from_jdk24", jdk24_26.access_flags_width == 2);

    // JVM_ACC_STATIC is bit 3 and lives in the low byte -> width-independent.
    check("jvm_acc_static_is_low_byte_bit3",
          jvm_acc_static == 0x0008u && (jvm_acc_static & 0xFFu) == jvm_acc_static);

    // The supported-band _dont_inline mask is u2-safe.
    check("dont_inline_mask_fits_u2",
          (dont_inline_mask_supported & 0xFFFFu) == dont_inline_mask_supported);
}

int main()
{
    test_set_dont_inline_null();
    test_set_dont_inline_invalid_pointer();
    test_flag_accessors_no_jvm_null();
    test_method_proxy_is_static_no_jvm();
    test_layout_contract_runtime();

    std::printf("\n%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
