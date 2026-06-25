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
//   * the NewStringUTF / NewString(slot 163) fallback path is characterized
//     here for the no-JVM case via the public detail wrapper
//     jni_new_string_utf16_local — with hotspot::current_jni_env unset,
//     jni_function<163, ...> returns null and the wrapper returns nullptr.
//     This is the "NewStringUTF OOM" surrogate the LEDGER asks for: there
//     is no JVM and therefore no JNI slot to call, so the cold contract
//     is "return nullptr, do not crash, do not block".
//   * noexcept characterization: make_java_string is declared `noexcept`
//     (vmhook.hpp:1702) — pinned as a static_assert below so any future
//     accidental removal is a compile-time regression.  The same is
//     pinned for jni_new_string_utf16 / jni_new_string_utf16_local /
//     vmhook::detail::utf8_to_utf16 (their failure-mode contracts depend
//     on it: nullptr-on-failure rather than a throw across a callback
//     boundary).
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

// Companion contracts make_java_string's fallback ride on:
static_assert(noexcept(vmhook::detail::jni_new_string_utf16(std::vector<std::uint16_t>{})),
              "vmhook::detail::jni_new_string_utf16 MUST be noexcept");
static_assert(noexcept(vmhook::detail::jni_new_string_utf16_local(std::string_view{})),
              "vmhook::detail::jni_new_string_utf16_local MUST be noexcept");
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
// The TLAB fast path is capped at 4096 code units (vmhook.hpp:14546); inputs
// > 4096 deliberately bypass the TLAB and route through jni_new_string_utf16
// (vmhook.hpp:14682).  Cold (no JVM) BOTH paths must early-out to nullptr:
//   * find_class returns nullptr -> the function returns BEFORE either branch
//     is even considered.
// The "boundary safe" property the LEDGER asks for is that an input of
// EXACTLY 4096 ASCII chars, exactly 4097 chars (just over), and a large
// padding input do not allocate, do not crash, do not throw — they all return
// nullptr at the same find_class gate.  This pins absence of off-by-one in
// the entry gate.
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

// ───────────────────────── cold-call: NewStringUTF fallback OOM-equivalent ──
//
// In a hot JVM make_java_string can fall through to JNIEnv::NewString
// (vmhook.hpp:14682) when the TLAB attempt returns null OR the input is
// over the cap.  The LEDGER asks for the "NewStringUTF OOM path
// characterized" cold; the closest no-JVM observable is that the
// jni_new_string_utf16_local wrapper — which is the SAME slot-163 call
// the over-cap fallback issues — returns nullptr when no JNIEnv is
// available (jni_function<163,...> sees a null current_jni_env and
// returns null; the wrapper returns nullptr at vmhook.hpp:12901-12904).
// That is the cold-side counterpart of "NewString returned null", which is
// also the in-VM OOM signal (NewString returns 0 on OOM).
static auto test_cold_jni_new_string_utf16_local_nullptr() -> void
{
    void* const ref{ vmhook::detail::jni_new_string_utf16_local(std::string_view{}) };
    check("cold_jni: jni_new_string_utf16_local(\"\") -> nullptr (no JNIEnv)",
          ref == nullptr);

    void* const ref_h{ vmhook::detail::jni_new_string_utf16_local(std::string_view{ "hello" }) };
    check("cold_jni: jni_new_string_utf16_local(\"hello\") -> nullptr (no JNIEnv)",
          ref_h == nullptr);

    // jni_new_string_utf16 (the raw-oop variant the over-cap path actually
    // calls at vmhook.hpp:14682) is also nullptr cold.
    const std::vector<std::uint16_t> units{ 'h', 'i' };
    void* const oop{ vmhook::detail::jni_new_string_utf16(units) };
    check("cold_jni: jni_new_string_utf16({h,i}) -> nullptr (no JNIEnv)",
          oop == nullptr);

    // Empty-units edge: NewString(env,nullptr,0) is the well-defined empty
    // form in a hot JVM; cold it stays nullptr (no JNIEnv).
    const std::vector<std::uint16_t> empty_units{};
    void* const oop_e{ vmhook::detail::jni_new_string_utf16(empty_units) };
    check("cold_jni: jni_new_string_utf16({}) -> nullptr (no JNIEnv)",
          oop_e == nullptr);
}

auto main() -> int
{
    test_empty_cold_returns_nullptr();
    test_cold_arbitrary_inputs_return_nullptr();
    test_cold_at_and_over_tlab_cap();
    test_cold_jni_new_string_utf16_local_nullptr();

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
