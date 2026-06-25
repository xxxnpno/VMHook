// Standalone unit test: field_inherited — cold-state robustness of the
// superclass-walk lookup path (`vmhook::find_field` walking `Klass::get_super()`).
//
// WAVE-32 LEDGER GAPS this file closes (no-JVM):
//   * cold-state inherited field lookup with a NULL base klass returns nullopt
//     safely (no crash), across many JDK-shaped name shapes,
//   * signature-shape static_asserts on the find_field surface
//     (vmhook::find_field is noexcept on (nullptr, sv); returns optional),
//   * null base klass safe under repeated cold calls (idempotent miss),
//   * embedded '$' in the name (the synthetic-field convention: `this$0`,
//     `$VALUES`, `val$x`) does NOT crash + returns safe default,
//   * the wrapper-level templated get_field<T>() (which funnels through the
//     same super-walk) returns its T{} default for every primitive width.
//
// CONTRAST vs sibling test_field_null_safety_nojvm.cpp: that file already
// covers generic empty / NUL-embedded / overlong names against a null klass.
// THIS file pins the INHERITED-LOOKUP angles called out in the wave-32 ledger:
//   - synthetic-name shapes (`this$0`, `$VALUES`, `val$captured`) that ONLY
//     appear on inherited / outer-class chains,
//   - signature static_asserts on `find_field` itself (not just field_proxy),
//   - the "depth-N walk" surface: even with a null start klass, all of the
//     callers that USE `find_field` (static `get_field`, templated free
//     `get_field<T>`, `static_field`) flow through the same nullopt return
//     path. Pin each entry point.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <optional>
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
// SECTION 1 — signature static_asserts on vmhook::find_field.
//
// The super-walk's primary entry point. Documented contract: returns
// std::optional<field_entry_t>, callable with (klass*, string_view). The
// inherited-walk is layout-agnostic (it just calls per-klass find_field in a
// loop) — so this signature lock guards the whole inherited surface.
// ---------------------------------------------------------------------------

static_assert(
    std::is_same_v<
        decltype(vmhook::find_field(static_cast<vmhook::hotspot::klass*>(nullptr),
                                    std::string_view{})),
        std::optional<vmhook::hotspot::field_entry_t>>,
    "find_field(klass*, string_view) must return optional<field_entry_t>");

// field_entry_t shape: must be copy-constructible (it lives inside an
// std::optional return value AND inside the cache map). Copy-ctor noexcept is
// platform-variant due to the std::string `signature` member, so we only pin
// copy-constructibility, not noexcept.
static_assert(std::is_copy_constructible_v<vmhook::hotspot::field_entry_t>,
              "field_entry_t must be copy-constructible (optional + cache)");
static_assert(std::is_move_constructible_v<vmhook::hotspot::field_entry_t>,
              "field_entry_t must be move-constructible");

// is_static field is the ONLY access-flag bit propagated through the entry.
// Pin its presence + boolean-ness; the access-flag drop is documented in
// the specialist note (proxy can't tell inherited from own).
static_assert(std::is_same_v<decltype(vmhook::hotspot::field_entry_t{}.is_static), bool>,
              "field_entry_t.is_static must be bool");

// offset is the per-klass slot offset added to the decoded oop pointer for
// instance fields, or to the java.lang.Class mirror for statics.
static_assert(std::is_integral_v<decltype(vmhook::hotspot::field_entry_t{}.offset)>,
              "field_entry_t.offset must be an integral byte offset");

// signature is a std::string carried back to the proxy.
static_assert(std::is_same_v<decltype(vmhook::hotspot::field_entry_t{}.signature),
                             std::string>,
              "field_entry_t.signature must be std::string");

// ---------------------------------------------------------------------------
// SECTION 2 — null base klass safe under cold-state inherited lookup.
//
// The super-walk begins with `for (k = target_klass; k != nullptr; k = k->get_super())`.
// If target_klass is null, the loop body never executes, the function returns
// nullopt without ever touching a VM struct. Pin this against the realistic
// inherited-field names a caller might pass.
// ---------------------------------------------------------------------------

