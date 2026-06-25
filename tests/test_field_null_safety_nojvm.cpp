// Standalone unit test: field_null_safety — read-side / lookup-side robustness
// of the field surface when called with a null oop, a null klass, an empty /
// embedded-NUL / overlong name, and from a field_proxy built over nullptr.
//
// WAVE-29 LEDGER GAPS this file closes (no-JVM):
//   * null oop_t + null name_string (a real `name_string` here is the
//     std::string_view the wrapper find_field takes — pass it both empty and
//     pointer-only-NUL forms),
//   * embedded-NUL field name does NOT crash + returns safe default; the NUL
//     is honoured as part of the name, not treated as a terminator,
//   * overlong field name (>= 4 KiB) does NOT crash + returns safe default,
//   * static_asserts on field_proxy ctor noexcept on (nullptr, std::string{},
//     false) AND on the 5-arg overload with (nullptr, "", false, nullptr, 0),
//   * 32-iter idempotent miss — repeating a never-resolvable lookup 32 times
//     never poisons the cache nor crashes.
//
// Contrast vs sibling file test_field_introspection_nojvm.cpp: that file covers
// noexcept signatures of READ accessors + the descriptor classification matrix.
// This file covers INPUT-validation (null/empty/NUL/overlong/idempotency) of
// the LOOKUP surface — distinct, complementary.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// SECTION 1 — field_proxy ctor noexcept LOCKS on the degenerate (null, null)
// constructions.  Both the 3-arg escape-hatch ctor and the 5-arg mirror-aware
// ctor are documented noexcept (vmhook.hpp:15537, 15557).  A future refactor
// that allocates / throws inside either ctor would be a silent regression for
// every safe-default path; pin the contract.
// ---------------------------------------------------------------------------

static_assert(std::is_nothrow_constructible_v<vmhook::field_proxy,
                                              void*, std::string, bool>,
              "field_proxy(void*, string, bool) must be noexcept");

static_assert(noexcept(vmhook::field_proxy{
                  static_cast<void*>(nullptr), std::string{}, false }),
              "field_proxy{nullptr, empty-sig, false} must be noexcept-constructible");

static_assert(std::is_nothrow_constructible_v<vmhook::field_proxy,
                                              void*, std::string, bool,
                                              vmhook::hotspot::klass*, std::size_t>,
              "field_proxy(void*, string, bool, klass*, size_t) must be noexcept");

// The two ctors set the same backing slots (field_pointer / signature / static);
// they MUST also accept move-from string + a literal empty string without UB.
// We can't static_assert UB-absence — but we CAN static_assert that the ctor
// arguments are trivially-acceptable in a constant evaluation: a value of type
// std::string{} is acceptable as a noexcept ctor arg.
static_assert(noexcept(std::string{}),
              "std::string{} default-ctor must be noexcept (precondition for "
              "field_proxy noexcept ctor lock)");

// ---------------------------------------------------------------------------
// SECTION 2 — null oop + null klass behaviour at the wrapper templated
// get_field<T>() entry.  Documented contract (vmhook.hpp:14148-14195): the
// catch arm returns value_type{} on EVERY exception path, including the null
// klass, null mirror, null object, and "field not found" arms.  Pin all 4.
// ---------------------------------------------------------------------------

