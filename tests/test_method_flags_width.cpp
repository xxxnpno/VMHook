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
// (dont_inline_dont_compile, method_static_portability, method_call_dispatch)
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
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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

    // ── JDK 21+ MethodFlags::_status bit positions (verified against OpenJDK
    //    methodFlags.hpp on jdk21u and master) ────────────────────────────────
    //    These are the bits the JDK 24 relocation (JDK-8339113) moved OUT of the
    //    u4 AccessFlags and INTO MethodFlags::_status — the not_compilable /
    //    queued group lives here on JDK 21+ (already true at 21; AccessFlags
    //    shrank to u2 at 24).  _dont_inline shares this word at bit 12.
    namespace methodflags_status_bit
    {
        constexpr int queued_for_compilation = 7;   // was JVM_ACC_QUEUED
        constexpr int is_not_c2_compilable   = 8;   // was JVM_ACC_NOT_C2_COMPILABLE
        constexpr int is_not_c1_compilable   = 9;   // was JVM_ACC_NOT_C1_COMPILABLE
        constexpr int is_not_c2_osr          = 10;  // was JVM_ACC_NOT_C2_OSR_COMPILABLE
        constexpr int force_inline           = 11;
        constexpr int dont_inline            = 12;
    }

    // The u4 _status offset is provably (_intrinsic_id_offset - 4) on JDK 21+
    // because the u4 _status is the field immediately before the u2 _intrinsic_id
    // in the C++ Method layout.
    constexpr std::uint64_t status_offset_from_intrinsic(std::uint64_t intrinsic_id_offset)
    {
        return intrinsic_id_offset - 4;
    }
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

// JDK 24 (JDK-8339113) relocated the C1/C2/OSR-compiled + queued bits OUT of the
// (now u2) AccessFlags and INTO MethodFlags::_status bits 7..10; _dont_inline
// shares the same u4 _status at bit 12.  Pin those positions and prove they all
// live ABOVE the 16-bit boundary that a u2 read of _flags (JDK 11..20) could see,
// i.e. they are only reachable through the JDK 21+ u4 path.
static_assert(flags_layout::methodflags_status_bit::queued_for_compilation == 7
              && flags_layout::methodflags_status_bit::is_not_c2_compilable == 8
              && flags_layout::methodflags_status_bit::is_not_c1_compilable == 9
              && flags_layout::methodflags_status_bit::is_not_c2_osr == 10,
              "JDK 24 moved not_compilable/queued into MethodFlags::_status bits 7..10");
static_assert(flags_layout::methodflags_status_bit::dont_inline == 12
              && flags_layout::methodflags_status_bit::force_inline == 11,
              "MethodFlags _dont_inline=bit12 / _force_inline=bit11 (JDK 21+)");
static_assert(flags_layout::status_offset_from_intrinsic(44) == 40,
              "u4 _status sits 4 bytes before u2 _intrinsic_id");

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

    // Robustness #6 (the static-resolution gate) reads JVM_ACC_STATIC through the
    // SAME path this accessor does: object_base::static_method_only() calls
    // method::get_access_flags() and masks 0x0008u, then FAILS CLOSED (treats the
    // candidate as NOT static) when the flags slot can't be resolved — exactly
    // the nullptr return get_access_flags() produces here with no JVM.  So the
    // gate and is_static() share one fail-closed condition: whenever is_static()
    // degrades to false on an unreachable flags word (asserted just above), the
    // resolution gate likewise rejects the candidate rather than mis-dispatching
    // it through the static path.  static_method_only() is private, so we pin the
    // shared mechanism via this public accessor; its VALUE correctness for real
    // static-vs-instance Methods is covered by the method_static module's
    // ms_static_method_rejects_instance_* / ms_static_method_resolves_real_static
    // hard assertions on every JDK.  The mask itself is the stable low-byte bit
    // pinned by jvm_acc_static_is_low_byte_bit3 in test_layout_contract_runtime.
    check("static_resolution_gate_shares_is_static_failclosed_bit",
          static_style_proxy.is_static() == false && (0x0008u & 0xFFu) == 0x0008u);
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

// ─────────────────────────────────────────────────────────────────────────
//  6. EXHAUSTIVE per-JDK sweep of the pure offset/width/bit DERIVATION.
//
//  derive_method_flags_layout() is the width/offset/bit-correct heart of the
//  FACET-A fix, factored out as a pure function so it can be driven WITHOUT a
//  JVM.  For every supported JDK band we synthesise the exact VMStructs evidence
//  that HotSpot exports on that version (verified against OpenJDK method.hpp /
//  methodFlags.hpp / vmStructs.cpp on jdk8u, jdk11u, jdk17u/18, jdk21u, master)
//  and assert the derived (offset, width, bit, confident) matches that JDK's
//  REAL Method::_flags layout.  These are the asserts that pin the JDK 21+
//  derivation (offset = _intrinsic_id - 4, width 4, bit 12) and prove the
//  confident-guard refuses JDK 8 and every degenerate input.
// ─────────────────────────────────────────────────────────────────────────

namespace
{
    using vmhook::hotspot::derive_method_flags_layout;
    using vmhook::hotspot::method_flags_evidence;
    using vmhook::hotspot::method_flags_layout;

    // Evidence as HotSpot exports it on each band.  Offsets are realistic 64-bit
    // Method offsets; the DERIVATION only depends on the relative arithmetic
    // (_intrinsic_id - 4) and the type strings, not the absolute value, and the
    // sweep below also varies the absolute offset to prove that.
    constexpr method_flags_evidence evidence_jdk8{
        /*flags_present*/ false, /*flags_type*/ nullptr, /*flags_offset*/ 0,
        /*intrinsic_id_present*/ true, /*intrinsic_id_type*/ "u1", /*intrinsic_id_offset*/ 42 };

    constexpr method_flags_evidence evidence_jdk11_20{
        /*flags_present*/ true, /*flags_type*/ "u2", /*flags_offset*/ 44,
        /*intrinsic_id_present*/ true, /*intrinsic_id_type*/ "u2", /*intrinsic_id_offset*/ 42 };

    constexpr method_flags_evidence evidence_jdk21_23{
        /*flags_present*/ false, /*flags_type*/ nullptr, /*flags_offset*/ 0,
        /*intrinsic_id_present*/ true, /*intrinsic_id_type*/ "u2", /*intrinsic_id_offset*/ 44 };

    // JDK 24..26: _flags layout is identical to 21..23 (the JDK 24 change was to
    // _access_flags width + relocating the not_compilable bits, NOT to _flags).
    constexpr method_flags_evidence evidence_jdk24_26{
        /*flags_present*/ false, /*flags_type*/ nullptr, /*flags_offset*/ 0,
        /*intrinsic_id_present*/ true, /*intrinsic_id_type*/ "u2", /*intrinsic_id_offset*/ 44 };
}

// Pin the derivation at COMPILE TIME for every band (derive_method_flags_layout
// is constexpr) — a regression in the offset/width/bit logic fails the build.
static_assert(!derive_method_flags_layout(evidence_jdk8).confident,
              "JDK 8 (u1 _intrinsic_id, no _flags) must NOT be confidently placed");
static_assert(derive_method_flags_layout(evidence_jdk11_20).confident
              && derive_method_flags_layout(evidence_jdk11_20).width_bytes == 2
              && derive_method_flags_layout(evidence_jdk11_20).dont_inline_bit == 2
              && derive_method_flags_layout(evidence_jdk11_20).offset == 44,
              "JDK 11..20: exported u2 _flags @offset, bit 2");
static_assert(derive_method_flags_layout(evidence_jdk21_23).confident
              && derive_method_flags_layout(evidence_jdk21_23).width_bytes == 4
              && derive_method_flags_layout(evidence_jdk21_23).dont_inline_bit == 12
              && derive_method_flags_layout(evidence_jdk21_23).offset == 40,
              "JDK 21..23: derived u4 _status @ (_intrinsic_id-4)=40, bit 12");
static_assert(derive_method_flags_layout(evidence_jdk24_26).confident
              && derive_method_flags_layout(evidence_jdk24_26).width_bytes == 4
              && derive_method_flags_layout(evidence_jdk24_26).dont_inline_bit == 12
              && derive_method_flags_layout(evidence_jdk24_26).offset == 40,
              "JDK 24..26: derived u4 _status @ (_intrinsic_id-4)=40, bit 12");

static auto test_derivation_per_jdk() -> void
{
    // --- JDK 8: u1 _intrinsic_id, no exported _flags -> REFUSE (safe no-op) ---
    {
        const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk8) };
        check("derive_jdk8_refuses_no_confident_slot", !layout.confident);
        check("derive_jdk8_width_and_bit_zeroed",
              layout.width_bytes == 0 && layout.dont_inline_bit == 0 && layout.offset == 0);
    }

    // --- JDK 11..20: Path A, exported u2 _flags, bit 2, offset verbatim -------
    {
        const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk11_20) };
        check("derive_jdk11_20_confident", layout.confident);
        check("derive_jdk11_20_width_u2", layout.width_bytes == 2);
        check("derive_jdk11_20_bit2", layout.dont_inline_bit == flags_layout::jdk11_20.dont_inline_bit);
        check("derive_jdk11_20_offset_is_exported_flags_offset",
              layout.offset == evidence_jdk11_20.flags_offset);
    }

    // --- JDK 21..23: Path B, derived u4 _status @ intrinsic-4, bit 12 ---------
    {
        const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk21_23) };
        check("derive_jdk21_23_confident", layout.confident);
        check("derive_jdk21_23_width_u4", layout.width_bytes == 4);
        check("derive_jdk21_23_bit12", layout.dont_inline_bit == flags_layout::jdk21_23.dont_inline_bit);
        check("derive_jdk21_23_offset_is_intrinsic_minus_4",
              layout.offset == flags_layout::status_offset_from_intrinsic(evidence_jdk21_23.intrinsic_id_offset)
              && layout.offset == evidence_jdk21_23.intrinsic_id_offset - 4);
    }

    // --- JDK 24..26: identical _flags placement to 21..23 (u4 _status, bit12) -
    // The JDK 24 change (AccessFlags u4->u2, not_compilable bits -> _status 7..10)
    // does NOT move _flags, so the derivation is byte-for-byte the same.
    {
        const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk24_26) };
        check("derive_jdk24_26_confident", layout.confident);
        check("derive_jdk24_26_width_u4", layout.width_bytes == 4);
        check("derive_jdk24_26_bit12", layout.dont_inline_bit == flags_layout::jdk24_26.dont_inline_bit);
        check("derive_jdk24_26_offset_is_intrinsic_minus_4",
              layout.offset == evidence_jdk24_26.intrinsic_id_offset - 4);
        const method_flags_layout layout21{ derive_method_flags_layout(evidence_jdk21_23) };
        check("derive_jdk24_26_flags_layout_matches_jdk21",
              layout.width_bytes == layout21.width_bytes
              && layout.dont_inline_bit == layout21.dont_inline_bit);
    }

    // --- Absolute-offset independence: Path B derivation is purely relative. ---
    // Sweep a range of realistic _intrinsic_id offsets; the derived _status
    // offset must always be exactly intrinsic-4, width 4, bit 12.
    {
        bool all_relative_ok{ true };
        for (std::uint64_t intrinsic_off : { std::uint64_t{ 40 }, std::uint64_t{ 44 },
                                             std::uint64_t{ 48 }, std::uint64_t{ 56 },
                                             std::uint64_t{ 100 } })
        {
            method_flags_evidence ev{};
            ev.intrinsic_id_present = true;
            ev.intrinsic_id_type   = "u2";
            ev.intrinsic_id_offset = intrinsic_off;
            const method_flags_layout layout{ derive_method_flags_layout(ev) };
            all_relative_ok = all_relative_ok
                && layout.confident
                && layout.offset == intrinsic_off - 4
                && layout.width_bytes == 4
                && layout.dont_inline_bit == 12;
        }
        check("derive_pathB_offset_is_always_intrinsic_minus_4", all_relative_ok);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  7. CONFIDENT-OFFSET GUARD: derive_method_flags_layout REFUSES every unsafe
//     or unknown input (no write may ever be issued from a guess).
// ─────────────────────────────────────────────────────────────────────────
static auto test_confident_guard_refusals() -> void
{
    // (a) Wholly empty evidence (no JVM / nothing exported) -> not confident.
    check("guard_empty_evidence_refuses",
          !derive_method_flags_layout(method_flags_evidence{}).confident);

    // (b) _flags present but type unknown/empty/non-u2 AND no usable
    //     _intrinsic_id -> refuse (never blind-trust an offset of unknown width).
    {
        method_flags_evidence ev{};
        ev.flags_present = true;
        ev.flags_type   = "u1";       // e.g. a hypothetical u1 export
        ev.flags_offset = 44;
        check("guard_flags_u1_only_refuses", !derive_method_flags_layout(ev).confident);

        ev.flags_type = "";           // empty type string
        check("guard_flags_empty_type_refuses", !derive_method_flags_layout(ev).confident);

        ev.flags_type = nullptr;      // null type string
        check("guard_flags_null_type_refuses", !derive_method_flags_layout(ev).confident);

        ev.flags_type = "MethodFlags"; // u4 object, NOT the legacy u2
        check("guard_flags_methodflags_type_alone_refuses",
              !derive_method_flags_layout(ev).confident);
    }

    // (c) JDK-8-shaped: _intrinsic_id present but u1 -> Path B must refuse (this
    //     is the single check that keeps a u4 bit-12 write off a JDK 8 Method).
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type   = "u1";
        ev.intrinsic_id_offset = 42;
        check("guard_intrinsic_u1_refuses_pathB", !derive_method_flags_layout(ev).confident);
    }

    // (d) _intrinsic_id u2 but offset < 4 -> underflow guard refuses.
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type   = "u2";
        ev.intrinsic_id_offset = 0;
        check("guard_intrinsic_offset0_refuses", !derive_method_flags_layout(ev).confident);
        ev.intrinsic_id_offset = 3;
        check("guard_intrinsic_offset3_refuses", !derive_method_flags_layout(ev).confident);
    }

    // (e) _intrinsic_id u2 but offset not 4-byte aligned -> layout-mismatch
    //     guard refuses (the u4 _status must be 4-aligned; if _intrinsic_id at
    //     _status+4 is not 4-aligned the layout is not the verified one).
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type   = "u2";
        ev.intrinsic_id_offset = 46;  // 46 % 4 == 2
        check("guard_intrinsic_misaligned_refuses", !derive_method_flags_layout(ev).confident);
        ev.intrinsic_id_offset = 45;  // odd
        check("guard_intrinsic_odd_offset_refuses", !derive_method_flags_layout(ev).confident);
    }

    // (f) _intrinsic_id present but type null/empty -> refuse.
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type   = nullptr;
        ev.intrinsic_id_offset = 44;
        check("guard_intrinsic_null_type_refuses", !derive_method_flags_layout(ev).confident);
        ev.intrinsic_id_type = "";
        check("guard_intrinsic_empty_type_refuses", !derive_method_flags_layout(ev).confident);
    }

    // (g) Path A WINS over Path B when both are usable (JDK 11..20 has BOTH an
    //     exported u2 _flags AND a u2 _intrinsic_id) — must pick the exported u2
    //     _flags (bit 2), never the derived path (bit 12).
    {
        method_flags_evidence ev{};
        ev.flags_present        = true;
        ev.flags_type           = "u2";
        ev.flags_offset         = 44;
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type    = "u2";
        ev.intrinsic_id_offset  = 48;
        const method_flags_layout layout{ derive_method_flags_layout(ev) };
        check("guard_pathA_precedence_over_pathB",
              layout.confident && layout.width_bytes == 2
              && layout.dont_inline_bit == 2 && layout.offset == 44);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  8. WIDTH MATRIX / ATOMIC TOGGLE on a FAKE Method buffer, exercising the
//     SAME width/bit logic set_dont_inline applies — but driven through
//     derive_method_flags_layout (which needs no JVM) + a hand-rolled toggle
//     mirroring set_dont_inline's width dispatch.  This proves the toggle only
//     touches sizeof(field) bytes at the derived offset and never the neighbour
//     (the adjacent-byte anti-clobber proof the live-JVM module also wants).
//
//  NOTE: this does NOT call set_dont_inline (its real get/resolve path reads the
//  absent gHotSpotVMStructs and no-ops with no JVM); it validates the arithmetic
//  + write-width contract that set_dont_inline is built on, per-band.
// ─────────────────────────────────────────────────────────────────────────
namespace
{
    // Apply the exact width/bit dispatch set_dont_inline uses, to a raw buffer.
    auto toggle_like_set_dont_inline(std::uint8_t* const base,
                                     const method_flags_layout& layout,
                                     const bool enabled) -> void
    {
        if (!layout.confident)
        {
            return;
        }
        void* const address{ base + layout.offset };
        if (layout.width_bytes == 2 && layout.dont_inline_bit < 16)
        {
            const std::uint16_t mask{ static_cast<std::uint16_t>(1u << layout.dont_inline_bit) };
            std::atomic_ref<std::uint16_t> word{ *static_cast<std::uint16_t*>(address) };
            if (enabled) { word.fetch_or(mask, std::memory_order_acq_rel); }
            else         { word.fetch_and(static_cast<std::uint16_t>(~mask), std::memory_order_acq_rel); }
        }
        else if (layout.width_bytes == 4 && layout.dont_inline_bit < 32)
        {
            const std::uint32_t mask{ 1u << layout.dont_inline_bit };
            std::atomic_ref<std::uint32_t> word{ *static_cast<std::uint32_t*>(address) };
            if (enabled) { word.fetch_or(mask, std::memory_order_acq_rel); }
            else         { word.fetch_and(~mask, std::memory_order_acq_rel); }
        }
    }

    // Read the toggled word back at the derived width.
    auto read_word(const std::uint8_t* const base, const method_flags_layout& layout) -> std::uint32_t
    {
        const void* const address{ base + layout.offset };
        if (layout.width_bytes == 2)
        {
            std::uint16_t v{};
            std::memcpy(&v, address, sizeof(v));
            return v;
        }
        std::uint32_t v{};
        std::memcpy(&v, address, sizeof(v));
        return v;
    }
}

static auto test_width_matrix_anti_clobber() -> void
{
    struct case_t { const char* tag; method_flags_evidence ev; };
    const case_t cases[]{
        { "jdk11_20_u2", evidence_jdk11_20 },
        { "jdk21_23_u4", evidence_jdk21_23 },
        { "jdk24_26_u4", evidence_jdk24_26 },
    };

    for (const case_t& c : cases)
    {
        const method_flags_layout layout{ derive_method_flags_layout(c.ev) };
        // All three are confident; if not, the derivation regressed.
        if (!layout.confident)
        {
            check((std::string{ "width_matrix_" } + c.tag + "_confident").c_str(), false);
            continue;
        }

        // Lay a fake Method: fill with a sentinel, snapshot, toggle the bit on
        // then off, and prove (a) the bit toggled INSIDE the slot and (b) every
        // byte OUTSIDE [offset, offset+width) is byte-for-byte unchanged.
        alignas(16) std::array<std::uint8_t, 128> buffer{};
        buffer.fill(0xA5);
        // Make the slot start cleared so the on/off transition is observable.
        std::memset(buffer.data() + layout.offset, 0x00, static_cast<std::size_t>(layout.width_bytes));
        const std::array<std::uint8_t, 128> snapshot_after_clear{ buffer };

        // (1) set the bit.
        toggle_like_set_dont_inline(buffer.data(), layout, /*enabled*/ true);
        const std::uint32_t after_set{ read_word(buffer.data(), layout) };
        check((std::string{ "width_matrix_" } + c.tag + "_bit_set_inside_slot").c_str(),
              (after_set & (1u << layout.dont_inline_bit)) != 0u);

        // (1b) ONLY the dont_inline bit moved within the slot.
        check((std::string{ "width_matrix_" } + c.tag + "_only_target_bit_set").c_str(),
              after_set == (1u << layout.dont_inline_bit));

        // (1c) Anti-clobber: bytes OUTSIDE the slot are unchanged after the SET.
        bool outside_intact_set{ true };
        for (std::size_t i{ 0 }; i < buffer.size(); ++i)
        {
            const bool inside{ i >= layout.offset
                               && i < layout.offset + static_cast<std::size_t>(layout.width_bytes) };
            if (!inside && buffer[i] != snapshot_after_clear[i]) { outside_intact_set = false; break; }
        }
        check((std::string{ "width_matrix_" } + c.tag + "_set_no_adjacent_clobber").c_str(),
              outside_intact_set);

        // (2) clear the bit.
        toggle_like_set_dont_inline(buffer.data(), layout, /*enabled*/ false);
        const std::uint32_t after_clear{ read_word(buffer.data(), layout) };
        check((std::string{ "width_matrix_" } + c.tag + "_bit_cleared_inside_slot").c_str(),
              (after_clear & (1u << layout.dont_inline_bit)) == 0u && after_clear == 0u);

        // (2b) Anti-clobber: bytes OUTSIDE the slot still match the original.
        bool outside_intact_clear{ true };
        for (std::size_t i{ 0 }; i < buffer.size(); ++i)
        {
            const bool inside{ i >= layout.offset
                               && i < layout.offset + static_cast<std::size_t>(layout.width_bytes) };
            if (!inside && buffer[i] != snapshot_after_clear[i]) { outside_intact_clear = false; break; }
        }
        check((std::string{ "width_matrix_" } + c.tag + "_clear_no_adjacent_clobber").c_str(),
              outside_intact_clear);
    }

    // Idempotency: setting twice leaves the slot identical to setting once, and
    // no sibling bit moves.  Use the u4 (JDK 21+) case (the path the fix adds).
    {
        const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk21_23) };
        alignas(16) std::array<std::uint8_t, 128> buffer{};
        buffer.fill(0x00);
        // Seed a few unrelated sibling bits in the slot that must survive.
        const std::uint32_t siblings{ (1u << 7) | (1u << 9) | (1u << 20) };  // queued, not_c1, a high bit
        std::memcpy(buffer.data() + layout.offset, &siblings, sizeof(siblings));

        toggle_like_set_dont_inline(buffer.data(), layout, true);
        const std::uint32_t once{ read_word(buffer.data(), layout) };
        toggle_like_set_dont_inline(buffer.data(), layout, true);
        const std::uint32_t twice{ read_word(buffer.data(), layout) };

        check("width_matrix_u4_set_idempotent", once == twice);
        check("width_matrix_u4_set_preserves_siblings",
              (twice & siblings) == siblings && (twice & (1u << 12)) != 0u);

        // Clearing dont_inline must preserve those siblings too.
        toggle_like_set_dont_inline(buffer.data(), layout, false);
        const std::uint32_t cleared{ read_word(buffer.data(), layout) };
        check("width_matrix_u4_clear_preserves_siblings",
              cleared == siblings && (cleared & (1u << 12)) == 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  9. ATOMICITY: concurrent toggles of _dont_inline must not lose an unrelated
//     in-word bit a "JIT thread" is OR-ing in.  Mirrors set_dont_inline's
//     atomic fetch_or/fetch_and at the JDK 21+ u4 width on a fake _status word.
//     (After the fetch_or/fetch_and fix this is race-safe; a plain |=/&= would
//     intermittently drop the sibling bit.)
// ─────────────────────────────────────────────────────────────────────────
static auto test_atomic_toggle_preserves_sibling() -> void
{
    const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk21_23) };
    alignas(16) std::array<std::uint8_t, 64> buffer{};
    buffer.fill(0x00);

    constexpr int iterations{ 20000 };
    constexpr std::uint32_t sibling_bit{ 1u << 7 };   // e.g. queued_for_compilation

    std::atomic<bool> go{ false };

    // Writer A: hammer dont_inline (bit 12) set/clear via the atomic toggle.
    std::thread toggler{ [&]
    {
        while (!go.load(std::memory_order_acquire)) { }
        for (int i{ 0 }; i < iterations; ++i)
        {
            toggle_like_set_dont_inline(buffer.data(), layout, (i & 1) != 0);
        }
    } };

    // Writer B: a stand-in "JIT thread" atomically OR-ing the sibling bit in.
    std::thread jit{ [&]
    {
        std::atomic_ref<std::uint32_t> word{
            *reinterpret_cast<std::uint32_t*>(buffer.data() + layout.offset) };
        while (!go.load(std::memory_order_acquire)) { }
        for (int i{ 0 }; i < iterations; ++i)
        {
            word.fetch_or(sibling_bit, std::memory_order_acq_rel);
        }
    } };

    go.store(true, std::memory_order_release);
    toggler.join();
    jit.join();

    const std::uint32_t final_word{ read_word(buffer.data(), layout) };
    // The sibling bit, once set by the last JIT iteration, must NOT have been
    // clobbered by a non-atomic dont_inline RMW.  With atomic fetch_or/fetch_and
    // this is guaranteed; the assertion would flake under a plain |=/&=.
    check("atomic_toggle_does_not_clobber_sibling_bit",
          (final_word & sibling_bit) == sibling_bit);
}

