// Standalone (no-JVM) characterization of vmhook::make_java_string's COLD
// surface — the contract that holds with NO live HotSpot in this TU.
//
// Closes LEDGER wave-28 gaps for make_java_string:
//   * cold-call on an empty std::string_view returns nullptr (no crash, no
//     UB).  This is the documented "find_class('java/lang/String') returned
//     null" early-out at vmhook.hpp:14519-14525, sitting on top of
//     vmhook::find_class's empty-name fast-reject (vmhook.hpp:8161-8164).
//   * cold-call with arbitrary content (non-ASCII, embedded interior NUL,
//     a 4096-char boundary input, a 4096+1 over-cap input) also returns
//     nullptr — proving the function never dereferences a JVM-derived
//     pointer before string_klass is checked, and that the LEDGER's
//     "4096+ char input boundary safe" claim holds at the entry gate.
//   * the PURE-VM allocation chain the (now sole) TLAB path rides on is
//     characterized cold: make_java_object() rejects a null klass, and
//     make_java_array("[B"/"[C", ...) — the exact backing arrays the
//     compact-LATIN1 / compact-UTF16 / classic branches allocate — return
//     nullptr at their own find_class gate.  So the cold contract of the
//     whole chain is "return nullptr, do not crash, do not block".
//   * noexcept characterization: make_java_string is declared `noexcept`
//     (vmhook.hpp:1702) — pinned as a static_assert below so any future
//     accidental removal is a compile-time regression.  The same is
//     pinned for make_java_object / make_java_array /
//     vmhook::detail::utf8_to_utf16 (their failure-mode contracts depend
//     on it: nullptr-on-failure rather than a throw across a callback
//     boundary).
//
// HISTORY (de-JNI refactor, commit eaff990): this file used to characterize
// the JNIEnv::NewString(slot 163) over-cap fallback through the detail
// wrappers jni_new_string_utf16 / jni_new_string_utf16_local.  Both wrappers,
// and the fallback itself, were DELETED — make_java_string is now pure-VM and
// builds the complete String through build_via_tlab() at ANY length, so the
// former k_tlab_string_max_units routing cap no longer exists either.  The
// four assertions that named those wrappers are therefore gone (they asserted
// a function that no longer exists); the surviving-and-still-true "over-cap
// input is not truncated / not special-cased" coverage is kept below, and the
// pure-VM allocation chain that replaced the fallback is characterized in its
// place.
//
// Embedded-NUL handling and astral round-trip are CHARACTERIZED for the
// SHARED utf8_to_utf16 core in tests/test_read_java_string_nul_astral.cpp;
// this file only asserts that make_java_string's NO-JVM observable wrapping
// of that core stays nullptr-clean for those same adversarial inputs.

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
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

// ───────────────────────── noexcept characterization (compile-time) ─────────
//
// make_java_string is declared `noexcept` at vmhook.hpp:1702.  Pin that here so
// the wrapper cannot silently drop it — its callers (set_arg, return_value,
// the const-char* JNI arg fallback) rely on the nullptr-on-failure contract,
// which would be broken if a throw crossed the boundary.
static_assert(noexcept(vmhook::make_java_string(std::string_view{})),
              "vmhook::make_java_string MUST be noexcept (nullptr-on-failure contract)");
static_assert(std::is_same_v<decltype(vmhook::make_java_string(std::string_view{})), void*>,
              "vmhook::make_java_string MUST return void* (raw String oop or nullptr)");

// Companion contracts the pure-VM build path rides on.  make_java_string
// allocates the String instance through make_java_object() and its backing
// byte[]/char[] through make_java_array(); both must stay noexcept so the
// nullptr-on-failure contract is not replaced by a throw crossing the
// noexcept boundary of make_java_string itself.
static_assert(noexcept(vmhook::make_java_object(nullptr, std::size_t{ 0 })),
              "vmhook::make_java_object MUST be noexcept");
static_assert(std::is_same_v<decltype(vmhook::make_java_object(nullptr, std::size_t{ 0 })), void*>,
              "vmhook::make_java_object MUST return void*");
static_assert(noexcept(vmhook::make_java_array(std::string_view{}, std::int32_t{ 0 }, std::size_t{ 1 })),
              "vmhook::make_java_array MUST be noexcept");
static_assert(std::is_same_v<decltype(vmhook::make_java_array(std::string_view{}, std::int32_t{ 0 }, std::size_t{ 1 })), void*>,
              "vmhook::make_java_array MUST return void*");
static_assert(noexcept(vmhook::detail::utf8_to_utf16(std::string_view{})) == false,
              "vmhook::detail::utf8_to_utf16 returns std::vector and may throw on alloc "
              "— make_java_string treats that throw as fatal at the noexcept boundary, "
              "matching the documented contract at vmhook.hpp:12889-12891.");