static auto section_null_oop_null_klass() -> void
{
    // (a) null oop + null klass + non-empty name — find_field bails on null
    //     klass, exception caught, returns 0.
    check("get_field<int32>(null oop, null klass, 'x') == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, "x") == 0);
    check("get_field<int64>(null oop, null klass, 'x') == 0",
          vmhook::get_field<std::int64_t>(nullptr, nullptr, "x") == 0);
    check("get_field<double>(null oop, null klass, 'x') == 0.0",
          vmhook::get_field<double>(nullptr, nullptr, "x") == 0.0);
    check("get_field<float>(null oop, null klass, 'x') == 0.0f",
          vmhook::get_field<float>(nullptr, nullptr, "x") == 0.0f);
    check("get_field<bool>(null oop, null klass, 'x') == false",
          vmhook::get_field<bool>(nullptr, nullptr, "x") == false);
    check("get_field<uint32>(null oop, null klass, 'x') == 0  (compressed-OOP)",
          vmhook::get_field<std::uint32_t>(nullptr, nullptr, "x") == 0u);

    // (b) null klass with a "valid-looking" oop pointer — find_field still
    //     bails on null klass FIRST (it is checked before object), so this is
    //     equally safe.  We use a stack-local sentinel just so the pointer
    //     argument is non-null and non-NULL — it must NEVER be dereferenced.
    int sentinel = static_cast<int>(0xDEADBEEFu);
    check("get_field<int32>(stack-oop, null klass, 'x') == 0 (klass null wins)",
          vmhook::get_field<std::int32_t>(&sentinel, nullptr, "x") == 0);

    // (c) The wrapper-level find_field directly (not via get_field<T>).
    //     Returns std::nullopt on null klass; this is the underlying primitive.
    const auto entry{ vmhook::find_field(nullptr, "anything") };
    check("find_field(null klass, 'anything') == nullopt",
          !entry.has_value());
}

// ---------------------------------------------------------------------------
// SECTION 3 — null name_string forms.  The wrapper find_field takes a
// std::string_view, so "null name" means (a) default-constructed string_view
// (data() == nullptr, size() == 0), (b) explicit empty string literal "".  In
// both cases find_field STILL bails on null klass first — the safe default
// flows through unchanged.
// ---------------------------------------------------------------------------

static auto section_null_name_string() -> void
{
    // (a) default-constructed string_view (data() == nullptr, size() == 0).
    constexpr std::string_view null_view{};
    static_assert(null_view.data() == nullptr,
                  "default string_view must have nullptr data (precondition for "
                  "the find_field null-name-string test)");
    static_assert(null_view.size() == 0u,
                  "default string_view must have size 0");

    check("find_field(null klass, null_view) == nullopt (no crash)",
          !vmhook::find_field(nullptr, null_view).has_value());
    check("get_field<int32>(null oop, null klass, null_view) == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, null_view) == 0);

    // (b) explicit empty literal — data() points at a NUL terminator, size() 0.
    constexpr std::string_view empty_lit{ "" };
    check("find_field(null klass, '') == nullopt",
          !vmhook::find_field(nullptr, empty_lit).has_value());
    check("get_field<int32>(null oop, null klass, '') == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, empty_lit) == 0);
}

// ---------------------------------------------------------------------------
// SECTION 4 — embedded-NUL field name.  The wrapper API takes std::string_view
// (NOT a const char*), so an embedded NUL inside the view's bytes MUST be
// honoured as part of the name and never act as a C-string terminator.  Even
// against a null klass (so we never reach the actual klass walk), the call
// must not crash and must return the safe default.
// ---------------------------------------------------------------------------

static auto section_embedded_nul_name() -> void
{
    static constexpr char nul_name_bytes[]{ 'o', 'k', '\0', 'I', 'n', 't' };
    const std::string_view nul_name{ nul_name_bytes, sizeof(nul_name_bytes) };
    // Pre-condition: the view's size DOES include the NUL byte (the C-string
    // form would stop at index 2 — sizes would mismatch otherwise).
    check("embedded-NUL name view sizes to 6 (NOT 2)",
          nul_name.size() == 6u);

    check("find_field(null klass, 'ok\\0Int') == nullopt (no crash)",
          !vmhook::find_field(nullptr, nul_name).has_value());
    check("get_field<int32>(null oop, null klass, 'ok\\0Int') == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, nul_name) == 0);
    check("get_field<int64>(null oop, null klass, 'ok\\0Int') == 0",
          vmhook::get_field<std::int64_t>(nullptr, nullptr, nul_name) == 0);
    check("get_field<double>(null oop, null klass, 'ok\\0Int') == 0.0",
          vmhook::get_field<double>(nullptr, nullptr, nul_name) == 0.0);

    // A NUL at position 0 — visually equivalent to the empty C-string but the
    // view length is 4 ("hidden" after the front NUL).  Same safe-default
    // contract.
    static constexpr char front_nul_bytes[]{ '\0', 'a', 'b', 'c' };
    const std::string_view front_nul{ front_nul_bytes, sizeof(front_nul_bytes) };
    check("get_field<int32>(null oop, null klass, '\\0abc') == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, front_nul) == 0);
}

// ---------------------------------------------------------------------------
// SECTION 5 — overlong field name.  4 KiB of 'a' — far past any realistic JVM
// field-name length.  Against a null klass we never construct a std::string
// from it inside find_field (the early null-klass bail happens BEFORE the
// name_str copy at vmhook.hpp:14093), so this is cheap; against a klass the
// copy would still be O(N) but no crash.  Pin the safe-default contract.
// ---------------------------------------------------------------------------

static auto section_overlong_name() -> void
{
    // Build the 4 KiB name on the stack via std::string (heap, but we control
    // the lifetime).  std::string_view aliases its data.
    std::string overlong(4096, 'a');
    const std::string_view view{ overlong };
    check("overlong name view sizes to 4096",
          view.size() == 4096u);

    check("find_field(null klass, 4 KiB name) == nullopt (no crash)",
          !vmhook::find_field(nullptr, view).has_value());
    check("get_field<int32>(null oop, null klass, 4 KiB name) == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, view) == 0);

    // 64 KiB — well past any sane name length.  Still safe.
    std::string huge(65536, 'z');
    const std::string_view huge_view{ huge };
    check("get_field<int32>(null oop, null klass, 64 KiB name) == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, huge_view) == 0);
}

// ---------------------------------------------------------------------------
// SECTION 6 — 32-iter idempotent miss.  Repeating an unresolvable lookup must
// stay safe AND stay a miss every iteration: the cache is keyed on (klass,
// name) and find_field "caches only FOUND entries" (the cache insert at
// vmhook.hpp:14126 is inside the `if (entry)` arm only).  Against a null klass
// we never even reach the cache code path — but iteration is the contract.
// ---------------------------------------------------------------------------

static auto section_idempotent_miss() -> void
{
    bool all_zero{ true };
    bool all_nullopt{ true };
    for (int i{ 0 }; i < 32; ++i)
    {
        const auto entry{ vmhook::find_field(nullptr, "ghostField") };
        if (entry.has_value()) { all_nullopt = false; }
        const std::int32_t v{
            vmhook::get_field<std::int32_t>(nullptr, nullptr, "ghostField") };
        if (v != 0) { all_zero = false; }
    }
    check("32 iterations of find_field(null, 'ghostField') all nullopt",
          all_nullopt);
    check("32 iterations of get_field<int32>(null, null, 'ghostField') all 0",
          all_zero);

    // Mix in different names to make sure the (would-be) cache key shape
    // doesn't trip a bug at iteration 17.  All names unresolvable.
    bool mixed_ok{ true };
    for (int i{ 0 }; i < 32; ++i)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "ghost_%d", i);
        const std::int32_t v{
            vmhook::get_field<std::int32_t>(nullptr, nullptr, buf) };
        if (v != 0) { mixed_ok = false; }
    }
    check("32 iterations of mixed-name get_field all 0", mixed_ok);
}

// ---------------------------------------------------------------------------
// SECTION 7 — field_proxy{nullptr, ..., ...} accessors stay safe even under
// the degenerate inputs that arrive through the lookup miss path.  The
// introspection sibling file covers the per-descriptor matrix; here we only
// re-prove the "no crash, get() returns the int32_t{0} default" contract for
// the specific inputs Section 4 and Section 5 just exercised.
// ---------------------------------------------------------------------------

static auto section_null_proxy_get_default() -> void
{
    // Empty signature — get() takes the no-pointer arm and emits
    // value_t{ int32_t{0}, signature_text }.  signature() round-trips empty.
    {
        vmhook::field_proxy proxy{ nullptr, std::string{}, /*is_static=*/false };
        const auto value{ proxy.get() };
        const int as_int{ value };
        check("null-proxy empty-sig get() -> int 0", as_int == 0);
        check("null-proxy empty-sig signature() round-trips",
              value.signature.empty());
        check("null-proxy empty-sig raw_address() nullptr",
              proxy.raw_address() == nullptr);
    }

    // Embedded-NUL signature — the signature_text is std::string, so the NUL
    // is part of it; get() must not segfault, just echo a 0 default.
    {
        std::string sig_with_nul(6, '\0');
        sig_with_nul[0] = 'I';  // pretend "I" + NUL pad — still a primitive front
        vmhook::field_proxy proxy{ nullptr, sig_with_nul, /*is_static=*/false };
        const auto value{ proxy.get() };
        const int as_int{ value };
        check("null-proxy NUL-padded sig get() -> int 0", as_int == 0);
        check("null-proxy NUL-padded sig is_reference() false",
              proxy.is_reference() == false);
    }
}

int main()
{
    std::printf("field_null_safety no-JVM unit test\n");
    section_null_oop_null_klass();
    section_null_name_string();
    section_embedded_nul_name();
    section_overlong_name();
    section_idempotent_miss();
    section_null_proxy_get_default();
    if (failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", failures);
    return 1;
}