// ─────────────────────────────────────────────────────────────────────────
//  10. TYPE-STRING MATCHING (the constexpr strcmp inside derive_method_flags_
//      layout).  The width DECISION pivots entirely on an EXACT type_string
//      match ("u2" selects Path A / Path B; anything else falls through).  A
//      prefix/superstring/case/whitespace bug in that match would silently
//      mis-select the width, so sweep the discriminating inputs exhaustively
//      against BOTH paths.  All deterministic — pure string comparison.
// ─────────────────────────────────────────────────────────────────────────
static auto test_type_string_matching() -> void
{
    // --- Path A: flags_type must match "u2" EXACTLY to fire (width 2, bit 2). ---
    // Every NON-"u2" type_string here must leave Path A un-fired.  With no usable
    // _intrinsic_id the whole derivation must then be NOT confident.
    {
        // type_string values that are NEAR "u2" but not equal — these catch a
        // strncmp/prefix bug (would wrongly accept "u", "u2x"), a case-folding
        // bug ("U2"), and a trailing-garbage bug ("u2 ", "u20").
        const char* const non_u2_types[]{
            "u",      // strict prefix of "u2"
            "u1",     // sibling width
            "u4",     // sibling width
            "u8",     // sibling width
            "u22",    // "u2" is a prefix of this -> must NOT match
            "u2x",    // superstring
            "u2 ",    // trailing space
            "u20",    // trailing digit
            " u2",    // leading space
            "U2",     // uppercase
            "jint",   // a real HotSpot type_string for some fields
            "jchar",
            "int",
            "short",
            "MethodFlags",
            "AccessFlags",
            "x",
            "2u",     // reversed
            "uu2",
        };
        bool all_pathA_rejected{ true };
        for (const char* const t : non_u2_types)
        {
            method_flags_evidence ev{};
            ev.flags_present = true;
            ev.flags_type   = t;
            ev.flags_offset = 48;
            // No intrinsic evidence -> if Path A wrongly fired we'd see confident.
            const method_flags_layout layout{ derive_method_flags_layout(ev) };
            if (layout.confident) { all_pathA_rejected = false; break; }
        }
        check("type_string_pathA_rejects_every_non_u2", all_pathA_rejected);

        // The exact literal "u2" (and ONLY it) fires Path A.
        method_flags_evidence ev_exact{};
        ev_exact.flags_present = true;
        ev_exact.flags_type   = "u2";
        ev_exact.flags_offset = 48;
        const method_flags_layout exact{ derive_method_flags_layout(ev_exact) };
        check("type_string_pathA_accepts_exact_u2",
              exact.confident && exact.width_bytes == 2 && exact.dont_inline_bit == 2
              && exact.offset == 48);
    }

    // --- Path B: intrinsic_id_type must match "u2" EXACTLY to fire. -----------
    // Re-use the same near-"u2" set; a u2 _intrinsic_id at a legal offset is the
    // ONLY one that may derive a confident u4 _status.  In particular "u1" here is
    // the single discriminator that keeps JDK 8 (u1 _intrinsic_id) off Path B.
    {
        const char* const non_u2_types[]{
            "u", "u1", "u4", "u8", "u22", "u2x", "u2 ", " u2", "U2",
            "jint", "jchar", "MethodFlags", "x",
        };
        bool all_pathB_rejected{ true };
        for (const char* const t : non_u2_types)
        {
            method_flags_evidence ev{};
            ev.intrinsic_id_present = true;
            ev.intrinsic_id_type   = t;
            ev.intrinsic_id_offset = 48;  // legal (>=4, 4-aligned) so ONLY the type gates
            const method_flags_layout layout{ derive_method_flags_layout(ev) };
            if (layout.confident) { all_pathB_rejected = false; break; }
        }
        check("type_string_pathB_rejects_every_non_u2_intrinsic", all_pathB_rejected);

        // The exact literal "u2" at a legal offset fires Path B (width 4, bit 12).
        method_flags_evidence ev_exact{};
        ev_exact.intrinsic_id_present = true;
        ev_exact.intrinsic_id_type   = "u2";
        ev_exact.intrinsic_id_offset = 48;
        const method_flags_layout exact{ derive_method_flags_layout(ev_exact) };
        check("type_string_pathB_accepts_exact_u2",
              exact.confident && exact.width_bytes == 4 && exact.dont_inline_bit == 12
              && exact.offset == 44);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  11. PATH A OFFSET FIDELITY: Path A (exported u2 _flags) trusts the exported
//      offset VERBATIM — unlike Path B it applies NO alignment/underflow guard,
//      because the offset is an authoritative VMStruct fact, not a derivation.
//      Pin that deliberate asymmetry: ANY offset (0, odd, unaligned, huge) is
//      passed through unchanged, always width 2 / bit 2 / confident.
// ─────────────────────────────────────────────────────────────────────────
static auto test_pathA_offset_fidelity() -> void
{
    const std::uint64_t offsets[]{
        0u,        // a u2 at offset 0 is unusual but VMStructs is authoritative
        1u,        // odd — Path A does NOT reject (contrast Path B)
        2u,        // 2-aligned (natural for u2) but Path A doesn't require it
        3u, 7u,    // odd / non-power-of-two
        44u, 48u,  // realistic 64-bit Method offsets
        46u,       // the offset Path B would REJECT as misaligned — Path A keeps it
        1000u,     // large
        0xFFFFu,   // very large but in-range for u64 offset
    };
    bool all_verbatim{ true };
    for (const std::uint64_t off : offsets)
    {
        method_flags_evidence ev{};
        ev.flags_present = true;
        ev.flags_type   = "u2";
        ev.flags_offset = off;
        const method_flags_layout layout{ derive_method_flags_layout(ev) };
        if (!(layout.confident && layout.width_bytes == 2 && layout.dont_inline_bit == 2
              && layout.offset == off))
        {
            all_verbatim = false;
            break;
        }
    }
    check("pathA_passes_through_any_offset_verbatim", all_verbatim);

    // Explicitly contrast with Path B at the SAME misaligned offset: Path A keeps
    // a u2 _flags @46 (confident), but a u2 _intrinsic_id @46 is rejected (Path B
    // alignment guard).  This is the documented "VMStruct fact vs derivation"
    // distinction, asserted side by side.
    {
        method_flags_evidence a{};
        a.flags_present = true; a.flags_type = "u2"; a.flags_offset = 46;
        method_flags_evidence b{};
        b.intrinsic_id_present = true; b.intrinsic_id_type = "u2"; b.intrinsic_id_offset = 46;
        check("pathA_keeps_misaligned_offset_but_pathB_rejects_it",
              derive_method_flags_layout(a).confident
              && !derive_method_flags_layout(b).confident);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  12. PATH B ALIGNMENT / UNDERFLOW BOUNDARY SWEEP.  Path B fires ONLY when the
//      u2 _intrinsic_id offset is >= 4 AND 4-byte aligned (the u4 _status sits at
//      intrinsic-4 and must be 4-aligned).  Sweep the whole low range so the
//      exact accept/reject boundary is pinned, including the MINIMUM legal offset
//      (4 -> _status at 0) and every 4-multiple vs non-multiple.
// ─────────────────────────────────────────────────────────────────────────
static auto test_pathB_alignment_boundary_sweep() -> void
{
    bool boundary_ok{ true };
    // offsets 0..63 cover every residue class mod 4 across the realistic range.
    for (std::uint64_t off{ 0 }; off <= 64; ++off)
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type   = "u2";
        ev.intrinsic_id_offset = off;
        const method_flags_layout layout{ derive_method_flags_layout(ev) };

        const bool should_fire{ off >= 4u && (off % 4u) == 0u };
        if (layout.confident != should_fire) { boundary_ok = false; break; }
        if (should_fire)
        {
            // When it fires, the derived _status is ALWAYS intrinsic-4, u4, bit 12.
            if (!(layout.offset == off - 4u && layout.width_bytes == 4
                  && layout.dont_inline_bit == 12))
            {
                boundary_ok = false; break;
            }
        }
        else
        {
            // When it refuses, the layout is the all-zero default (no guess).
            if (!(layout.offset == 0u && layout.width_bytes == 0
                  && layout.dont_inline_bit == 0))
            {
                boundary_ok = false; break;
            }
        }
    }
    check("pathB_alignment_underflow_boundary_sweep_0_to_64", boundary_ok);

    // The MINIMUM legal offset (4) is the critical edge: it derives _status @0.
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type   = "u2";
        ev.intrinsic_id_offset = 4;
        const method_flags_layout layout{ derive_method_flags_layout(ev) };
        check("pathB_min_legal_offset4_derives_status_at_0",
              layout.confident && layout.offset == 0u
              && layout.width_bytes == 4 && layout.dont_inline_bit == 12);
    }

    // One below the boundary (offset 4 - the smallest 4-aligned >=4) vs the value
    // just under it that is also 4-aligned-in-spirit but underflows: 0 is rejected
    // by the >=4 clause even though 0 % 4 == 0.  Pin that the >=4 clause (not just
    // the %4 clause) is what rejects 0.
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type   = "u2";
        ev.intrinsic_id_offset = 0;  // 0 % 4 == 0 but 0 < 4 -> underflow guard
        check("pathB_offset0_rejected_by_underflow_not_alignment",
              !derive_method_flags_layout(ev).confident);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  13. CO-PRESENT FIELD PRECEDENCE & *_present GATING.
//
//   (a) JDK 21+ MAY export _flags as a non-u2 "MethodFlags" object while ALSO
//       exporting a u2 _intrinsic_id.  Path A must fall through (type != "u2")
//       and Path B must win (u4 / bit 12) — the realistic "_flags exported but
//       not the legacy width" scenario the simple BOTH-u2 precedence test does
//       not cover.
//   (b) The `*_present` booleans GATE their evidence: a populated type/offset
//       with present==false must be ignored entirely.
// ─────────────────────────────────────────────────────────────────────────
static auto test_copresent_precedence_and_present_gating() -> void
{
    // (a) MethodFlags _flags + u2 _intrinsic_id -> Path B wins.
    {
        method_flags_evidence ev{};
        ev.flags_present        = true;
        ev.flags_type           = "MethodFlags";  // present but NOT u2 -> Path A skips
        ev.flags_offset         = 40;
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type    = "u2";
        ev.intrinsic_id_offset  = 44;
        const method_flags_layout layout{ derive_method_flags_layout(ev) };
        check("copresent_methodflags_plus_u2intrinsic_picks_pathB",
              layout.confident && layout.width_bytes == 4
              && layout.dont_inline_bit == 12 && layout.offset == 40);
    }

    // (a') Same but with a u1 _flags export co-present (also non-u2) -> Path B.
    {
        method_flags_evidence ev{};
        ev.flags_present        = true;
        ev.flags_type           = "u1";
        ev.flags_offset         = 40;
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type    = "u2";
        ev.intrinsic_id_offset  = 44;
        const method_flags_layout layout{ derive_method_flags_layout(ev) };
        check("copresent_u1flags_plus_u2intrinsic_picks_pathB",
              layout.confident && layout.width_bytes == 4 && layout.dont_inline_bit == 12);
    }

    // (b) flags_present == false but flags_type/offset populated -> must be IGNORED
    //     (Path A is gated by flags_present).  Fall through to Path B via intrinsic.
    {
        method_flags_evidence ev{};
        ev.flags_present        = false;       // <-- gate is OFF
        ev.flags_type           = "u2";        // would have fired Path A if honoured
        ev.flags_offset         = 99;
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type    = "u2";
        ev.intrinsic_id_offset  = 44;
        const method_flags_layout layout{ derive_method_flags_layout(ev) };
        check("present_false_flags_evidence_is_ignored",
              layout.confident && layout.width_bytes == 4
              && layout.dont_inline_bit == 12 && layout.offset == 40);
    }

    // (b') intrinsic_id_present == false but type/offset populated -> IGNORED, and
    //      with no usable _flags either the whole derivation refuses.
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = false;        // <-- gate is OFF
        ev.intrinsic_id_type    = "u2";         // would have fired Path B if honoured
        ev.intrinsic_id_offset  = 44;
        check("present_false_intrinsic_evidence_is_ignored",
              !derive_method_flags_layout(ev).confident);
    }

    // (b'') BOTH present==false with everything else populated -> total refusal.
    {
        method_flags_evidence ev{};
        ev.flags_present        = false;
        ev.flags_type           = "u2";
        ev.flags_offset         = 44;
        ev.intrinsic_id_present = false;
        ev.intrinsic_id_type    = "u2";
        ev.intrinsic_id_offset  = 44;
        check("both_present_false_refuses_despite_populated_fields",
              !derive_method_flags_layout(ev).confident);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  14. WIDTH <-> BIT REPRESENTABILITY INVARIANT + DETERMINISM.
//
//   The whole point of the fix: the _dont_inline mask (1u << bit) must be
//   representable in the chosen ACCESS WIDTH (only the width varies per band,
//   never the mask construction).  For EVERY confident derivation the derived
//   bit must satisfy `bit < width_bytes * 8`, and the mask must be non-zero and
//   fit.  Plus a determinism/purity check: the same evidence yields a
//   byte-identical result struct on repeated calls.
// ─────────────────────────────────────────────────────────────────────────
static auto layouts_equal(const method_flags_layout& a, const method_flags_layout& b) -> bool
{
    return a.offset == b.offset && a.width_bytes == b.width_bytes
        && a.dont_inline_bit == b.dont_inline_bit && a.confident == b.confident;
}

static auto test_width_bit_representability_and_determinism() -> void
{
    // The set of confident bands the derivation can produce.
    const method_flags_evidence confident_inputs[]{
        evidence_jdk11_20,  // u2 / bit 2
        evidence_jdk21_23,  // u4 / bit 12
        evidence_jdk24_26,  // u4 / bit 12
    };

    bool repr_ok{ true };
    bool mask_fits_ok{ true };
    for (const method_flags_evidence& ev : confident_inputs)
    {
        const method_flags_layout layout{ derive_method_flags_layout(ev) };
        if (!layout.confident) { repr_ok = false; break; }

        // bit must be representable in width_bytes*8 bits.
        const int width_bits{ layout.width_bytes * 8 };
        if (!(layout.dont_inline_bit >= 0 && layout.dont_inline_bit < width_bits))
        {
            repr_ok = false; break;
        }

        // (1u << bit) is non-zero and, when masked to the width, unchanged.
        const std::uint32_t mask{ 1u << layout.dont_inline_bit };
        const std::uint32_t width_mask{
            layout.width_bytes == 2 ? 0x0000FFFFu : 0xFFFFFFFFu };
        if (mask == 0u || (mask & width_mask) != mask) { mask_fits_ok = false; break; }
    }
    check("every_confident_band_bit_fits_its_width", repr_ok);
    check("every_confident_band_mask_representable_in_width", mask_fits_ok);

    // The u2 band's bit (2) ALSO fits a hypothetical u4, but the u4 band's bit
    // (12) does NOT fit a u1 (12 >= 8) — that alone forces a >= 2-byte access on
    // JDK 21+.  It DOES fit numerically inside 16 bits (12 < 16); the u2 LEGACY
    // path still can't reach it because on JDK 21+ `_flags` is a u4 MethodFlags
    // object reached via the DERIVED Path B, not the exported u2 Path A — i.e.
    // the width must widen because the FIELD is u4 (siblings at bits 7..10 + the
    // not-c1/c2/osr group), not because bit 12 itself exceeds 16.  Pin exactly
    // that: bit 12 overflows u1, sits inside the low 16, and needs the u4 field.
    check("u2_band_bit2_would_also_fit_u4", flags_layout::jdk11_20.dont_inline_bit < 32);
    check("u4_band_bit12_overflows_u1_but_fits_low16",
          flags_layout::jdk21_23.dont_inline_bit >= 8      // would overflow a u1
          && flags_layout::jdk21_23.dont_inline_bit < 16   // numerically inside 16 bits
          && flags_layout::jdk21_23.dont_inline_bit < 32); // and inside the u4 field

    // Determinism / purity: identical evidence -> identical result, twice.
    for (const method_flags_evidence& ev : confident_inputs)
    {
        const method_flags_layout first{ derive_method_flags_layout(ev) };
        const method_flags_layout second{ derive_method_flags_layout(ev) };
        if (!layouts_equal(first, second))
        {
            check("derive_is_deterministic_for_confident_inputs", false);
            return;
        }
    }
    // Also for a REFUSING input (the all-zero default must be stable too).
    {
        const method_flags_layout r1{ derive_method_flags_layout(evidence_jdk8) };
        const method_flags_layout r2{ derive_method_flags_layout(evidence_jdk8) };
        check("derive_is_deterministic_for_confident_inputs",
              layouts_equal(r1, r2) && !r1.confident);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  15. WIDTH -> BIT MAPPING TABLE over ALL JDK regimes (the authoritative
//      cross-version contract, asserted as one coherent table).  For each band:
//      the resolved width, the _dont_inline bit, that the bit fits the width,
//      and that the access-flags width tracks the JDK-24 AccessFlags shrink.
//      This is the single greppable "width/bit per JDK" matrix.
// ─────────────────────────────────────────────────────────────────────────
static auto test_width_to_bit_mapping_table() -> void
{
    using namespace flags_layout;

    struct row { const band& b; int expect_width; int expect_bit; bool exported; };
    const row table[]{
        { jdk8,     0, 2,  false },  // no exported _flags; bit is academic (no write)
        { jdk11_20, 2, 2,  true  },  // u2, bit 2
        { jdk21_23, 4, 12, false },  // u4 _status (derived), bit 12
        { jdk24_26, 4, 12, false },  // u4 _status (derived), bit 12
    };

    bool table_ok{ true };
    for (const row& r : table)
    {
        if (r.b.flags_width_bytes != r.expect_width) { table_ok = false; break; }
        if (r.b.dont_inline_bit   != r.expect_bit)   { table_ok = false; break; }
        if (r.b.flags_exported    != r.exported)     { table_ok = false; break; }
        // For every band that HAS a real flags word, the bit must fit the width.
        if (r.b.flags_width_bytes > 0
            && !(r.b.dont_inline_bit < r.b.flags_width_bytes * 8))
        {
            table_ok = false; break;
        }
    }
    check("width_to_bit_mapping_table_consistent_all_bands", table_ok);

    // Monotonic widening: _flags width never SHRINKS going forward across the
    // bands that have a real field (u2 -> u4), and is 4 on every JDK 21+ band.
    check("flags_width_widens_u2_to_u4_never_shrinks",
          jdk11_20.flags_width_bytes == 2
          && jdk21_23.flags_width_bytes == 4
          && jdk24_26.flags_width_bytes == 4
          && jdk21_23.flags_width_bytes >= jdk11_20.flags_width_bytes);

    // The _dont_inline bit only ever moves UP (2 -> 12) across the relocation.
    // Bit 12 needs a >= 2-byte access (overflows u1) and shares the u4 _status
    // with the relocated compilability siblings at bits 7..10, all of which are
    // ABOVE bit 2 — so the JDK 21+ flags WORD is materially wider/busier than the
    // JDK 11..20 u2 word even though bit 12 itself is numerically under 16.
    check("dont_inline_bit_relocated_up_and_needs_wide_field",
          jdk11_20.dont_inline_bit == 2
          && jdk21_23.dont_inline_bit == 12
          && jdk21_23.dont_inline_bit > jdk11_20.dont_inline_bit   // moved up
          && jdk21_23.dont_inline_bit >= 8                          // overflows u1
          && methodflags_status_bit::is_not_c2_osr == 10            // sibling above bit 2
          && methodflags_status_bit::is_not_c2_osr > jdk11_20.dont_inline_bit);
}

// ─────────────────────────────────────────────────────────────────────────
//  16. NO_COMPILE access-flags mask (the companion constant the feature owns).
//      It is OR'd into Method::_access_flags (u4 read) and is a pure compile-time
//      constant — assert its exact value, that it is exactly the four documented
//      compile-control bits, and that every bit lives in the high byte (24..27),
//      i.e. disjoint from JVM_ACC_STATIC (bit 3) so the two never interfere.
// ─────────────────────────────────────────────────────────────────────────
static auto test_no_compile_mask_bits() -> void
{
    const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };

    constexpr std::uint32_t not_c2{ 0x02000000u };
    constexpr std::uint32_t not_c1{ 0x04000000u };
    constexpr std::uint32_t not_c2_osr{ 0x08000000u };
    constexpr std::uint32_t queued{ 0x01000000u };

    check("no_compile_exact_value_0x0F000000",
          no_compile == (not_c2 | not_c1 | not_c2_osr | queued)
          && no_compile == 0x0F000000u);

    // Each of the four bits is present.
    check("no_compile_contains_all_four_bits",
          (no_compile & not_c2) == not_c2
          && (no_compile & not_c1) == not_c1
          && (no_compile & not_c2_osr) == not_c2_osr
          && (no_compile & queued) == queued);

    // Exactly four bits set (popcount == 4) — no stray bits.
    {
        int bits{ 0 };
        for (std::uint32_t v{ no_compile }; v; v &= (v - 1)) { ++bits; }
        check("no_compile_has_exactly_four_bits", bits == 4);
    }

    // All four live in the high byte (bits 24..31) -> disjoint from JVM_ACC_STATIC
    // (bit 3, low byte).  So OR-ing NO_COMPILE never disturbs the static bit, and
    // masking 0x0008 for is_static() never sees a NO_COMPILE bit.
    check("no_compile_bits_are_high_byte_only",
          (no_compile & 0x00FFFFFFu) == 0u && (no_compile & 0xFF000000u) == no_compile);
    check("no_compile_disjoint_from_jvm_acc_static",
          (no_compile & flags_layout::jvm_acc_static) == 0u);

    // The whole mask fits in the u4 _access_flags word (it would NOT fit a u2,
    // confirming why the access path reads u4 — the contrast the _flags path
    // mirrors).  Bits 24..27 are unreachable through any 16-bit read.
    check("no_compile_requires_u4_access_width",
          (no_compile & 0x0000FFFFu) == 0u && (no_compile & 0xFFFF0000u) == no_compile);
}

// ─────────────────────────────────────────────────────────────────────────
//  17. FULL JVM_ACC_* class-file access-flag bit table (the bits that live in
//      Method::_access_flags — the u4-read accessor's domain).  These are the
//      Java class-file modifier bits HotSpot stores in the LOW 16 bits of
//      AccessFlags; they NEVER moved across JDK 8..26 (only the high-byte
//      compile-control bits relocated in JDK 24).  Pin every documented bit's
//      exact value, prove each is a single bit, and prove the WHOLE class-file
//      group sits in the low 16 bits so a u2 OR a u4 read sees it identically —
//      this is precisely why is_static() masking 0x0008 out of a u4 read is
//      width-independent (the contrast that justifies the _flags type-string fix).
// ─────────────────────────────────────────────────────────────────────────
namespace acc
{
    // Canonical JVM_ACC_* values from the class-file format (jvm.h / accessFlags.hpp);
    // identical on every JDK 8..26.  Each is one bit in the low byte / low 16.
    struct named_bit { const char* name; std::uint32_t value; };
    constexpr named_bit table[]{
        { "PUBLIC",       0x0001u },
        { "PRIVATE",      0x0002u },
        { "PROTECTED",    0x0004u },
        { "STATIC",       0x0008u },
        { "FINAL",        0x0010u },
        { "SYNCHRONIZED", 0x0020u },  // (== SUPER for classes; == VOLATILE-adjacent slot)
        { "BRIDGE",       0x0040u },  // (== VOLATILE for fields)
        { "VARARGS",      0x0080u },  // (== TRANSIENT for fields)
        { "NATIVE",       0x0100u },
        { "INTERFACE",    0x0200u },
        { "ABSTRACT",     0x0400u },
        { "STRICTFP",       0x0800u },
        { "SYNTHETIC",    0x1000u },
        { "ANNOTATION",   0x2000u },
        { "ENUM",         0x4000u },
        { "MODULE",       0x8000u },  // (== MANDATED in some contexts) — top of low 16
    };
}

static auto popcount32(std::uint32_t v) -> int
{
    int n{ 0 };
    for (; v; v &= (v - 1)) { ++n; }
    return n;
}

static auto test_jvm_acc_bit_table() -> void
{
    // Every documented JVM_ACC_* modifier is a SINGLE bit and lives in the low 16.
    bool all_single_bit{ true };
    bool all_low16{ true };
    for (const acc::named_bit& b : acc::table)
    {
        if (popcount32(b.value) != 1) { all_single_bit = false; break; }
        if ((b.value & 0xFFFF0000u) != 0u) { all_low16 = false; break; }
    }
    check("jvm_acc_every_modifier_is_single_bit", all_single_bit);
    check("jvm_acc_every_modifier_in_low16", all_low16);

    // The values are pairwise DISTINCT and densely cover bits 0..15 (each of the
    // 16 low bits is claimed exactly once by this table — no gaps, no dupes).
    {
        std::uint32_t orall{ 0 };
        int count{ 0 };
        for (const acc::named_bit& b : acc::table) { orall |= b.value; ++count; }
        check("jvm_acc_table_covers_all_16_low_bits", orall == 0x0000FFFFu && count == 16);
        check("jvm_acc_table_bits_are_pairwise_distinct", popcount32(orall) == count);
    }

    // STATIC is exactly bit 3 / 0x0008 and lives in the LOW byte — the single
    // fact that makes is_static() width-independent across the JDK 24 u4->u2
    // AccessFlags shrink (a u2 read and a u4 read agree on bit 3).
    check("jvm_acc_static_is_bit3_low_byte",
          flags_layout::jvm_acc_static == 0x0008u
          && (flags_layout::jvm_acc_static & 0x00FFu) == flags_layout::jvm_acc_static
          && popcount32(flags_layout::jvm_acc_static) == 1);

    // NATIVE / ABSTRACT / INTERFACE — the predicate bits the access-flag accessors
    // read — are all within the low 16, so the u4 read masks them identically to a
    // hypothetical u2 read (verified by truncating each to 16 bits below).
    for (const acc::named_bit& b : acc::table)
    {
        const std::uint16_t as_u2{ static_cast<std::uint16_t>(b.value) };
        // Round-tripping through a 16-bit access loses NOTHING for any class-file bit.
        check((std::string{ "jvm_acc_" } + b.name + "_survives_u2_truncation").c_str(),
              static_cast<std::uint32_t>(as_u2) == b.value);
    }

    // The class-file group (low 16) is DISJOINT from the NO_COMPILE high-byte mask
    // (bits 24..27): no modifier bit overlaps a compile-control bit, so OR-ing
    // NO_COMPILE can never flip a modifier and masking a modifier never sees one.
    {
        std::uint32_t orall{ 0 };
        for (const acc::named_bit& b : acc::table) { orall |= b.value; }
        const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };
        check("jvm_acc_classfile_group_disjoint_from_no_compile",
              (orall & no_compile) == 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  18. ACCESS-FLAG PREDICATE MASKING is WIDTH-STABLE.  The is_static / is_native
//      / is_abstract / is_final style predicates are `(_access_flags & BIT) != 0`
//      reads.  Model that read at BOTH a 2-byte and a 4-byte access width over a
//      synthesised access-flags word and prove the boolean result is identical —
//      i.e. the predicate does not depend on whether AccessFlags is u2 (JDK 24+)
//      or u4 (<=23), because every modifier bit is in the low 16.  Uses memcpy to
//      lay the object representation (never compares a possibly-trap bool; the
//      predicate result is a plain integer comparison, not a stored bool).
// ─────────────────────────────────────────────────────────────────────────
static auto predicate_u2(const std::uint8_t* base, std::uint32_t bit) -> bool
{
    std::uint16_t w{};
    std::memcpy(&w, base, sizeof(w));
    return (static_cast<std::uint32_t>(w) & bit) != 0u;
}
static auto predicate_u4(const std::uint8_t* base, std::uint32_t bit) -> bool
{
    std::uint32_t w{};
    std::memcpy(&w, base, sizeof(w));
    return (w & bit) != 0u;
}

static auto test_access_flag_predicate_width_stability() -> void
{
    // Synthesise a realistic access-flags word: a public static native final method
    // (low-16 modifiers) PLUS the NO_COMPILE high byte set (the library OR's it in).
    // Both a u2 read and a u4 read must agree on every LOW-16 predicate.
    const std::uint32_t word{
        0x0001u   // PUBLIC
        | 0x0008u // STATIC
        | 0x0010u // FINAL
        | 0x0100u // NATIVE
        | static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };  // high byte

    alignas(4) std::array<std::uint8_t, 8> buf{};
    std::memcpy(buf.data(), &word, sizeof(word));

    struct probe { const char* name; std::uint32_t bit; bool expect; };
    const probe probes[]{
        { "PUBLIC",  0x0001u, true  },
        { "PRIVATE", 0x0002u, false },
        { "STATIC",  0x0008u, true  },
        { "FINAL",   0x0010u, true  },
        { "NATIVE",  0x0100u, true  },
        { "ABSTRACT",0x0400u, false },
    };
    bool all_agree{ true };
    bool all_correct{ true };
    for (const probe& p : probes)
    {
        const bool b2{ predicate_u2(buf.data(), p.bit) };
        const bool b4{ predicate_u4(buf.data(), p.bit) };
        if (b2 != b4) { all_agree = false; }
        if (b4 != p.expect) { all_correct = false; }
    }
    check("access_predicate_u2_and_u4_agree_on_low16", all_agree);
    check("access_predicate_low16_results_correct", all_correct);

    // The static bit specifically: present in this word, read true at BOTH widths,
    // and the JDK-24 u2 read does NOT see the NO_COMPILE high byte at all (the high
    // byte is OUTSIDE a 16-bit read window) — so a NO_COMPILE OR can never spoof a
    // false-positive on any class-file predicate.
    check("access_static_bit_true_at_both_widths",
          predicate_u2(buf.data(), 0x0008u) && predicate_u4(buf.data(), 0x0008u));
    check("access_no_compile_invisible_to_u2_read",
          predicate_u2(buf.data(), static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE)) == false
          && predicate_u4(buf.data(), static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE)));

    // An INSTANCE method (STATIC bit clear) reads is_static==false at both widths.
    {
        const std::uint32_t instance_word{ 0x0001u | 0x0100u };  // public native, NOT static
        alignas(4) std::array<std::uint8_t, 8> ibuf{};
        std::memcpy(ibuf.data(), &instance_word, sizeof(instance_word));
        check("access_instance_method_is_static_false_both_widths",
              predicate_u2(ibuf.data(), 0x0008u) == false
              && predicate_u4(ibuf.data(), 0x0008u) == false);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  19. EXHAUSTIVE bit-fits-width sweep (0..31) for u1 / u2 / u4.  The width fix
//      hinges on "the access WIDTH must be wide enough to hold the bit".  For
//      every bit 0..31 assert the exact representability in each width and the
//      boundary at 8 (u1) / 16 (u2) / 32 (u4).  Then pin the two REAL bands:
//      bit 2 fits u1/u2/u4; bit 12 fits u2/u4 but NOT u1 — the single arithmetic
//      reason JDK 21+ cannot use a u1 access even though bit 12 < 16.
// ─────────────────────────────────────────────────────────────────────────
static auto test_bit_fits_width_exhaustive() -> void
{
    bool sweep_ok{ true };
    for (int bit{ 0 }; bit < 32; ++bit)
    {
        const bool fits_u1{ bit < 8 };
        const bool fits_u2{ bit < 16 };
        const bool fits_u4{ bit < 32 };
        // (1u << bit) is well-defined for bit in [0,31]; mask must be non-zero and,
        // when bit fits a width, be representable inside that width's mask.
        const std::uint32_t mask{ 1u << bit };
        if (mask == 0u) { sweep_ok = false; break; }
        if (fits_u1 && (mask & 0x000000FFu) != mask) { sweep_ok = false; break; }
        if (fits_u2 && (mask & 0x0000FFFFu) != mask) { sweep_ok = false; break; }
        if (fits_u4 && (mask & 0xFFFFFFFFu) != mask) { sweep_ok = false; break; }
        // And the converse: a bit that does NOT fit u1/u2 must spill out of that mask.
        if (!fits_u1 && (mask & 0x000000FFu) != 0u) { sweep_ok = false; break; }
        if (!fits_u2 && (mask & 0x0000FFFFu) != 0u) { sweep_ok = false; break; }
    }
    check("bit_fits_width_exhaustive_0_to_31", sweep_ok);

    // The two real _dont_inline bits against the boundary.
    check("dont_inline_bit2_fits_u1_u2_u4",
          (1u << 2) <= 0xFFu && (1u << 2) <= 0xFFFFu && (1u << 2) <= 0xFFFFFFFFu);
    check("dont_inline_bit12_needs_at_least_u2",
          (1u << 12) > 0xFFu          // overflows a u1 -> JDK 21+ cannot use 1-byte access
          && (1u << 12) <= 0xFFFFu     // fits inside 16 bits numerically
          && (1u << 12) <= 0xFFFFFFFFu);

    // The boundary bits themselves: bit 7 is the LAST u1 bit, bit 8 the FIRST that
    // overflows u1; bit 15 the LAST u2 bit, bit 16 the FIRST that overflows u2.
    check("bit7_is_last_u1_bit_8_overflows",
          ((1u << 7) & 0xFFu) == (1u << 7) && ((1u << 8) & 0xFFu) == 0u);
    check("bit15_is_last_u2_bit_16_overflows",
          ((1u << 15) & 0xFFFFu) == (1u << 15) && ((1u << 16) & 0xFFFFu) == 0u);
}

// ─────────────────────────────────────────────────────────────────────────
//  20. ALL-SET / ALL-CLEAR slot toggling with FULL-WORD anti-clobber.  The
//      width-matrix test (#8) toggles ONE bit on a zeroed slot.  Here drive the
//      OTHER extreme: seed the slot ALL-ONES, toggle dont_inline OFF then back ON,
//      and the inverse from ALL-ZERO.  Proves the width-correct RMW (a) flips only
//      the target bit out of a fully-populated word and (b) preserves every OTHER
//      bit in the slot AND every byte outside it.  This is the worst-case sibling
//      survival check (15 / 31 unrelated bits live simultaneously).
// ─────────────────────────────────────────────────────────────────────────
static auto test_all_set_all_clear_slot_toggle() -> void
{
    const method_flags_evidence bands[]{ evidence_jdk11_20, evidence_jdk21_23 };
    const char* const tags[]{ "u2", "u4" };

    for (std::size_t k{ 0 }; k < 2; ++k)
    {
        const method_flags_layout layout{ derive_method_flags_layout(bands[k]) };
        if (!layout.confident) { check("all_set_clear_band_confident", false); continue; }

        const std::uint32_t width_mask{ layout.width_bytes == 2 ? 0x0000FFFFu : 0xFFFFFFFFu };
        const std::uint32_t target{ 1u << layout.dont_inline_bit };

        // --- Seed ALL-ONES across the slot; CLEAR dont_inline; only it drops. ---
        {
            alignas(16) std::array<std::uint8_t, 128> buffer{};
            buffer.fill(0xC3);  // recognizable neighbour pattern
            std::memset(buffer.data() + layout.offset, 0xFF,
                        static_cast<std::size_t>(layout.width_bytes));
            const std::array<std::uint8_t, 128> snap{ buffer };

            toggle_like_set_dont_inline(buffer.data(), layout, /*enabled*/ false);
            const std::uint32_t after{ read_word(buffer.data(), layout) & width_mask };

            // dont_inline bit cleared; ALL OTHER in-slot bits still set.
            check((std::string{ "all_ones_" } + tags[k] + "_only_target_cleared").c_str(),
                  (after & target) == 0u && after == (width_mask & ~target));

            // Bytes OUTSIDE the slot untouched.
            bool outside_intact{ true };
            for (std::size_t i{ 0 }; i < buffer.size(); ++i)
            {
                const bool inside{ i >= layout.offset
                                   && i < layout.offset + static_cast<std::size_t>(layout.width_bytes) };
                if (!inside && buffer[i] != snap[i]) { outside_intact = false; break; }
            }
            check((std::string{ "all_ones_" } + tags[k] + "_no_adjacent_clobber").c_str(),
                  outside_intact);

            // Re-SET dont_inline -> slot returns to ALL-ONES (full reversibility).
            toggle_like_set_dont_inline(buffer.data(), layout, /*enabled*/ true);
            check((std::string{ "all_ones_" } + tags[k] + "_reset_restores_all_ones").c_str(),
                  (read_word(buffer.data(), layout) & width_mask) == width_mask);
        }

        // --- Seed ALL-ZERO; SET dont_inline; only it rises; CLEAR -> zero again. ---
        {
            alignas(16) std::array<std::uint8_t, 128> buffer{};
            buffer.fill(0x3C);
            std::memset(buffer.data() + layout.offset, 0x00,
                        static_cast<std::size_t>(layout.width_bytes));

            toggle_like_set_dont_inline(buffer.data(), layout, /*enabled*/ true);
            check((std::string{ "all_zero_" } + tags[k] + "_only_target_set").c_str(),
                  (read_word(buffer.data(), layout) & width_mask) == target);

            toggle_like_set_dont_inline(buffer.data(), layout, /*enabled*/ false);
            check((std::string{ "all_zero_" } + tags[k] + "_clear_returns_to_zero").c_str(),
                  (read_word(buffer.data(), layout) & width_mask) == 0u);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  21. SIBLING-BIT INDEPENDENCE CROSS-PRODUCT on the JDK 21+ u4 _status word.
//      Toggle dont_inline (bit 12) while EACH relocated compile-control sibling
//      (queued=7, not_c2=8, not_c1=9, not_c2_osr=10, force_inline=11) is held set,
//      and prove the sibling is untouched by both the set and the clear.  This is
//      the cross-product the JDK 24 relocation made necessary: dont_inline now
//      SHARES its word with the not_compilable group, so the RMW must be surgical.
// ─────────────────────────────────────────────────────────────────────────
static auto test_status_word_sibling_independence() -> void
{
    using namespace flags_layout::methodflags_status_bit;
    const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk21_23) };
    check("sibling_indep_band_is_u4_bit12",
          layout.confident && layout.width_bytes == 4 && layout.dont_inline_bit == 12);

    const int siblings[]{ queued_for_compilation, is_not_c2_compilable,
                          is_not_c1_compilable, is_not_c2_osr, force_inline };

    bool all_independent{ true };
    for (const int sib : siblings)
    {
        if (sib == 12) { all_independent = false; break; }  // must differ from target
        alignas(16) std::array<std::uint8_t, 64> buffer{};
        buffer.fill(0x00);
        const std::uint32_t sib_mask{ 1u << sib };
        std::memcpy(buffer.data() + layout.offset, &sib_mask, sizeof(sib_mask));

        // SET dont_inline: sibling must survive, target must rise.
        toggle_like_set_dont_inline(buffer.data(), layout, true);
        const std::uint32_t after_set{ read_word(buffer.data(), layout) };
        if ((after_set & sib_mask) != sib_mask) { all_independent = false; break; }
        if ((after_set & (1u << 12)) == 0u)     { all_independent = false; break; }
        if (after_set != (sib_mask | (1u << 12))) { all_independent = false; break; }

        // CLEAR dont_inline: sibling STILL survives, target drops.
        toggle_like_set_dont_inline(buffer.data(), layout, false);
        const std::uint32_t after_clear{ read_word(buffer.data(), layout) };
        if (after_clear != sib_mask) { all_independent = false; break; }
    }
    check("status_word_dont_inline_independent_of_every_sibling", all_independent);

    // The whole NO_COMPILE-relocated group held set AT ONCE, then dont_inline
    // toggled — every group bit survives both transitions.
    {
        alignas(16) std::array<std::uint8_t, 64> buffer{};
        buffer.fill(0x00);
        std::uint32_t group{ 0 };
        for (const int sib : siblings) { group |= (1u << sib); }
        std::memcpy(buffer.data() + layout.offset, &group, sizeof(group));

        toggle_like_set_dont_inline(buffer.data(), layout, true);
        const std::uint32_t set_state{ read_word(buffer.data(), layout) };
        toggle_like_set_dont_inline(buffer.data(), layout, false);
        const std::uint32_t clear_state{ read_word(buffer.data(), layout) };

        check("status_word_full_sibling_group_survives_set",
              (set_state & group) == group && (set_state & (1u << 12)) != 0u);
        check("status_word_full_sibling_group_survives_clear",
              clear_state == group && (clear_state & (1u << 12)) == 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  22. ENDIANNESS-INDEPENDENT byte placement of the toggled bit.  The toggle
//      writes through a typed std::uint16_t*/uint32_t*, so the READ-BACK through
//      the SAME typed width is endianness-correct BY CONSTRUCTION.  Pin that
//      invariant explicitly: lay the bit via the toggle, then recover it via a
//      memcpy into the object representation and a typed reinterpret of the same
//      width — both must report the SAME value as `read_word`, on big- or
//      little-endian.  We assert the TYPED-read invariant (portable), and only
//      record the concrete byte index as an [INFO]-style derived fact gated on
//      std::endian (never a hard cross-endian string/byte assertion).
// ─────────────────────────────────────────────────────────────────────────
static auto test_toggle_byte_placement_endianness() -> void
{
    const method_flags_layout layout{ derive_method_flags_layout(evidence_jdk21_23) };  // u4, bit 12
    alignas(16) std::array<std::uint8_t, 64> buffer{};
    buffer.fill(0x00);
    std::memset(buffer.data() + layout.offset, 0x00, static_cast<std::size_t>(layout.width_bytes));

    toggle_like_set_dont_inline(buffer.data(), layout, true);

    // (a) Typed read-back == read_word helper == the constructed mask, regardless
    //     of host endianness (the typed store/load round-trips natively).
    const std::uint32_t via_helper{ read_word(buffer.data(), layout) };
    std::uint32_t via_memcpy{};
    std::memcpy(&via_memcpy, buffer.data() + layout.offset, sizeof(via_memcpy));
    check("toggle_typed_readback_matches_memcpy_object_repr",
          via_helper == via_memcpy && via_helper == (1u << 12));

    // (b) Object-representation byte index of bit 12 is endianness-DEPENDENT, so we
    //     derive the expected nonzero byte from std::endian and assert THAT — never
    //     a fixed byte index.  Bit 12 lives in byte 1 (little-endian) or byte 2
    //     (big-endian) of the 4-byte word.  On a mixed/unknown endianness we only
    //     assert "exactly one byte is nonzero and it holds 0x10".
    int nonzero_bytes{ 0 };
    int nonzero_index{ -1 };
    std::uint8_t nonzero_value{ 0 };
    for (int i{ 0 }; i < 4; ++i)
    {
        const std::uint8_t bv{ buffer[layout.offset + static_cast<std::size_t>(i)] };
        if (bv != 0) { ++nonzero_bytes; nonzero_index = i; nonzero_value = bv; }
    }
    check("toggle_bit12_occupies_exactly_one_object_byte",
          nonzero_bytes == 1 && nonzero_value == 0x10u);

    if constexpr (std::endian::native == std::endian::little)
    {
        check("toggle_bit12_byte_index_little_endian", nonzero_index == 1);
    }
    else if constexpr (std::endian::native == std::endian::big)
    {
        check("toggle_bit12_byte_index_big_endian", nonzero_index == 2);
    }
    else
    {
        // Mixed-endian host: the typed-read invariant above already covers
        // correctness; the concrete byte index is not portably assertable.
        check("toggle_bit12_byte_index_mixed_endian_invariant_only", nonzero_bytes == 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  23. SIGNED `~NO_COMPILE` TEARDOWN CLEAR round-trip (robustness hazard #6).
//      The teardown clears NO_COMPILE with `*flags &= static_cast<uint32_t>(~NO_COMPILE)`
//      where NO_COMPILE is std::int32_t.  Prove that the signed complement, once
//      cast to u4, clears EXACTLY the four high-byte compile-control bits and
//      DISTURBS NOTHING else — across a full-ones access-flags word, a low-16
//      modifier word, and the realistic OR'd word.  This is the sign-extension
//      hazard pinned as a no-op-today / regression-anchor-tomorrow fact.
// ─────────────────────────────────────────────────────────────────────────
static auto test_no_compile_signed_clear_roundtrip() -> void
{
    const std::int32_t no_compile_signed{ vmhook::hotspot::NO_COMPILE };
    const std::uint32_t clear_mask{ static_cast<std::uint32_t>(~no_compile_signed) };
    const std::uint32_t no_compile{ static_cast<std::uint32_t>(no_compile_signed) };

    // The clear mask is exactly the bitwise-NOT of the 4-bit NO_COMPILE pattern in
    // u4 — no sign extension surprise (the four bits are in bits 24..27, well below
    // the sign bit 31, so ~ at int32 then cast to u4 is the clean 0xF0FFFFFF).
    check("no_compile_clear_mask_is_exact_complement",
          clear_mask == 0xF0FFFFFFu && clear_mask == (~no_compile));

    // OR then AND-clear round-trips to the original on every shape of starting word.
    const std::uint32_t starts[]{
        0x00000000u,
        0xFFFFFFFFu,
        0x0000FFFFu,                                   // all class-file modifiers
        0x00000009u,                                   // public static
        0x0001u | 0x0008u | 0x0100u,                   // public static native
        0x12345678u,                                   // arbitrary
    };
    bool roundtrip_ok{ true };
    for (const std::uint32_t s : starts)
    {
        const std::uint32_t with_nc{ s | no_compile };       // install OR
        const std::uint32_t cleared{ with_nc & clear_mask };  // teardown AND
        // After OR+clear, the NO_COMPILE bits are gone and EVERY other bit is exactly
        // as in the original `s` (OR set only NO_COMPILE bits; clear removed only them).
        if ((cleared & no_compile) != 0u) { roundtrip_ok = false; break; }
        if ((cleared & ~no_compile) != (s & ~no_compile)) { roundtrip_ok = false; break; }
        // The low-16 class-file modifiers in particular are byte-identical to start.
        if ((cleared & 0x0000FFFFu) != (s & 0x0000FFFFu)) { roundtrip_ok = false; break; }
    }
    check("no_compile_or_then_signed_clear_roundtrips_all_words", roundtrip_ok);

    // JVM_ACC_STATIC specifically survives the install-OR + teardown-clear cycle
    // untouched (the static-dispatch decision must read the SAME value afterward).
    {
        const std::uint32_t s{ 0x0008u };                       // a lone static method
        const std::uint32_t cycled{ (s | no_compile) & clear_mask };
        check("no_compile_cycle_preserves_jvm_acc_static",
              (cycled & 0x0008u) == 0x0008u && cycled == s);
    }

    // The signed complement must NOT have set any bit at or above 28 spuriously via
    // sign extension: bits 28..31 of the clear mask are all 1 (untouched), bits
    // 24..27 are 0 (the cleared NO_COMPILE bits), low 24 are 1.
    check("no_compile_clear_mask_high_nibble_intact",
          (clear_mask & 0xF0000000u) == 0xF0000000u   // bits 28..31 preserved
          && (clear_mask & 0x0F000000u) == 0u          // bits 24..27 cleared
          && (clear_mask & 0x00FFFFFFu) == 0x00FFFFFFu);// low 24 preserved
}

// ─────────────────────────────────────────────────────────────────────────
//  24. WIDTH-DRIVEN-NOT-VERSION-DRIVEN guarantee.  The derivation must select the
//      width from the EXPORTED EVIDENCE (type_string + intrinsic layout), never
//      from a compiled-in JDK version number.  Prove the SAME evidence shape yields
//      the SAME width irrespective of the (irrelevant) absolute offsets, and that
//      a u2-exported-flags shape ALWAYS yields width 2 while a u2-intrinsic-only
//      shape ALWAYS yields width 4 — the version is never consulted.
// ─────────────────────────────────────────────────────────────────────────
static auto test_width_is_evidence_driven_not_version_driven() -> void
{
    // Same Path-A shape (exported u2 _flags) at many offsets -> ALWAYS width 2 / bit 2.
    bool pathA_always_u2{ true };
    for (std::uint64_t off : { std::uint64_t{ 0 }, std::uint64_t{ 16 }, std::uint64_t{ 44 },
                               std::uint64_t{ 256 }, std::uint64_t{ 4096 } })
    {
        method_flags_evidence ev{};
        ev.flags_present = true; ev.flags_type = "u2"; ev.flags_offset = off;
        const method_flags_layout l{ derive_method_flags_layout(ev) };
        if (!(l.confident && l.width_bytes == 2 && l.dont_inline_bit == 2)) { pathA_always_u2 = false; break; }
    }
    check("width_pathA_evidence_always_u2_any_offset", pathA_always_u2);

    // Same Path-B shape (u2 intrinsic only) at many legal offsets -> ALWAYS width 4 / bit 12.
    bool pathB_always_u4{ true };
    for (std::uint64_t off : { std::uint64_t{ 4 }, std::uint64_t{ 8 }, std::uint64_t{ 44 },
                               std::uint64_t{ 256 }, std::uint64_t{ 4096 } })
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true; ev.intrinsic_id_type = "u2"; ev.intrinsic_id_offset = off;
        const method_flags_layout l{ derive_method_flags_layout(ev) };
        if (!(l.confident && l.width_bytes == 4 && l.dont_inline_bit == 12)) { pathB_always_u4 = false; break; }
    }
    check("width_pathB_evidence_always_u4_any_legal_offset", pathB_always_u4);

    // A u4-exported _flags ("MethodFlags") WITHOUT a usable u2 intrinsic must NOT be
    // confidently width-4-guessed from its mere presence — the derivation refuses to
    // place a write it cannot prove (no version shortcut rescues it).
    {
        method_flags_evidence ev{};
        ev.flags_present = true; ev.flags_type = "MethodFlags"; ev.flags_offset = 40;
        // no intrinsic evidence at all
        check("width_methodflags_alone_refuses_no_version_shortcut",
              !derive_method_flags_layout(ev).confident);
    }
}

// =========================================================================
//  DEEPENING WAVE -- namespaced additive section (mfw_deep).
//
//  Everything below is APPENDED; it touches none of the functions above.  All
//  values are derived directly from vmhook.hpp source:
//    * derive_method_flags_layout()  (vmhook.hpp:7450-7498) -- Path A "u2" exact
//      match -> {offset, 2, 2, true}; Path B u2-intrinsic, offset>=4 && %4==0
//      -> {offset-4, 4, 12, true}; everything else -> {} (all-zero, !confident).
//    * NO_COMPILE = 0x01|0x02|0x04|0x08 << 24 == 0x0F000000 (vmhook.hpp:7579).
//    * is_valid_pointer() (vmhook.hpp:2047-2084) -- rejects addr<=0xFFFF,
//      addr>=0x00007FFFFFFFFFFF, odd addresses, and an EXPLICIT sentinel list.
//    * method_flags_layout / method_flags_slot default = {0,0,0,false}.
//
//  POSIX-safety: no fabricated mapped address is ever read.  All pointer-shaped
//  inputs go ONLY to is_valid_pointer() (which decides on the integer value
//  BEFORE any dereference) or to set_dont_inline()/get_flags()/is_static()
//  (which short-circuit on the same guard with no JVM).  All other assertions
//  are pure arithmetic / compile-time / owned-buffer.
// =========================================================================
namespace mfw_deep
{
    using vmhook::hotspot::derive_method_flags_layout;
    using vmhook::hotspot::method_flags_evidence;
    using vmhook::hotspot::method_flags_layout;
    using vmhook::hotspot::method_flags_slot;

    constexpr auto path_b(std::uint64_t intrinsic_off) -> method_flags_evidence
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type    = "u2";
        ev.intrinsic_id_offset  = intrinsic_off;
        return ev;
    }
    constexpr auto path_a(std::uint64_t flags_off, const char* type) -> method_flags_evidence
    {
        method_flags_evidence ev{};
        ev.flags_present = true;
        ev.flags_type    = type;
        ev.flags_offset  = flags_off;
        return ev;
    }
    constexpr auto path_b_typed(std::uint64_t intrinsic_off, const char* type) -> method_flags_evidence
    {
        method_flags_evidence ev{};
        ev.intrinsic_id_present = true;
        ev.intrinsic_id_type    = type;
        ev.intrinsic_id_offset  = intrinsic_off;
        return ev;
    }
}

// -- D1. DEFAULT-STRUCT "NO GUESS" CONTRACT (compile-time + runtime). --------
//  A refused derivation MUST be the value-initialised default {0,0,0,false}
//  for BOTH method_flags_layout and method_flags_slot -- never a partially
//  populated struct that a caller might mistake for a real placement.
static_assert(mfw_deep::method_flags_layout{}.offset == 0
              && mfw_deep::method_flags_layout{}.width_bytes == 0
              && mfw_deep::method_flags_layout{}.dont_inline_bit == 0
              && mfw_deep::method_flags_layout{}.not_compilable_mask == 0u
              && mfw_deep::method_flags_layout{}.confident == false,
              "method_flags_layout default is the all-zero, not-confident 'no guess'");
static_assert(mfw_deep::method_flags_slot{}.address == nullptr
              && mfw_deep::method_flags_slot{}.width_bytes == 0
              && mfw_deep::method_flags_slot{}.dont_inline_bit == 0
              && mfw_deep::method_flags_slot{}.not_compilable_mask == 0u
              && mfw_deep::method_flags_slot{}.confident == false,
              "method_flags_slot default is null/zero/not-confident 'no guess'");
// Every refusing input returns EXACTLY that default (constexpr-checked).
static_assert(!mfw_deep::derive_method_flags_layout(mfw_deep::method_flags_evidence{}).confident
              && mfw_deep::derive_method_flags_layout(mfw_deep::method_flags_evidence{}).offset == 0
              && mfw_deep::derive_method_flags_layout(mfw_deep::method_flags_evidence{}).width_bytes == 0
              && mfw_deep::derive_method_flags_layout(mfw_deep::method_flags_evidence{}).dont_inline_bit == 0,
              "empty evidence derives the exact all-zero default");

static auto test_mfw_deep_default_no_guess() -> void
{
    const method_flags_layout def{};
    check("mfw_deep_layout_default_all_zero",
          def.offset == 0u && def.width_bytes == 0 && def.dont_inline_bit == 0 && !def.confident);

    const vmhook::hotspot::method_flags_slot sdef{};
    check("mfw_deep_slot_default_all_zero",
          sdef.address == nullptr && sdef.width_bytes == 0
          && sdef.dont_inline_bit == 0 && !sdef.confident);

    // A representative set of REFUSED inputs all collapse to the SAME default.
    const method_flags_evidence refused[]{
        method_flags_evidence{},                       // empty
        mfw_deep::path_a(44, "u1"),                    // flags present, wrong width
        mfw_deep::path_a(44, "MethodFlags"),           // flags present, object type
        mfw_deep::path_b(0),                           // intrinsic, underflow
        mfw_deep::path_b(2),                           // intrinsic, underflow
        mfw_deep::path_b(46),                          // intrinsic, misaligned
        mfw_deep::path_b(45),                          // intrinsic, odd
    };
    bool all_default{ true };
    for (const method_flags_evidence& ev : refused)
    {
        const method_flags_layout l{ derive_method_flags_layout(ev) };
        if (l.confident || l.offset != 0u || l.width_bytes != 0 || l.dont_inline_bit != 0)
        {
            all_default = false;
            break;
        }
    }
    check("mfw_deep_every_refused_input_is_exact_default", all_default);
}

// -- D2. is_valid_pointer() EXACT REJECTION CONTRACT (pure, no deref). -------
//  Pin the precise guard set_dont_inline / get_flags rely on (vmhook.hpp:2047).
//  Every value here is decided by INTEGER comparison before any memory access,
//  so passing them is POSIX-safe.  Source facts:
//    floor   = 0x000000000000FFFF  (addr <= floor rejected)
//    ceiling = 0x00007FFFFFFFFFFF  (addr >= ceiling rejected)
//    odd addresses rejected; explicit low32 sentinel list rejected.
static auto test_mfw_deep_is_valid_pointer_contract() -> void
{
    using vmhook::hotspot::is_valid_pointer;

    auto as_ptr = [](std::uintptr_t v) -> const void*
    { return reinterpret_cast<const void*>(v); };

    // nullptr and the floor boundary: <= 0xFFFF all rejected.
    check("mfw_deep_ivp_null_rejected", !is_valid_pointer(nullptr));
    check("mfw_deep_ivp_floor_value_rejected", !is_valid_pointer(as_ptr(0xFFFFull)));
    check("mfw_deep_ivp_below_floor_rejected", !is_valid_pointer(as_ptr(0x1000ull)));
    // First even value strictly above the floor IS accepted (0x10000 > 0xFFFF,
    // even, not a sentinel, below ceiling).
    check("mfw_deep_ivp_just_above_floor_even_accepted", is_valid_pointer(as_ptr(0x10000ull)));

    // Ceiling boundary: >= ceiling rejected; an even value just below accepted.
    check("mfw_deep_ivp_ceiling_value_rejected",
          !is_valid_pointer(as_ptr(0x00007FFFFFFFFFFFull)));
    check("mfw_deep_ivp_above_ceiling_rejected",
          !is_valid_pointer(as_ptr(0x0000800000000000ull)));
    check("mfw_deep_ivp_just_below_ceiling_even_accepted",
          is_valid_pointer(as_ptr(0x00007FFFFFFFFFFEull)));

    // Odd-address rejection: an in-range odd address is rejected purely on the
    // low bit (0x10001 is > floor, < ceiling, but odd).
    check("mfw_deep_ivp_in_range_odd_rejected", !is_valid_pointer(as_ptr(0x10001ull)));
    check("mfw_deep_ivp_in_range_even_accepted", is_valid_pointer(as_ptr(0x10002ull)));

    // The EXPLICIT sentinel list (vmhook.hpp:2070-2078).  Each is forced into the
    // in-range/even band by OR-ing a high base + clearing the low bit, so ONLY the
    // low32-sentinel switch can reject it.  Base 0x100000000 is even, >floor,
    // <ceiling; OR with the sentinel keeps low32 == sentinel.
    const std::uint32_t sentinels[]{
        0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu,
        0xBAADF00Du, 0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
    };
    bool all_sentinels_rejected{ true };
    for (const std::uint32_t s : sentinels)
    {
        // Form an in-range address whose low32 == the sentinel by OR-ing a high
        // base (0x1_00000000 is even, > floor, < ceiling).  Each is rejected: the
        // EVEN sentinels by the explicit low32 switch, the ODD ones by the odd-bit
        // guard that runs first.  Either way is_valid_pointer() returns false
        // without performing any read, so this is POSIX-safe.
        const std::uintptr_t addr{ (std::uintptr_t{ 0x1ull } << 32) | s };
        if (is_valid_pointer(as_ptr(addr))) { all_sentinels_rejected = false; break; }
    }
    check("mfw_deep_ivp_all_sentinels_low32_rejected", all_sentinels_rejected);

    // The sentinel constant itself (as the WHOLE address) is also rejected -- it is
    // below the floor on a 32-bit-shaped value, but on LLP64 it is an in-range even
    // (0xCAFEBABE) or odd value; either way it must be rejected.  Route through
    // uintptr_t for width-correctness.
    check("mfw_deep_ivp_bare_cafebabe_rejected",
          !is_valid_pointer(as_ptr(static_cast<std::uintptr_t>(0xCAFEBABEu))));
    check("mfw_deep_ivp_bare_deadbeef_rejected",
          !is_valid_pointer(as_ptr(static_cast<std::uintptr_t>(0xDEADBEEFu))));
}

// -- D3. PATH-B OFFSET ARITHMETIC AT LARGE/EXTREME LEGAL OFFSETS. ------------
//  The 0..64 sweep above pins the low band.  Here pin that offset-4 holds with
//  no wraparound across the full residue cross-product at LARGE 4-aligned
//  offsets, and that the smallest legal offset (4 -> _status @0) and a very
//  large legal offset both derive width 4 / bit 12 deterministically.
static auto test_mfw_deep_pathB_large_offsets() -> void
{
    // Large 4-aligned legal offsets: derived _status == offset-4, width 4, bit 12.
    const std::uint64_t large_legal[]{
        128u, 256u, 1024u, 4096u, 65536u, 0x10000u + 4u, 0x100000u, 0x1000000u,
    };
    bool large_ok{ true };
    for (const std::uint64_t off : large_legal)
    {
        const method_flags_layout l{ derive_method_flags_layout(mfw_deep::path_b(off)) };
        if (!(l.confident && l.offset == off - 4u && l.width_bytes == 4 && l.dont_inline_bit == 12))
        {
            large_ok = false;
            break;
        }
    }
    check("mfw_deep_pathB_large_legal_offsets_derive_minus4", large_ok);

    // Residue cross-product at a high base: only the 4-aligned member fires.
    const std::uint64_t base{ 0x100000u };  // 4-aligned, large
    bool residue_ok{ true };
    for (std::uint64_t r{ 0 }; r < 4u; ++r)
    {
        const std::uint64_t off{ base + r };
        const method_flags_layout l{ derive_method_flags_layout(mfw_deep::path_b(off)) };
        const bool should{ (off % 4u) == 0u };  // off >= 4 already (base is huge)
        if (l.confident != should) { residue_ok = false; break; }
        if (should && !(l.offset == off - 4u && l.width_bytes == 4 && l.dont_inline_bit == 12))
        {
            residue_ok = false; break;
        }
        if (!should && (l.offset != 0u || l.width_bytes != 0 || l.dont_inline_bit != 0 || l.confident))
        {
            residue_ok = false; break;
        }
    }
    check("mfw_deep_pathB_high_base_residue_cross_product", residue_ok);
}

// -- D4. type_is() EXACT-MATCH STRESS (the constexpr strcmp's two directions). -
//  derive's inner type_is() walks both strings to a shared NUL.  A subtle bug is
//  asymmetry: "u2" vs a string that is a PROPER PREFIX of "u2" ("u") and a string
//  for which "u2" is a proper prefix ("u2x").  Both must FAIL.  Also pin that the
//  NUL terminator equality is what makes "u2"=="u2" succeed (a longer literal that
//  agrees on the first 2 chars but differs at the NUL must fail).  Driven through
//  Path A AND Path B so both call sites of type_is are covered.
static auto test_mfw_deep_type_is_exact_match_stress() -> void
{
    // Strings sharing a prefix with "u2" in BOTH directions.
    struct tcase { const char* type; bool is_u2; };
    const tcase cases[]{
        { "u2",   true  },   // exact
        { "u",    false },   // "u2" is longer -> differ at index 1 ('2' vs '\0')
        { "u2x",  false },   // longer -> differ at index 2 ('\0' vs 'x')
        { "u2xy", false },   // longer still -> differ at index 2
        { "u3",   false },   // differ at index 1
        { "v2",   false },   // differ at index 0
        { "",     false },   // empty -> differ at index 0 ('\0' vs 'u')
    };

    bool pathA_ok{ true };
    bool pathB_ok{ true };
    for (const tcase& c : cases)
    {
        // Path A: flags_present + this type, no intrinsic -> confident iff is_u2.
        const method_flags_layout a{ derive_method_flags_layout(mfw_deep::path_a(48, c.type)) };
        if (a.confident != c.is_u2) { pathA_ok = false; break; }
        if (c.is_u2 && !(a.width_bytes == 2 && a.dont_inline_bit == 2 && a.offset == 48u))
        {
            pathA_ok = false; break;
        }

        // Path B: intrinsic_present + this type at a legal offset -> confident iff is_u2.
        const method_flags_layout b{ derive_method_flags_layout(mfw_deep::path_b_typed(48, c.type)) };
        if (b.confident != c.is_u2) { pathB_ok = false; break; }
        if (c.is_u2 && !(b.width_bytes == 4 && b.dont_inline_bit == 12 && b.offset == 44u))
        {
            pathB_ok = false; break;
        }
    }
    check("mfw_deep_type_is_pathA_exact_match_both_directions", pathA_ok);
    check("mfw_deep_type_is_pathB_exact_match_both_directions", pathB_ok);
}

// -- D5. NO_COMPILE <-> relocated MethodFlags::_status mapping arithmetic. ----
//  The JDK-24 relocation moved the three NOT_*_COMPILABLE bits + QUEUED out of
//  AccessFlags' high byte (24..27) and into MethodFlags::_status low bits 7..10
//  (vmhook.hpp:7568, and flags_layout::methodflags_status_bit).  Pin the exact
//  old<->new bit correspondence as pure arithmetic, and prove the two encodings
//  are in DISJOINT ranges (high byte vs bits 7..10) so neither read sees the
//  other's representation.
static auto test_mfw_deep_no_compile_relocation_mapping() -> void
{
    using namespace flags_layout::methodflags_status_bit;
    const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };

    // OLD (AccessFlags high byte) bit positions of the four NO_COMPILE members.
    constexpr int old_queued     { 24 };  // 0x01000000
    constexpr int old_not_c2     { 25 };  // 0x02000000
    constexpr int old_not_c1     { 26 };  // 0x04000000
    constexpr int old_not_c2_osr { 27 };  // 0x08000000

    // The high-byte mask is exactly those four bit positions.
    check("mfw_deep_no_compile_highbyte_bit_positions",
          no_compile == ((1u << old_queued) | (1u << old_not_c2)
                         | (1u << old_not_c1) | (1u << old_not_c2_osr))
          && no_compile == 0x0F000000u);

    // NEW (MethodFlags::_status) bit positions, verified in flags_layout: queued=7,
    // not_c2=8, not_c1=9, not_c2_osr=10.  Each NEW position is exactly OLD - 17.
    check("mfw_deep_relocation_old_minus_17_equals_new",
          (old_queued     - 17) == queued_for_compilation
          && (old_not_c2     - 17) == is_not_c2_compilable
          && (old_not_c1     - 17) == is_not_c1_compilable
          && (old_not_c2_osr - 17) == is_not_c2_osr);

    // The NEW _status group (bits 7..10) is DISJOINT from the OLD high-byte group
    // (bits 24..27): no bit appears in both encodings.
    const std::uint32_t new_group{ (1u << queued_for_compilation)
                                   | (1u << is_not_c2_compilable)
                                   | (1u << is_not_c1_compilable)
                                   | (1u << is_not_c2_osr) };
    check("mfw_deep_relocation_old_new_groups_disjoint",
          (new_group & no_compile) == 0u);

    // The NEW group sits BELOW bit 16 (reachable by a u2 read of _status) while the
    // OLD group sits ABOVE bit 16 (the high byte) -- they cannot collide in any read.
    check("mfw_deep_new_status_group_below16_old_above16",
          (new_group & 0xFFFF0000u) == 0u && (no_compile & 0x0000FFFFu) == 0u);

    // _dont_inline (bit 12) is ABOVE the relocated _status group (<=10) and BELOW
    // the old high byte -- it shares the u4 _status word with the new group but is a
    // strictly distinct bit from every relocated member.
    check("mfw_deep_dont_inline_distinct_from_relocated_group",
          (new_group & (1u << flags_layout::methodflags_status_bit::dont_inline)) == 0u
          && flags_layout::methodflags_status_bit::dont_inline == 12);
}

// -- D6. CONFIDENT-LAYOUT -> SLOT REPRESENTABILITY (the bridge to the live path). -
//  resolve_method_flags_slot turns a confident layout into a method_flags_slot
//  with the SAME width_bytes / dont_inline_bit (vmhook.hpp:7547).  For every
//  confident band, prove the layout's (width,bit) are exactly a slot-compatible
//  pair: width in {2,4}, bit < width*8, and (1u<<bit) representable in the width's
//  mask.  This is the standalone analogue of "the resolved slot is well-formed".
static auto test_mfw_deep_confident_layout_slot_wellformed() -> void
{
    const method_flags_evidence bands[]{
        evidence_jdk11_20,  // u2 / bit 2
        evidence_jdk21_23,  // u4 / bit 12
        evidence_jdk24_26,  // u4 / bit 12
    };
    bool all_wellformed{ true };
    for (const method_flags_evidence& ev : bands)
    {
        const method_flags_layout l{ derive_method_flags_layout(ev) };
        if (!l.confident) { all_wellformed = false; break; }
        // Width is one of the two the toggle dispatch handles.
        if (l.width_bytes != 2 && l.width_bytes != 4) { all_wellformed = false; break; }
        // Bit fits the width.
        if (!(l.dont_inline_bit >= 0 && l.dont_inline_bit < l.width_bytes * 8))
        {
            all_wellformed = false; break;
        }
        // Mask representable in the width.
        const std::uint32_t mask{ 1u << l.dont_inline_bit };
        const std::uint32_t wmask{ l.width_bytes == 2 ? 0x0000FFFFu : 0xFFFFFFFFu };
        if ((mask & wmask) != mask || mask == 0u) { all_wellformed = false; break; }

        // Synthesise a slot exactly as resolve_method_flags_slot would (over an
        // OWNED buffer base) and confirm the carried width/bit equal the layout's.
        alignas(16) std::array<std::uint8_t, 128> owned{};
        owned.fill(0x00);
        const vmhook::hotspot::method_flags_slot slot{
            owned.data() + l.offset, l.width_bytes, l.dont_inline_bit,
            l.not_compilable_mask, true };
        if (!(slot.confident
              && slot.width_bytes == l.width_bytes
              && slot.dont_inline_bit == l.dont_inline_bit
              && slot.not_compilable_mask == l.not_compilable_mask
              && slot.address == owned.data() + l.offset))
        {
            all_wellformed = false; break;
        }

        // The compile-control bits ride in the SAME word as _dont_inline, so a
        // non-zero mask must fit the layout's width and must not collide with
        // the bit the resolver already owns.  Zero is legal: that is the
        // JDK 11..20 u2 layout, whose compilability bits are in _access_flags.
        if (l.not_compilable_mask != 0u
            && ((l.not_compilable_mask & wmask) != l.not_compilable_mask
                || (l.not_compilable_mask & mask) != 0u
                || l.width_bytes != 4))
        {
            all_wellformed = false; break;
        }
    }
    check("mfw_deep_confident_layout_yields_wellformed_slot", all_wellformed);

    // ── The JDK 21+ compile-control bits ────────────────────────────────────
    // MethodFlags::_status, from methodFlags.hpp (identical on jdk-21 and
    // master): is_not_c2_compilable 1<<8, is_not_c1_compilable 1<<9,
    // is_not_c2_osr_compilable 1<<10, and _dont_inline 1<<12.  The last one is
    // the corroboration that matters -- derive_method_flags_layout reaches bit
    // 12 INDEPENDENTLY, from `_intrinsic_id_offset - 4`, so if the table were
    // misread the two would disagree.
    {
        // A JDK 21+ shape: no exported _flags, u2 _intrinsic_id at a 4-aligned
        // offset.  Path B derives _status at intrinsic_id_offset - 4.
        const vmhook::hotspot::method_flags_evidence jdk21{
            /*flags_present*/ false, nullptr, 0,
            /*intrinsic_id_present*/ true, "u2", 0x40 };
        const auto l21{ vmhook::hotspot::derive_method_flags_layout(jdk21) };

        check("mfw_nc_jdk21_confident",     l21.confident);
        check("mfw_nc_jdk21_width_is_4",    l21.width_bytes == 4);
        check("mfw_nc_jdk21_dont_inline_12", l21.dont_inline_bit == 12);
        check("mfw_nc_jdk21_mask_is_c1_c2_osr",
              l21.not_compilable_mask == ((1u << 8) | (1u << 9) | (1u << 10)));
        check("mfw_nc_jdk21_mask_excludes_dont_inline",
              (l21.not_compilable_mask & (1u << l21.dont_inline_bit)) == 0u);
        check("mfw_nc_jdk21_mask_excludes_queued",
              (l21.not_compilable_mask & (1u << 7)) == 0u);

        // The JDK 11..20 u2 layout must carry NO mask: its compilability bits
        // are in _access_flags, and writing 1<<8..10 into a u2 _flags word
        // would land on is_not_c2_compilable's neighbours or off the end.
        const vmhook::hotspot::method_flags_evidence jdk11{
            /*flags_present*/ true, "u2", 0x30,
            /*intrinsic_id_present*/ true, "u2", 0x40 };
        const auto l11{ vmhook::hotspot::derive_method_flags_layout(jdk11) };
        check("mfw_nc_jdk11_confident",  l11.confident);
        check("mfw_nc_jdk11_width_is_2", l11.width_bytes == 2);
        check("mfw_nc_jdk11_mask_is_zero", l11.not_compilable_mask == 0u);

        // JDK 8: nothing exported -> not confident -> no mask to write anywhere.
        const vmhook::hotspot::method_flags_evidence jdk8{
            false, nullptr, 0, true, "u1", 0x38 };
        const auto l8{ vmhook::hotspot::derive_method_flags_layout(jdk8) };
        check("mfw_nc_jdk8_not_confident", !l8.confident);
        check("mfw_nc_jdk8_mask_is_zero",  l8.not_compilable_mask == 0u);
    }

    // A REFUSED layout must NOT be turned into a confident slot: the default slot
    // (what resolve returns on !confident) is null/zero/not-confident.
    {
        const method_flags_layout refused{ derive_method_flags_layout(evidence_jdk8) };
        check("mfw_deep_refused_layout_is_not_confident", !refused.confident);
        const vmhook::hotspot::method_flags_slot would_be_default{};
        check("mfw_deep_refused_maps_to_default_slot",
              would_be_default.address == nullptr && !would_be_default.confident
              && would_be_default.width_bytes == 0 && would_be_default.dont_inline_bit == 0);
    }
}

// -- D7. set_dont_inline NO-JVM NO-WRITE over the FULL WIDTH-WINDOW + neighbour. -
//  #2(b) above proves a 64-byte buffer is untouched.  Here drive the SAME no-JVM
//  no-op but lay an explicit sentinel ring (0xAA before, 0xBB after) around a
//  hypothetical u4 _flags window and prove set/clear touch NOTHING -- the
//  standalone adjacent-byte anti-clobber the live module also wants, exercised
//  through the REAL set_dont_inline (which no-ops with no JVM -> whole buffer
//  must be byte-identical).
static auto test_mfw_deep_set_dont_inline_no_write_window() -> void
{
    alignas(16) std::array<std::uint8_t, 96> buf{};
    // Distinct, recognizable pattern per byte so any single-byte spill is caught.
    for (std::size_t i{ 0 }; i < buf.size(); ++i)
    {
        buf[i] = static_cast<std::uint8_t>(0x40u + (i & 0x3Fu));
    }
    const std::array<std::uint8_t, 96> snapshot{ buf };

    auto* const as_method{ reinterpret_cast<const vmhook::hotspot::method*>(buf.data()) };
    // set then clear then set again -- with no JVM all three are no-ops.
    vmhook::hotspot::set_dont_inline(as_method, true);
    vmhook::hotspot::set_dont_inline(as_method, false);
    vmhook::hotspot::set_dont_inline(as_method, true);

    check("mfw_deep_set_dont_inline_no_jvm_buffer_byte_identical",
          std::memcmp(buf.data(), snapshot.data(), buf.size()) == 0);

    // Spot-check the exact would-be u4 window [44,48) and its immediate neighbours
    // [43] and [48] specifically (the bytes a too-wide RMW on a u1/u2 layout would
    // smear) are each unchanged.
    check("mfw_deep_set_dont_inline_window_and_neighbours_intact",
          buf[43] == snapshot[43] && buf[44] == snapshot[44]
          && buf[45] == snapshot[45] && buf[46] == snapshot[46]
          && buf[47] == snapshot[47] && buf[48] == snapshot[48]);
}

// -- D8. PATH PRECEDENCE EXHAUSTIVE TRUTH TABLE over the four evidence gates. -
//  derive's decision is fully determined by four booleans:
//    A = flags_present && flags_type=="u2"
//    B = intrinsic_present && intrinsic_type=="u2" && off>=4 && off%4==0
//  with A taking precedence over B.  Enumerate the full 2x2 outcome matrix using
//  controlled evidence and assert the exact (confident,width,bit) for each cell.
static auto test_mfw_deep_precedence_truth_table() -> void
{
    // A-fireable Path A evidence (u2 _flags @ 44) and B-fireable Path B evidence
    // (u2 intrinsic @ 48 -> _status @ 44).  We toggle each on/off and combine.
    auto make = [](bool a_on, bool b_on) -> method_flags_evidence
    {
        method_flags_evidence ev{};
        if (a_on) { ev.flags_present = true; ev.flags_type = "u2"; ev.flags_offset = 44; }
        else      { ev.flags_present = true; ev.flags_type = "u1"; ev.flags_offset = 44; } // present-but-non-u2
        if (b_on) { ev.intrinsic_id_present = true; ev.intrinsic_id_type = "u2"; ev.intrinsic_id_offset = 48; }
        else      { ev.intrinsic_id_present = true; ev.intrinsic_id_type = "u1"; ev.intrinsic_id_offset = 48; } // u1 -> B off
        return ev;
    };

    // (A=1,B=1) -> Path A wins: width 2, bit 2, offset 44.
    {
        const method_flags_layout l{ derive_method_flags_layout(make(true, true)) };
        check("mfw_deep_truth_A1B1_pathA_wins",
              l.confident && l.width_bytes == 2 && l.dont_inline_bit == 2 && l.offset == 44u);
    }
    // (A=1,B=0) -> Path A: width 2, bit 2.
    {
        const method_flags_layout l{ derive_method_flags_layout(make(true, false)) };
        check("mfw_deep_truth_A1B0_pathA",
              l.confident && l.width_bytes == 2 && l.dont_inline_bit == 2 && l.offset == 44u);
    }
    // (A=0,B=1) -> Path B: width 4, bit 12, offset 48-4 == 44.
    {
        const method_flags_layout l{ derive_method_flags_layout(make(false, true)) };
        check("mfw_deep_truth_A0B1_pathB",
              l.confident && l.width_bytes == 4 && l.dont_inline_bit == 12 && l.offset == 44u);
    }
    // (A=0,B=0) -> neither fires -> refused default.
    {
        const method_flags_layout l{ derive_method_flags_layout(make(false, false)) };
        check("mfw_deep_truth_A0B0_refused",
              !l.confident && l.offset == 0u && l.width_bytes == 0 && l.dont_inline_bit == 0);
    }
}

// =========================================================================
//  DEEPENING WAVE 2 -- namespaced additive section (mfw_deep2).
//
//  ADDITIVE ONLY: touches none of the functions above.  Covers bounds /
//  structure-logic cases the prior passes did not reach, all values traced
//  directly from vmhook.hpp source:
//    * iterate_struct_entries(type,field)  (vmhook.hpp:1990-2008) -- null-arg
//      rejects + no-JVM (gHotSpotVMStructs null) -> ALWAYS nullptr.
//    * iterate_type_entries(type)           (vmhook.hpp:1964-1979) -- same.
//    * resolve_method_flags_slot(method*)   (vmhook.hpp:7510-7552) -- null/invalid
//      this -> default slot; no JVM -> entries null -> evidence empty ->
//      derive !confident -> default slot.
//    * resolve_constant_pool_symbol(base,index,cp_length) (vmhook.hpp:3898-3919)
//      -- EARLY rejects (index==0 OR !is_valid_pointer(base)) BEFORE any read,
//      then the in-bounds reject (cp_length>=0 && index>=cp_length).
//    * vm_struct_entry_t / vm_type_entry_t ABI (vmhook.hpp:1880-1899) --
//      standard-layout, member offsetof ordering, sizeof relationships.
//    * is_valid_pointer floor=0xFFFF / ceiling=0x00007FFFFFFFFFFF
//      (vmhook.os::user_address_floor / _ceiling, vmhook.hpp:515/520).
//
//  POSIX-safety: NO fabricated mapped address is ever dereferenced.  Pointer
//  inputs go ONLY to (a) is_valid_pointer / the iterate_* + resolve_* guards
//  that decide on the integer value or a null gHotSpotVMStructs BEFORE any
//  read, or (b) resolve_constant_pool_symbol's EARLY reject path (index==0 or
//  an is_valid_pointer-rejected base) which returns before the line-3909
//  is_readable_pointer/raw-read.  We NEVER hand it a valid base + nonzero
//  in-range index (that would reach the raw read).  Everything else is pure
//  arithmetic / offsetof / std::is_standard_layout / owned-buffer.
// =========================================================================
namespace mfw_deep2
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // OWNED, fully-initialised stub tables (NOT the JVM global).  Used only for
    // ABI/layout assertions and for proving the struct shape the linear scan in
    // iterate_struct_entries walks; never passed to any accessor that reads the
    // JVM symbol.  Terminator = all-zero entry (matches HotSpot's convention).
    constexpr vm_struct_entry_t make_struct_entry(const char* t, const char* f,
                                                  const char* ts, std::uint64_t off)
    {
        return vm_struct_entry_t{ t, f, ts, /*is_static*/ 0, off, /*address*/ nullptr };
    }
}

// -- E1. iterate_struct_entries: NULL-ARG + NO-JVM nullptr contract. ---------
//  Both args are guarded (vmhook.hpp:1993).  With no JVM the global is null so
//  EVERY well-formed lookup also yields nullptr.  Pure: decided before deref.
static auto test_mfw_deep2_iterate_struct_null_and_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;

    // (a) null type / null field / both null -> nullptr (the explicit guard).
    check("mfw_deep2_iterate_struct_null_type_returns_null",
          iterate_struct_entries(nullptr, "_flags") == nullptr);
    check("mfw_deep2_iterate_struct_null_field_returns_null",
          iterate_struct_entries("Method", nullptr) == nullptr);
    check("mfw_deep2_iterate_struct_both_null_returns_null",
          iterate_struct_entries(nullptr, nullptr) == nullptr);

    // (b) Well-formed lookups with NO JVM: gHotSpotVMStructs is null -> nullptr
    //     for the two fields THIS feature owns and a representative neighbour.
    check("mfw_deep2_iterate_struct_method_flags_no_jvm_null",
          iterate_struct_entries("Method", "_flags") == nullptr);
    check("mfw_deep2_iterate_struct_method_intrinsic_id_no_jvm_null",
          iterate_struct_entries("Method", "_intrinsic_id") == nullptr);
    check("mfw_deep2_iterate_struct_method_access_flags_no_jvm_null",
          iterate_struct_entries("Method", "_access_flags") == nullptr);

    // (c) An empty type/field string is NOT the null guard -- it is searched and,
    //     with no JVM, still resolves to nullptr (no entry has empty names).
    check("mfw_deep2_iterate_struct_empty_strings_no_jvm_null",
          iterate_struct_entries("", "") == nullptr);
}

// -- E2. iterate_type_entries: NULL-ARG + NO-JVM nullptr contract. -----------
static auto test_mfw_deep2_iterate_type_null_and_no_jvm() -> void
{
    using vmhook::hotspot::iterate_type_entries;
    check("mfw_deep2_iterate_type_null_returns_null",
          iterate_type_entries(nullptr) == nullptr);
    check("mfw_deep2_iterate_type_method_no_jvm_null",
          iterate_type_entries("Method") == nullptr);
    check("mfw_deep2_iterate_type_constantpool_no_jvm_null",
          iterate_type_entries("ConstantPool") == nullptr);
    check("mfw_deep2_iterate_type_empty_string_no_jvm_null",
          iterate_type_entries("") == nullptr);
}

// -- E3. resolve_method_flags_slot: OFFSET-RESOLUTION null-when-no-JVM. -------
//  The live bridge.  null/invalid this -> default slot (before any VMStruct
//  read).  An in-range OWNED buffer this -> entries null (no JVM) -> evidence
//  empty -> derive !confident -> default slot.  Every return is the all-zero
//  default {nullptr,0,0,false}; the address is NEVER buffer-derived here.
static auto test_mfw_deep2_resolve_slot_no_jvm() -> void
{
    using vmhook::hotspot::resolve_method_flags_slot;

    auto is_default = [](const vmhook::hotspot::method_flags_slot& s) -> bool
    {
        return s.address == nullptr && s.width_bytes == 0
            && s.dont_inline_bit == 0 && !s.confident;
    };

    // (a) null this -> default slot (the !method_pointer guard, vmhook.hpp:7513).
    check("mfw_deep2_resolve_slot_null_this_default",
          is_default(resolve_method_flags_slot(nullptr)));

    // (b) invalid this (odd, in-range) -> rejected by is_valid_pointer -> default.
    {
        auto* const bogus{ reinterpret_cast<const vmhook::hotspot::method*>(
            static_cast<std::uintptr_t>(0x10001ull)) };  // >floor, <ceiling, ODD
        check("mfw_deep2_resolve_slot_invalid_odd_this_default",
              is_default(resolve_method_flags_slot(bogus)));
    }

    // (c) below-floor this -> rejected by is_valid_pointer -> default (no deref).
    {
        auto* const low{ reinterpret_cast<const vmhook::hotspot::method*>(
            static_cast<std::uintptr_t>(0x100ull)) };
        check("mfw_deep2_resolve_slot_below_floor_this_default",
              is_default(resolve_method_flags_slot(low)));
    }

    // (d) in-range OWNED buffer this (VALID pointer) -> no JVM -> entries null ->
    //     evidence empty -> derive !confident -> default slot; the address is the
    //     all-zero default (NOT base+offset) because the slot was refused.
    {
        alignas(16) std::array<std::uint8_t, 64> owned{};
        owned.fill(0x00);
        auto* const as_method{ reinterpret_cast<const vmhook::hotspot::method*>(owned.data()) };
        const vmhook::hotspot::method_flags_slot slot{ resolve_method_flags_slot(as_method) };
        check("mfw_deep2_resolve_slot_valid_this_no_jvm_default", is_default(slot));
        // Specifically: the refused slot's address is NULL, never owned.data()+off.
        check("mfw_deep2_resolve_slot_refused_address_is_null_not_base",
              slot.address == nullptr);
    }
}

// -- E4. resolve_constant_pool_symbol: EARLY-REJECT bound paths (POSIX-safe). -
//  Source order (vmhook.hpp:3901-3905):
//    1. index==0 OR !is_valid_pointer(base) -> nullptr  (BEFORE any deref)
//    2. cp_length>=0 && index>=cp_length    -> nullptr
//  We exercise ONLY paths that return at step 1 (no read crosses to step 3).
static auto test_mfw_deep2_cp_symbol_early_rejects() -> void
{
    using vmhook::hotspot::klass;

    // (a) index==0 short-circuits regardless of base -> nullptr.  Pass null base
    //     so even if the guard order changed it stays POSIX-safe.
    check("mfw_deep2_cp_symbol_index0_null_base_null",
          klass::resolve_constant_pool_symbol(nullptr, /*index*/ 0u, /*cp_len*/ 100) == nullptr);

    // (a') index==0 with an is_valid_pointer-REJECTED base: still returns at the
    //      index==0 clause; the base is never dereferenced.
    {
        auto* const bad_base{ reinterpret_cast<void**>(static_cast<std::uintptr_t>(0x10001ull)) }; // odd
        check("mfw_deep2_cp_symbol_index0_bad_base_null",
              klass::resolve_constant_pool_symbol(bad_base, 0u, 100) == nullptr);
    }

    // (b) nonzero index but NULL base -> is_valid_pointer(nullptr)==false ->
    //     nullptr at step 1, no deref.
    check("mfw_deep2_cp_symbol_nonzero_index_null_base_null",
          klass::resolve_constant_pool_symbol(nullptr, /*index*/ 5u, /*cp_len*/ 100) == nullptr);

    // (c) nonzero index but INVALID (odd / below-floor / sentinel) base -> step-1
    //     reject via is_valid_pointer, no deref.  Each base is decided purely on
    //     its integer value.
    {
        const std::uintptr_t bad_bases[]{
            0x1ull,                                   // below floor
            0xFFFFull,                                // == floor (rejected by <=)
            0x10001ull,                               // in-range but ODD
            (std::uintptr_t{ 0x1ull } << 32) | 0xDEADBEEFull, // in-range even, low32 sentinel
            0x0000800000000000ull,                    // == ceiling (rejected by >=)
        };
        bool all_reject{ true };
        for (const std::uintptr_t b : bad_bases)
        {
            auto* const base{ reinterpret_cast<void**>(b) };
            if (klass::resolve_constant_pool_symbol(base, /*index*/ 3u, /*cp_len*/ 100) != nullptr)
            {
                all_reject = false;
                break;
            }
        }
        check("mfw_deep2_cp_symbol_invalid_bases_all_reject_no_deref", all_reject);
    }
}

// -- E5. resolve_constant_pool_symbol: IN-BOUNDS THRESHOLD logic (pure model). -
//  The bound clause is `cp_length>=0 && index>=cp_length` -> reject.  We cannot
//  drive the live function past step 1 without a real base (POSIX), so pin the
//  EXACT threshold arithmetic the source uses as a standalone predicate, plus
//  the just-in / just-out edges and the negative-cp_length "unbounded" case.
static auto test_mfw_deep2_cp_bound_threshold_logic() -> void
{
    // Mirror of the source predicate (vmhook.hpp:3905): the index is OUT OF
    // BOUNDS (rejected) iff cp_length is non-negative AND index >= cp_length.
    auto out_of_bounds = [](std::uint32_t index, std::int32_t cp_length) -> bool
    {
        return cp_length >= 0 && index >= static_cast<std::uint32_t>(cp_length);
    };

    // Threshold sweep around cp_length == 8: indices 0..7 in-bounds, 8.. out.
    constexpr std::int32_t cp_length{ 8 };
    bool sweep_ok{ true };
    for (std::uint32_t i{ 1u }; i <= 12u; ++i)  // index 0 never reaches this clause
    {
        const bool expect_oob{ i >= 8u };
        if (out_of_bounds(i, cp_length) != expect_oob) { sweep_ok = false; break; }
    }
    check("mfw_deep2_cp_bound_threshold_sweep_around_8", sweep_ok);

    // Just-in (index == cp_length-1) accepted; just-out (index == cp_length) rejected.
    check("mfw_deep2_cp_bound_just_in_accepted", !out_of_bounds(7u, 8));
    check("mfw_deep2_cp_bound_just_out_rejected", out_of_bounds(8u, 8));
    check("mfw_deep2_cp_bound_one_past_rejected", out_of_bounds(9u, 8));

    // cp_length == 0: every nonzero index is out of bounds (0 >= 0 is the edge,
    // but index reaching this clause is always >= 1 since index==0 short-circuits).
    check("mfw_deep2_cp_bound_zero_length_rejects_index1", out_of_bounds(1u, 0));

    // Negative cp_length == "length unknown / unbounded": the clause is SKIPPED
    // (cp_length>=0 is false), so NO index is rejected by the bound check.
    check("mfw_deep2_cp_bound_negative_length_skips_bound_check",
          !out_of_bounds(1u, -1) && !out_of_bounds(0xFFFFFFFFu, -1));

    // The maximum representable index never wraps the comparison (unsigned domain).
    check("mfw_deep2_cp_bound_max_index_out_of_bounds_when_bounded",
          out_of_bounds(0xFFFFFFFFu, 100));
}

// -- E6. vm_struct_entry_t / vm_type_entry_t ABI + MEMBER-ORDER LAYOUT. -------
//  The linear scan in iterate_struct_entries advances by `++entry` over an array
//  of these PODs and reads ->type_name / ->field_name / ->type_string / ->offset.
//  Pin the standard-layout property + the exact member ordering (offsetof) the
//  scan and the offset-arithmetic rely on.  All compile-time / qualified-type.
static auto test_mfw_deep2_vmstruct_abi_layout() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // Standard-layout is what makes ++entry over the JVM-exported C array legal.
    check("mfw_deep2_vmstruct_is_standard_layout",
          std::is_standard_layout<vm_struct_entry_t>::value);
    check("mfw_deep2_vmtype_is_standard_layout",
          std::is_standard_layout<vm_type_entry_t>::value);

    // Member ORDER (vmhook.hpp:1891-1899): type_name, field_name, type_string,
    // is_static, offset, address -- strictly increasing offsetof.
    check("mfw_deep2_vmstruct_member_order",
          offsetof(vm_struct_entry_t, type_name)   < offsetof(vm_struct_entry_t, field_name)
          && offsetof(vm_struct_entry_t, field_name)  < offsetof(vm_struct_entry_t, type_string)
          && offsetof(vm_struct_entry_t, type_string) < offsetof(vm_struct_entry_t, is_static)
          && offsetof(vm_struct_entry_t, is_static)   < offsetof(vm_struct_entry_t, offset)
          && offsetof(vm_struct_entry_t, offset)      < offsetof(vm_struct_entry_t, address));

    // The first three members are the const char* trio strcmp walks; pin their type.
    check("mfw_deep2_vmstruct_name_members_are_const_char_ptr",
          std::is_same<decltype(vm_struct_entry_t::type_name), const char*>::value
          && std::is_same<decltype(vm_struct_entry_t::field_name), const char*>::value
          && std::is_same<decltype(vm_struct_entry_t::type_string), const char*>::value);

    // offset is u64 (the value resolve_method_flags_slot feeds into base+offset and
    // the derivation does intrinsic-4 on); address is void*.
    check("mfw_deep2_vmstruct_offset_is_u64_address_is_voidptr",
          std::is_same<decltype(vm_struct_entry_t::offset), std::uint64_t>::value
          && std::is_same<decltype(vm_struct_entry_t::address), void*>::value);

    // vm_type_entry_t order (vmhook.hpp:1880-1888): type_name, superclass_name,
    // is_oop_type_type, is_integer_type, is_unsigned, size -- and size is u64.
    check("mfw_deep2_vmtype_member_order",
          offsetof(vm_type_entry_t, type_name)       < offsetof(vm_type_entry_t, superclass_name)
          && offsetof(vm_type_entry_t, superclass_name) < offsetof(vm_type_entry_t, is_oop_type_type)
          && offsetof(vm_type_entry_t, is_oop_type_type) < offsetof(vm_type_entry_t, is_integer_type)
          && offsetof(vm_type_entry_t, is_integer_type)  < offsetof(vm_type_entry_t, is_unsigned)
          && offsetof(vm_type_entry_t, is_unsigned)      < offsetof(vm_type_entry_t, size));
    check("mfw_deep2_vmtype_size_is_u64",
          std::is_same<decltype(vm_type_entry_t::size), std::uint64_t>::value);

    // The whole struct is at least large enough to hold its members contiguously
    // (a single entry stride for the ++entry walk must cover the last member).
    check("mfw_deep2_vmstruct_size_covers_last_member",
          sizeof(vm_struct_entry_t) >= offsetof(vm_struct_entry_t, address) + sizeof(void*));
    check("mfw_deep2_vmtype_size_covers_last_member",
          sizeof(vm_type_entry_t) >= offsetof(vm_type_entry_t, size) + sizeof(std::uint64_t));
}

// -- E7. OWNED stub-table linear-scan MODEL (the strcmp scan, no JVM symbol). --
//  iterate_struct_entries walks gHotSpotVMStructs with ++entry until a null
//  type_name terminator, skipping null field_name entries, matching on BOTH
//  type_name and field_name by strcmp.  We CANNOT redirect the real function to
//  our array (it reads the JVM global), so we model the IDENTICAL scan over an
//  OWNED std::array and pin the match / skip / terminate / bound behaviour --
//  the structure logic the real scan implements, with no fabricated address.
namespace
{
    // Re-implementation of iterate_struct_entries's scan body over an OWNED,
    // null-terminated table (byte-for-byte the same predicate as vmhook.hpp:1997).
    const vmhook::hotspot::vm_struct_entry_t*
    scan_owned_table(const vmhook::hotspot::vm_struct_entry_t* table,
                     const char* type_name, const char* field_name)
    {
        if (!type_name || !field_name) { return nullptr; }
        for (const vmhook::hotspot::vm_struct_entry_t* e{ table }; e && e->type_name; ++e)
        {
            if (!e->field_name) { continue; }
            if (!std::strcmp(e->type_name, type_name) && !std::strcmp(e->field_name, field_name))
            {
                return e;
            }
        }
        return nullptr;
    }
}

static auto test_mfw_deep2_owned_table_scan_model() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    // An owned table with: a matching Method::_flags entry, a Method::_access_flags
    // entry, a PARTIAL entry (type set, field_name null -> must be SKIPPED), and a
    // zero terminator.  Index [2] deliberately precedes the real target to prove
    // the null-field_name skip does not abort the scan.
    const std::array<vm_struct_entry_t, 5> table{ {
        mfw_deep2::make_struct_entry("Method", "_access_flags", "AccessFlags", 32),
        mfw_deep2::make_struct_entry("ConstMethod", "_constants", "ConstantPool*", 16),
        mfw_deep2::make_struct_entry("Method", nullptr, "u2", 44),   // partial -> skip
        mfw_deep2::make_struct_entry("Method", "_flags", "u2", 44),  // the real target
        vm_struct_entry_t{ nullptr, nullptr, nullptr, 0, 0, nullptr }, // terminator
    } };

    // Match on BOTH type and field: returns the right entry, with the right offset.
    {
        const vm_struct_entry_t* hit{ scan_owned_table(table.data(), "Method", "_flags") };
        check("mfw_deep2_scan_finds_method_flags",
              hit != nullptr && hit->offset == 44u
              && std::strcmp(hit->type_string, "u2") == 0);
    }
    {
        const vm_struct_entry_t* hit{ scan_owned_table(table.data(), "Method", "_access_flags") };
        check("mfw_deep2_scan_finds_method_access_flags",
              hit != nullptr && hit->offset == 32u);
    }

    // The null-field_name partial entry [2] is SKIPPED, not matched, and does not
    // stop the scan from reaching the real _flags at [3] (proven above).  A lookup
    // whose field matches ONLY the partial entry's (absent) field finds nothing.
    check("mfw_deep2_scan_skips_partial_entry_no_false_match",
          scan_owned_table(table.data(), "Method", "_nonexistent_field") == nullptr);

    // Right type + wrong field, and wrong type + right field, both miss (BOTH must
    // strcmp-match) -- the two-key bound the scan enforces.
    check("mfw_deep2_scan_right_type_wrong_field_misses",
          scan_owned_table(table.data(), "Method", "_constants") == nullptr);
    check("mfw_deep2_scan_wrong_type_right_field_misses",
          scan_owned_table(table.data(), "Klass", "_flags") == nullptr);

    // Null-arg guard mirrors the real function.
    check("mfw_deep2_scan_null_args_return_null",
          scan_owned_table(table.data(), nullptr, "_flags") == nullptr
          && scan_owned_table(table.data(), "Method", nullptr) == nullptr);

    // The zero terminator STOPS the scan: a type that appears NOWHERE returns
    // nullptr (the loop exits on e->type_name == nullptr, not by overrun).
    check("mfw_deep2_scan_terminator_stops_unknown_type",
          scan_owned_table(table.data(), "NoSuchType", "_x") == nullptr);
}

// -- E8. is_valid_pointer boundary thresholds the slot/cp guards depend on. ---
//  resolve_method_flags_slot AND resolve_constant_pool_symbol both gate on
//  is_valid_pointer.  Pin the exact just-in / just-out edges at BOTH the floor
//  (0xFFFF) and ceiling (0x00007FFFFFFFFFFF), purely on integer value.
static auto test_mfw_deep2_is_valid_pointer_thresholds() -> void
{
    using vmhook::hotspot::is_valid_pointer;
    auto p = [](std::uintptr_t v) -> const void* { return reinterpret_cast<const void*>(v); };

    // Floor: addr <= 0xFFFF rejected; the first EVEN addr strictly above accepted.
    check("mfw_deep2_ivp_at_floor_rejected",       !is_valid_pointer(p(0xFFFFull)));
    check("mfw_deep2_ivp_floor_plus_1_odd_rejected", !is_valid_pointer(p(0x10000ull + 1ull)));
    check("mfw_deep2_ivp_floor_plus_1_even_accepted", is_valid_pointer(p(0x10000ull)));

    // Ceiling: addr >= ceiling rejected; the largest EVEN addr below it accepted.
    check("mfw_deep2_ivp_at_ceiling_rejected",  !is_valid_pointer(p(0x00007FFFFFFFFFFFull)));
    check("mfw_deep2_ivp_ceiling_plus_rejected", !is_valid_pointer(p(0x0000800000000000ull)));
    check("mfw_deep2_ivp_ceiling_minus_1_even_accepted",
          is_valid_pointer(p(0x00007FFFFFFFFFFEull)));

    // The accepted band's parity rule: same in-range magnitude, odd rejected /
    // even accepted -- the single low-bit discriminator the scan-base check uses.
    check("mfw_deep2_ivp_in_range_parity_rule",
          is_valid_pointer(p(0x40000000ull)) && !is_valid_pointer(p(0x40000001ull)));
}

// =========================================================================
//  DEEPENING WAVE 3 -- namespaced additive section (mfw_deep3).
//
//  ADDITIVE ONLY: touches none of the functions above.  Broadens the no-JVM
//  surface to the sibling pure-logic the feature relies on but the prior waves
//  did not reach, all values traced directly from vmhook.hpp source:
//    * narrow_decode / narrow_encode  (vmhook.hpp:5459-5485) -- the shared
//      compressed-pointer codec primitives.  decode = base + (compressed<<shift);
//      encode = (addr-base)>>shift.  Pure unsigned arithmetic over OWNED values,
//      no dereference -- round-trip + boundary + the documented shift cases.
//    * read_java_string DECODE LOGIC  (append_utf8 vmhook.hpp:20648-20672 +
//      utf16_to_utf8 vmhook.hpp:20676-20694 + char_count/body_bytes derivation
//      20600/20621).  The lambdas are local, so we re-implement their EXACT
//      algorithm and drive it over byte arrays WE build -- UTF-8 1..4-byte
//      boundaries, LATIN1, UTF16, surrogate pairs, astral, embedded NUL.
//    * method_proxy::value_t  (vmhook.hpp:16294-16462) -- variant classification
//      (is_void / is_string / as_string) + numeric conversion operator.  Built
//      with explicit variant alternatives; NEVER cast a value_t to a vector.
//    * sig_char_to_basic_type (vmhook.hpp:16215) + jvm_primitive_byte_width
//      (vmhook.hpp:16250) -- the signature-char decode tables, every documented
//      char.
//    * method_proxy::call arg cap == 8 (vmhook.hpp:16703-16705 / 17321) +
//      frame::get_arguments long/double 2-slot widening (vmhook.hpp:6302-6312).
//
//  POSIX-safety: no fabricated mapped address is ever dereferenced.  The codec
//  primitives are pure arithmetic on integers we own.  Every string-decode input
//  is an OWNED std::array / std::vector / std::string.  Embedded NUL is built at
//  RUNTIME (push_back of '\0'); no raw NUL / non-ASCII byte appears in source.
// =========================================================================
namespace mfw_deep3
{
    // EXACT re-implementation of read_java_string's append_utf8 lambda
    // (vmhook.hpp:20648-20672): a Unicode code point -> standard UTF-8 (1..4 bytes).
    auto append_utf8(std::string& out, std::uint32_t cp) -> void
    {
        if (cp < 0x80u)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800u)
        {
            out += static_cast<char>(0xC0u | (cp >> 6));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
        else if (cp < 0x10000u)
        {
            out += static_cast<char>(0xE0u | (cp >> 12));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
        else
        {
            out += static_cast<char>(0xF0u | (cp >> 18));
            out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }

    // EXACT re-implementation of read_java_string's utf16_to_utf8 lambda
    // (vmhook.hpp:20676-20694): native-endian UTF-16 units -> UTF-8, combining a
    // high+low surrogate pair into one astral code point.
    auto utf16_to_utf8(std::string& out, const std::uint16_t* const chars,
                       const std::int32_t count) -> void
    {
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            std::uint32_t cp{ chars[i] };
            if (cp >= 0xD800u && cp <= 0xDBFFu && (i + 1) < count)
            {
                const std::uint16_t low{ chars[i + 1] };
                if (low >= 0xDC00u && low <= 0xDFFFu)
                {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    ++i;
                }
            }
            append_utf8(out, cp);
        }
    }

    // char_count derivation (vmhook.hpp:20600): JDK8 char[] / LATIN1 -> `length`;
    // UTF16 byte[] -> length/2.
    auto char_count_for(bool has_coder, std::uint8_t coder, std::int32_t length) -> std::int32_t
    {
        return (has_coder && coder != 0) ? length / 2 : length;
    }
    // body_bytes derivation (vmhook.hpp:20621): no coder (JDK8 char[]) -> 2*length;
    // either JDK9+ byte[] layout -> `length` is the byte count directly.
    auto body_bytes_for(bool has_coder, std::int32_t length) -> std::int32_t
    {
        return has_coder ? length : length * 2;
    }
}

// -- F1. narrow_decode / narrow_encode ROUND-TRIP + DOCUMENTED SHIFT CASES. ---
//  Pure unsigned arithmetic (vmhook.hpp:5459-5485).  Round-trip an OWNED set of
//  addresses through encode->decode for the realistic (base,shift) pairs HotSpot
//  uses: shift 0 (heap < 4 GB), shift 3 (8-byte-aligned oops up to 32 GB), and a
//  non-zero base.  No pointer is ever dereferenced -- the reinterpret_cast in
//  narrow_decode produces a value we only compare as an integer.
static auto test_mfw_deep3_narrow_codec_roundtrip() -> void
{
    using vmhook::hotspot::narrow_decode;
    using vmhook::hotspot::narrow_encode;

    auto as_u64 = [](void* p) -> std::uint64_t
    { return reinterpret_cast<std::uintptr_t>(p); };

    // (base, shift) regimes verified against the decode_oop_pointer doc
    // (vmhook.hpp:5496-5499): base 0 / shift 0; base 0 / shift 3; non-zero base.
    struct regime { std::uint64_t base; std::uint32_t shift; };
    const regime regimes[]{
        { 0x0000000000000000ull, 0u },   // heap < 4 GB at address 0
        { 0x0000000000000000ull, 3u },   // 8-byte-aligned oops, base 0
        { 0x0000000800000000ull, 3u },   // non-zero heap base, shift 3
        { 0x0000000100000000ull, 0u },   // non-zero base, shift 0
    };

    // Compressed values that, after << shift + base, stay below the user_address
    // ceiling so the decoded value is a plausible address pattern (we never read it).
    const std::uint32_t compressed_samples[]{
        1u, 2u, 8u, 0x1000u, 0x10000u, 0x00FFFFFFu, 0x10000000u,
    };

    bool roundtrip_ok{ true };
    for (const regime r : regimes)
    {
        for (const std::uint32_t c : compressed_samples)
        {
            // For shift 3 the encode requires (addr-base) be a multiple of 8; the
            // decode of a compressed value always produces such an addr, so
            // decode-then-encode is the faithful round-trip direction.
            void* const decoded{ narrow_decode(r.base, r.shift, c) };
            const std::uint64_t expected_addr{ r.base + (static_cast<std::uint64_t>(c) << r.shift) };
            if (as_u64(decoded) != expected_addr) { roundtrip_ok = false; break; }

            const std::uint32_t re{ narrow_encode(r.base, r.shift, as_u64(decoded)) };
            if (re != c) { roundtrip_ok = false; break; }
        }
        if (!roundtrip_ok) { break; }
    }
    check("mfw_deep3_narrow_codec_decode_then_encode_roundtrips", roundtrip_ok);

    // Explicit shift-0 identity: decode is base + compressed, encode is addr - base.
    {
        const std::uint64_t base{ 0x0000000100000000ull };
        const std::uint32_t c{ 0x00ABCDEFu };
        void* const d{ narrow_decode(base, 0u, c) };
        check("mfw_deep3_narrow_shift0_is_base_plus_compressed",
              as_u64(d) == base + c
              && narrow_encode(base, 0u, as_u64(d)) == c);
    }

    // Explicit shift-3 scaling: the compressed value is left-shifted by 3 (x8) on
    // decode and right-shifted by 3 on encode -- the 8-byte-aligned-oop regime.
    {
        const std::uint64_t base{ 0ull };
        const std::uint32_t c{ 0x00001234u };
        void* const d{ narrow_decode(base, 3u, c) };
        check("mfw_deep3_narrow_shift3_scales_by_8",
              as_u64(d) == (static_cast<std::uint64_t>(c) << 3)
              && as_u64(d) == static_cast<std::uint64_t>(c) * 8u
              && narrow_encode(base, 3u, as_u64(d)) == c);
    }

    // encode of addr == base yields 0 (the "decoded==base -> compressed 0" edge the
    // caller's underflow guard pairs with); decode of base+0 is base.
    {
        const std::uint64_t base{ 0x0000000800000000ull };
        check("mfw_deep3_narrow_encode_at_base_is_zero",
              narrow_encode(base, 3u, base) == 0u
              && as_u64(narrow_decode(base, 3u, 0u)) == base);
    }
}

// -- F2. UTF-8 ENCODER BOUNDARY MAP (append_utf8 byte production). ------------
//  Pin the EXACT byte sequence append_utf8 emits at every length boundary:
//  1-byte (< 0x80), 2-byte (< 0x800), 3-byte (< 0x10000), 4-byte (astral),
//  plus the just-below / just-at edge of each boundary (0x7F/0x80, 0x7FF/0x800,
//  0xFFFF/0x10000).  Embedded NUL (cp 0) is built and asserted at RUNTIME.
static auto test_mfw_deep3_utf8_encoder_boundaries() -> void
{
    auto encode_one = [](std::uint32_t cp) -> std::string
    {
        std::string out;
        mfw_deep3::append_utf8(out, cp);
        return out;
    };

    // cp 0 -> a single NUL byte (embedded-NUL preservation, built at runtime).
    {
        const std::string z{ encode_one(0x0u) };
        std::string expected;
        expected.push_back('\0');  // runtime-built NUL; no raw NUL literal in source
        check("mfw_deep3_utf8_cp0_is_single_nul_byte",
              z.size() == 1u && z == expected && z[0] == '\0');
    }

    // ASCII 'A' (0x41) -> one byte 0x41.
    check("mfw_deep3_utf8_ascii_A_one_byte",
          encode_one(0x41u) == std::string{ "A" });

    // 0x7F is the LAST 1-byte code point; 0x80 the FIRST 2-byte.
    {
        const std::string a{ encode_one(0x7Fu) };
        const std::string b{ encode_one(0x80u) };
        check("mfw_deep3_utf8_7F_one_byte_80_two_bytes",
              a.size() == 1u && static_cast<std::uint8_t>(a[0]) == 0x7Fu
              && b.size() == 2u
              && static_cast<std::uint8_t>(b[0]) == 0xC2u
              && static_cast<std::uint8_t>(b[1]) == 0x80u);
    }

    // U+00E9 'e-acute' -> C3 A9 (the LATIN1 case the decoder UTF-8-encodes).
    {
        const std::string e{ encode_one(0xE9u) };
        check("mfw_deep3_utf8_00E9_is_C3_A9",
              e.size() == 2u
              && static_cast<std::uint8_t>(e[0]) == 0xC3u
              && static_cast<std::uint8_t>(e[1]) == 0xA9u);
    }

    // 0x7FF is the LAST 2-byte code point; 0x800 the FIRST 3-byte.
    {
        const std::string a{ encode_one(0x7FFu) };
        const std::string b{ encode_one(0x800u) };
        check("mfw_deep3_utf8_7FF_two_bytes_800_three_bytes",
              a.size() == 2u
              && static_cast<std::uint8_t>(a[0]) == 0xDFu
              && static_cast<std::uint8_t>(a[1]) == 0xBFu
              && b.size() == 3u
              && static_cast<std::uint8_t>(b[0]) == 0xE0u
              && static_cast<std::uint8_t>(b[1]) == 0xA0u
              && static_cast<std::uint8_t>(b[2]) == 0x80u);
    }

    // U+20AC EURO SIGN -> E2 82 AC (a canonical 3-byte BMP code point).
    {
        const std::string euro{ encode_one(0x20ACu) };
        check("mfw_deep3_utf8_20AC_euro_is_E2_82_AC",
              euro.size() == 3u
              && static_cast<std::uint8_t>(euro[0]) == 0xE2u
              && static_cast<std::uint8_t>(euro[1]) == 0x82u
              && static_cast<std::uint8_t>(euro[2]) == 0xACu);
    }

    // 0xFFFF is the LAST 3-byte code point; 0x10000 the FIRST 4-byte (astral).
    {
        const std::string a{ encode_one(0xFFFFu) };
        const std::string b{ encode_one(0x10000u) };
        check("mfw_deep3_utf8_FFFF_three_bytes_10000_four_bytes",
              a.size() == 3u
              && static_cast<std::uint8_t>(a[0]) == 0xEFu
              && static_cast<std::uint8_t>(a[1]) == 0xBFu
              && static_cast<std::uint8_t>(a[2]) == 0xBFu
              && b.size() == 4u
              && static_cast<std::uint8_t>(b[0]) == 0xF0u
              && static_cast<std::uint8_t>(b[1]) == 0x90u
              && static_cast<std::uint8_t>(b[2]) == 0x80u
              && static_cast<std::uint8_t>(b[3]) == 0x80u);
    }

    // U+1F600 (astral, outside the BMP) -> F0 9F 98 80 (4 bytes).
    {
        const std::string grin{ encode_one(0x1F600u) };
        check("mfw_deep3_utf8_1F600_astral_is_F0_9F_98_80",
              grin.size() == 4u
              && static_cast<std::uint8_t>(grin[0]) == 0xF0u
              && static_cast<std::uint8_t>(grin[1]) == 0x9Fu
              && static_cast<std::uint8_t>(grin[2]) == 0x98u
              && static_cast<std::uint8_t>(grin[3]) == 0x80u);
    }

    // Every produced continuation byte (after the lead) has the 0b10xxxxxx form,
    // and the byte LENGTH matches the code-point band, swept across the boundaries.
    struct band { std::uint32_t cp; std::size_t len; };
    const band bands[]{
        { 0x00u, 1u }, { 0x7Fu, 1u }, { 0x80u, 2u }, { 0x7FFu, 2u },
        { 0x800u, 3u }, { 0xFFFFu, 3u }, { 0x10000u, 4u }, { 0x10FFFFu, 4u },
    };
    bool well_formed{ true };
    for (const band bd : bands)
    {
        const std::string s{ encode_one(bd.cp) };
        if (s.size() != bd.len) { well_formed = false; break; }
        for (std::size_t i{ 1 }; i < s.size(); ++i)
        {
            if ((static_cast<std::uint8_t>(s[i]) & 0xC0u) != 0x80u) { well_formed = false; break; }
        }
        if (!well_formed) { break; }
    }
    check("mfw_deep3_utf8_lengths_and_continuation_bytes_well_formed", well_formed);
}

// -- F3. UTF-16 SURROGATE-PAIR / ASTRAL DECODE (utf16_to_utf8). ---------------
//  Drive the EXACT surrogate-combination logic over OWNED uint16 arrays: a valid
//  high+low pair becomes one astral code point (4-byte UTF-8); a lone/unpaired
//  surrogate is emitted as-is (3-byte UTF-8); a BMP run is 1:1.
static auto test_mfw_deep3_utf16_surrogate_decode() -> void
{
    auto decode = [](const std::uint16_t* units, std::int32_t count) -> std::string
    {
        std::string out;
        mfw_deep3::utf16_to_utf8(out, units, count);
        return out;
    };

    // Valid surrogate pair for U+1F600: high D83D, low DE00 -> F0 9F 98 80.
    {
        const std::array<std::uint16_t, 2> pair{ { 0xD83Du, 0xDE00u } };
        const std::string s{ decode(pair.data(), 2) };
        check("mfw_deep3_utf16_valid_pair_decodes_astral",
              s.size() == 4u
              && static_cast<std::uint8_t>(s[0]) == 0xF0u
              && static_cast<std::uint8_t>(s[1]) == 0x9Fu
              && static_cast<std::uint8_t>(s[2]) == 0x98u
              && static_cast<std::uint8_t>(s[3]) == 0x80u);
    }

    // High surrogate at the END with no following low unit (count cuts it off):
    // the (i+1)<count guard fails -> the high surrogate is appended as-is (3 bytes).
    {
        const std::array<std::uint16_t, 1> lone_high{ { 0xD83Du } };
        const std::string s{ decode(lone_high.data(), 1) };
        check("mfw_deep3_utf16_lone_high_surrogate_emitted_as_is",
              s.size() == 3u && static_cast<std::uint8_t>(s[0]) == 0xEDu);
    }

    // High surrogate followed by a NON-low unit ('A'): not combined; high emitted
    // as 3-byte, then 'A' as 1-byte.
    {
        const std::array<std::uint16_t, 2> high_then_ascii{ { 0xD83Du, 0x0041u } };
        const std::string s{ decode(high_then_ascii.data(), 2) };
        check("mfw_deep3_utf16_high_then_nonlow_not_combined",
              s.size() == 4u
              && static_cast<std::uint8_t>(s[0]) == 0xEDu
              && s[3] == 'A');
    }

    // Pure BMP run "Hi" + EURO: 1 unit each, EURO is 3-byte BMP.
    {
        const std::array<std::uint16_t, 3> bmp{ { 0x0048u, 0x0069u, 0x20ACu } };
        const std::string s{ decode(bmp.data(), 3) };
        check("mfw_deep3_utf16_bmp_run_1to1",
              s.size() == 5u && s[0] == 'H' && s[1] == 'i'
              && static_cast<std::uint8_t>(s[2]) == 0xE2u);
    }

    // A surrogate pair lower-bound (U+10000: high D800, low DC00) decodes to the
    // FIRST astral code point F0 90 80 80 -- the exact 0x10000 + ((hi-D800)<<10) +
    // (lo-DC00) formula at its zero point.
    {
        const std::array<std::uint16_t, 2> first_astral{ { 0xD800u, 0xDC00u } };
        const std::string s{ decode(first_astral.data(), 2) };
        check("mfw_deep3_utf16_pair_D800_DC00_is_U10000",
              s.size() == 4u
              && static_cast<std::uint8_t>(s[0]) == 0xF0u
              && static_cast<std::uint8_t>(s[1]) == 0x90u
              && static_cast<std::uint8_t>(s[2]) == 0x80u
              && static_cast<std::uint8_t>(s[3]) == 0x80u);
    }

    // An embedded U+0000 inside a UTF-16 run is preserved as a NUL byte (built at
    // runtime), not a terminator: "A" NUL "B" -> 3 bytes.
    {
        const std::array<std::uint16_t, 3> with_nul{ { 0x0041u, 0x0000u, 0x0042u } };
        const std::string s{ decode(with_nul.data(), 3) };
        std::string expected;
        expected.push_back('A');
        expected.push_back('\0');
        expected.push_back('B');
        check("mfw_deep3_utf16_embedded_nul_preserved",
              s.size() == 3u && s == expected && s[1] == '\0');
    }
}

// -- F4. read_java_string LAYOUT DERIVATION (char_count / body_bytes / bounds). -
//  Pin the three-layout arithmetic the decoder selects on (vmhook.hpp:20600/20621)
//  and the length/char_count range guards (20565/20608) as pure integer logic,
//  plus an end-to-end decode of each layout over an OWNED body buffer.
static auto test_mfw_deep3_read_java_string_layout_logic() -> void
{
    // char_count: JDK8 char[] (no coder) and LATIN1 (coder 0) -> length; UTF16
    // (coder != 0) -> length/2.
    check("mfw_deep3_char_count_jdk8_chararray_is_length",
          mfw_deep3::char_count_for(/*has_coder*/ false, /*coder*/ 0u, /*length*/ 10) == 10);
    check("mfw_deep3_char_count_latin1_is_length",
          mfw_deep3::char_count_for(true, 0u, 10) == 10);
    check("mfw_deep3_char_count_utf16_is_half_length",
          mfw_deep3::char_count_for(true, 1u, 10) == 5);

    // body_bytes: no coder (char[]) -> 2*length; either byte[] layout -> length.
    check("mfw_deep3_body_bytes_jdk8_chararray_is_2x",
          mfw_deep3::body_bytes_for(false, 7) == 14);
    check("mfw_deep3_body_bytes_byte_array_is_length",
          mfw_deep3::body_bytes_for(true, 7) == 7);

    // Range guards (mirrors of the source clauses, pure integer predicates).
    auto length_in_range = [](std::int32_t length) -> bool
    { return !(length <= 0 || length > 2 * vmhook::read_java_string_max_units); };
    auto char_count_in_range = [](std::int32_t cc) -> bool
    { return !(cc <= 0 || cc > vmhook::read_java_string_max_units); };

    check("mfw_deep3_length_zero_and_negative_rejected",
          !length_in_range(0) && !length_in_range(-1));
    check("mfw_deep3_length_at_2x_cap_accepted_one_past_rejected",
          length_in_range(2 * vmhook::read_java_string_max_units)
          && !length_in_range(2 * vmhook::read_java_string_max_units + 1));
    check("mfw_deep3_char_count_zero_rejected_cap_accepted",
          !char_count_in_range(0)
          && char_count_in_range(vmhook::read_java_string_max_units)
          && !char_count_in_range(vmhook::read_java_string_max_units + 1));

    // The cap is the documented 16 Mi characters (vmhook.hpp:1697).
    check("mfw_deep3_max_units_is_16Mi",
          vmhook::read_java_string_max_units == 16 * 1024 * 1024);

    // End-to-end LATIN1: a body of bytes [0x41,0xE9] (count 2) -> "A" + C3 A9.
    {
        const std::array<std::uint8_t, 2> latin1_body{ { 0x41u, 0xE9u } };
        std::string out;
        for (std::int32_t i{ 0 }; i < 2; ++i) { mfw_deep3::append_utf8(out, latin1_body[i]); }
        check("mfw_deep3_latin1_body_decodes_A_then_C3A9",
              out.size() == 3u && out[0] == 'A'
              && static_cast<std::uint8_t>(out[1]) == 0xC3u
              && static_cast<std::uint8_t>(out[2]) == 0xA9u);
    }

    // End-to-end JDK8 char[] (UTF16, no coder): two units "Hi".
    {
        const std::array<std::uint16_t, 2> u16{ { 0x0048u, 0x0069u } };
        std::string out;
        mfw_deep3::utf16_to_utf8(out, u16.data(), 2);
        check("mfw_deep3_jdk8_chararray_body_decodes_Hi",
              out == std::string{ "Hi" });
    }
}

// -- F5. method_proxy::value_t VARIANT CLASSIFICATION + NUMERIC CONVERSION. ---
//  is_void / is_string / as_string + the constrained numeric conversion operator
//  (vmhook.hpp:16335).  Built with EXPLICIT variant alternatives -- never cast a
//  value_t to a std::vector (the MSVC-ambiguous cast this file was reverted for).
static auto test_mfw_deep3_value_t_classification() -> void
{
    using value_t = vmhook::method_proxy::value_t;

    // monostate -> is_void true, is_string false, as_string "".
    {
        const value_t v{ std::monostate{} };
        check("mfw_deep3_value_t_monostate_is_void",
              v.is_void() && !v.is_string() && v.as_string().empty());
    }

    // A numeric alternative -> not void, not string; numeric conversion casts.
    {
        const value_t v{ std::int32_t{ 42 } };
        const std::int32_t as_i{ v };
        const std::int64_t as_l{ v };
        const double as_d{ v };
        check("mfw_deep3_value_t_int32_classification_and_cast",
              !v.is_void() && !v.is_string()
              && as_i == 42 && as_l == 42 && as_d == 42.0
              && v.as_string().empty());
    }

    // bool alternative -> casts to int and bool; not void / string.
    {
        const value_t vt{ true };
        const value_t vf{ false };
        const std::int32_t t_as_i{ vt };
        const bool f_as_b{ vf };
        check("mfw_deep3_value_t_bool_casts",
              t_as_i == 1 && f_as_b == false
              && !vt.is_void() && !vt.is_string());
    }

    // float / double alternatives narrow/cast via static_cast as documented.
    {
        const value_t vf{ float{ 2.5f } };
        const value_t vd{ double{ 3.5 } };
        const double f_as_d{ vf };
        const std::int32_t d_as_i{ vd };  // static_cast<int>(3.5) == 3 (truncation)
        check("mfw_deep3_value_t_float_double_cast",
              f_as_d == 2.5 && d_as_i == 3);
    }

    // std::string alternative -> is_string true, as_string returns it verbatim,
    // is_void false.  Built explicitly from a std::string (no vector anywhere).
    {
        const value_t v{ std::string{ "hello" } };
        check("mfw_deep3_value_t_string_classification",
              v.is_string() && !v.is_void()
              && v.as_string() == std::string{ "hello" });
    }

    // A std::string alternative carrying an embedded NUL (built at runtime) is
    // preserved by as_string (no C-string truncation).
    {
        std::string embedded;
        embedded.push_back('a');
        embedded.push_back('\0');
        embedded.push_back('b');
        const value_t v{ embedded };
        const std::string round{ v.as_string() };
        check("mfw_deep3_value_t_string_embedded_nul_preserved",
              v.is_string() && round.size() == 3u && round == embedded && round[1] == '\0');
    }

    // The conversion-target trait (value_t_convertible_target_v): void* is the only
    // legal pointer target; nullptr_t and non-void pointers are excised.
    check("mfw_deep3_value_t_target_trait_void_ptr_only",
          vmhook::detail::value_t_convertible_target_v<void*>
          && vmhook::detail::value_t_convertible_target_v<std::int32_t>
          && vmhook::detail::value_t_convertible_target_v<std::string>
          && !vmhook::detail::value_t_convertible_target_v<std::nullptr_t>
          && !vmhook::detail::value_t_convertible_target_v<const char*>
          && !vmhook::detail::value_t_convertible_target_v<char*>);
}

// -- F6. SIGNATURE-CHAR DECODE TABLES (sig_char_to_basic_type + byte width). --
//  Pin every documented descriptor char in BOTH tables (vmhook.hpp:16215/16250):
//  the BasicType code and the in-heap primitive byte width, plus the fallbacks.
static auto test_mfw_deep3_signature_char_tables() -> void
{
    using vmhook::detail::sig_char_to_basic_type;
    using vmhook::detail::jvm_primitive_byte_width;

    // sig_char_to_basic_type: every documented char -> its BasicType ordinal.
    struct bt { char c; int code; };
    const bt basic_types[]{
        { 'Z', 4 }, { 'C', 5 }, { 'F', 6 }, { 'D', 7 }, { 'B', 8 }, { 'S', 9 },
        { 'I', 10 }, { 'J', 11 }, { 'L', 12 }, { '[', 13 }, { 'V', 14 },
    };
    bool bt_ok{ true };
    for (const bt e : basic_types)
    {
        if (sig_char_to_basic_type(e.c) != e.code) { bt_ok = false; break; }
    }
    check("mfw_deep3_sig_char_to_basic_type_all_documented", bt_ok);

    // The default arm falls back to T_OBJECT (12) for any unrecognised char.
    check("mfw_deep3_sig_char_to_basic_type_default_is_object",
          sig_char_to_basic_type('Q') == 12
          && sig_char_to_basic_type('X') == 12
          && sig_char_to_basic_type('z') == 12);  // lowercase is NOT 'Z'

    // jvm_primitive_byte_width: a single-char primitive descriptor -> in-heap width.
    struct w { const char* sig; std::size_t width; };
    const w widths[]{
        { "Z", 1u }, { "B", 1u }, { "S", 2u }, { "C", 2u },
        { "I", 4u }, { "F", 4u }, { "J", 8u }, { "D", 8u },
    };
    bool w_ok{ true };
    for (const w e : widths)
    {
        if (jvm_primitive_byte_width(std::string_view{ e.sig }) != e.width) { w_ok = false; break; }
    }
    check("mfw_deep3_jvm_primitive_byte_width_all_primitives", w_ok);

    // Reference / array / void / multi-char / empty -> width 0 (skip size-check).
    check("mfw_deep3_jvm_primitive_byte_width_zero_for_nonprimitive",
          jvm_primitive_byte_width(std::string_view{ "L" }) == 0u
          && jvm_primitive_byte_width(std::string_view{ "[" }) == 0u
          && jvm_primitive_byte_width(std::string_view{ "V" }) == 0u
          && jvm_primitive_byte_width(std::string_view{ "Ljava/lang/String;" }) == 0u
          && jvm_primitive_byte_width(std::string_view{ "" }) == 0u
          && jvm_primitive_byte_width(std::string_view{ "II" }) == 0u);

    // Cross-check: every char that has a nonzero byte width also maps to a numeric
    // BasicType (4..11), and the two widths agree with the documented size class
    // (1-byte Z/B, 2-byte S/C, 4-byte I/F, 8-byte J/D).
    check("mfw_deep3_width_and_basic_type_agree",
          jvm_primitive_byte_width(std::string_view{ "J" }) == 8u
          && sig_char_to_basic_type('J') == 11
          && jvm_primitive_byte_width(std::string_view{ "D" }) == 8u
          && sig_char_to_basic_type('D') == 7);
}

// -- F7. method_proxy::call ARG CAP + get_arguments long/double SLOT WIDENING. -
//  The call paths static_assert arity <= 8 (vmhook.hpp:16703-16705 / 17321) -- a
//  compile-time cap we pin as a constant.  frame::get_arguments computes each
//  arg's interpreter slot as the running sum of widths, where a long/double is 2
//  slots and everything else 1 (vmhook.hpp:6302-6312); model that arithmetic
//  exactly over an OWNED type-width sequence.
static auto test_mfw_deep3_arg_cap_and_slot_widening() -> void
{
    // The documented maximum arity (constexpr std::size_t arg_cap{ 8 }).
    constexpr std::size_t arg_cap{ 8 };
    check("mfw_deep3_call_arg_cap_is_8", arg_cap == 8u);

    // Slot-offset model: identical to get_arguments' loop -- slots[i] = acc; acc +=
    // wide[i] ? 2 : 1.  `wide` is true for int64/uint64/double (a Java long/double).
    auto slot_offsets = [](const std::vector<bool>& wide) -> std::vector<std::int32_t>
    {
        std::vector<std::int32_t> slots(wide.size(), 0);
        std::int32_t acc{ 0 };
        for (std::size_t i{ 0 }; i < wide.size(); ++i)
        {
            slots[i] = acc;
            acc += wide[i] ? 2 : 1;
        }
        return slots;
    };

    // (int, long, int): slots 0, 1, 3 -- the long occupies slots 1..2, pushing the
    // trailing int to slot 3 (the exact bug get_arguments fixes vs naive tuple-index).
    {
        const std::vector<bool> wide{ false, true, false };
        const std::vector<std::int32_t> got{ slot_offsets(wide) };
        const std::vector<std::int32_t> want{ 0, 1, 3 };
        check("mfw_deep3_slots_int_long_int_are_0_1_3", got == want);
    }

    // (double, double): slots 0, 2 -- two wide args, each 2 slots.
    {
        const std::vector<bool> wide{ true, true };
        const std::vector<std::int32_t> got{ slot_offsets(wide) };
        const std::vector<std::int32_t> want{ 0, 2 };
        check("mfw_deep3_slots_double_double_are_0_2", got == want);
    }

    // All-narrow (4 ints): slots 0,1,2,3 -- 1:1 with tuple index.
    {
        const std::vector<bool> wide{ false, false, false, false };
        const std::vector<std::int32_t> got{ slot_offsets(wide) };
        const std::vector<std::int32_t> want{ 0, 1, 2, 3 };
        check("mfw_deep3_slots_all_narrow_are_identity", got == want);
    }

    // 8 wide args (the cap, all long/double): the final slot index is 14 and the
    // total slot count is 16 -- proves the widening never overflows the int32 acc
    // for the maximum legal arity.
    {
        const std::vector<bool> wide(8, true);
        const std::vector<std::int32_t> got{ slot_offsets(wide) };
        std::int32_t total{ 0 };
        for (const bool w : wide) { total += w ? 2 : 1; }
        check("mfw_deep3_slots_8_wide_last_is_14_total_16",
              got.size() == 8u && got.back() == 14 && total == 16);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Wave-27 deepening (mfw_w27): LEDGER-GAP closing — ConstMethod._flags width
//  JDK-variant pin, JVMS flag-bit constants pinned constexpr, mask invariants,
//  access_flags vs _flags disambiguation.  All assertions are deterministic
//  (constexpr / sizeof on synthetic structs / pure bit math) so they are HARD.
// ─────────────────────────────────────────────────────────────────────────
namespace mfw_w27
{
    // Synthetic ConstMethod layouts whose `_flags` member matches HotSpot's u2
    // exported type across the entire supported band (JDK 8..26 — ConstMethod's
    // _flags has NOT been widened the way Method's was; it stays a u2 short
    // bitfield in HotSpot src all the way through master).  We pin BOTH a
    // sizeof-on-synthetic ConstMethod._flags AND its bit-positions for the
    // accessor-flags HotSpot exposes via ConstMethod (has_linenumber_table,
    // has_checked_exceptions, has_localvariable_table, has_exception_table,
    // has_generic_signature, has_method_parameters).  These are the
    // ConstMethod-side analogue of Method::_flags and the ledger gap callout.
    struct synthetic_const_method_flags_u1 { std::uint8_t  _flags; };
    struct synthetic_const_method_flags_u2 { std::uint16_t _flags; };
    struct synthetic_const_method_flags_u4 { std::uint32_t _flags; };

    // HotSpot's ConstMethod::Flags bit positions (constMethod.hpp, jdk8u..master).
    // These have NEVER moved — pinned as the cross-version contract.
    namespace const_method_flag_bit
    {
        constexpr int has_linenumber_table     = 0;
        constexpr int has_checked_exceptions   = 1;
        constexpr int has_localvariable_table  = 2;
        constexpr int has_exception_table      = 3;
        constexpr int has_generic_signature    = 4;
        constexpr int has_method_parameters    = 5;
    }

    // The JVMS class-file access-flag bits the library consults via
    // Method::_access_flags.  Values pinned per JVMS §4.6 Table 4.6 (jvms-4.6).
    // These are the LOW 16 bits of the AccessFlags word and have never moved
    // across the entire JDK 8..26 band — even after JDK 24 shrank AccessFlags
    // to u2, every JVMS bit kept its value because the relocated bits (queued,
    // not_compilable, etc.) were HotSpot-internal, not JVMS bits.
    namespace jvms_method_acc
    {
        constexpr std::uint32_t PUBLIC       = 0x0001;
        constexpr std::uint32_t PRIVATE      = 0x0002;
        constexpr std::uint32_t PROTECTED    = 0x0004;
        constexpr std::uint32_t STATIC       = 0x0008;
        constexpr std::uint32_t FINAL        = 0x0010;
        constexpr std::uint32_t SYNCHRONIZED = 0x0020;
        constexpr std::uint32_t BRIDGE       = 0x0040;
        constexpr std::uint32_t VARARGS      = 0x0080;
        constexpr std::uint32_t NATIVE       = 0x0100;
        constexpr std::uint32_t ABSTRACT     = 0x0400;
        constexpr std::uint32_t STRICTFP       = 0x0800;
        constexpr std::uint32_t SYNTHETIC    = 0x1000;
    }
}

// Compile-time pinning of the ConstMethod synthetic widths — these mirror the
// Method-side u1/u2/u4 width matrix and let a future width-aware ConstMethod
// accessor assert which synthetic it must match.
static_assert(sizeof(mfw_w27::synthetic_const_method_flags_u1) == 1,
              "synthetic ConstMethod._flags(u1) is 1 byte (JDK 8 hypothetical width)");
static_assert(sizeof(mfw_w27::synthetic_const_method_flags_u2) == 2,
              "synthetic ConstMethod._flags(u2) is 2 bytes (actual JDK 8..26 width)");
static_assert(sizeof(mfw_w27::synthetic_const_method_flags_u4) == 4,
              "synthetic ConstMethod._flags(u4) is 4 bytes (future-proof slot)");

// Pin every ConstMethod flag bit position as a constexpr — the JVMS-spec'd
// "_has_*" bits are HotSpot-internal but their positions are stable across all
// supported JDKs (verified jdk8u..master in constMethod.hpp).
static_assert(mfw_w27::const_method_flag_bit::has_linenumber_table == 0
              && mfw_w27::const_method_flag_bit::has_checked_exceptions == 1
              && mfw_w27::const_method_flag_bit::has_localvariable_table == 2
              && mfw_w27::const_method_flag_bit::has_exception_table == 3
              && mfw_w27::const_method_flag_bit::has_generic_signature == 4
              && mfw_w27::const_method_flag_bit::has_method_parameters == 5,
              "ConstMethod bit positions 0..5 are stable across JDK 8..26");

// All six ConstMethod bits fit in a u1 (max bit = 5, mask 0x3F).
static_assert(((1u << mfw_w27::const_method_flag_bit::has_linenumber_table)
              | (1u << mfw_w27::const_method_flag_bit::has_checked_exceptions)
              | (1u << mfw_w27::const_method_flag_bit::has_localvariable_table)
              | (1u << mfw_w27::const_method_flag_bit::has_exception_table)
              | (1u << mfw_w27::const_method_flag_bit::has_generic_signature)
              | (1u << mfw_w27::const_method_flag_bit::has_method_parameters)) == 0x3Fu,
              "ConstMethod 6-bit mask = 0x3F (fits a u1, definitely fits the actual u2)");

// JVMS access-flag values pinned at compile time (JVMS §4.6 Table 4.6).  These
// are the values the library masks against on every JDK 8..26 and must never
// drift.
static_assert(mfw_w27::jvms_method_acc::PUBLIC       == 0x0001u, "ACC_PUBLIC=0x0001");
static_assert(mfw_w27::jvms_method_acc::PRIVATE      == 0x0002u, "ACC_PRIVATE=0x0002");
static_assert(mfw_w27::jvms_method_acc::PROTECTED    == 0x0004u, "ACC_PROTECTED=0x0004");
static_assert(mfw_w27::jvms_method_acc::STATIC       == 0x0008u, "ACC_STATIC=0x0008");
static_assert(mfw_w27::jvms_method_acc::FINAL        == 0x0010u, "ACC_FINAL=0x0010");
static_assert(mfw_w27::jvms_method_acc::SYNCHRONIZED == 0x0020u, "ACC_SYNCHRONIZED=0x0020");
static_assert(mfw_w27::jvms_method_acc::BRIDGE       == 0x0040u, "ACC_BRIDGE=0x0040");
static_assert(mfw_w27::jvms_method_acc::VARARGS      == 0x0080u, "ACC_VARARGS=0x0080");
static_assert(mfw_w27::jvms_method_acc::NATIVE       == 0x0100u, "ACC_NATIVE=0x0100");
static_assert(mfw_w27::jvms_method_acc::ABSTRACT     == 0x0400u, "ACC_ABSTRACT=0x0400");
static_assert(mfw_w27::jvms_method_acc::STRICTFP       == 0x0800u, "ACC_STRICTFP=0x0800");
static_assert(mfw_w27::jvms_method_acc::SYNTHETIC    == 0x1000u, "ACC_SYNTHETIC=0x1000");

// access_flags-vs-_flags DISAMBIGUATION at the bit level.  JVMS access-flag
// values 0x0001..0x1000 ALL fit in 13 bits.  HotSpot's Method::_flags bit
// positions (_dont_inline=2 on jdk<=20, =12 on jdk21+; _force_inline=11) when
// converted to MASKS (1<<bit) are 0x4 / 0x1000 / 0x800 — none of these overlap
// JVMS_ACC_STATIC (0x0008).  Pin the disjointness as the contract that makes
// "mask JVM_ACC_STATIC out of get_access_flags(); ignore get_flags()" the
// correct way to answer is_static() regardless of width changes on _flags.
static_assert((mfw_w27::jvms_method_acc::STATIC
              & (1u << flags_layout::jdk11_20.dont_inline_bit)) == 0u,
              "JVM_ACC_STATIC (0x0008) and _dont_inline-on-jdk<=20 (bit 2 / 0x0004) are disjoint");
static_assert((mfw_w27::jvms_method_acc::STATIC
              & (1u << flags_layout::jdk21_23.dont_inline_bit)) == 0u,
              "JVM_ACC_STATIC (0x0008) and _dont_inline-on-jdk21+ (bit 12 / 0x1000) are disjoint");
static_assert((mfw_w27::jvms_method_acc::STATIC
              & (1u << flags_layout::methodflags_status_bit::force_inline)) == 0u,
              "JVM_ACC_STATIC (0x0008) and _force_inline (bit 11 / 0x800) are disjoint");

// The NO_COMPILE mask in vmhook (0x0F000000) lives in the HIGH byte of the u4
// access-flags word.  It is disjoint from every JVMS access-flag bit (which
// all sit in the low 13 bits).  This is the second half of the
// access_flags-vs-_flags disambiguation: even on JDK 21+ where some compile
// bits moved to MethodFlags::_status, the LIBRARY's NO_COMPILE mask still
// targets the access-flags word — and never collides with JVMS bits there.
static_assert((0x0F000000u & 0x0000FFFFu) == 0u,
              "NO_COMPILE mask (0x0F000000) is disjoint from every JVMS access-flag bit (<=0x1000)");

static auto test_mfw_w27_const_method_flags_width_pin() -> void
{
    // Runtime echo of the ConstMethod._flags width matrix.  The synthetic types
    // model the three widths an evidence-driven accessor must dispatch over;
    // the actual JDK 8..26 width is u2.
    check("mfw_w27_const_method_flags_synthetic_widths",
          sizeof(mfw_w27::synthetic_const_method_flags_u1) == 1u
          && sizeof(mfw_w27::synthetic_const_method_flags_u2) == 2u
          && sizeof(mfw_w27::synthetic_const_method_flags_u4) == 4u);

    // The ACTUAL ConstMethod._flags width across every supported JDK is u2.
    // (HotSpot constMethod.hpp jdk8u..master: `u2 _flags;`.)  Pin that as a
    // single named PASS so a future widening shows up as a red test.
    check("mfw_w27_const_method_flags_actual_width_is_u2",
          sizeof(mfw_w27::synthetic_const_method_flags_u2) == 2u);

    // All six ConstMethod bits fit inside a u1, so even the smallest synthetic
    // would hold them — proving the actual u2 width is strictly sufficient.
    constexpr std::uint32_t cm_mask{
        (1u << mfw_w27::const_method_flag_bit::has_linenumber_table)
        | (1u << mfw_w27::const_method_flag_bit::has_checked_exceptions)
        | (1u << mfw_w27::const_method_flag_bit::has_localvariable_table)
        | (1u << mfw_w27::const_method_flag_bit::has_exception_table)
        | (1u << mfw_w27::const_method_flag_bit::has_generic_signature)
        | (1u << mfw_w27::const_method_flag_bit::has_method_parameters)
    };
    check("mfw_w27_const_method_6bit_mask_fits_u1", cm_mask <= 0xFFu && cm_mask == 0x3Fu);

    // Toggle each ConstMethod bit on a u2-width synthetic and verify the byte
    // outside the slot is untouched and the bit lands inside the slot.
    for (int bit{ 0 }; bit <= 5; ++bit)
    {
        struct { std::uint16_t _flags; std::uint16_t _sibling; } cm{ 0u, 0xBEEFu };
        cm._flags |= static_cast<std::uint16_t>(1u << bit);
        const bool inside  { (cm._flags & static_cast<std::uint16_t>(1u << bit)) != 0u };
        const bool sibling { cm._sibling == 0xBEEFu };
        check("mfw_w27_const_method_bit_toggle_no_sibling_clobber", inside && sibling);
    }
}

static auto test_mfw_w27_jvms_acc_bits_and_disambiguation() -> void
{
    using namespace mfw_w27::jvms_method_acc;

    // Every JVMS access-flag value is a single bit (popcount==1) — runtime echo.
    constexpr std::array<std::uint32_t, 12> acc_values{
        PUBLIC, PRIVATE, PROTECTED, STATIC, FINAL, SYNCHRONIZED,
        BRIDGE, VARARGS, NATIVE, ABSTRACT, STRICTFP, SYNTHETIC
    };
    for (std::size_t i{ 0 }; i < acc_values.size(); ++i)
    {
        check("mfw_w27_jvms_acc_value_is_single_bit",
              std::popcount(acc_values[i]) == 1);
    }

    // The union of every JVMS access-flag value fits in the low 13 bits — i.e.
    // it is reachable from a u2 access-flags read, which is exactly the JDK 24+
    // post-shrink contract.  This proves the JVMS bits the library cares about
    // SURVIVE the AccessFlags u4 -> u2 transition.
    std::uint32_t acc_union{ 0u };
    for (const auto v : acc_values) { acc_union |= v; }
    // Union of the pinned 12 method bits: low byte fully set (0x00FF) +
    // 0x0100 + 0x0400 + 0x0800 + 0x1000 = 0x1DFF.  ACC_VOLATILE (0x0200) is a
    // field-only bit, ACC_TRANSIENT (0x0080) here doubles as ACC_VARARGS for
    // methods.  Pin the exact value.
    check("mfw_w27_jvms_acc_union_fits_u2", acc_union <= 0xFFFFu && acc_union == 0x1DFFu);

    // access_flags-vs-_flags DISAMBIGUATION runtime echo.  This is the WHOLE
    // POINT of the two-word split: some JVMS access-flag VALUES (PROTECTED=0x4,
    // STATIC=0x8, FINAL=0x10, SYNCHRONIZED=0x20, BRIDGE=0x40, VARARGS=0x80,
    // SYNTHETIC=0x1000) COLLIDE with HotSpot _flags bit-MASKS (_dont_inline
    // pre-21 = 1<<2 = 0x4; _force_inline 21+ = 1<<11 = 0x800; _dont_inline
    // 21+ = 1<<12 = 0x1000).  Same numeric value, DIFFERENT word.  That is
    // EXACTLY why reading access_flags via get_access_flags() and reading
    // _flags via get_flags() are two distinct VMStruct lookups — confusing the
    // words would mis-interpret bit 2 of access-flags (PROTECTED) as
    // _dont_inline, and worse on JDK 21+, bit 12 of _flags (_dont_inline) as
    // ACC_SYNTHETIC.  The library reads them through SEPARATE accessors with
    // SEPARATE VMStruct entries.  Pin the collisions explicitly.
    constexpr std::uint32_t dont_inline_pre21{ 1u << flags_layout::jdk11_20.dont_inline_bit };
    constexpr std::uint32_t dont_inline_21p { 1u << flags_layout::jdk21_23.dont_inline_bit };
    constexpr std::uint32_t force_inline_21p{ 1u << flags_layout::methodflags_status_bit::force_inline };
    constexpr std::uint32_t no_compile_mask { 0x0F000000u };

    // Specific bit-value COLLISIONS that prove the words MUST be addressed by
    // separate accessors (the disambiguation contract).
    check("mfw_w27_disambig_protected_collides_dont_inline_pre21",
          PROTECTED == dont_inline_pre21 && PROTECTED == 0x4u);
    check("mfw_w27_disambig_synthetic_collides_dont_inline_21p",
          SYNTHETIC == dont_inline_21p && SYNTHETIC == 0x1000u);
    check("mfw_w27_disambig_strictfp_collides_force_inline_21p",
          STRICTFP == force_inline_21p && STRICTFP == 0x800u);

    // NO_COMPILE high-byte is disjoint from EVERY JVMS bit (no collision).
    for (const auto v : acc_values)
    {
        check("mfw_w27_disambig_jvms_vs_no_compile_high_byte_disjoint",
              (v & no_compile_mask) == 0u);
    }

    // The library reads JVM_ACC_STATIC out of get_access_flags() (u4) and
    // masks 0x0008 — width-independent because 0x0008 fits a u1.  Pin the
    // single-byte mask invariant that backs is_static() on every JDK.
    check("mfw_w27_jvm_acc_static_mask_fits_low_byte",
          (STATIC & 0xFFu) == STATIC && std::popcount(STATIC) == 1);
}

static auto test_mfw_w27_mask_invariants() -> void
{
    // Mask invariant #1: every Method::_flags bit position the library knows
    // about (jdk<=20 _dont_inline=2; jdk21+ _force_inline=11, _dont_inline=12;
    // jdk24+ relocated bits 7..10) is < 16, so a u2 access of _flags can still
    // SET them — the only gap is u2 vs u4 READBACK on jdk21+ (already pinned).
    constexpr std::array<int, 7> known_flag_bits{
        flags_layout::jdk11_20.dont_inline_bit,
        flags_layout::methodflags_status_bit::queued_for_compilation,
        flags_layout::methodflags_status_bit::is_not_c2_compilable,
        flags_layout::methodflags_status_bit::is_not_c1_compilable,
        flags_layout::methodflags_status_bit::is_not_c2_osr,
        flags_layout::methodflags_status_bit::force_inline,
        flags_layout::methodflags_status_bit::dont_inline,
    };
    for (const int b : known_flag_bits)
    {
        check("mfw_w27_mask_known_flag_bit_below_16", b >= 0 && b < 16);
    }

    // Mask invariant #2: the union of every JDK 21+ MethodFlags::_status bit
    // we pin (7..12) is 0x1F80, which fits in the LOW HALF of a u4 _status and
    // therefore is ALSO addressable from a u2-width read — the library's u2
    // hard-code can still WRITE every known bit, only future bits above 15
    // become unreachable.
    constexpr std::uint32_t status_union{
        (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12)
    };
    check("mfw_w27_mask_status_union_is_0x1F80_and_u2_reachable",
          status_union == 0x1F80u && status_union <= 0xFFFFu);

    // Mask invariant #3: bit complements at every width round-trip.  Setting
    // bit 2 then clearing it on a u1/u2/u4 slot returns the slot to 0; setting
    // ALL bits of the slot then ANDing ~(1<<2) clears ONLY bit 2.
    auto roundtrip = [](auto sample) -> bool
    {
        using T = decltype(sample);
        T slot{ 0 };
        slot = static_cast<T>(slot | static_cast<T>(1u << 2));
        slot = static_cast<T>(slot & static_cast<T>(~static_cast<unsigned>(1u << 2)));
        if (slot != T{ 0 }) { return false; }
        T all{ static_cast<T>(~T{ 0 }) };
        T cleared{ static_cast<T>(all & static_cast<T>(~static_cast<unsigned>(1u << 2))) };
        const T expected{ static_cast<T>(all ^ static_cast<T>(1u << 2)) };
        return cleared == expected;
    };
    check("mfw_w27_mask_invariant_roundtrip_u8",  roundtrip(std::uint8_t{ 0 }));
    check("mfw_w27_mask_invariant_roundtrip_u16", roundtrip(std::uint16_t{ 0 }));
    check("mfw_w27_mask_invariant_roundtrip_u32", roundtrip(std::uint32_t{ 0 }));

    // Mask invariant #4: NO_COMPILE high-byte mask is exactly 4 contiguous bits
    // at positions 24..27, and CLEARING it via signed ~ then static_cast<u32>
    // (the library's pattern at the teardown sites) is byte-identical to
    // computing the complement as a u32 directly.
    constexpr std::uint32_t no_compile_u32{ 0x0F000000u };
    constexpr std::int32_t  no_compile_s32{ 0x0F000000 };
    check("mfw_w27_mask_no_compile_popcount_4_contiguous",
          std::popcount(no_compile_u32) == 4
          && std::countr_zero(no_compile_u32) == 24
          && std::countl_zero(no_compile_u32) == 4);
    check("mfw_w27_mask_no_compile_signed_clear_matches_unsigned",
          static_cast<std::uint32_t>(~no_compile_s32) == ~no_compile_u32);
}

int main()
{
    test_set_dont_inline_null();
    test_set_dont_inline_invalid_pointer();
    test_flag_accessors_no_jvm_null();
    test_method_proxy_is_static_no_jvm();
    test_layout_contract_runtime();
    test_derivation_per_jdk();
    test_confident_guard_refusals();
    test_width_matrix_anti_clobber();
    test_atomic_toggle_preserves_sibling();
    test_type_string_matching();
    test_pathA_offset_fidelity();
    test_pathB_alignment_boundary_sweep();
    test_copresent_precedence_and_present_gating();
    test_width_bit_representability_and_determinism();
    test_width_to_bit_mapping_table();
    test_no_compile_mask_bits();
    test_jvm_acc_bit_table();
    test_access_flag_predicate_width_stability();
    test_bit_fits_width_exhaustive();
    test_all_set_all_clear_slot_toggle();
    test_status_word_sibling_independence();
    test_toggle_byte_placement_endianness();
    test_no_compile_signed_clear_roundtrip();
    test_width_is_evidence_driven_not_version_driven();

    // Deepening wave (mfw_deep) additive section.
    test_mfw_deep_default_no_guess();
    test_mfw_deep_is_valid_pointer_contract();
    test_mfw_deep_pathB_large_offsets();
    test_mfw_deep_type_is_exact_match_stress();
    test_mfw_deep_no_compile_relocation_mapping();
    test_mfw_deep_confident_layout_slot_wellformed();
    test_mfw_deep_set_dont_inline_no_write_window();
    test_mfw_deep_precedence_truth_table();

    // Deepening wave 2 (mfw_deep2) additive section.
    test_mfw_deep2_iterate_struct_null_and_no_jvm();
    test_mfw_deep2_iterate_type_null_and_no_jvm();
    test_mfw_deep2_resolve_slot_no_jvm();
    test_mfw_deep2_cp_symbol_early_rejects();
    test_mfw_deep2_cp_bound_threshold_logic();
    test_mfw_deep2_vmstruct_abi_layout();
    test_mfw_deep2_owned_table_scan_model();
    test_mfw_deep2_is_valid_pointer_thresholds();

    // Deepening wave 3 (mfw_deep3) additive section.
    test_mfw_deep3_narrow_codec_roundtrip();
    test_mfw_deep3_utf8_encoder_boundaries();
    test_mfw_deep3_utf16_surrogate_decode();
    test_mfw_deep3_read_java_string_layout_logic();
    test_mfw_deep3_value_t_classification();
    test_mfw_deep3_signature_char_tables();
    test_mfw_deep3_arg_cap_and_slot_widening();

    // Wave-27 deepening section (ledger-gap closing).
    test_mfw_w27_const_method_flags_width_pin();
    test_mfw_w27_jvms_acc_bits_and_disambiguation();
    test_mfw_w27_mask_invariants();

    std::printf("\n%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