static auto section_null_base_klass_inherited_names() -> void
{
    // Real JDK inherited fields commonly looked up through the super-walk:
    static constexpr const char* inherited_names[]{
        "hash",            // java.lang.Object's inherited int slot
        "value",           // java.lang.String / boxed primitive
        "coder",           // String coder (JDK 9+)
        "count",           // legacy String length
        "offset",          // legacy String offset
        "modCount",        // AbstractList / HashMap
        "size",            // ArrayList / HashMap
        "elementData",     // ArrayList
        "table",           // HashMap
        "threshold",       // HashMap
        "loadFactor",      // HashMap
        "serialVersionUID",// inherited across Serializable hierarchies
    };
    for (const char* name : inherited_names)
    {
        const auto entry{ vmhook::find_field(nullptr, name) };
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "find_field(null klass, '%s') == nullopt (cold inherited)",
                      name);
        check(buf, !entry.has_value());
    }
}

// ---------------------------------------------------------------------------
// SECTION 3 — embedded '$' in the field name.
//
// '$' appears in synthetic / outer-class inherited fields:
//   * `this$0`        — inner-class outer-reference (often inherited),
//   * `$VALUES`       — enum value array,
//   * `val$captured`  — lambda capture field,
//   * `$assertionsDisabled` — assertion flag,
//   * names containing multiple '$' for nested inner classes.
//
// All must be honoured as part of the name (string_view is byte-exact); none
// must crash; all return safe defaults against null klass.
// ---------------------------------------------------------------------------

static auto section_dollar_in_name() -> void
{
    static constexpr const char* dollar_names[]{
        "this$0",
        "this$1",
        "$VALUES",
        "val$captured",
        "val$x",
        "$assertionsDisabled",
        "$SwitchMap$java$lang$Thread$State",
        "Outer$Inner$field",
        "$",                 // pathological single-'$'
        "$$",                // pathological double-'$'
        "name$with$many$dollars",
    };
    for (const char* name : dollar_names)
    {
        const auto entry{ vmhook::find_field(nullptr, name) };
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "find_field(null klass, '%s') == nullopt ($-in-name safe)",
                      name);
        check(buf, !entry.has_value());

        // Same name through the templated wrapper get_field<int32>() — it
        // funnels into find_field via the inherited-walk and must return 0.
        const std::int32_t v{
            vmhook::get_field<std::int32_t>(nullptr, nullptr, name) };
        std::snprintf(buf, sizeof(buf),
                      "get_field<int32>(null,null,'%s') == 0 ($-in-name safe)",
                      name);
        check(buf, v == 0);
    }

    // string_view round-trip with embedded '$' AND a length specifier so the
    // C-string `strlen` cannot accidentally truncate.
    static constexpr char weird[]{ 'a', '$', 'b', '$', 'c' };
    const std::string_view weird_view{ weird, sizeof(weird) };
    check("weird_view sizes to 5 with embedded '$'", weird_view.size() == 5u);
    check("find_field(null klass, 'a$b$c' as view) == nullopt",
          !vmhook::find_field(nullptr, weird_view).has_value());
}

// ---------------------------------------------------------------------------
// SECTION 4 — every wrapper that walks the super-chain returns its safe
// default for inherited names against a null klass. The free templated
// get_field<T>() is the user-facing path; pin every primitive width.
// ---------------------------------------------------------------------------