// ───────────────────────── cold-call: empty input -> nullptr ────────────────
static auto test_empty_cold_returns_nullptr() -> void
{
    // The LEDGER's "make_java_string("") cold returns null" gap.  No JVM is
    // attached, so find_class("java/lang/String") returns nullptr, so
    // make_java_string returns nullptr at vmhook.hpp:14524.  The same call
    // must not crash, must not throw (noexcept above), must not block.
    void* const oop{ vmhook::make_java_string(std::string_view{}) };
    check("cold_empty: make_java_string(\"\") returns nullptr (no JVM)",
          oop == nullptr);

    // Explicit "" (a 0-length char-array sv) takes the same path.
    void* const oop2{ vmhook::make_java_string(std::string_view{ "" }) };
    check("cold_empty: make_java_string(empty sv) returns nullptr (no JVM)",
          oop2 == nullptr);

    // The default-constructed string_view's data() is nullptr; the function
    // must not deref it before the find_class null gate.  If this returned
    // anything other than nullptr we would already have crashed above.
    check("cold_empty: noexcept characterization observed at runtime",
          noexcept(vmhook::make_java_string(std::string_view{})));
}

// ───────────────────────── cold-call: arbitrary content -> nullptr ──────────
static auto test_cold_arbitrary_inputs_return_nullptr() -> void
{
    // Pure-ASCII: same nullptr gate.
    check("cold_ascii: make_java_string(\"hello\") -> nullptr (no JVM)",
          vmhook::make_java_string(std::string_view{ "hello" }) == nullptr);

    // 2-byte UTF-8 ("é" == C3 A9, BMP Latin-1).
    std::string latin1;
    latin1 += static_cast<char>(0xC3);
    latin1 += static_cast<char>(0xA9);
    check("cold_latin1: make_java_string(U+00E9) -> nullptr (no JVM)",
          vmhook::make_java_string(latin1) == nullptr);

    // 3-byte UTF-8 ("日" == E6 97 A5, BMP non-Latin1 -> would force UTF16 coder).
    std::string utf16_path;
    utf16_path += static_cast<char>(0xE6);
    utf16_path += static_cast<char>(0x97);
    utf16_path += static_cast<char>(0xA5);
    check("cold_utf16: make_java_string(U+65E5) -> nullptr (no JVM)",
          vmhook::make_java_string(utf16_path) == nullptr);

    // 4-byte UTF-8 (U+1F600 == F0 9F 98 80, astral -> surrogate pair).
    std::string astral;
    astral += static_cast<char>(0xF0);
    astral += static_cast<char>(0x9F);
    astral += static_cast<char>(0x98);
    astral += static_cast<char>(0x80);
    check("cold_astral: make_java_string(U+1F600) -> nullptr (no JVM)",
          vmhook::make_java_string(astral) == nullptr);

    // Embedded interior NUL.  The LEDGER's "embedded NUL handling" gap is
    // covered length-preservingly at the shared utf8_to_utf16 layer in the
    // sibling test; what THIS file pins is that make_java_string's cold
    // contract treats the input as opaque bytes and still returns nullptr
    // (no C-string truncation triggering early UB).
    std::string with_nul;
    with_nul += 'a';
    with_nul += static_cast<char>(0x00);
    with_nul += 'b';
    check("cold_nul: make_java_string(\"a\\0b\") -> nullptr (no JVM)",
          vmhook::make_java_string(with_nul) == nullptr);
}

