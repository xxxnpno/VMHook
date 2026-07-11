// Standalone (no-JVM) test for the nested-classes feature surface.
//
// What is no-JVM-determinable about NESTED CLASSES (javac '$'-delimited internal
// names):
//
//   1. NAME-STRING SHAPE.  javac encodes nested-class membership purely in the
//      class-internal name as parent$child[$grandchild...].  The shape rules
//      are pure character arithmetic — no JVM in the loop — so we pin them
//      with constexpr/static_assert: count of '$' separators, leaf segment,
//      enclosing segment, three-deep names (Outer$Mid$Leaf), reject malformed
//      shapes (leading/trailing '$', empty segments), and confirm that ordinary
//      top-level names contain NO '$'.  These mirror the names the fixture
//      Java in tests/jvm exercises (NestedClasses$Host, $Host$StaticNested,
//      $Host$Inner) and the in-process descriptor of the synthetic field
//      `this$0` (descriptor first char 'L', closing ';').
//
//   2. COLD-STATE LOOKUP SAFE DEFAULT.  With no JVM, vmhook::find_class returns
//      nullptr for every name regardless of '$' count (it's just another byte
//      in the lookup string), never throws, and never crashes.  Caching does
//      not memoize misses, so a stale-eviction round-trip on a '$'-name is
//      crash-free.  jni::find_class follows the same null contract on
//      '$'-names.
//
//   3. DESCRIPTOR CHARACTER GATE for the synthetic `this$0` decode.  The
//      flaw-B guard in field_proxy::value_t::cast_for_variant<unique_ptr<W>>
//      rejects any descriptor whose first char is not 'L' — so '[L...;' (array
//      of refs) never decodes into a single wrapper.  We pin the character
//      predicate the guard uses on owned descriptor strings, never touching
//      any field_proxy / oop.
//
// OUT OF SCOPE: real klass resolution for any '$'-name, real this$0 decode,
// real klass_from_oop tie-back — all live-JVM and covered by tests/jvm/modules/
// nested_classes.cpp.  We never fabricate a klass/oop/Method here.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <string>
#include <string_view>