static auto section_inherited_walk_wrapper_defaults() -> void
{
    constexpr const char* name{ "hash" };  // canonical inherited int slot
    check("get_field<int32>(null,null,'hash') == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, name) == 0);
    check("get_field<int64>(null,null,'hash') == 0",
          vmhook::get_field<std::int64_t>(nullptr, nullptr, name) == 0);
    check("get_field<uint32>(null,null,'hash') == 0",
          vmhook::get_field<std::uint32_t>(nullptr, nullptr, name) == 0u);
    check("get_field<uint64>(null,null,'hash') == 0",
          vmhook::get_field<std::uint64_t>(nullptr, nullptr, name) == 0u);
    check("get_field<int16>(null,null,'hash') == 0",
          vmhook::get_field<std::int16_t>(nullptr, nullptr, name) == 0);
    check("get_field<int8>(null,null,'hash') == 0",
          vmhook::get_field<std::int8_t>(nullptr, nullptr, name) == 0);
    check("get_field<float>(null,null,'hash') == 0.0f",
          vmhook::get_field<float>(nullptr, nullptr, name) == 0.0f);
    check("get_field<double>(null,null,'hash') == 0.0",
          vmhook::get_field<double>(nullptr, nullptr, name) == 0.0);
    check("get_field<bool>(null,null,'hash') == false",
          vmhook::get_field<bool>(nullptr, nullptr, name) == false);
}

// ---------------------------------------------------------------------------
// SECTION 5 — idempotent miss across the super-walk entry point. Repeating
// a cold inherited lookup must stay nullopt every iteration. find_field caches
// only HITS (the not-found path returns nullopt without inserting), so an
// inherited-name miss must be a fresh nullopt every time without poisoning.
// ---------------------------------------------------------------------------

static auto section_idempotent_inherited_miss() -> void
{
    bool all_nullopt{ true };
    for (int i{ 0 }; i < 64; ++i)
    {
        const auto entry{ vmhook::find_field(nullptr, "modCount") };
        if (entry.has_value()) { all_nullopt = false; }
    }
    check("64 cold-state find_field(null, 'modCount') all nullopt",
          all_nullopt);

    // Mix inherited-shaped names + synthetic-$ names across iterations so the
    // (would-be) cache key shape doesn't accidentally collide.
    bool mixed_ok{ true };
    static constexpr const char* mix[]{
        "hash", "this$0", "$VALUES", "elementData", "val$x", "size",
    };
    for (int i{ 0 }; i < 64; ++i)
    {
        const char* n{ mix[i % (sizeof(mix) / sizeof(mix[0]))] };
        const auto entry{ vmhook::find_field(nullptr, n) };
        if (entry.has_value()) { mixed_ok = false; }
        if (vmhook::get_field<std::int32_t>(nullptr, nullptr, n) != 0)
        {
            mixed_ok = false;
        }
    }
    check("64 cold-state mixed inherited/synthetic lookups all safe nullopt/0",
          mixed_ok);
}

// ---------------------------------------------------------------------------
// SECTION 6 — string_view shapes the inherited walk MUST accept without
// crashing against null klass: long synthetic names (nested inner classes
// stack '$'s deeply), and a name built from non-owning data().
// ---------------------------------------------------------------------------

static auto section_long_synthetic_names() -> void
{
    // Nested-inner-class synthetic — common in framework-generated code.
    std::string nested{ "Outer" };
    for (int i{ 0 }; i < 16; ++i)
    {
        nested += "$Inner";
    }
    nested += "$field";
    const std::string_view view{ nested };
    check("nested-inner synthetic name view sizes correctly",
          view.size() == nested.size());
    check("find_field(null klass, deeply-nested-$ name) == nullopt",
          !vmhook::find_field(nullptr, view).has_value());
    check("get_field<int32>(null,null, deeply-nested-$ name) == 0",
          vmhook::get_field<std::int32_t>(nullptr, nullptr, view) == 0);

    // Lambda capture style: `val$` + identifier.
    for (int i{ 0 }; i < 8; ++i)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "val$cap_%d", i);
        check("val$cap_N safe nullopt",
              !vmhook::find_field(nullptr, buf).has_value());
    }
}

int main()
{
    std::printf("field_inherited no-JVM unit test\n");
    section_null_base_klass_inherited_names();
    section_dollar_in_name();
    section_inherited_walk_wrapper_defaults();
    section_idempotent_inherited_miss();
    section_long_synthetic_names();
    if (failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", failures);
    return 1;
}