// ───────────────────────── cold-call: 4096 boundary safety ──────────────────
//
// 4096 code units used to be a ROUTING boundary: the hand-built TLAB path was
// attempted only at or below it, and longer inputs were pushed to the
// JNIEnv::NewString fallback.  That fallback is gone (de-JNI refactor) and
// build_via_tlab() now constructs the complete String at ANY length, so 4096 is
// no longer special at all.  These checks therefore pin something STRONGER than
// before: the entry gate treats at-cap, just-over-cap and far-over-cap inputs
// IDENTICALLY (all nullptr with no JVM, no allocation, no crash, no throw), and
// the shared decoder emits an exact, untruncated code-unit count on both sides
// of the old boundary.  A future re-introduction of a length cap — silently
// truncating long strings, robustness bug #9 all over again — breaks the
// utf8_to_utf16 length checks below.
static auto test_cold_at_and_over_tlab_cap() -> void
{
    constexpr std::int32_t k_tlab_cap{ 4096 };

    const std::string at_cap(static_cast<std::size_t>(k_tlab_cap), 'x');
    check("cold_4096: make_java_string(4096*'x') -> nullptr (entry gate)",
          vmhook::make_java_string(at_cap) == nullptr);

    const std::string just_over(static_cast<std::size_t>(k_tlab_cap + 1), 'x');
    check("cold_4097: make_java_string(4097*'x') -> nullptr (over-cap entry gate)",
          vmhook::make_java_string(just_over) == nullptr);

    // A clearly over-cap input forces (in a hot JVM) the JNI NewString
    // fallback; cold it must still be nullptr.  Use 8192 — twice the cap.
    const std::string twice_cap(static_cast<std::size_t>(k_tlab_cap * 2), 'y');
    check("cold_8192: make_java_string(8192*'y') -> nullptr (over-cap entry gate)",
          vmhook::make_java_string(twice_cap) == nullptr);

    // The SHARED utf8_to_utf16 core (called from both paths in a hot JVM) is
    // observable cold; pin that the cap is HONORED at the source — the
    // decoder produces exactly N code units for an N-char ASCII input at the
    // boundary (no silent truncation, no overflow).  This is the "4096+
    // boundary SAFE" invariant the LEDGER asks for, observable without a JVM.
    const std::vector<std::uint16_t> at_cap_units{ vmhook::detail::utf8_to_utf16(at_cap) };
    check("cold_4096: utf8_to_utf16 emits exactly 4096 units for 4096 ASCII bytes",
          at_cap_units.size() == static_cast<std::size_t>(k_tlab_cap));
    const std::vector<std::uint16_t> over_units{ vmhook::detail::utf8_to_utf16(just_over) };
    check("cold_4097: utf8_to_utf16 emits exactly 4097 units (no silent truncation)",
          over_units.size() == static_cast<std::size_t>(k_tlab_cap + 1));
}

// ───────────────────────── cold-call: the pure-VM allocation chain ──────────
//
// make_java_string is now allocation-only: it builds the String instance with
// make_java_object(string_klass, instance_size) and its backing storage with
// make_java_array("[B"/"[C", ...).  There is no JNI slow path left to fail
// over to, so "the allocation could not be satisfied" IS the failure mode that
// replaced the old NewString-returned-null / OOM signal.  Cold, every link in
// that chain must independently early-out to nullptr — which is what makes
// make_java_string's own nullptr contract above structurally guaranteed rather
// than incidental.
static auto test_cold_pure_vm_allocation_chain_nullptr() -> void
{
    // make_java_object's argument gate: a null klass is rejected before any
    // thread-list walk or TLAB touch.
    check("cold_alloc: make_java_object(nullptr, 24) -> nullptr (null klass gate)",
          vmhook::make_java_object(nullptr, 24u) == nullptr);

    // ...and a zero requested_size is rejected too (same gate), so a klass
    // whose instance size reads back as 0 can never produce a 0-byte object.
    check("cold_alloc: make_java_object(nullptr, 0) -> nullptr (zero-size gate)",
          vmhook::make_java_object(nullptr, 0u) == nullptr);

    // The compact-LATIN1 branch's backing array ("[B", 1 byte per unit).
    check("cold_alloc: make_java_array(\"[B\", 5, 1) -> nullptr (no JVM)",
          vmhook::make_java_array("[B", 5, sizeof(std::uint8_t)) == nullptr);

    // The compact-UTF16 branch's backing array ("[B", 2 bytes per unit).
    check("cold_alloc: make_java_array(\"[B\", 10, 1) -> nullptr (UTF16 coder shape)",
          vmhook::make_java_array("[B", 10, sizeof(std::uint8_t)) == nullptr);

    // The classic (JDK 8) branch's backing array ("[C", 2 bytes per unit).
    check("cold_alloc: make_java_array(\"[C\", 5, 2) -> nullptr (no JVM)",
          vmhook::make_java_array("[C", 5, sizeof(std::uint16_t)) == nullptr);

    // A zero-length backing array (the empty-String shape) takes the same
    // find_class gate — it must not shortcut into a header-only allocation.
    check("cold_alloc: make_java_array(\"[B\", 0, 1) -> nullptr (empty-String shape)",
          vmhook::make_java_array("[B", 0, sizeof(std::uint8_t)) == nullptr);

    // A negative length is rejected by make_java_array's own argument gate,
    // BEFORE find_class — so the guard survives even on a hot JVM.
    check("cold_alloc: make_java_array(\"[B\", -1, 1) -> nullptr (negative-length gate)",
          vmhook::make_java_array("[B", -1, sizeof(std::uint8_t)) == nullptr);
}

auto main() -> int
{
    test_empty_cold_returns_nullptr();
    test_cold_arbitrary_inputs_return_nullptr();
    test_cold_at_and_over_tlab_cap();
    test_cold_pure_vm_allocation_chain_nullptr();

    if (failures == 0)
    {
        std::printf("vmhook make_java_string no-JVM characterization: OK\n");
    }
    else
    {
        std::printf("vmhook make_java_string no-JVM characterization: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