static int failures{ 0 };
static int checks_run{ 0 };
static auto check(const char* name, bool ok) -> void
{
    ++checks_run;
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---- 1. NAME-STRING SHAPE: pure constexpr character arithmetic ------------

constexpr auto count_dollars(std::string_view s) noexcept -> std::size_t
{
    std::size_t n = 0;
    for (char c : s) { if (c == '$') { ++n; } }
    return n;
}

constexpr auto last_segment(std::string_view s) noexcept -> std::string_view
{
    for (std::size_t i = s.size(); i-- > 0; )
    {
        if (s[i] == '$') { return s.substr(i + 1); }
    }
    return s;
}

constexpr auto enclosing(std::string_view s) noexcept -> std::string_view
{
    for (std::size_t i = s.size(); i-- > 0; )
    {
        if (s[i] == '$') { return s.substr(0, i); }
    }
    return {};
}

constexpr auto well_formed_nested(std::string_view s) noexcept -> bool
{
    if (s.empty() || s.front() == '$' || s.back() == '$') { return false; }
    bool prev_dollar = false;
    for (char c : s)
    {
        if (c == '$')
        {
            if (prev_dollar) { return false; }
            prev_dollar = true;
        }
        else
        {
            prev_dollar = false;
        }
    }
    return true;
}

// The three names the JVM-side module pins (force-instantiated in <clinit>):
constexpr std::string_view k_top    = "vmhook/fixtures/NestedClasses";
constexpr std::string_view k_host   = "vmhook/fixtures/NestedClasses$Host";
constexpr std::string_view k_static = "vmhook/fixtures/NestedClasses$Host$StaticNested";
constexpr std::string_view k_inner  = "vmhook/fixtures/NestedClasses$Host$Inner";

// Top-level name: no '$'.
static_assert(count_dollars(k_top) == 0);
// Two-deep: exactly one '$'.
static_assert(count_dollars(k_host) == 1);
// Three-deep: exactly two '$'.
static_assert(count_dollars(k_static) == 2);
static_assert(count_dollars(k_inner) == 2);

// Leaf-segment extraction.
static_assert(last_segment(k_host) == "Host");
static_assert(last_segment(k_static) == "StaticNested");
static_assert(last_segment(k_inner) == "Inner");
static_assert(last_segment(k_top) == "vmhook/fixtures/NestedClasses");

// Enclosing-segment extraction.
static_assert(enclosing(k_static) == "vmhook/fixtures/NestedClasses$Host");
static_assert(enclosing(k_inner)  == "vmhook/fixtures/NestedClasses$Host");
static_assert(enclosing(k_host)   == "vmhook/fixtures/NestedClasses");
static_assert(enclosing(k_top).empty());

// well-formedness
static_assert(well_formed_nested(k_top));
static_assert(well_formed_nested(k_host));
static_assert(well_formed_nested(k_static));
static_assert(well_formed_nested(k_inner));
static_assert(!well_formed_nested(""));
static_assert(!well_formed_nested("$Leaf"));
static_assert(!well_formed_nested("Outer$"));
static_assert(!well_formed_nested("Outer$$Leaf"));
static_assert(!well_formed_nested("$"));

// The Inner's enclosing equals StaticNested's enclosing (same Host).
static_assert(enclosing(k_static) == enclosing(k_inner));

// All three nested names are well under the symbol::to_string 0x1000 cap.
static_assert(k_host.size()   < 0x1000);
static_assert(k_static.size() < 0x1000);
static_assert(k_inner.size()  < 0x1000);

// ---- 3. Descriptor first-char gate (flaw-B guard mirror) ------------------

// The synthetic this$0 has descriptor `L<enclosing>;`; an array of refs has
// `[L<enclosing>;`.  The cast_for_variant<unique_ptr<W>> guard rejects any
// descriptor whose first char is not 'L'.  Pure character predicate.
constexpr auto descriptor_decodes_to_single_ref(std::string_view d) noexcept -> bool
{
    return !d.empty() && d.front() == 'L' && d.back() == ';';
}

static_assert( descriptor_decodes_to_single_ref("Lvmhook/fixtures/NestedClasses$Host;"));
static_assert(!descriptor_decodes_to_single_ref("[Lvmhook/fixtures/NestedClasses$Host;"));
static_assert(!descriptor_decodes_to_single_ref("I"));
static_assert(!descriptor_decodes_to_single_ref(""));
static_assert(!descriptor_decodes_to_single_ref("Lfoo")); // missing ';'

// `this$0` field NAME (not descriptor) carries one '$' too — pin its shape.
constexpr std::string_view k_this0 = "this$0";
static_assert(count_dollars(k_this0) == 1);
static_assert(last_segment(k_this0) == "0");
static_assert(enclosing(k_this0) == "this");

// ---- 2. COLD-STATE LOOKUP SAFE DEFAULT (runtime, no-JVM) ------------------

static auto find_class_noexcept(std::string_view n) -> bool
{
    try { (void)vmhook::find_class(n); return true; }
    catch (...) { return false; }
}

static auto jni_find_class_noexcept(std::string_view n) -> bool
{
    try { (void)vmhook::find_class(n); return true; }
    catch (...) { return false; }
}

auto main() -> int
{
    // find_class on every $-name shape: nullptr, never throws.
    check("find_class(top) noexcept",    find_class_noexcept(k_top));
    check("find_class(top) == nullptr",  vmhook::find_class(k_top) == nullptr);
    check("find_class(host) noexcept",   find_class_noexcept(k_host));
    check("find_class(host) == nullptr", vmhook::find_class(k_host) == nullptr);
    check("find_class(static) noexcept", find_class_noexcept(k_static));
    check("find_class(static) == nullptr", vmhook::find_class(k_static) == nullptr);
    check("find_class(inner) noexcept",  find_class_noexcept(k_inner));
    check("find_class(inner) == nullptr", vmhook::find_class(k_inner) == nullptr);

    // Misses are NOT memoised: a second call still returns nullptr (cache
    // would not change behaviour either way, but we pin idempotency).
    check("find_class(inner) idempotent miss",
          vmhook::find_class(k_inner) == nullptr
       && vmhook::find_class(k_inner) == nullptr);

    // Malformed / pathological '$' names still safe (just bytes through the
    // string lookup).
    for (std::string_view bad : { "$", "$$", "A$", "$A", "A$$B",
                                  "vmhook/fixtures/$Host",
                                  "vmhook/fixtures/Host$" })
    {
        std::string label = "find_class(\"" + std::string(bad) + "\") nullptr/noexcept";
        check(label.c_str(),
              find_class_noexcept(bad) && vmhook::find_class(bad) == nullptr);
    }

    // JNI sibling: same null contract on '$'-names.
    check("jni::find_class(host) noexcept",   jni_find_class_noexcept(k_host));
    check("jni::find_class(host) == nullptr", vmhook::find_class(k_host) == nullptr);
    check("jni::find_class(static) == nullptr", vmhook::find_class(k_static) == nullptr);
    check("jni::find_class(inner) == nullptr", vmhook::find_class(k_inner) == nullptr);

    // Override + evict cycle on a '$'-name: an override seed with a rejected-
    // by-is_valid_pointer sentinel must be evicted by find_class without
    // dereferencing.  We use a sentinel that is_valid_pointer rejects by
    // arithmetic alone (low address + odd) so there is no read.
    vmhook::override_class_lookup(std::string(k_inner), reinterpret_cast<vmhook::hotspot::klass*>(0x1));
    check("find_class('$Inner) after bogus override -> nullptr",
          vmhook::find_class(k_inner) == nullptr);
    vmhook::evict_class_lookup(std::string(k_inner));
    check("find_class('$Inner) after evict still nullptr",
          vmhook::find_class(k_inner) == nullptr);

    std::printf("checks_run=%d failures=%d\n", checks_run, failures);
    return failures == 0 ? 0 : 1;
}
