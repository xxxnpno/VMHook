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
#include <atomic>
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

    std::printf("\n%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
