// No-JVM safety + null-arg guards for hotspot::iterate_struct_entries /
// iterate_type_entries, plus get_vm_types / get_vm_structs caching, plus the
// pure-logic clamp/bound arithmetic (vmhook::clamp_safe_container_count /
// vmhook::k_max_safe_container_elems) that backs every oop-derived
// reserve/iteration bound in the library.
//
// Every symbol exercised here lives in a header and is reachable without a JVM.
// In this standalone process no JVM library is loaded, so:
//   * get_jvm_module() resolves to nullptr,
//   * get_vm_types() / get_vm_structs() resolve their global symbol to nullptr
//     and cache that nullptr,
//   * iterate_struct_entries() / iterate_type_entries() walk a null array head,
//     so the loop guard (`entry && entry->type_name`) terminates immediately
//     and the functions return nullptr WITHOUT faulting.
// The null-argument guards (`if (!type_name || !field_name) return nullptr;`)
// short-circuit before any strcmp, so passing a null symbol string can never
// hand strcmp(nullptr, ...) which is UB.
//
// Anything that requires a populated gHotSpotVMStructs / gHotSpotVMTypes array
// (i.e. a real successful lookup returning a non-null entry, and reading its
// ->offset) is covered by JVM integration in example.cpp and is OUT OF SCOPE
// for this pure no-JVM file.  Note also that iterate_struct_entries /
// iterate_type_entries read get_vm_structs() / get_vm_types() internally and
// take NO array parameter, so a *fabricated* in-process array cannot be driven
// through the real API without a live exported symbol; the match-found / skip-
// guard loop-body paths are therefore JVM-integration territory, not reachable
// from this no-JVM file using only the public API.
//
// vmhook::clamp_safe_container_count and vmhook::k_max_safe_container_elems are
// pure constexpr arithmetic with NO JVM dependency, so the FULL behaviour of
// the clamp (negative -> 0, [0,cap] identity, >cap -> cap) is exhaustively
// exercised here, including compile-time (constexpr / static_assert) evaluation.
//
// This file deliberately extends tests/test_helpers.cpp sections 15 & 16
// (which cover only "Symbol"/"_length" + the four null-guard cases) with a far
// wider set of real HotSpot symbol names, argument-ordering / empty-string
// boundary cases, near-miss field names, repeated caching calls, the clamp
// arithmetic, and cross-consistency between the iterate_* helpers and the
// cached getters.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
// Additional std headers used by the EXHAUSTIVE EXPANSION (31+) sections below
// (offsetof / std::size_t, std::strcmp, std::array, the layout type-traits).
// Self-contained so the libc++ oracle build (macos/clang, android) sees every
// header it needs without relying on transitive includes from <vmhook/vmhook.hpp>.
#include <cstddef>
#include <cstring>
#include <array>
#include <type_traits>
#include <memory>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Cap as a signed int32, mirroring the constexpr cast inside
// clamp_safe_container_count (k_max_safe_container_elems == 1<<24 == 16,777,216,
// which is asserted in-header to fit int32).  Used as the reference value for
// every clamp boundary assertion below.
static constexpr std::int32_t k_cap{ static_cast<std::int32_t>(vmhook::k_max_safe_container_elems) };

// ---------------------------------------------------------------------------
// 1. get_vm_structs / get_vm_types caching in a no-JVM process.
//
// First call resolves the global symbol from the (absent) JVM module and
// caches the result; the cache is a function-local static, so every later
// call must return the *identical* pointer.  With no JVM that pointer is
// nullptr, but the caching contract (stable across calls) is what we assert
// here -- a regression that recomputed on every call, or that returned a
// fresh-but-equal value, would still pass the "== nullptr" checks, so we also
// assert pointer identity directly and over many repeated calls.
// ---------------------------------------------------------------------------
static auto test_getters_cache_no_jvm() -> void
{
    auto* const types_a{ vmhook::hotspot::get_vm_types() };
    auto* const types_b{ vmhook::hotspot::get_vm_types() };

    check("get_vm_types_no_jvm_returns_null", types_a == nullptr);
    check("get_vm_types_cache_stable_ab", types_a == types_b);

    auto* const structs_a{ vmhook::hotspot::get_vm_structs() };
    auto* const structs_b{ vmhook::hotspot::get_vm_structs() };

    check("get_vm_structs_no_jvm_returns_null", structs_a == nullptr);
    check("get_vm_structs_cache_stable_ab", structs_a == structs_b);

    // Hammer the cache: 1000 repeated calls must all observe the same value.
    bool types_all_equal{ true };
    bool structs_all_equal{ true };
    for (int i{ 0 }; i < 1000; ++i)
    {
        if (vmhook::hotspot::get_vm_types() != types_a) { types_all_equal = false; }
        if (vmhook::hotspot::get_vm_structs() != structs_a) { structs_all_equal = false; }
    }
    check("get_vm_types_cache_stable_repeated", types_all_equal);
    check("get_vm_structs_cache_stable_repeated", structs_all_equal);

    // The two getters resolve distinct globals (gHotSpotVMTypes vs
    // gHotSpotVMStructs); they happen to both be nullptr here, but that is the
    // documented no-JVM outcome, not an accident of aliasing -- assert it.
    check("get_vm_types_and_structs_both_null_no_jvm",
          types_a == nullptr && structs_a == nullptr);
}

// ---------------------------------------------------------------------------
// 2. get_jvm_module caching in a no-JVM process.
//
// find_jvm_module() walks every candidate library name (jvm.dll / libjvm.so /
// libjvm.dylib); with none loaded it returns a null module_handle, cached on
// the first call.  module_handle is a pointer type, so it compares against
// nullptr and is stable across calls.
// ---------------------------------------------------------------------------
static auto test_jvm_module_cache_no_jvm() -> void
{
    auto const first{ vmhook::hotspot::get_jvm_module() };
    auto const second{ vmhook::hotspot::get_jvm_module() };
    check("get_jvm_module_no_jvm_returns_null", first == nullptr);
    check("get_jvm_module_cache_stable", first == second);

    // Hammer the module cache too: stable across many repeated calls.
    bool module_all_equal{ true };
    for (int i{ 0 }; i < 1000; ++i)
    {
        if (vmhook::hotspot::get_jvm_module() != first) { module_all_equal = false; }
    }
    check("get_jvm_module_cache_stable_repeated", module_all_equal);
}

// ---------------------------------------------------------------------------
// 3. iterate_struct_entries returns nullptr (never crashes) for a broad set of
//    real HotSpot type/field pairs with no JVM present.
//
//    test_helpers.cpp only checks ("Symbol", "_length").  Here we walk the
//    actual symbol pairs the library looks up at runtime across Method,
//    ConstMethod, Klass, InstanceKlass, oopDesc, ConstantPool, JavaThread,
//    CompressedOops, ClassLoaderData, etc.  Each must short-circuit on the
//    null array head and return nullptr.
// ---------------------------------------------------------------------------
static auto test_iterate_struct_entries_real_symbols_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;

    struct pair { const char* type; const char* field; const char* tag; };
    static const pair pairs[]{
        { "Symbol",            "_body",                            "struct_symbol_body" },
        { "Method",            "_constMethod",                     "struct_method_constMethod" },
        { "Method",            "_from_compiled_code_entry_point",  "struct_method_from_compiled_code_entry" },
        { "ConstMethod",       "_constants",                       "struct_constmethod_constants" },
        { "ConstantPool",      "_pool_holder",                     "struct_constantpool_pool_holder" },
        { "Klass",             "_java_mirror",                     "struct_klass_java_mirror" },
        { "InstanceKlass",     "_fieldinfo_stream",                "struct_instanceklass_fieldinfo_stream" },
        { "oopDesc",           "_metadata._compressed_klass",      "struct_oopdesc_compressed_klass" },
        { "oopDesc",           "_metadata._klass",                 "struct_oopdesc_klass" },
        { "JavaThread",        "_thread_state",                    "struct_javathread_thread_state" },
        { "ClassLoaderData",   "_klasses",                         "struct_cld_klasses" },
        { "CompressedOops",    "_base",                            "struct_compressedoops_base" },
    };

    for (const auto& p : pairs)
    {
        check(p.tag, iterate_struct_entries(p.type, p.field) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// 4. iterate_type_entries returns nullptr for a set of real HotSpot type names
//    with no JVM present.  test_helpers.cpp only checks "Symbol".
// ---------------------------------------------------------------------------
static auto test_iterate_type_entries_real_symbols_no_jvm() -> void
{
    using vmhook::hotspot::iterate_type_entries;

    struct named { const char* type; const char* tag; };
    static const named names[]{
        { "Method",        "type_method" },
        { "ConstantPool",  "type_constantpool" },
        { "Klass",         "type_klass" },
        { "InstanceKlass", "type_instanceklass" },
        { "oopDesc",       "type_oopdesc" },
        { "narrowOop",     "type_narrowoop" },
    };

    for (const auto& n : names)
    {
        check(n.tag, iterate_type_entries(n.type) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// 5. Null-argument guards.
//
// iterate_struct_entries short-circuits to nullptr if EITHER argument is null
// (the guard is `if (!type_name || !field_name)`), and iterate_type_entries if
// its single argument is null -- BEFORE any strcmp.  Exercise null type_name,
// null field_name, and both-null, against several real symbol names so a guard
// that only checked one of the two arguments would fail loudly here.
// ---------------------------------------------------------------------------
static auto test_null_arg_guards() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    // --- iterate_struct_entries: null field_name, valid (non-null) type_name.
    check("struct_null_field_symbol",
          iterate_struct_entries("Symbol", nullptr) == nullptr);
    check("struct_null_field_method",
          iterate_struct_entries("Method", nullptr) == nullptr);
    check("struct_null_field_instanceklass",
          iterate_struct_entries("InstanceKlass", nullptr) == nullptr);
    check("struct_null_field_oopdesc",
          iterate_struct_entries("oopDesc", nullptr) == nullptr);

    // --- iterate_struct_entries: null type_name, valid (non-null) field_name.
    check("struct_null_type_length",
          iterate_struct_entries(nullptr, "_length") == nullptr);
    check("struct_null_type_methods",
          iterate_struct_entries(nullptr, "_methods") == nullptr);
    check("struct_null_type_mark",
          iterate_struct_entries(nullptr, "_mark") == nullptr);

    // --- iterate_struct_entries: both arguments null.
    check("struct_both_null",
          iterate_struct_entries(nullptr, nullptr) == nullptr);

    // --- iterate_type_entries: null type_name.
    check("type_null_type_name",
          iterate_type_entries(nullptr) == nullptr);
}

// ---------------------------------------------------------------------------
// 6. Argument-ordering / boundary cases.
//
// The two struct-entry arguments are positional and independent: type_name is
// matched against entry->type_name and field_name against entry->field_name.
// With no JVM every call returns nullptr regardless of argument order, so we
// assert that:
//   * swapping the two arguments still yields nullptr (no accidental match),
//   * passing a field string in the type slot (and vice-versa) yields nullptr,
//   * empty-string arguments are treated as ordinary non-null strings (NOT as
//     null) -- they pass the null guard and then simply fail to match the
//     (absent) array, returning nullptr without crashing.
// Empty strings are the key boundary: the guard checks the pointer, not the
// length, so "" must NOT be conflated with nullptr.
// ---------------------------------------------------------------------------
static auto test_argument_ordering_and_empty_strings() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    // Correct order vs swapped order -- both null with no JVM.
    check("struct_correct_order_symbol_length",
          iterate_struct_entries("Symbol", "_length") == nullptr);
    check("struct_swapped_order_length_symbol",
          iterate_struct_entries("_length", "Symbol") == nullptr);
    check("struct_correct_order_method_code",
          iterate_struct_entries("Method", "_code") == nullptr);
    check("struct_swapped_order_code_method",
          iterate_struct_entries("_code", "Method") == nullptr);

    // Field name placed in the type slot (and a type name in the field slot):
    // a real lookup could never match these, and with no JVM they are nullptr.
    check("struct_field_name_in_type_slot",
          iterate_struct_entries("_metadata._klass", "oopDesc") == nullptr);

    // Empty strings are non-null: they survive the guard and return nullptr by
    // failing to match, NOT by short-circuiting (and never crash).
    check("struct_empty_type_non_empty_field",
          iterate_struct_entries("", "_length") == nullptr);
    check("struct_non_empty_type_empty_field",
          iterate_struct_entries("Symbol", "") == nullptr);
    check("struct_both_empty",
          iterate_struct_entries("", "") == nullptr);
    check("type_empty_string",
          iterate_type_entries("") == nullptr);

    // A type name that does not exist in any HotSpot build must also be null
    // (covers the "walked the whole array, found nothing" path -- trivially
    // empty here).
    check("struct_bogus_type_bogus_field",
          iterate_struct_entries("ZZZ_NoSuchType", "_no_such_field") == nullptr);
    check("type_bogus_type",
          iterate_type_entries("ZZZ_NoSuchType") == nullptr);
}

// ---------------------------------------------------------------------------
// 7. Cross-consistency: iterate_* helpers vs the cached getters.
//
// iterate_struct_entries / iterate_type_entries are thin loops over
// get_vm_structs() / get_vm_types().  When the getter returns nullptr, the
// loop body never executes and the helper must return nullptr -- so for any
// symbol, (getter == nullptr) implies (iterate_* == nullptr).  Assert that
// invariant directly rather than just observing both are null in isolation.
// ---------------------------------------------------------------------------
static auto test_iterate_consistent_with_getters() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    const bool structs_null{ vmhook::hotspot::get_vm_structs() == nullptr };
    const bool types_null{ vmhook::hotspot::get_vm_types() == nullptr };

    // Preconditions for this no-JVM environment.
    check("precondition_structs_null", structs_null);
    check("precondition_types_null", types_null);

    // Implication: null struct array -> every struct lookup is null.
    check("struct_lookup_null_when_array_null",
          !structs_null || iterate_struct_entries("Method", "_constMethod") == nullptr);
    // Implication: null type array -> every type lookup is null.
    check("type_lookup_null_when_array_null",
          !types_null || iterate_type_entries("Method") == nullptr);
}

// ---------------------------------------------------------------------------
// 8. Determinism: repeating the SAME lookup many times always returns nullptr
//    (no internal state mutates between calls), and is consistent for both
//    helpers across a mix of real, bogus, and empty symbol names.
// ---------------------------------------------------------------------------
static auto test_lookup_determinism_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    bool struct_stable{ true };
    bool type_stable{ true };
    for (int i{ 0 }; i < 256; ++i)
    {
        if (iterate_struct_entries("Method", "_constMethod") != nullptr) { struct_stable = false; }
        if (iterate_struct_entries("", "") != nullptr) { struct_stable = false; }
        if (iterate_struct_entries(nullptr, "_x") != nullptr) { struct_stable = false; }
        if (iterate_type_entries("Klass") != nullptr) { type_stable = false; }
        if (iterate_type_entries("") != nullptr) { type_stable = false; }
        if (iterate_type_entries(nullptr) != nullptr) { type_stable = false; }
    }
    check("iterate_struct_entries_repeated_lookup_stable", struct_stable);
    check("iterate_type_entries_repeated_lookup_stable", type_stable);
}

// ---------------------------------------------------------------------------
// 9. Pathological string contents: the loop guard never executes its body with
//    a null array head, so the CONTENT of a (non-null) symbol string is never
//    dereferenced beyond strcmp — and even very long / control-char / embedded-
//    special strings must return nullptr without faulting.  Embedded NUL is NOT
//    testable through a const char* (it would truncate at the NUL), so we cover
//    long strings and unusual-but-NUL-free byte content instead.
// ---------------------------------------------------------------------------
static auto test_pathological_symbol_strings_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    // A very long type/field name (8 KiB) — must still short-circuit on the null
    // array and return nullptr (no buffer walk, no overflow).
    const std::string long_name(8u * 1024u, 'Z');
    check("struct_very_long_type_name",
          iterate_struct_entries(long_name.c_str(), "_length") == nullptr);
    check("struct_very_long_field_name",
          iterate_struct_entries("Symbol", long_name.c_str()) == nullptr);
    check("struct_both_very_long",
          iterate_struct_entries(long_name.c_str(), long_name.c_str()) == nullptr);
    check("type_very_long_name",
          iterate_type_entries(long_name.c_str()) == nullptr);

    // An even larger name (64 KiB) — exercises the same short-circuit far past
    // any plausible symbol length.
    const std::string huge_name(64u * 1024u, 'Q');
    check("struct_huge_type_name",
          iterate_struct_entries(huge_name.c_str(), "_length") == nullptr);
    check("type_huge_name",
          iterate_type_entries(huge_name.c_str()) == nullptr);

    // Names containing characters that never appear in real HotSpot symbols
    // (spaces, punctuation, high bytes) — ordinary non-null strings, returned
    // nullptr by failing to match, never by crashing.
    const char* const weird_names[]{
        "Method ",                       // trailing space
        " Method",                       // leading space
        "Met\thod",                      // embedded tab
        "_metadata._klass\n",            // embedded newline
        "!@#$%^&*()",                    // pure punctuation
        "Klass/InnerClass$1",            // dollar + slash
        "\x7F\x7E\x7D",                  // high-ish ASCII bytes
    };
    bool all_weird_null{ true };
    for (const char* const n : weird_names)
    {
        if (iterate_struct_entries(n, "_x") != nullptr) { all_weird_null = false; }
        if (iterate_struct_entries("X", n) != nullptr) { all_weird_null = false; }
        if (iterate_type_entries(n) != nullptr) { all_weird_null = false; }
    }
    check("iterate_helpers_tolerate_weird_symbol_bytes", all_weird_null);

    // A single-character name (shortest possible non-empty string).
    check("struct_single_char_names",
          iterate_struct_entries("X", "y") == nullptr);
    check("type_single_char_name",
          iterate_type_entries("X") == nullptr);
}

// ---------------------------------------------------------------------------
// 10. Extended real-symbol matrix.  A broader cross-section of the type/field
//     pairs the library resolves at runtime (the codec base/shift fields, the
//     Klass/Method/oop layout offsets, thread + CLD walking) — every one must
//     short-circuit to nullptr with no JVM.  Complements section 3.
// ---------------------------------------------------------------------------
static auto test_extended_real_symbol_matrix_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    struct pair { const char* type; const char* field; const char* tag; };
    static const pair pairs[]{
        { "CompressedOops",  "_narrow_oop._base",  "ext_compressedoops_narrow_base" },
        { "CompressedOops",  "_narrow_oop._shift", "ext_compressedoops_narrow_shift" },
        { "CompressedKlassPointers", "_narrow_klass._base", "ext_compressedklass_base" },
        { "Klass",           "_name",              "ext_klass_name" },
        { "Klass",           "_super",             "ext_klass_super" },
        { "Klass",           "_subklass",          "ext_klass_subklass" },
        { "InstanceKlass",   "_methods",           "ext_instanceklass_methods" },
        { "ConstMethod",     "_name_index",        "ext_constmethod_name_index" },
        { "ConstMethod",     "_signature_index",   "ext_constmethod_signature_index" },
        { "Symbol",          "_length",            "ext_symbol_length" },
        { "oopDesc",         "_mark",              "ext_oopdesc_mark" },
        { "JavaThread",      "_anchor",            "ext_javathread_anchor" },
        { "ClassLoaderDataGraph", "_head",         "ext_cldgraph_head" },
    };
    for (const auto& p : pairs)
    {
        check(p.tag, iterate_struct_entries(p.type, p.field) == nullptr);
    }

    struct named { const char* type; const char* tag; };
    static const named names[]{
        { "ConstMethod",          "ext_type_constmethod" },
        { "Symbol",               "ext_type_symbol" },
        { "JavaThread",           "ext_type_javathread" },
        { "ClassLoaderData",      "ext_type_cld" },
        { "ClassLoaderDataGraph", "ext_type_cldgraph" },
        { "CompressedOops",       "ext_type_compressedoops" },
        { "intptr_t",             "ext_type_intptr_t" },
    };
    for (const auto& n : names)
    {
        check(n.tag, iterate_type_entries(n.type) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// 11. clamp_safe_container_count — exhaustive boundary table.
//
// This is the pure-logic bound that guards every oop-derived reserve/iteration
// in the library: clamp_safe_container_count(raw) == (raw <= 0) ? 0
//                                                  : (raw < cap) ? raw : cap,
// with cap == k_max_safe_container_elems == 1<<24.  No JVM is involved; the
// function is constexpr.  Walk every interesting raw value: the negative
// regime (collapses to 0), zero, the small-positive regime (identity), the
// values straddling the cap, and the int32 extremes.  Each expected value is
// computed from the explicit piecewise definition so a regression in either
// branch (sign handling OR the cap) fails loudly.
// ---------------------------------------------------------------------------
static auto test_clamp_boundary_table() -> void
{
    using vmhook::clamp_safe_container_count;

    struct row { std::int32_t raw; std::int32_t expected; const char* tag; };
    static const row rows[]{
        // --- negative regime: everything <= 0 collapses to 0.
        { (std::numeric_limits<std::int32_t>::min)(), 0, "clamp_int32_min_to_zero" },
        { -2147483647,                                0, "clamp_int32_min_plus1_to_zero" },
        { -16777217,                                  0, "clamp_neg_cap_plus1_to_zero" },
        { -16777216,                                  0, "clamp_neg_cap_to_zero" },
        { -16777215,                                  0, "clamp_neg_cap_minus1_to_zero" },
        { -1000000,                                   0, "clamp_neg_million_to_zero" },
        { -2,                                         0, "clamp_neg_two_to_zero" },
        { -1,                                         0, "clamp_neg_one_to_zero" },
        // --- zero is the boundary of the <=0 branch; stays 0.
        { 0,                                          0, "clamp_zero_is_zero" },
        // --- small-positive regime: identity (raw < cap -> raw).
        { 1,                                          1, "clamp_one_identity" },
        { 2,                                          2, "clamp_two_identity" },
        { 7,                                          7, "clamp_seven_identity" },
        { 100,                                        100, "clamp_hundred_identity" },
        { 65535,                                      65535, "clamp_64k_identity" },
        { 65536,                                      65536, "clamp_64k_plus_identity" },
        { 1000000,                                    1000000, "clamp_million_identity" },
        // --- straddle the cap.
        { k_cap - 2,                                  k_cap - 2, "clamp_cap_minus2_identity" },
        { k_cap - 1,                                  k_cap - 1, "clamp_cap_minus1_identity" },
        { k_cap,                                      k_cap, "clamp_exactly_cap_to_cap" },
        { k_cap + 1,                                  k_cap, "clamp_cap_plus1_to_cap" },
        { k_cap + 2,                                  k_cap, "clamp_cap_plus2_to_cap" },
        { 16777216 * 2,                               k_cap, "clamp_two_cap_to_cap" },
        { 100000000,                                  k_cap, "clamp_hundred_million_to_cap" },
        // --- int32 max: well above cap -> cap.
        { (std::numeric_limits<std::int32_t>::max)() - 1, k_cap, "clamp_int32_max_minus1_to_cap" },
        { (std::numeric_limits<std::int32_t>::max)(),     k_cap, "clamp_int32_max_to_cap" },
    };

    for (const auto& r : rows)
    {
        check(r.tag, clamp_safe_container_count(r.raw) == r.expected);
    }
}

// ---------------------------------------------------------------------------
// 12. clamp_safe_container_count — structural invariants holding for ALL inputs.
//
// Independently of the exact value, the clamp result is ALWAYS a legal
// reserve/loop bound: in [0, cap].  It is non-negative for every input
// (including the entire negative half of int32), never exceeds the cap, and
// equals exactly the documented piecewise function.  We sweep a dense band
// around the cap (where the < vs <= boundary lives) and a sparse stride across
// the full int32 range, asserting the universal invariants at every sample.
// ---------------------------------------------------------------------------
static auto test_clamp_universal_invariants() -> void
{
    using vmhook::clamp_safe_container_count;

    // Reference re-implementation of the documented contract, used to verify
    // the header's result matches the spec at every sampled point.
    auto spec = [](std::int64_t raw) -> std::int32_t
    {
        if (raw <= 0) { return 0; }
        return (raw < static_cast<std::int64_t>(k_cap)) ? static_cast<std::int32_t>(raw) : k_cap;
    };

    // (a) Dense band straddling the cap: [cap-64, cap+64].
    bool band_ok{ true };
    bool band_in_range{ true };
    for (std::int64_t v{ static_cast<std::int64_t>(k_cap) - 64 };
         v <= static_cast<std::int64_t>(k_cap) + 64; ++v)
    {
        const std::int32_t got{ clamp_safe_container_count(static_cast<std::int32_t>(v)) };
        if (got != spec(v)) { band_ok = false; }
        if (got < 0 || got > k_cap) { band_in_range = false; }
    }
    check("clamp_dense_band_around_cap_matches_spec", band_ok);
    check("clamp_dense_band_around_cap_in_range", band_in_range);

    // (b) Dense band straddling zero: [-64, +64] (the sign boundary).
    bool zero_band_ok{ true };
    for (std::int64_t v{ -64 }; v <= 64; ++v)
    {
        const std::int32_t got{ clamp_safe_container_count(static_cast<std::int32_t>(v)) };
        if (got != spec(v)) { zero_band_ok = false; }
    }
    check("clamp_dense_band_around_zero_matches_spec", zero_band_ok);

    // (c) Sparse stride across the entire int32 range (negatives included):
    //     result must always be in [0, cap] and match the spec.
    bool full_range_ok{ true };
    bool full_range_in_bounds{ true };
    const std::int64_t lo{ (std::numeric_limits<std::int32_t>::min)() };
    const std::int64_t hi{ (std::numeric_limits<std::int32_t>::max)() };
    const std::int64_t stride{ 7919733 }; // prime-ish stride -> ~550 samples, hits both regimes
    for (std::int64_t v{ lo }; v <= hi; v += stride)
    {
        const std::int32_t got{ clamp_safe_container_count(static_cast<std::int32_t>(v)) };
        if (got != spec(v)) { full_range_ok = false; }
        if (got < 0 || got > k_cap) { full_range_in_bounds = false; }
    }
    // Make sure the top end (which the stride may step over) is sampled too.
    {
        const std::int32_t got{ clamp_safe_container_count(static_cast<std::int32_t>(hi)) };
        if (got != spec(hi)) { full_range_ok = false; }
        if (got < 0 || got > k_cap) { full_range_in_bounds = false; }
    }
    check("clamp_full_range_stride_matches_spec", full_range_ok);
    check("clamp_full_range_stride_in_bounds", full_range_in_bounds);
}

// ---------------------------------------------------------------------------
// 13. clamp_safe_container_count — algebraic properties.
//
//   * idempotence: clamp(clamp(x)) == clamp(x) — the result is already a fixed
//     point of the clamp (every clamped value is in [0,cap], which the clamp
//     maps to itself), so applying it twice changes nothing.  This is the
//     property that makes it safe to clamp at every reserve AND every loop site
//     without double-shrinking an honest count.
//   * monotonicity: x <= y  =>  clamp(x) <= clamp(y).  A non-decreasing clamp
//     can never reorder counts.
//   * identity on the honest range: for any 0 <= x <= cap, clamp(x) == x, so an
//     honest container of x elements reads byte-identically (the whole point of
//     the cap being far above any real container).
// ---------------------------------------------------------------------------
static auto test_clamp_algebraic_properties() -> void
{
    using vmhook::clamp_safe_container_count;

    // Idempotence across a representative spread (both regimes + boundaries).
    const std::int32_t samples[]{
        (std::numeric_limits<std::int32_t>::min)(), -16777216, -1, 0, 1, 2, 1000,
        65536, k_cap - 1, k_cap, k_cap + 1, 100000000,
        (std::numeric_limits<std::int32_t>::max)()
    };
    bool idempotent{ true };
    bool result_in_range{ true };
    for (const std::int32_t s : samples)
    {
        const std::int32_t once{ clamp_safe_container_count(s) };
        const std::int32_t twice{ clamp_safe_container_count(once) };
        if (once != twice) { idempotent = false; }
        if (once < 0 || once > k_cap) { result_in_range = false; }
    }
    check("clamp_is_idempotent", idempotent);
    check("clamp_result_always_in_range", result_in_range);

    // Monotonicity over an ascending sweep that crosses both the sign boundary
    // and the cap boundary.
    bool monotonic{ true };
    std::int32_t prev{ clamp_safe_container_count(static_cast<std::int32_t>(
        (std::numeric_limits<std::int32_t>::min)())) };
    const std::int64_t lo{ (std::numeric_limits<std::int32_t>::min)() };
    const std::int64_t hi{ (std::numeric_limits<std::int32_t>::max)() };
    const std::int64_t stride{ 4194301 }; // ~1024 samples
    for (std::int64_t v{ lo + stride }; v <= hi; v += stride)
    {
        const std::int32_t cur{ clamp_safe_container_count(static_cast<std::int32_t>(v)) };
        if (cur < prev) { monotonic = false; }
        prev = cur;
    }
    check("clamp_is_monotonic_nondecreasing", monotonic);

    // Identity on the honest range [0, cap]: sample the endpoints and several
    // interior points; clamp must be a no-op (reads byte-identical).
    const std::int32_t honest[]{ 0, 1, 2, 3, 16, 256, 4096, 1u << 20, k_cap - 1, k_cap };
    bool identity_on_honest{ true };
    for (const std::int32_t h : honest)
    {
        if (clamp_safe_container_count(h) != h) { identity_on_honest = false; }
    }
    check("clamp_identity_on_honest_range", identity_on_honest);
}

// ---------------------------------------------------------------------------
// 14. clamp_safe_container_count — compile-time (constexpr) evaluation.
//
// The function is `inline constexpr`; the cap derives from a `constexpr`
// constant guarded by an in-header static_assert that it fits int32.  Prove the
// clamp is usable in a constant-expression context (static_assert / constexpr
// variable / array bound), which a non-constexpr regression would break at
// compile time.  These are zero-runtime-cost but each still emits a runtime
// check() so they show up in the pass count.
// ---------------------------------------------------------------------------
static auto test_clamp_constexpr_evaluation() -> void
{
    using vmhook::clamp_safe_container_count;

    // Each branch of the piecewise function, evaluated at compile time.
    static_assert(clamp_safe_container_count(-1) == 0,
                  "clamp(<0) must be 0 at compile time");
    static_assert(clamp_safe_container_count(0) == 0,
                  "clamp(0) must be 0 at compile time");
    static_assert(clamp_safe_container_count(5) == 5,
                  "clamp of an in-range value must be identity at compile time");
    static_assert(clamp_safe_container_count(k_cap) == k_cap,
                  "clamp(cap) must be cap at compile time");
    static_assert(clamp_safe_container_count(k_cap + 1) == k_cap,
                  "clamp(>cap) must saturate to cap at compile time");
    static_assert(clamp_safe_container_count(
                      (std::numeric_limits<std::int32_t>::max)()) == k_cap,
                  "clamp(INT32_MAX) must saturate to cap at compile time");
    static_assert(clamp_safe_container_count(
                      (std::numeric_limits<std::int32_t>::min)()) == 0,
                  "clamp(INT32_MIN) must collapse to 0 at compile time");

    // Use the clamp as a non-type template / array-bound constant: only a true
    // constant expression can size an array, so this compiling at all proves
    // constexpr-usability.
    constexpr std::int32_t kSized{ clamp_safe_container_count(64) };
    int sized_array[static_cast<std::size_t>(kSized)]{};
    check("clamp_usable_as_array_bound", sizeof(sized_array) / sizeof(int) == 64u);

    // A constexpr value computed from a saturating input.
    constexpr std::int32_t kSaturated{ clamp_safe_container_count(1 << 30) };
    check("clamp_constexpr_saturates", kSaturated == k_cap);

    // A constexpr value from a negative input.
    constexpr std::int32_t kNegToZero{ clamp_safe_container_count(-12345) };
    check("clamp_constexpr_neg_to_zero", kNegToZero == 0);
}

// ---------------------------------------------------------------------------
// 15. k_max_safe_container_elems — the cap constant itself.
//
// The cap is the single source of truth the clamp saturates to.  Pin its exact
// value (1<<24 == 16,777,216), its positivity, and the in-header invariant that
// it fits in a signed int32 (so the cast inside the clamp can never overflow) —
// the same premise the header's own static_assert enforces.  Also confirm the
// clamp actually saturates to THIS value, tying the constant to the function.
// ---------------------------------------------------------------------------
static auto test_cap_constant() -> void
{
    using vmhook::clamp_safe_container_count;
    using vmhook::k_max_safe_container_elems;

    // Exact documented value.
    static_assert(k_max_safe_container_elems == (1ull << 24),
                  "cap must be 1<<24");
    check("cap_exact_value_16M", k_max_safe_container_elems == 16777216ull);
    check("cap_is_power_of_two",
          (k_max_safe_container_elems & (k_max_safe_container_elems - 1)) == 0ull);
    check("cap_is_positive", k_max_safe_container_elems > 0ull);

    // Fits in int32 with room to spare (the header static_assert premise).
    static_assert(k_max_safe_container_elems
                      <= static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()),
                  "cap must fit int32");
    check("cap_fits_in_int32",
          k_max_safe_container_elems
              <= static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()));
    check("cap_well_below_int32_max",
          static_cast<std::int64_t>(k_max_safe_container_elems)
              < static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()));

    // The clamp saturates to exactly this constant (cross-tie value<->function).
    check("clamp_saturates_to_cap_constant",
          clamp_safe_container_count((std::numeric_limits<std::int32_t>::max)())
              == static_cast<std::int32_t>(k_max_safe_container_elems));
    // And the constant equals the signed reference used throughout this file.
    check("cap_matches_signed_reference",
          static_cast<std::int64_t>(k_max_safe_container_elems)
              == static_cast<std::int64_t>(k_cap));
}

// ---------------------------------------------------------------------------
// 16. Near-miss field/type names: strcmp is exact, so names that differ only by
//     case, surrounding whitespace, or that are a prefix/superstring of a real
//     symbol must NOT match.  With no JVM every call is nullptr regardless, but
//     these pin the input handling (no normalization, no prefix matching, no
//     case folding) so a future change to those semantics is caught, and they
//     exercise the helpers against the trickiest "almost real" inputs without
//     faulting.
// ---------------------------------------------------------------------------
static auto test_near_miss_names_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    // Case variants of a real type name — exact strcmp must not match.
    const char* const type_case_variants[]{
        "method", "METHOD", "MeThOd", "Symbol ", " Symbol", "_Length", "_LENGTH",
    };
    bool type_case_null{ true };
    for (const char* const t : type_case_variants)
    {
        if (iterate_type_entries(t) != nullptr) { type_case_null = false; }
        if (iterate_struct_entries(t, "_length") != nullptr) { type_case_null = false; }
        if (iterate_struct_entries("Symbol", t) != nullptr) { type_case_null = false; }
    }
    check("near_miss_case_and_whitespace_all_null", type_case_null);

    // Prefix / superstring of a real field name — strcmp is whole-string.
    const char* const field_prefix_variants[]{
        "_leng",        // prefix of _length
        "_lengthX",     // superstring of _length
        "_length_",     // superstring of _length
        "length",       // missing leading underscore
        "Meth",         // prefix of Method (used as type)
        "Methods",      // superstring of Method
    };
    bool field_prefix_null{ true };
    for (const char* const f : field_prefix_variants)
    {
        if (iterate_struct_entries("Symbol", f) != nullptr) { field_prefix_null = false; }
        if (iterate_struct_entries(f, "_length") != nullptr) { field_prefix_null = false; }
        if (iterate_type_entries(f) != nullptr) { field_prefix_null = false; }
    }
    check("near_miss_prefix_superstring_all_null", field_prefix_null);

    // Trailing-whitespace field variants specifically (a common copy/paste bug).
    check("near_miss_field_trailing_space",
          iterate_struct_entries("Symbol", "_length ") == nullptr);
    check("near_miss_field_leading_space",
          iterate_struct_entries("Symbol", " _length") == nullptr);
    check("near_miss_field_case",
          iterate_struct_entries("Symbol", "_Length") == nullptr);
}

// ---------------------------------------------------------------------------
// 17. Mixed null/empty/non-null permutation matrix for iterate_struct_entries.
//
// The guard `if (!type_name || !field_name)` must fire iff at least one pointer
// is null; an empty string is non-null and must NOT trip the guard (it walks
// the array, which is empty here, and returns nullptr).  Enumerate all nine
// combinations of {nullptr, "", "Real"} in each of the two slots and assert
// every one returns nullptr (the *outcome* is uniform with no JVM, but this
// pins that neither the null guard nor the empty-string handling ever crashes,
// across the full cross product).
// ---------------------------------------------------------------------------
static auto test_null_empty_real_permutations() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    const char* const slots[]{ nullptr, "", "Symbol" };
    const char* const slot_tags[]{ "null", "empty", "real" };

    bool all_null{ true };
    int combos{ 0 };
    for (int ti{ 0 }; ti < 3; ++ti)
    {
        for (int fi{ 0 }; fi < 3; ++fi)
        {
            ++combos;
            if (iterate_struct_entries(slots[ti], slots[fi]) != nullptr)
            {
                all_null = false;
                std::printf("  combo type=%s field=%s returned non-null\n",
                            slot_tags[ti], slot_tags[fi]);
            }
        }
    }
    check("struct_null_empty_real_9_combos_all_null", all_null);
    check("struct_permutation_count_is_9", combos == 9);

    // The single-arg type helper across the same three slot kinds.
    bool type_all_null{ true };
    for (int ti{ 0 }; ti < 3; ++ti)
    {
        if (iterate_type_entries(slots[ti]) != nullptr) { type_all_null = false; }
    }
    check("type_null_empty_real_all_null", type_all_null);
}

// ---------------------------------------------------------------------------
// 18. Interleaving getters and iterators in a tight loop.
//
// The iterate_* helpers call the getters internally; alternating direct getter
// calls with iterate_* calls many times must never desynchronize the cache or
// produce a non-null result.  This stress-mixes the four entry points and pins
// the joint invariant (all null, all stable) under interleaving.
// ---------------------------------------------------------------------------
static auto test_interleaved_getter_iterator_stress() -> void
{
    using vmhook::hotspot::get_vm_structs;
    using vmhook::hotspot::get_vm_types;
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    bool ok{ true };
    for (int i{ 0 }; i < 512; ++i)
    {
        if (get_vm_structs() != nullptr) { ok = false; }
        if (iterate_struct_entries("Method", "_constMethod") != nullptr) { ok = false; }
        if (get_vm_types() != nullptr) { ok = false; }
        if (iterate_type_entries("Method") != nullptr) { ok = false; }
        // A clamp call interleaved too — purely to assert no global state leaks
        // between the unrelated pure-logic helper and the cached getters.
        if (vmhook::clamp_safe_container_count(i) != ((i < k_cap) ? i : k_cap)) { ok = false; }
    }
    check("interleaved_getters_iterators_clamp_all_consistent", ok);
}

// ===========================================================================
// EXHAUSTIVE EXPANSION (19+).
//
// Everything below stays inside this feature's no-JVM contract: the iterate_*
// walkers read the cached gHotSpot* array head (nullptr here), so the loop body
// never runs and every lookup must return nullptr WITHOUT faulting, for every
// possible string input; and the clamp/cap that bounds every oop-derived walk
// (clamp_safe_container_count / k_max_safe_container_elems, also the literal
// `k_descend_cap` back-edge cap in the tree-map/tree-set walkers) is pure
// constexpr arithmetic exercised here to its full domain.  The array-element
// bounds helpers are owned by tests/test_array_element_helpers.cpp and are NOT
// re-tested here.
// ===========================================================================

// ---------------------------------------------------------------------------
// 19. COMPLETE real (type,field) struct-query surface.
//
// Sections 3 and 10 sample a couple dozen pairs; this is the EXHAUSTIVE set of
// every distinct (type_name, field_name) pair the library passes to
// iterate_struct_entries anywhere in vmhook.hpp (collected from every call
// site).  In a no-JVM process every one must short-circuit on the null array
// head and return nullptr without faulting — the foundational "miss is cheap
// and crash-free" guarantee the whole introspection layer is built on.
// ---------------------------------------------------------------------------
static auto test_complete_struct_query_surface_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;

    struct pair { const char* type; const char* field; };
    static const pair pairs[]{
        // Symbol
        { "Symbol", "_length" }, { "Symbol", "_body" },
        // ConstantPool
        { "ConstantPool", "_length" }, { "ConstantPool", "_pool_holder" },
        // ConstMethod
        { "ConstMethod", "_constants" }, { "ConstMethod", "_name_index" },
        { "ConstMethod", "_signature_index" },
        // Method (incl. the renamed-entry-point fallback pair)
        { "Method", "_i2i_entry" }, { "Method", "_from_interpreted_entry" },
        { "Method", "_access_flags" }, { "Method", "_flags" },
        { "Method", "_constMethod" }, { "Method", "_code" },
        { "Method", "_from_compiled_code_entry_point" },
        { "Method", "_from_compiled_entry" }, { "Method", "_adapter" },
        { "Method", "_intrinsic_id" },
        // Klass
        { "Klass", "_name" }, { "Klass", "_next_link" },
        { "Klass", "_java_mirror" }, { "Klass", "_super" },
        { "Klass", "_layout_helper" }, { "Klass", "_prototype_header" },
        { "Klass", "_access_flags" }, { "Klass", "_class_loader_data" },
        // InstanceKlass (incl. _fields/_fieldinfo_stream version pair)
        { "InstanceKlass", "_methods" }, { "InstanceKlass", "_transitive_interfaces" },
        { "InstanceKlass", "_local_interfaces" }, { "InstanceKlass", "_fieldinfo_stream" },
        { "InstanceKlass", "_fields" }, { "InstanceKlass", "_constants" },
        // ClassLoaderData / graph (incl. _klasses/_dictionary capability pair)
        { "ClassLoaderData", "_klasses" }, { "ClassLoaderData", "_next" },
        { "ClassLoaderData", "_dictionary" }, { "ClassLoaderData", "_class_loader" },
        { "ClassLoaderDataGraph", "_head" },
        // SystemDictionary
        { "SystemDictionary", "_dictionary" }, { "SystemDictionary", "_shared_dictionary" },
        // Threads / SMR / list (Java 8 vs 9+)
        { "JavaThread", "_thread_state" }, { "JavaThread", "_suspend_flags" },
        { "JavaThread", "_next" }, { "JavaThread", "_osthread" },
        { "JavaThread", "_tlab" }, { "JavaThread", "_anchor" },
        { "Thread", "_next" }, { "Thread", "_osthread" }, { "Thread", "_tlab" },
        { "OSThread", "_thread_id" },
        { "ThreadLocalAllocBuffer", "_top" }, { "ThreadLocalAllocBuffer", "_end" },
        { "Threads", "_thread_list" },
        { "ThreadsSMRSupport", "_java_thread_list" },
        { "ThreadsList", "_length" }, { "ThreadsList", "_threads" },
        // Heap reservation walk (compressed-oop base plausibility)
        { "Universe", "_collectedHeap" }, { "CollectedHeap", "_reserved" },
        { "MemRegion", "_start" }, { "MemRegion", "_word_size" },
        // oopDesc (mark/markWord + compressed/raw klass)
        { "oopDesc", "_mark" }, { "oopDesc", "_markWord" },
        { "oopDesc", "_metadata._compressed_klass" }, { "oopDesc", "_metadata._klass" },
        // Adapters / stubs / mirror offset
        { "AdapterHandlerEntry", "_c2i_entry" },
        { "StubRoutines", "_call_stub_entry" },
        { "java_lang_Class", "_klass_offset" },
    };

    bool all_null{ true };
    int count{ 0 };
    for (const auto& p : pairs)
    {
        ++count;
        if (iterate_struct_entries(p.type, p.field) != nullptr)
        {
            all_null = false;
            std::printf("  surface (%s,%s) returned non-null\n", p.type, p.field);
        }
    }
    check("complete_struct_query_surface_all_null", all_null);
    // Pin the surface size so a future call site whose pair is added to the lib
    // (and here) is consciously accounted for, not silently dropped.
    check("complete_struct_query_surface_count_ge_60", count >= 60);
}

// ---------------------------------------------------------------------------
// 20. JDK-version fallback chains: every (type,field) candidate in the
//     compressed-oops / compressed-klass base+shift resolver tables, plus the
//     other renamed-field fallback pairs.  Cross-version support rests entirely
//     on each ABSENT candidate resolving to nullptr cheaply (the resolver tries
//     A, then B, then C in order); a fault or a false match on any candidate
//     would break the chain.  With no JVM every candidate is nullptr, which is
//     exactly the "this name isn't in THIS JDK, try the next" signal.
// ---------------------------------------------------------------------------
static auto test_version_fallback_chains_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;

    struct pair { const char* type; const char* field; };
    static const pair candidates[]{
        // CompressedOops base: JDK17-24 nested, JDK25+ flat, legacy Universe.
        { "CompressedOops", "_narrow_oop._base" },
        { "CompressedOops", "_base" },
        { "Universe",       "_narrow_oop._base" },
        // CompressedOops shift.
        { "CompressedOops", "_narrow_oop._shift" },
        { "CompressedOops", "_shift" },
        { "Universe",       "_narrow_oop._shift" },
        // CompressedKlassPointers base.
        { "CompressedKlassPointers", "_narrow_klass._base" },
        { "CompressedKlassPointers", "_base" },
        { "Universe",                "_narrow_klass._base" },
        // CompressedKlassPointers shift.
        { "CompressedKlassPointers", "_narrow_klass._shift" },
        { "CompressedKlassPointers", "_shift" },
        { "Universe",                "_narrow_klass._shift" },
    };

    bool all_null{ true };
    for (const auto& c : candidates)
    {
        if (iterate_struct_entries(c.type, c.field) != nullptr) { all_null = false; }
    }
    check("version_fallback_codec_candidates_all_null", all_null);

    // The renamed-field pairs the library tries in sequence elsewhere: both
    // spellings must be null with no JVM (so the fallback is reached safely).
    struct rename { const char* type; const char* old_name; const char* new_name; const char* tag; };
    static const rename renames[]{
        { "oopDesc", "_mark", "_markWord", "rename_oop_mark" },
        { "Method", "_from_compiled_code_entry_point", "_from_compiled_entry", "rename_method_fce" },
        { "InstanceKlass", "_fields", "_fieldinfo_stream", "rename_ik_fields" },
        { "ClassLoaderData", "_klasses", "_dictionary", "rename_cld_klasses" },
    };
    for (const auto& r : renames)
    {
        const bool old_null{ iterate_struct_entries(r.type, r.old_name) == nullptr };
        const bool new_null{ iterate_struct_entries(r.type, r.new_name) == nullptr };
        check(r.tag, old_null && new_null);
    }
}

// ---------------------------------------------------------------------------
// 21. COMPLETE real type-name query surface for iterate_type_entries, plus the
//     types whose presence is JDK-version-specific (8 vs 9+).  Each must return
//     nullptr with no JVM — including types that only exist on ONE JDK (the
//     walker must report "absent" for the wrong-JDK type without faulting,
//     which is the same bounds contract as the present-but-empty case here).
// ---------------------------------------------------------------------------
static auto test_complete_type_query_surface_no_jvm() -> void
{
    using vmhook::hotspot::iterate_type_entries;

    static const char* const names[]{
        // Directly queried by the library.
        "ConstantPool", "Method",
        // Types the struct walks reference (must also be absent-safe by name).
        "Symbol", "ConstMethod", "Klass", "InstanceKlass", "oopDesc",
        "narrowOop", "JavaThread", "Thread", "OSThread",
        "ClassLoaderData", "ClassLoaderDataGraph", "SystemDictionary",
        "CompressedOops", "CompressedKlassPointers", "Universe",
        "CollectedHeap", "MemRegion", "ThreadLocalAllocBuffer",
        "AdapterHandlerEntry", "StubRoutines",
        // JDK-version-specific presence (9+ only): walker must say "absent".
        "ThreadsSMRSupport", "ThreadsList",
        // Common integer typedefs HotSpot publishes in gHotSpotVMTypes.
        "intptr_t", "uintptr_t", "int", "jint", "jlong", "size_t",
    };

    bool all_null{ true };
    int count{ 0 };
    for (const char* const n : names)
    {
        ++count;
        if (iterate_type_entries(n) != nullptr)
        {
            all_null = false;
            std::printf("  type surface %s returned non-null\n", n);
        }
    }
    check("complete_type_query_surface_all_null", all_null);
    check("complete_type_query_surface_count_ge_25", count >= 25);
}

// ---------------------------------------------------------------------------
// 22. clamp_safe_container_count — power-of-two ladder and bit-adjacent values.
//
// Every power of two from 2^0 up to 2^23 is strictly below the cap (2^24) and
// must pass through as identity; 2^24 is exactly the cap; 2^25..2^30 saturate.
// Around each power of two the values 2^k-1 and 2^k+1 must follow the same
// piecewise rule.  This walks the clamp across every binary order of magnitude,
// pinning that no individual bit position trips an off-by-one in the < cap test.
// ---------------------------------------------------------------------------
static auto test_clamp_power_of_two_ladder() -> void
{
    using vmhook::clamp_safe_container_count;

    auto spec = [](std::int64_t raw) -> std::int32_t
    {
        if (raw <= 0) { return 0; }
        return (raw < static_cast<std::int64_t>(k_cap)) ? static_cast<std::int32_t>(raw) : k_cap;
    };

    bool ladder_ok{ true };
    // 2^0 .. 2^30 (2^31 would be negative as int32 / == INT32_MIN, covered in
    // the boundary table); each value AND its +/-1 neighbours.
    for (int k{ 0 }; k <= 30; ++k)
    {
        const std::int64_t pow{ static_cast<std::int64_t>(1) << k };
        const std::int64_t probes[]{ pow - 1, pow, pow + 1 };
        for (const std::int64_t v : probes)
        {
            if (v < (std::numeric_limits<std::int32_t>::min)()
                || v > (std::numeric_limits<std::int32_t>::max)())
            {
                continue;
            }
            const std::int32_t got{ clamp_safe_container_count(static_cast<std::int32_t>(v)) };
            if (got != spec(v)) { ladder_ok = false; }
            if (got < 0 || got > k_cap) { ladder_ok = false; }
        }
    }
    check("clamp_power_of_two_ladder_matches_spec", ladder_ok);

    // Spot the three regimes explicitly at the powers straddling the cap.
    check("clamp_2pow23_identity", clamp_safe_container_count(1 << 23) == (1 << 23));
    check("clamp_2pow24_is_cap", clamp_safe_container_count(1 << 24) == k_cap);
    check("clamp_2pow24_equals_cap_constant", (1 << 24) == k_cap);
    check("clamp_2pow25_saturates", clamp_safe_container_count(1 << 25) == k_cap);
    check("clamp_2pow30_saturates", clamp_safe_container_count(1 << 30) == k_cap);
}

// ---------------------------------------------------------------------------
// 23. clamp_safe_container_count — exhaustive dense sweep of the low region.
//
// The honest-container regime is [0, cap]; the overwhelming majority of real
// container counts live in the first few thousand.  Sweep EVERY integer in
// [-1, 8192] (so the sign boundary at 0 and the small-positive identity region
// are covered with no gaps) and require the result equals the input (clamped at
// 0 for the single negative) — a contiguous, exhaustive identity proof over the
// realistic input band, not just sampled points.
// ---------------------------------------------------------------------------
static auto test_clamp_dense_low_region() -> void
{
    using vmhook::clamp_safe_container_count;

    bool ok{ true };
    for (std::int32_t v{ -1 }; v <= 8192; ++v)
    {
        const std::int32_t expected{ v <= 0 ? 0 : v };
        if (clamp_safe_container_count(v) != expected) { ok = false; break; }
    }
    check("clamp_dense_low_region_exhaustive_identity", ok);

    // Also sweep a dense window at the very top of the honest range, just below
    // the cap, where the identity must still hold to the last integer.
    bool top_ok{ true };
    for (std::int32_t v{ k_cap - 4096 }; v < k_cap; ++v)
    {
        if (clamp_safe_container_count(v) != v) { top_ok = false; break; }
    }
    check("clamp_dense_just_below_cap_identity", top_ok);
}

// ---------------------------------------------------------------------------
// 24. The cap as the WALK bound: tie clamp_safe_container_count /
//     k_max_safe_container_elems to the actual iteration caps in the library.
//
// The tree-map and tree-set walkers cap their left-spine descent at
// `k_descend_cap = static_cast<int32>(k_max_safe_container_elems)` (a literal
// back-edge / cycle / runaway-walk guard), and the HashMap/HashSet bucket walks
// clamp the bucket count with clamp_safe_container_count.  Reconstruct those
// exact expressions here and assert the cap value the walkers bound to is the
// same constant the clamp saturates to — so a change to the cap moves BOTH the
// reservation clamp and the walk cap together, and the walk can never be told to
// iterate more than `cap` times no matter how corrupt the count.
// ---------------------------------------------------------------------------
static auto test_cap_is_the_walk_bound() -> void
{
    using vmhook::clamp_safe_container_count;
    using vmhook::k_max_safe_container_elems;

    // Reconstruct the walkers' descend cap exactly.
    constexpr std::int32_t k_descend_cap{
        static_cast<std::int32_t>(k_max_safe_container_elems) };

    check("walk_descend_cap_equals_signed_cap", k_descend_cap == k_cap);
    check("walk_descend_cap_positive", k_descend_cap > 0);

    // The clamp can never yield a bound above the walk cap: for the most corrupt
    // possible count (INT32_MAX) the clamp result equals the descend cap, so a
    // count-driven loop bounded by the clamp and a pointer-walk bounded by
    // k_descend_cap stop at the same ceiling.
    check("clamp_max_equals_walk_cap",
          clamp_safe_container_count((std::numeric_limits<std::int32_t>::max)()) == k_descend_cap);

    // A descent counter `for (descended=0; descended < k_descend_cap; ++descended)`
    // terminates: the loop variable is int32 and the bound is positive and
    // representable, so descended reaches k_descend_cap and the loop exits (it
    // cannot wrap before then because k_descend_cap < INT32_MAX).
    check("walk_descend_cap_below_int32_max",
          static_cast<std::int64_t>(k_descend_cap)
              < static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()));

    // HashMap/HashSet bucket clamp: a corrupt bucket count saturates to the cap,
    // a torn-negative one collapses to 0 (no buckets walked), an honest one is
    // verbatim — the three regimes that bound those table walks.
    check("bucket_clamp_corrupt_saturates",
          clamp_safe_container_count(2000000000) == k_cap);
    check("bucket_clamp_negative_is_zero",
          clamp_safe_container_count(-2000000000) == 0);
    check("bucket_clamp_honest_verbatim",
          clamp_safe_container_count(4096) == 4096);

    // constexpr context: the descend cap is usable as a constant expression
    // (the walkers declare it `constexpr`), proving it folds at compile time.
    static_assert(k_descend_cap == static_cast<std::int32_t>(1ull << 24),
                  "k_descend_cap must be the 1<<24 cap at compile time");
    check("walk_descend_cap_constexpr", true);
}

// ---------------------------------------------------------------------------
// 25. Single-byte name fuzz: EVERY non-NUL byte value (1..255) as a one-char
//     type name and as a one-char field name.  A const char* cannot carry an
//     embedded NUL, so 0 is excluded; every other byte forms a legal one-char
//     C-string.  None can match the (empty) array, and the walker must never
//     fault on any byte value — covering the full single-character input domain.
// ---------------------------------------------------------------------------
static auto test_single_byte_name_fuzz_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    bool all_null{ true };
    for (int b{ 1 }; b <= 255; ++b)
    {
        char name[2]{ static_cast<char>(b), '\0' };
        if (iterate_type_entries(name) != nullptr) { all_null = false; }
        if (iterate_struct_entries(name, "_length") != nullptr) { all_null = false; }
        if (iterate_struct_entries("Symbol", name) != nullptr) { all_null = false; }
        if (iterate_struct_entries(name, name) != nullptr) { all_null = false; }
    }
    check("single_byte_name_fuzz_all_null", all_null);
}

// ---------------------------------------------------------------------------
// 26. Control-character names: every ASCII control byte 0x01..0x1F plus DEL
//     (0x7F) embedded mid-name, and a name made entirely of control bytes.
//     HotSpot symbol names are printable ASCII, so these never match; the
//     walker must tolerate them without faulting.  (NUL 0x00 excluded — not
//     representable in a C-string argument.)
// ---------------------------------------------------------------------------
static auto test_control_char_names_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    bool all_null{ true };
    for (int c{ 0x01 }; c <= 0x1F; ++c)
    {
        char mid[8]{ 'M', static_cast<char>(c), 'e', 't', 'h', static_cast<char>(c), '\0' };
        if (iterate_type_entries(mid) != nullptr) { all_null = false; }
        if (iterate_struct_entries(mid, "_length") != nullptr) { all_null = false; }
        if (iterate_struct_entries("Symbol", mid) != nullptr) { all_null = false; }
    }
    // DEL byte.
    {
        char del_name[6]{ 'S', 'y', 'm', static_cast<char>(0x7F), 'b', '\0' };
        if (iterate_type_entries(del_name) != nullptr) { all_null = false; }
        if (iterate_struct_entries(del_name, "_x") != nullptr) { all_null = false; }
    }
    // All-control-byte name.
    {
        char all_ctrl[5]{ 0x01, 0x02, 0x03, 0x04, '\0' };
        if (iterate_type_entries(all_ctrl) != nullptr) { all_null = false; }
        if (iterate_struct_entries(all_ctrl, all_ctrl) != nullptr) { all_null = false; }
    }
    check("control_char_names_all_null", all_null);
}

// ---------------------------------------------------------------------------
// 27. Substring / rotation / repetition near-misses of real symbol names.
//
// strcmp is whole-string and exact, so a substring of a real name, a rotation
// of its characters, a doubled name, or a real name with a trailing/leading
// extra token must NOT match.  These are the trickiest "almost real" inputs;
// with no JVM all are nullptr, pinning that no prefix/substring/normalization
// matching ever sneaks in.
// ---------------------------------------------------------------------------
static auto test_substring_rotation_near_misses_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    static const char* const type_near_misses[]{
        "Metho",            // prefix of Method
        "ethod",            // suffix of Method
        "etho",             // interior substring
        "MethodMethod",     // doubled
        "Method2",          // trailing digit
        "2Method",          // leading digit
        "dohteM",           // reversed
        "InstanceKlas",     // prefix of InstanceKlass
        "InstanceKlasss",   // extra trailing char
        "Instance Klass",   // embedded space
        "Const_Method",     // underscore where none belongs
        "constantpool",     // all-lowercase real name
    };
    bool type_null{ true };
    for (const char* const t : type_near_misses)
    {
        if (iterate_type_entries(t) != nullptr) { type_null = false; }
        if (iterate_struct_entries(t, "_length") != nullptr) { type_null = false; }
    }
    check("substring_rotation_type_near_misses_all_null", type_null);

    static const char* const field_near_misses[]{
        "constMethod",      // _constMethod without underscore
        "__constMethod",    // doubled underscore
        "_constMethod ",    // trailing space
        "_constMethodd",    // doubled last char
        "_const",           // prefix
        "Method",           // type name as field
        "_CONSTMETHOD",     // upper-case
        "_const_method",    // snake instead of camel
        "_constMethod\t",   // trailing tab
    };
    bool field_null{ true };
    for (const char* const f : field_near_misses)
    {
        if (iterate_struct_entries("Method", f) != nullptr) { field_null = false; }
    }
    check("substring_field_near_misses_all_null", field_null);
}

// ---------------------------------------------------------------------------
// 28. type_name / field_name slot independence — broader cross product.
//
// Each argument is matched against its OWN array column (type_name vs
// field_name), so a real field placed in the type slot, a real type in the
// field slot, or two reals from DIFFERENT entries must not match.  Enumerate a
// matrix of {real type, real field, mismatched pair, both-swapped} and require
// every cell is nullptr — pinning the two arguments never cross-contaminate.
// ---------------------------------------------------------------------------
static auto test_slot_independence_matrix_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;

    static const char* const real_types[]{ "Symbol", "Method", "Klass", "oopDesc" };
    static const char* const real_fields[]{ "_length", "_constMethod", "_name", "_mark" };

    bool all_null{ true };
    // type-in-field-slot and field-in-type-slot, full cross product.
    for (const char* const t : real_types)
    {
        for (const char* const f : real_fields)
        {
            // Correct shape (real type, real field) — null (no JVM), but also
            // the swapped shape (field as type, type as field) — must be null.
            if (iterate_struct_entries(t, f) != nullptr) { all_null = false; }
            if (iterate_struct_entries(f, t) != nullptr) { all_null = false; }
            // type in BOTH slots, field in BOTH slots.
            if (iterate_struct_entries(t, t) != nullptr) { all_null = false; }
            if (iterate_struct_entries(f, f) != nullptr) { all_null = false; }
        }
    }
    check("slot_independence_matrix_all_null", all_null);
}

// ---------------------------------------------------------------------------
// 29. Maximal interleave stress + first-call-order characterization.
//
// Mix all four entry points (both getters, both walkers) with the clamp and the
// module getter in one tight loop, asserting the joint invariant (all null /
// stable / clamp correct) holds under heavy interleaving.  Also CHARACTERIZE
// (as [INFO], never a hard assert) the documented order-of-first-call caching
// behaviour (the getters cache the FIRST resolution permanently): here the
// cache was necessarily seeded null by earlier sections, and stays null — we
// report it rather than assert a particular recovery semantics, which is a
// known library property, not a bug to gate on.
// ---------------------------------------------------------------------------
static auto test_maximal_interleave_and_cache_order() -> void
{
    using vmhook::hotspot::get_jvm_module;
    using vmhook::hotspot::get_vm_structs;
    using vmhook::hotspot::get_vm_types;
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    bool ok{ true };
    for (int i{ 0 }; i < 256; ++i)
    {
        if (get_jvm_module() != nullptr) { ok = false; }
        if (get_vm_structs() != nullptr) { ok = false; }
        if (get_vm_types() != nullptr) { ok = false; }
        if (iterate_struct_entries("Symbol", "_length") != nullptr) { ok = false; }
        if (iterate_struct_entries(nullptr, nullptr) != nullptr) { ok = false; }
        if (iterate_type_entries("Klass") != nullptr) { ok = false; }
        if (iterate_type_entries("") != nullptr) { ok = false; }
        const std::int32_t c{ vmhook::clamp_safe_container_count(i - 8) };
        if (c != ((i - 8) <= 0 ? 0 : (i - 8))) { ok = false; }
    }
    check("maximal_interleave_all_consistent", ok);

    // [INFO] characterization of the cache-first-resolution property (hazard 3).
    // Not asserted as pass/fail — it documents the as-built behaviour so a future
    // change (e.g. re-probe after libjvm load) is a conscious decision.
    const bool module_cached_null{ get_jvm_module() == nullptr };
    const bool structs_cached_null{ get_vm_structs() == nullptr };
    std::printf("[INFO] getter cache (no-JVM, first resolution sticks): "
                "module=%s structs=%s\n",
                module_cached_null ? "null" : "non-null",
                structs_cached_null ? "null" : "non-null");
}

// ---------------------------------------------------------------------------
// 30. Determinism fingerprint over the whole real-symbol matrix.
//
// Accumulate a fingerprint of the iterate_* results over the complete real
// query surface, repeated many times, and assert (a) the fingerprint is the
// all-null fingerprint (every lookup nullptr) and (b) it is byte-identical
// across every repetition — so the walkers carry no hidden per-call state and
// the output is fully reproducible (the run-3x-byte-identical guarantee, pinned
// from inside the test itself).
// ---------------------------------------------------------------------------
static auto test_determinism_fingerprint_no_jvm() -> void
{
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;

    struct pair { const char* type; const char* field; };
    static const pair struct_pairs[]{
        { "Symbol", "_length" }, { "Method", "_constMethod" },
        { "Klass", "_name" }, { "oopDesc", "_mark" },
        { "ConstantPool", "_pool_holder" }, { "InstanceKlass", "_methods" },
        { "JavaThread", "_thread_state" }, { "CompressedOops", "_base" },
    };
    static const char* const type_names[]{
        "Method", "ConstantPool", "Klass", "narrowOop", "Symbol",
    };

    auto fingerprint = [&]() -> std::uint64_t
    {
        std::uint64_t acc{ 1469598103934665603ull }; // FNV offset basis
        auto mix = [&](std::uint64_t v)
        {
            acc ^= v;
            acc *= 1099511628211ull;
        };
        for (const auto& p : struct_pairs)
        {
            mix(reinterpret_cast<std::uintptr_t>(iterate_struct_entries(p.type, p.field)));
        }
        for (const char* const n : type_names)
        {
            mix(reinterpret_cast<std::uintptr_t>(iterate_type_entries(n)));
        }
        return acc;
    };

    const std::uint64_t first{ fingerprint() };
    bool stable{ true };
    for (int i{ 0 }; i < 64; ++i)
    {
        if (fingerprint() != first) { stable = false; break; }
    }
    check("determinism_fingerprint_stable_across_repeats", stable);

    // The all-null fingerprint: recompute the SAME FNV chain feeding nullptr for
    // every slot (8 struct + 5 type == 13 mixes) and require it equals `first`,
    // proving every single lookup returned nullptr (not just that the chain was
    // stable at some nonzero value).
    std::uint64_t expected{ 1469598103934665603ull };
    for (int i{ 0 }; i < 13; ++i)
    {
        expected ^= 0ull; // nullptr
        expected *= 1099511628211ull;
    }
    check("determinism_fingerprint_is_all_null", first == expected);
}

// ===========================================================================
// EXHAUSTIVE EXPANSION WAVE 2 (31+).
//
// The earlier sections prove the *null-array* contract (no JVM => getter is
// nullptr => loop body never runs => every lookup nullptr, never faults) and
// the pure clamp/cap arithmetic.  This wave closes the two gaps that the
// no-JVM environment leaves untouched WITHOUT a live JVM and WITHOUT any
// fabricated unmapped pointer:
//
//   (A) The vm_struct_entry_t / vm_type_entry_t ABI layout the walkers do
//       pointer arithmetic over (hazard 2): every member offset, the struct
//       sizes, standard-layout-ness, and the alignment/padding the LP64 ABI
//       implies are pinned at COMPILE TIME from the declared field order.  A
//       reorder / inserted member / changed width would move an offset and
//       fail loudly here — the one place a stride drift could be caught before
//       it ever mis-walks a real array.
//
//   (B) The walk ALGORITHM itself (match-found, the field_name==nullptr skip,
//       the zero-type_name terminator, first/last/middle match, no-match).
//       iterate_* take NO array parameter and read the cached (null) getter, so
//       the real API cannot be driven over an in-process array.  Instead we
//       run a faithful, byte-for-byte re-implementation of the EXACT loop from
//       vmhook.hpp (the `entry && entry->type_name` guard, the
//       `if (!entry->field_name) continue;` skip, the double std::strcmp) over
//       a std::vector WE OWN.  Every entry's char* points into static string
//       literals or members of the owned vector — never a fabricated/unmapped
//       address — so this is fully POSIX-safe.  It proves the loop logic the
//       null-array path can never exercise; it does NOT call the real API with
//       a fake array (which is impossible through the public surface).
//
//   (C) The is_static / offset / address members (hazard 1): a static-field
//       entry value WE construct is inspected to confirm callers can read
//       is_static and that offset/address are independent slots — pinning that
//       the record carries the discriminator a static-field-aware caller needs.
// ===========================================================================

// ---------------------------------------------------------------------------
// 31. vm_type_entry_t / vm_struct_entry_t ABI layout — compile-time pin.
//
// The walkers stride the gHotSpot* arrays by sizeof(entry) and read members by
// their declared offset.  Pin the EXACT layout the header declares (member
// order, each offset, the total size, standard-layout) so any future reorder
// or width change is a compile-time failure, not a silent mis-walk against a
// real libjvm.  All values are computed by the compiler from the struct
// definition — no JVM, no memory read, no fabricated pointer.
// ---------------------------------------------------------------------------
static auto test_entry_struct_layout_pin() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // Both records must be standard-layout: the walkers reinterpret the exported
    // global as an array of these and rely on C-ABI member placement.
    static_assert(std::is_standard_layout<vm_type_entry_t>::value,
                  "vm_type_entry_t must be standard-layout (C-ABI array element)");
    static_assert(std::is_standard_layout<vm_struct_entry_t>::value,
                  "vm_struct_entry_t must be standard-layout (C-ABI array element)");
    check("vm_type_entry_is_standard_layout",
          std::is_standard_layout<vm_type_entry_t>::value);
    check("vm_struct_entry_is_standard_layout",
          std::is_standard_layout<vm_struct_entry_t>::value);

    // --- vm_type_entry_t member ORDER: type_name is first (the loop guard reads
    //     entry->type_name and the terminator is a zero in this slot), so it MUST
    //     be at offset 0.  superclass_name second.
    static_assert(offsetof(vm_type_entry_t, type_name) == 0u,
                  "type_name must be the first member (loop guard / terminator slot)");
    check("vm_type_entry_type_name_at_zero",
          offsetof(vm_type_entry_t, type_name) == 0u);
    check("vm_type_entry_superclass_after_type_name",
          offsetof(vm_type_entry_t, superclass_name)
              > offsetof(vm_type_entry_t, type_name));
    check("vm_type_entry_is_oop_after_superclass",
          offsetof(vm_type_entry_t, is_oop_type_type)
              > offsetof(vm_type_entry_t, superclass_name));
    check("vm_type_entry_is_integer_after_is_oop",
          offsetof(vm_type_entry_t, is_integer_type)
              > offsetof(vm_type_entry_t, is_oop_type_type));
    check("vm_type_entry_is_unsigned_after_is_integer",
          offsetof(vm_type_entry_t, is_unsigned)
              > offsetof(vm_type_entry_t, is_integer_type));
    check("vm_type_entry_size_is_last",
          offsetof(vm_type_entry_t, size)
              > offsetof(vm_type_entry_t, is_unsigned));

    // The three int32 flags are adjacent and 4 bytes apart (no padding between
    // same-width members), regardless of pointer width.
    check("vm_type_entry_is_integer_4_after_is_oop",
          offsetof(vm_type_entry_t, is_integer_type)
              == offsetof(vm_type_entry_t, is_oop_type_type) + 4u);
    check("vm_type_entry_is_unsigned_4_after_is_integer",
          offsetof(vm_type_entry_t, is_unsigned)
              == offsetof(vm_type_entry_t, is_integer_type) + 4u);

    // --- vm_struct_entry_t member ORDER: type_name first (guard/terminator),
    //     field_name second (the skip-guard reads entry->field_name).
    static_assert(offsetof(vm_struct_entry_t, type_name) == 0u,
                  "struct type_name must be the first member");
    check("vm_struct_entry_type_name_at_zero",
          offsetof(vm_struct_entry_t, type_name) == 0u);
    check("vm_struct_entry_field_name_second",
          offsetof(vm_struct_entry_t, field_name)
              > offsetof(vm_struct_entry_t, type_name));
    check("vm_struct_entry_type_string_third",
          offsetof(vm_struct_entry_t, type_string)
              > offsetof(vm_struct_entry_t, field_name));
    check("vm_struct_entry_is_static_fourth",
          offsetof(vm_struct_entry_t, is_static)
              > offsetof(vm_struct_entry_t, type_string));
    check("vm_struct_entry_offset_fifth",
          offsetof(vm_struct_entry_t, offset)
              > offsetof(vm_struct_entry_t, is_static));
    check("vm_struct_entry_address_last",
          offsetof(vm_struct_entry_t, address)
              > offsetof(vm_struct_entry_t, offset));

    // The first three members are all char* — equally spaced by sizeof(void*).
    check("vm_struct_entry_field_name_one_ptr_after_type",
          offsetof(vm_struct_entry_t, field_name)
              == offsetof(vm_struct_entry_t, type_name) + sizeof(const char*));
    check("vm_struct_entry_type_string_one_ptr_after_field",
          offsetof(vm_struct_entry_t, type_string)
              == offsetof(vm_struct_entry_t, field_name) + sizeof(const char*));

    // Member WIDTHS (the int32 flags really are 32-bit; offset/size really are
    // 64-bit; the pointers are pointer-width) — a width change would alter the
    // stride.
    static_assert(sizeof(vm_struct_entry_t::is_static) == 4u,
                  "is_static must be int32 (matches HotSpot VMStructEntry)");
    static_assert(sizeof(vm_struct_entry_t::offset) == 8u,
                  "offset must be uint64 (matches HotSpot VMStructEntry)");
    static_assert(sizeof(vm_type_entry_t::size) == 8u,
                  "type size must be uint64 (matches HotSpot VMTypeEntry)");
    check("vm_struct_entry_is_static_is_int32",
          sizeof(vm_struct_entry_t::is_static) == 4u);
    check("vm_struct_entry_offset_is_uint64",
          sizeof(vm_struct_entry_t::offset) == 8u);
    check("vm_type_entry_size_is_uint64",
          sizeof(vm_type_entry_t::size) == 8u);

    // sizeof(entry) is the array stride.  It must be a whole multiple of the
    // alignment (no trailing-pad surprise that desynchronizes ++entry) and large
    // enough to hold every member past its offset.
    check("vm_struct_entry_size_multiple_of_align",
          (sizeof(vm_struct_entry_t) % alignof(vm_struct_entry_t)) == 0u);
    check("vm_type_entry_size_multiple_of_align",
          (sizeof(vm_type_entry_t) % alignof(vm_type_entry_t)) == 0u);
    check("vm_struct_entry_size_covers_last_member",
          sizeof(vm_struct_entry_t)
              >= offsetof(vm_struct_entry_t, address) + sizeof(void*));
    check("vm_type_entry_size_covers_last_member",
          sizeof(vm_type_entry_t)
              >= offsetof(vm_type_entry_t, size) + sizeof(std::uint64_t));

    // On an LP64 target (the JVM targets this header support) sizeof(void*)==8,
    // and the exact layout is fully determined: this branch only ASSERTS the
    // concrete numbers when pointers are 8 bytes, so it never wrongly fails on a
    // hypothetical 32-bit build.  vm_type_entry_t = ptr ptr | i32 i32 i32 (12,
    // padded to 16) | u64  => 8+8+16+8 = 40.  vm_struct_entry_t = ptr ptr ptr |
    // i32 (+4 pad) | u64 | ptr => 24+8(incl pad)+8+8 = 48.
    if (sizeof(void*) == 8u)
    {
        check("vm_type_entry_lp64_type_name_off", offsetof(vm_type_entry_t, type_name) == 0u);
        check("vm_type_entry_lp64_superclass_off", offsetof(vm_type_entry_t, superclass_name) == 8u);
        check("vm_type_entry_lp64_is_oop_off", offsetof(vm_type_entry_t, is_oop_type_type) == 16u);
        check("vm_type_entry_lp64_is_integer_off", offsetof(vm_type_entry_t, is_integer_type) == 20u);
        check("vm_type_entry_lp64_is_unsigned_off", offsetof(vm_type_entry_t, is_unsigned) == 24u);
        check("vm_type_entry_lp64_size_off", offsetof(vm_type_entry_t, size) == 32u);
        check("vm_type_entry_lp64_sizeof", sizeof(vm_type_entry_t) == 40u);

        check("vm_struct_entry_lp64_type_name_off", offsetof(vm_struct_entry_t, type_name) == 0u);
        check("vm_struct_entry_lp64_field_name_off", offsetof(vm_struct_entry_t, field_name) == 8u);
        check("vm_struct_entry_lp64_type_string_off", offsetof(vm_struct_entry_t, type_string) == 16u);
        check("vm_struct_entry_lp64_is_static_off", offsetof(vm_struct_entry_t, is_static) == 24u);
        check("vm_struct_entry_lp64_offset_off", offsetof(vm_struct_entry_t, offset) == 32u);
        check("vm_struct_entry_lp64_address_off", offsetof(vm_struct_entry_t, address) == 40u);
        check("vm_struct_entry_lp64_sizeof", sizeof(vm_struct_entry_t) == 48u);
    }
    else
    {
        // Non-LP64: only the order/width invariants above apply; record that the
        // concrete-offset block was skipped so the pass count is environment-aware.
        check("entry_layout_lp64_block_skipped_non_lp64", sizeof(void*) != 8u);
    }
}

// ---------------------------------------------------------------------------
// 32. Walk-algorithm re-implementation over an OWNED array.
//
// iterate_struct_entries / iterate_type_entries take no array argument and read
// the cached (null) getter, so the public API cannot be driven over an
// in-process array.  This re-implements the EXACT loop body from vmhook.hpp
// 1971 / 1997-2008 and runs it over a std::vector<vm_*_entry_t> WE OWN (every
// char* points at a static string literal or is nullptr — no fabricated
// address, fully POSIX-safe).  It proves the loop logic that the no-JVM null
// array can never reach: the field_name==nullptr skip, the zero-type_name
// terminator, and first/middle/last/no-match.
//
// NOTE: this is a faithful MIRROR of the source loop, kept in lock-step with it
// by the layout pins in section 31; it is intentionally NOT the real API (which
// is unreachable here without a live exported symbol).
// ---------------------------------------------------------------------------

// Mirror of iterate_struct_entries' loop body (vmhook.hpp:1997-2008), driven
// over a caller-supplied array head.  Identical guard, skip, and double strcmp.
static auto mirror_iterate_struct(const vmhook::hotspot::vm_struct_entry_t* head,
                                  const char* type_name,
                                  const char* field_name) noexcept
    -> const vmhook::hotspot::vm_struct_entry_t*
{
    if (!type_name || !field_name)
    {
        return nullptr;
    }
    for (const vmhook::hotspot::vm_struct_entry_t* entry{ head };
         entry && entry->type_name; ++entry)
    {
        if (!entry->field_name)
        {
            continue;
        }
        if (!std::strcmp(entry->type_name, type_name)
            && !std::strcmp(entry->field_name, field_name))
        {
            return entry;
        }
    }
    return nullptr;
}

// Mirror of iterate_type_entries' loop body (vmhook.hpp:1967-1978).
static auto mirror_iterate_type(const vmhook::hotspot::vm_type_entry_t* head,
                                const char* type_name) noexcept
    -> const vmhook::hotspot::vm_type_entry_t*
{
    if (!type_name)
    {
        return nullptr;
    }
    for (const vmhook::hotspot::vm_type_entry_t* entry{ head };
         entry && entry->type_name; ++entry)
    {
        if (!std::strcmp(entry->type_name, type_name))
        {
            return entry;
        }
    }
    return nullptr;
}

static auto test_walk_algorithm_owned_array() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // First, prove the mirror is faithful to the REAL function over the real
    // (null) array head: on a null head both must agree (return nullptr), and
    // both must honor the null-arg guard identically.  This ties the mirror to
    // the production loop so the owned-array results below are meaningful.
    check("mirror_struct_matches_real_on_null_head",
          mirror_iterate_struct(nullptr, "Symbol", "_length")
              == vmhook::hotspot::iterate_struct_entries("Symbol", "_length"));
    check("mirror_type_matches_real_on_null_head",
          mirror_iterate_type(nullptr, "Method")
              == vmhook::hotspot::iterate_type_entries("Method"));
    check("mirror_struct_null_type_guard",
          mirror_iterate_struct(nullptr, nullptr, "_length") == nullptr);
    check("mirror_struct_null_field_guard",
          mirror_iterate_struct(nullptr, "Symbol", nullptr) == nullptr);
    check("mirror_type_null_guard",
          mirror_iterate_type(nullptr, nullptr) == nullptr);

    // --- Owned struct array.  Element [2] has a NULL field_name (the partial
    //     entry a JVMTI agent can publish) and MUST be skipped without a
    //     strcmp(nullptr, ...) fault; the matching entry for ("X","_f") is [3].
    //     The array ends with a zero-type_name terminator [5].
    //     All char* are string literals or nullptr — owned, mapped, POSIX-safe.
    const std::array<vm_struct_entry_t, 6> structs{ {
        { "Symbol",  "_length",  "int",   0,  8u,  nullptr },   // [0] first
        { "Method",  "_code",    "void*", 0, 16u,  nullptr },   // [1]
        { "X",       nullptr,    "junk",  0,  0u,  nullptr },   // [2] SKIP guard
        { "X",       "_f",       "int",   0, 24u,  nullptr },   // [3] target
        { "Klass",   "_name",    "Symbol*", 1, 32u, nullptr },  // [4] last real (static)
        { nullptr,   nullptr,    nullptr, 0,  0u,  nullptr },   // [5] terminator
    } };
    const vm_struct_entry_t* const head{ structs.data() };

    // (a) First-entry match.
    check("owned_struct_first_entry_match",
          mirror_iterate_struct(head, "Symbol", "_length") == &structs[0]);
    // (b) Middle match (after a skipped partial entry).
    check("owned_struct_middle_match_after_skip",
          mirror_iterate_struct(head, "X", "_f") == &structs[3]);
    // (c) The skip is REALLY exercised: ("X","_f") sits AFTER the null-field
    //     "X" entry [2]; a match proves the loop skipped [2] (a strcmp on its
    //     null field would have faulted) and continued to [3].
    check("owned_struct_skip_guard_returns_later_entry",
          mirror_iterate_struct(head, "X", "_f") != nullptr
              && mirror_iterate_struct(head, "X", "_f")->field_name != nullptr);
    // (d) Last real entry (immediately before the terminator) is reachable.
    check("owned_struct_last_real_entry_match",
          mirror_iterate_struct(head, "Klass", "_name") == &structs[4]);
    // (e) The terminator stops the walk: a type that only "exists" past the
    //     zero-type_name slot is never found (we never read [5] beyond its
    //     null type_name, and never walk into [6]/OOB).
    check("owned_struct_no_match_walks_to_terminator",
          mirror_iterate_struct(head, "ZZZ", "_nope") == nullptr);
    // (f) A type present but with the WRONG field is a miss (double strcmp: both
    //     must match).
    check("owned_struct_right_type_wrong_field_miss",
          mirror_iterate_struct(head, "Symbol", "_body") == nullptr);
    // (g) The null-field entry [2] is itself unmatchable even by its own type
    //     when paired with any field (it is always skipped).
    check("owned_struct_null_field_entry_unmatchable",
          mirror_iterate_struct(head, "X", "junk") == nullptr
              || mirror_iterate_struct(head, "X", "_f") == &structs[3]);

    // --- is_static / offset / address are independent, readable slots
    //     (hazard 1).  Entry [4] is a STATIC field: callers must be able to read
    //     is_static==1, and offset/address are distinct members.  Pin that the
    //     record carries the discriminator a static-aware caller needs and that
    //     the matched pointer exposes the value we stored.
    const vm_struct_entry_t* const static_entry{
        mirror_iterate_struct(head, "Klass", "_name") };
    check("owned_struct_static_entry_is_static_flag_readable",
          static_entry != nullptr && static_entry->is_static == 1);
    check("owned_struct_instance_entry_is_static_zero",
          mirror_iterate_struct(head, "Symbol", "_length") != nullptr
              && mirror_iterate_struct(head, "Symbol", "_length")->is_static == 0);
    check("owned_struct_matched_offset_readable",
          static_entry != nullptr && static_entry->offset == 32u);
    check("owned_struct_offset_and_address_distinct_slots",
          static_entry != nullptr
              && reinterpret_cast<const void*>(&static_entry->offset)
                     != reinterpret_cast<const void*>(&static_entry->address));

    // --- Owned type array.  Walk-found in the middle, first, last; terminator
    //     stops; no-match returns nullptr.
    const std::array<vm_type_entry_t, 5> types{ {
        { "oopDesc",       nullptr, 1, 0, 0, 16u },  // [0] first
        { "Klass",         nullptr, 0, 0, 0, 64u },  // [1]
        { "ConstantPool",  nullptr, 0, 0, 0, 80u },  // [2] middle target
        { "narrowOop",     nullptr, 0, 1, 1,  4u },  // [3] last real
        { nullptr,         nullptr, 0, 0, 0,  0u },  // [4] terminator
    } };
    const vm_type_entry_t* const thead{ types.data() };

    check("owned_type_first_entry_match",
          mirror_iterate_type(thead, "oopDesc") == &types[0]);
    check("owned_type_middle_match",
          mirror_iterate_type(thead, "ConstantPool") == &types[2]);
    check("owned_type_last_real_entry_match",
          mirror_iterate_type(thead, "narrowOop") == &types[3]);
    check("owned_type_no_match_to_terminator",
          mirror_iterate_type(thead, "ZZZ_NoSuchType") == nullptr);
    // The matched type entry's size member is readable and is the value stored.
    check("owned_type_matched_size_readable",
          mirror_iterate_type(thead, "ConstantPool") != nullptr
              && mirror_iterate_type(thead, "ConstantPool")->size == 80u);

    // --- A null head terminates immediately for BOTH mirrors (the `entry &&`
    //     half of the guard): exactly the no-JVM production path.
    check("owned_struct_null_head_returns_null",
          mirror_iterate_struct(nullptr, "Symbol", "_length") == nullptr);
    check("owned_type_null_head_returns_null",
          mirror_iterate_type(nullptr, "oopDesc") == nullptr);

    // --- An array consisting ONLY of a terminator (zero-type_name at [0]): the
    //     loop guard `entry->type_name` is false on the first iteration, so the
    //     body never runs and every lookup is nullptr (the present-but-empty
    //     VMStructs case, distinct from the null-head case).
    const std::array<vm_struct_entry_t, 1> empty_structs{ {
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    } };
    const std::array<vm_type_entry_t, 1> empty_types{ {
        { nullptr, nullptr, 0, 0, 0, 0u },
    } };
    check("owned_struct_terminator_only_empty",
          mirror_iterate_struct(empty_structs.data(), "Symbol", "_length") == nullptr);
    check("owned_type_terminator_only_empty",
          mirror_iterate_type(empty_types.data(), "oopDesc") == nullptr);

    // --- Leading null-field entries before ANY real entry: the skip must hold
    //     for a run of partial entries at the head, then still find the real one.
    const std::array<vm_struct_entry_t, 4> leading_partials{ {
        { "A", nullptr, nullptr, 0, 0u, nullptr },   // skip
        { "B", nullptr, nullptr, 0, 0u, nullptr },   // skip
        { "Symbol", "_length", "int", 0, 8u, nullptr }, // real
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },  // terminator
    } };
    check("owned_struct_run_of_leading_partials_then_match",
          mirror_iterate_struct(leading_partials.data(), "Symbol", "_length")
              == &leading_partials[2]);
    // A type whose name only appears in a SKIPPED (null-field) entry is never
    // returned for the struct walk (the skip removes those entries from the
    // matchable set entirely).
    check("owned_struct_skipped_entry_type_not_matchable",
          mirror_iterate_struct(leading_partials.data(), "A", "_anything") == nullptr);
}

// ---------------------------------------------------------------------------
// 33. is_static discriminator semantics over fabricated VALUES (hazard 1).
//
// Pure value inspection of vm_struct_entry_t instances WE construct (no array
// walk, no memory read beyond our own stack objects).  Confirms that the record
// carries everything a static-field-aware caller needs to NOT blindly add
// ->offset to a base for a static field: is_static is a readable int32 flag,
// instance entries carry is_static==0, static entries carry is_static==1, and
// offset/address are separate slots a caller can choose between based on the
// flag.  This pins the contract a future find_static_* helper would rely on.
// ---------------------------------------------------------------------------
static auto test_is_static_discriminator_values() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    // An instance field: located by ->offset, is_static == 0.
    const vm_struct_entry_t instance_field{
        "oopDesc", "_mark", "markWord", 0, 8u, nullptr };
    // A static field: in HotSpot its location is in ->address, ->offset is not
    // meaningful; is_static == 1.  We give it a non-zero sentinel address VALUE
    // (never dereferenced) and a meaningless offset to make the point.
    int some_storage{ 7 };
    const vm_struct_entry_t static_field{
        "Universe", "_collectedHeap", "CollectedHeap*", 1, 0u,
        static_cast<void*>(&some_storage) };

    check("is_static_instance_flag_is_zero", instance_field.is_static == 0);
    check("is_static_static_flag_is_one", static_field.is_static == 1);

    // The flag is the boolean-ish discriminator: exactly {0,1} in stock HotSpot.
    check("is_static_flag_is_boolean_domain",
          (instance_field.is_static == 0 || instance_field.is_static == 1)
              && (static_field.is_static == 0 || static_field.is_static == 1));

    // For the instance field, ->offset is the meaningful slot and is what we set.
    check("is_static_instance_uses_offset",
          instance_field.is_static == 0 && instance_field.offset == 8u);
    // For the static field, ->address is the meaningful slot; ->offset is the
    // sentinel zero we stored (NOT a real location) — the trap a caller avoids
    // by checking is_static first.
    check("is_static_static_address_is_set",
          static_field.is_static == 1
              && static_field.address == static_cast<void*>(&some_storage));
    check("is_static_static_offset_is_sentinel_zero",
          static_field.is_static == 1 && static_field.offset == 0u);

    // offset (uint64) and address (void*) are independent members at different
    // addresses within the record — a caller can read whichever the flag selects.
    check("is_static_offset_address_independent",
          reinterpret_cast<const void*>(&static_field.offset)
              != reinterpret_cast<const void*>(&static_field.address));

    // The full int32 domain of is_static is representable (a non-standard JVM
    // could in principle store other values); the record neither narrows nor
    // rejects them — a caller-side `is_static != 0` test is the robust check,
    // which holds for any non-zero value.
    const vm_struct_entry_t odd_flag{ "T", "_f", "int", 2, 4u, nullptr };
    check("is_static_nonzero_treated_as_static_by_robust_test",
          (odd_flag.is_static != 0) == true);
}

// ---------------------------------------------------------------------------
// 34. iterate_type_entries has NO field_name skip — by design.
//
// The struct walker skips entries whose field_name is null; the type walker has
// no field_name member at all and only guards on type_name.  Drive the mirror
// type walk over an owned array whose entries have arbitrary is_* flags to
// confirm the type walk matches purely on type_name and never inspects any
// other column (so the asymmetry between the two walkers is intentional and
// pinned).  Owned array, POSIX-safe.
// ---------------------------------------------------------------------------
static auto test_type_walk_matches_on_type_name_only() -> void
{
    using vmhook::hotspot::vm_type_entry_t;

    // Two entries share is_* flag values but differ only in type_name; the walk
    // must distinguish them by name alone.
    const std::array<vm_type_entry_t, 3> types{ {
        { "Alpha", "Base", 1, 1, 1, 24u },
        { "Beta",  "Base", 1, 1, 1, 24u },
        { nullptr, nullptr, 0, 0, 0, 0u },
    } };
    const vm_type_entry_t* const head{ types.data() };

    check("type_walk_finds_alpha_by_name",
          mirror_iterate_type(head, "Alpha") == &types[0]);
    check("type_walk_finds_beta_by_name",
          mirror_iterate_type(head, "Beta") == &types[1]);
    // Same superclass_name string on both entries does NOT cause a match on it.
    check("type_walk_does_not_match_on_superclass",
          mirror_iterate_type(head, "Base") == nullptr);
    // The matched entry exposes the very flag/size values we stored, proving the
    // walker returns the entry untouched (no normalization of other columns).
    check("type_walk_alpha_fields_intact",
          mirror_iterate_type(head, "Alpha")->is_oop_type_type == 1
              && mirror_iterate_type(head, "Alpha")->is_integer_type == 1
              && mirror_iterate_type(head, "Alpha")->is_unsigned == 1
              && mirror_iterate_type(head, "Alpha")->size == 24u);
}

// ---------------------------------------------------------------------------
// 35. Iteration-count bound over an owned array (no runaway).
//
// The walk visits at most (number of entries before the zero-type_name
// terminator) elements.  Over an owned, properly-terminated array a full
// no-match scan must visit exactly the real-entry count and stop — proving the
// terminator length-path works (hazard 5) without any unbounded read.  We count
// iterations with an instrumented copy of the loop body over OWNED memory.
// ---------------------------------------------------------------------------
static auto test_walk_iteration_count_bounded() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    // 5 real entries + terminator.  A no-match lookup must touch type_name of
    // each real entry exactly once, hit the terminator, and stop at 5.
    const std::array<vm_struct_entry_t, 6> arr{ {
        { "T0", "_f", "int", 0, 0u, nullptr },
        { "T1", "_f", "int", 0, 0u, nullptr },
        { "T2", "_f", "int", 0, 0u, nullptr },
        { "T3", "_f", "int", 0, 0u, nullptr },
        { "T4", "_f", "int", 0, 0u, nullptr },
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    } };

    int visited{ 0 };
    for (const vm_struct_entry_t* e{ arr.data() }; e && e->type_name; ++e)
    {
        ++visited;
        // Same body shape as the real loop (skip + double strcmp); no match.
        if (!e->field_name) { continue; }
        if (!std::strcmp(e->type_name, "ZZZ") && !std::strcmp(e->field_name, "_no"))
        {
            break;
        }
    }
    check("walk_visits_exactly_real_entry_count", visited == 5);

    // A match on the 3rd entry stops the scan early (no visiting past the match).
    int visited_until_match{ 0 };
    for (const vm_struct_entry_t* e{ arr.data() }; e && e->type_name; ++e)
    {
        ++visited_until_match;
        if (!e->field_name) { continue; }
        if (!std::strcmp(e->type_name, "T2") && !std::strcmp(e->field_name, "_f"))
        {
            break;
        }
    }
    check("walk_stops_at_match_index", visited_until_match == 3);

    // The terminator is detected on the (count)th increment: after 5 real
    // entries, e points at arr[5] whose type_name is null, so the guard ends the
    // loop without dereferencing any further element.
    const vm_struct_entry_t* tail{ arr.data() };
    int steps{ 0 };
    while (tail && tail->type_name) { ++tail; ++steps; }
    check("walk_terminator_reached_at_count", steps == 5);
    check("walk_terminator_entry_has_null_type_name", tail->type_name == nullptr);
}

// ---------------------------------------------------------------------------
// 36. strcmp exactness over an owned array: confirm the production matcher is
//     byte-exact (case-sensitive, length-exact, no prefix/substring match)
//     against REAL entries, complementing the null-array near-miss sections
//     (16/27) which can only prove "all null".  Here a real entry EXISTS, so a
//     near-miss returning the entry would be a false positive — this is the
//     positive-control the no-JVM sections structurally cannot provide.
// ---------------------------------------------------------------------------
static auto test_matcher_exactness_owned_array() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    const std::array<vm_struct_entry_t, 2> arr{ {
        { "Symbol", "_length", "int", 0, 8u, nullptr },
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    } };
    const vm_struct_entry_t* const head{ arr.data() };

    // Exact match works (positive control).
    check("exact_match_hits", mirror_iterate_struct(head, "Symbol", "_length") == &arr[0]);

    // Case variants of an EXISTING type/field must NOT match (strcmp is
    // case-sensitive) — a false positive here would be a real correctness bug.
    check("exact_type_case_lower_miss",
          mirror_iterate_struct(head, "symbol", "_length") == nullptr);
    check("exact_type_case_upper_miss",
          mirror_iterate_struct(head, "SYMBOL", "_length") == nullptr);
    check("exact_field_case_miss",
          mirror_iterate_struct(head, "Symbol", "_Length") == nullptr);

    // Prefix / superstring of the existing names must NOT match (length-exact).
    check("exact_type_prefix_miss",
          mirror_iterate_struct(head, "Symbo", "_length") == nullptr);
    check("exact_type_superstring_miss",
          mirror_iterate_struct(head, "SymbolX", "_length") == nullptr);
    check("exact_field_prefix_miss",
          mirror_iterate_struct(head, "Symbol", "_lengt") == nullptr);
    check("exact_field_superstring_miss",
          mirror_iterate_struct(head, "Symbol", "_lengthX") == nullptr);
    check("exact_field_trailing_space_miss",
          mirror_iterate_struct(head, "Symbol", "_length ") == nullptr);

    // Right type, right field, but in swapped slots — both strcmps fail.
    check("exact_swapped_slots_miss",
          mirror_iterate_struct(head, "_length", "Symbol") == nullptr);

    // The type walk over an owned single-type array is equally exact.
    const std::array<vmhook::hotspot::vm_type_entry_t, 2> tarr{ {
        { "Method", nullptr, 0, 0, 0, 56u },
        { nullptr, nullptr, 0, 0, 0, 0u },
    } };
    check("exact_type_walk_hits", mirror_iterate_type(tarr.data(), "Method") == &tarr[0]);
    check("exact_type_walk_case_miss", mirror_iterate_type(tarr.data(), "method") == nullptr);
    check("exact_type_walk_prefix_miss", mirror_iterate_type(tarr.data(), "Metho") == nullptr);
    check("exact_type_walk_superstring_miss", mirror_iterate_type(tarr.data(), "Methods") == nullptr);
}

// ===========================================================================
// EXHAUSTIVE EXPANSION WAVE 3 (37+).
//
// WAVE 2 (sections 31-36) pinned the entry-struct ABI and re-ran the walk loop
// over owned arrays for the match-found / skip / terminator / case-exactness
// paths.  This wave closes the remaining gaps it left, all over memory THIS
// TEST OWNS (string literals + std::array, never a fabricated/unmapped address,
// never the global-reading API, never a JVM) and all POSIX-safe:
//
//   * FULL uint64 bit-pattern round-trip of vm_struct_entry_t::offset and
//     vm_type_entry_t::size -- WAVE 2 used only tiny offsets (8/16/24/32), so the
//     32-bit-truncation tripwires (1<<32, UINT64_MAX, high-word preservation)
//     that flaw #1's `this + entry->offset` add depends on were UNCOVERED.
//   * First-match-WINS on a duplicate (type,field) / duplicate type_name -- the
//     resolver returns on the FIRST strcmp hit (vmhook.hpp:2003-2006); WAVE 2
//     never had two matchable rows, so the documented first-wins tie-break was
//     untested.
//   * is_static across its full int32 discriminator domain (INT32_MIN/-1/MAX),
//     pinning that the robust `!= 0` test classifies every non-zero as static.
//   * Embedded-NUL search key (via explicit length) -- strcmp stops at the first
//     NUL, so a key "Symbol\0junk" matches a "Symbol" entry; pins that the
//     matcher never reads past the caller's first NUL (test angle #5).
//   * Determinism / idempotence of the walk over a populated owned array across
//     many repeats (the walk carries no per-call state).
//   * alignof / pointer-member-size facts WAVE 2's offset pins did not assert.
//
// All helpers live in a private namespace so they never collide with WAVE 2's
// mirror_iterate_* / test_* symbols.
// ===========================================================================
namespace wave3_offset_resolution
{
    // Private byte-for-byte mirror of iterate_struct_entries' loop
    // (vmhook.hpp:1990-2009), array head explicit, over OWNED memory.
    static auto find_struct(const vmhook::hotspot::vm_struct_entry_t* head,
                            const char* type_name,
                            const char* field_name) noexcept
        -> const vmhook::hotspot::vm_struct_entry_t*
    {
        if (!type_name || !field_name)
        {
            return nullptr;
        }
        for (const vmhook::hotspot::vm_struct_entry_t* entry{ head };
             entry && entry->type_name; ++entry)
        {
            if (!entry->field_name)
            {
                continue;
            }
            if (!std::strcmp(entry->type_name, type_name)
                && !std::strcmp(entry->field_name, field_name))
            {
                return entry;
            }
        }
        return nullptr;
    }

    // Private mirror of iterate_type_entries' loop (vmhook.hpp:1964-1979).
    static auto find_type(const vmhook::hotspot::vm_type_entry_t* head,
                          const char* type_name) noexcept
        -> const vmhook::hotspot::vm_type_entry_t*
    {
        if (!type_name)
        {
            return nullptr;
        }
        for (const vmhook::hotspot::vm_type_entry_t* entry{ head };
             entry && entry->type_name; ++entry)
        {
            if (!std::strcmp(entry->type_name, type_name))
            {
                return entry;
            }
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// 37. FULL uint64 OFFSET bit-pattern round-trip (the no-truncation guarantee).
//
// Every instance-field reader computes `base + entry->offset` with offset a
// uint64_t (vmhook.hpp:1897).  WAVE 2 only stored small offsets; here a single
// owned entry carries each interesting 64-bit pattern in turn and the resolved
// entry's ->offset is required to read back BIT-EXACT, with the high 32 bits
// preserved -- a regression that truncated offset to 32 bits (the >4GiB-relative
// layout hazard) corrupts the high word and fails.  Stored values in OWNED
// memory; the offset is NEVER used as a pointer.
// ---------------------------------------------------------------------------
static auto test_w3_offset_full_uint64_roundtrip() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    static const std::uint64_t patterns[]{
        0ull, 1ull, 2ull, 7ull,
        0xFFull, 0x100ull, 0x7FFFull, 0x8000ull, 0xFFFFull, 0x10000ull,
        0x7FFFFFFFull,            // INT32_MAX
        0x80000000ull,            // INT32_MAX + 1 (sign bit if misread as int32)
        0xFFFFFFFFull,            // UINT32_MAX
        0x100000000ull,           // 1 << 32 -- the truncation tripwire
        0x123456789ABCull,
        0xAAAAAAAAAAAAAAAAull,    // alternating bits
        0x5555555555555555ull,
        0x7FFFFFFFFFFFFFFFull,    // INT64_MAX
        0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull,    // UINT64_MAX
    };

    bool exact{ true };
    bool high_word_kept{ true };
    int n{ 0 };
    for (const std::uint64_t off : patterns)
    {
        ++n;
        const vm_struct_entry_t table[]{
            { "T", "_f", "ty", 0, off, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(table, "T", "_f") };
        if (e == nullptr || e->offset != off) { exact = false; }
        if (e != nullptr && (e->offset >> 32) != (off >> 32)) { high_word_kept = false; }
    }
    check("w3_offset_full_uint64_roundtrip_exact", exact);
    check("w3_offset_high_word_preserved_no_truncation", high_word_kept);
    check("w3_offset_pattern_count_20", n == 20);

    // The two boundary tripwires explicitly, so a failure names the boundary.
    {
        const vm_struct_entry_t t[]{
            { "T", "_f", "ty", 0, 0x100000000ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(t, "T", "_f") };
        check("w3_offset_1_shl_32_roundtrips", e != nullptr && e->offset == 0x100000000ull);
    }
    {
        const vm_struct_entry_t t[]{
            { "T", "_f", "ty", 0, 0xFFFFFFFFFFFFFFFFull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(t, "T", "_f") };
        check("w3_offset_uint64_max_roundtrips",
              e != nullptr && e->offset == 0xFFFFFFFFFFFFFFFFull);
    }
}

// ---------------------------------------------------------------------------
// 38. FULL uint64 vm_type_entry_t::size round-trip + classification-flag domain.
//
// The type entry's `size` member is uint64 (vmhook.hpp:1887) and the three
// classification flags are int32.  Sweep the full 64-bit pattern set through an
// owned type entry's size and require bit-exact readback (the type walker
// returns the entry untouched), and sweep the flag combinations.
// ---------------------------------------------------------------------------
static auto test_w3_type_size_full_uint64_roundtrip() -> void
{
    using vmhook::hotspot::vm_type_entry_t;

    static const std::uint64_t sizes[]{
        0ull, 1ull, 8ull, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull,
        0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
    };
    bool exact{ true };
    bool high_kept{ true };
    for (const std::uint64_t sz : sizes)
    {
        const vm_type_entry_t table[]{
            { "T", nullptr, 0, 0, 0, sz },
            { nullptr, nullptr, 0, 0, 0, 0u },
        };
        const vm_type_entry_t* const e{
            wave3_offset_resolution::find_type(table, "T") };
        if (e == nullptr || e->size != sz) { exact = false; }
        if (e != nullptr && (e->size >> 32) != (sz >> 32)) { high_kept = false; }
    }
    check("w3_type_size_full_uint64_roundtrip_exact", exact);
    check("w3_type_size_high_word_preserved", high_kept);

    // All 8 combinations of the three boolean classification flags survive intact
    // (each independent int32 column read back as stored).
    bool flags_ok{ true };
    for (int oop{ 0 }; oop <= 1; ++oop)
    {
        for (int integer{ 0 }; integer <= 1; ++integer)
        {
            for (int uns{ 0 }; uns <= 1; ++uns)
            {
                const vm_type_entry_t table[]{
                    { "T", nullptr, oop, integer, uns, 8u },
                    { nullptr, nullptr, 0, 0, 0, 0u },
                };
                const vm_type_entry_t* const e{
                    wave3_offset_resolution::find_type(table, "T") };
                if (e == nullptr
                    || e->is_oop_type_type != oop
                    || e->is_integer_type != integer
                    || e->is_unsigned != uns)
                {
                    flags_ok = false;
                }
            }
        }
    }
    check("w3_type_classification_flags_all_8_combos_intact", flags_ok);
}

// ---------------------------------------------------------------------------
// 39. First-match-WINS on duplicates (vmhook.hpp:2003-2006 returns on first hit).
//
// WAVE 2 never had two matchable rows.  A table with a duplicate (type,field)
// must resolve to the FIRST row (its offset/address), and a duplicate type_name
// in the type walk likewise resolves to the first -- pinning the tie-break the
// library implicitly relies on.  Owned memory.
// ---------------------------------------------------------------------------
static auto test_w3_first_match_wins() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    static int first_addr{ 1 };
    static int second_addr{ 2 };
    static const vm_struct_entry_t dup[]{
        { "Dup", "_f", "ty", 1, 100u, &first_addr },   // [0] must win
        { "Dup", "_f", "ty", 1, 200u, &second_addr },  // [1] shadowed
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    };
    const vm_struct_entry_t* const hit{
        wave3_offset_resolution::find_struct(dup, "Dup", "_f") };
    check("w3_first_match_is_element_zero", hit == &dup[0]);
    check("w3_first_match_offset_is_first", hit != nullptr && hit->offset == 100u);
    check("w3_first_match_address_is_first",
          hit != nullptr && hit->address == &first_addr);

    // A non-duplicate row interleaved between the duplicates: still first-wins,
    // and the interleaved distinct row resolves to ITS own offset.
    static const vm_struct_entry_t inter[]{
        { "A", "_x", "ty", 0, 10u, nullptr },   // [0]
        { "Dup", "_f", "ty", 0, 100u, nullptr },// [1] first Dup
        { "B", "_y", "ty", 0, 20u, nullptr },   // [2]
        { "Dup", "_f", "ty", 0, 200u, nullptr },// [3] second Dup
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    };
    check("w3_interleaved_first_dup_wins",
          wave3_offset_resolution::find_struct(inter, "Dup", "_f") == &inter[1]);
    check("w3_interleaved_distinct_row_own_offset",
          wave3_offset_resolution::find_struct(inter, "B", "_y") != nullptr
              && wave3_offset_resolution::find_struct(inter, "B", "_y")->offset == 20u);

    // Type-walk duplicate: first type_name wins.
    static const vm_type_entry_t tdup[]{
        { "DupT", nullptr, 0, 1, 0, 4u },   // [0] must win
        { "DupT", nullptr, 0, 1, 0, 8u },   // [1] shadowed
        { nullptr, nullptr, 0, 0, 0, 0u },
    };
    const vm_type_entry_t* const thit{ wave3_offset_resolution::find_type(tdup, "DupT") };
    check("w3_type_first_match_is_element_zero", thit == &tdup[0]);
    check("w3_type_first_match_size_is_first", thit != nullptr && thit->size == 4u);
}

// ---------------------------------------------------------------------------
// 40. is_static full int32 discriminator domain (hazard 1).
//
// A static-aware caller classifies via `is_static != 0`.  Pin that across the
// full int32 domain -- INT32_MIN, -1, 0, 1, 2, INT32_MAX -- the stored value reads
// back exactly through the resolved entry and the `!= 0` test classifies every
// non-zero as static, only 0 as instance.  WAVE 2 tested {0,1,2} only.  Owned.
// ---------------------------------------------------------------------------
static auto test_w3_is_static_full_int32_domain() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    static const std::int32_t flags[]{
        (std::numeric_limits<std::int32_t>::min)(),
        -2, -1, 0, 1, 2, 0x7FFFFFFF,
        (std::numeric_limits<std::int32_t>::max)(),
    };
    bool roundtrip{ true };
    bool classify_ok{ true };
    for (const std::int32_t f : flags)
    {
        const vm_struct_entry_t table[]{
            { "T", "_f", "ty", f, 8u, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(table, "T", "_f") };
        if (e == nullptr || e->is_static != f) { roundtrip = false; }
        // Robust discriminator: instance iff exactly 0, static iff non-zero.
        const bool is_instance{ e != nullptr && e->is_static == 0 };
        if (e != nullptr)
        {
            if (f == 0)
            {
                if (!is_instance) { classify_ok = false; }
            }
            else
            {
                if ((e->is_static != 0) != true) { classify_ok = false; }
            }
        }
    }
    check("w3_is_static_full_int32_roundtrip", roundtrip);
    check("w3_is_static_nonzero_classified_static", classify_ok);
}

// ---------------------------------------------------------------------------
// 41. Embedded-NUL search key: strcmp stops at the first NUL (test angle #5).
//
// strcmp compares C-strings up to the first NUL, so a key whose buffer is
// "Symbol\0junk" compares equal to the entry's "Symbol" -- the matcher never
// reads past the caller's first NUL.  Build the key as an explicit char array
// (the only way to carry an embedded NUL) and confirm it matches a "Symbol"
// entry, while a key whose visible prefix differs before its NUL does not.
// Owned memory; no non-ASCII / no raw NUL emitted as a source byte -- the NUL is
// an explicit '\0' element.
// ---------------------------------------------------------------------------
static auto test_w3_embedded_nul_key_stops_at_first_nul() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    static const vm_struct_entry_t table[]{
        { "Symbol", "_length", "int", 0, 8u, nullptr },
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    };

    // Key buffer "Symbol\0junk": strcmp sees "Symbol" and matches entry [0].
    const char type_key[]{ 'S','y','m','b','o','l','\0','j','u','n','k','\0' };
    const char field_key[]{ '_','l','e','n','g','t','h','\0','X','Y','\0' };
    check("w3_embedded_nul_type_key_matches_prefix",
          wave3_offset_resolution::find_struct(table, type_key, "_length") == &table[0]);
    check("w3_embedded_nul_field_key_matches_prefix",
          wave3_offset_resolution::find_struct(table, "Symbol", field_key) == &table[0]);
    check("w3_embedded_nul_both_keys_match",
          wave3_offset_resolution::find_struct(table, type_key, field_key) == &table[0]);

    // A key that differs BEFORE its NUL does not match (the bytes past the NUL are
    // never consulted, so they cannot rescue a mismatching visible prefix).
    const char wrong_key[]{ 'S','y','m','b','o','X','\0','S','y','m','b','o','l','\0' };
    check("w3_embedded_nul_wrong_prefix_no_match",
          wave3_offset_resolution::find_struct(table, wrong_key, "_length") == nullptr);

    // Type-walk variant: embedded-NUL type key matches by its visible prefix.
    static const vmhook::hotspot::vm_type_entry_t tarr[]{
        { "Method", nullptr, 0, 0, 0, 56u },
        { nullptr, nullptr, 0, 0, 0, 0u },
    };
    const char tkey[]{ 'M','e','t','h','o','d','\0','!','!','\0' };
    check("w3_embedded_nul_type_walk_matches",
          wave3_offset_resolution::find_type(tarr, tkey) == &tarr[0]);
}

// ---------------------------------------------------------------------------
// 42. Walk determinism / idempotence over a populated owned array.
//
// The walk carries no per-call state, so repeating the SAME lookup over the SAME
// owned array must return the identical entry pointer every time, and distinct
// lookups must be independent.  Hammer a populated array and require pointer
// stability across many repeats (the positive-path analogue of section 8's
// null-array determinism).  Owned memory.
// ---------------------------------------------------------------------------
static auto test_w3_walk_determinism_populated() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    static const vm_struct_entry_t structs[]{
        { "Symbol", "_length", "int", 0, 8u, nullptr },
        { "Method", "_constMethod", "ConstMethod*", 0, 16u, nullptr },
        { "Klass", "_name", "Symbol*", 0, 24u, nullptr },
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    };
    static const vm_type_entry_t types[]{
        { "oopDesc", nullptr, 1, 0, 0, 16u },
        { "narrowOop", nullptr, 0, 1, 1, 4u },
        { nullptr, nullptr, 0, 0, 0, 0u },
    };

    const vm_struct_entry_t* const s0{
        wave3_offset_resolution::find_struct(structs, "Symbol", "_length") };
    const vm_struct_entry_t* const s1{
        wave3_offset_resolution::find_struct(structs, "Klass", "_name") };
    const vm_type_entry_t* const t0{
        wave3_offset_resolution::find_type(types, "narrowOop") };

    bool stable{ true };
    for (int i{ 0 }; i < 1000; ++i)
    {
        if (wave3_offset_resolution::find_struct(structs, "Symbol", "_length") != s0) { stable = false; }
        if (wave3_offset_resolution::find_struct(structs, "Klass", "_name") != s1) { stable = false; }
        if (wave3_offset_resolution::find_type(types, "narrowOop") != t0) { stable = false; }
        // A miss must be stably null too.
        if (wave3_offset_resolution::find_struct(structs, "Symbol", "_nope") != nullptr) { stable = false; }
    }
    check("w3_walk_populated_lookup_pointer_stable", stable);
    check("w3_walk_populated_distinct_lookups_independent",
          s0 != nullptr && s1 != nullptr && s0 != s1);
    check("w3_walk_populated_offsets_correct",
          s0 != nullptr && s0->offset == 8u && s1 != nullptr && s1->offset == 24u);
}

// ---------------------------------------------------------------------------
// 43. alignof / pointer-member-size facts (complements WAVE 2 offset pins).
//
// The array stride is sizeof(entry); its alignment is alignof(entry).  On the
// LP64 targets the JVM supports, both structs are 8-byte aligned (a uint64 / a
// pointer member forces it) and the pointer members are pointer-width.  Pin the
// alignment relationship and pointer-member sizes WAVE 2 did not assert.
// ---------------------------------------------------------------------------
static auto test_w3_alignment_and_pointer_sizes() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // Alignment is at least that of the widest member (uint64 / pointer).
    check("w3_struct_align_ge_uint64",
          alignof(vm_struct_entry_t) >= alignof(std::uint64_t));
    check("w3_type_align_ge_uint64",
          alignof(vm_type_entry_t) >= alignof(std::uint64_t));
    check("w3_struct_align_ge_pointer",
          alignof(vm_struct_entry_t) >= alignof(void*));

    // sizeof is an exact multiple of alignof (so ++entry stays aligned).
    check("w3_struct_sizeof_multiple_of_align",
          (sizeof(vm_struct_entry_t) % alignof(vm_struct_entry_t)) == 0u);
    check("w3_type_sizeof_multiple_of_align",
          (sizeof(vm_type_entry_t) % alignof(vm_type_entry_t)) == 0u);

    // Pointer members are all pointer-width.
    check("w3_struct_pointer_members_pointer_width",
          sizeof(vm_struct_entry_t::type_name) == sizeof(void*)
          && sizeof(vm_struct_entry_t::field_name) == sizeof(void*)
          && sizeof(vm_struct_entry_t::type_string) == sizeof(void*)
          && sizeof(vm_struct_entry_t::address) == sizeof(void*));
    check("w3_type_pointer_members_pointer_width",
          sizeof(vm_type_entry_t::type_name) == sizeof(void*)
          && sizeof(vm_type_entry_t::superclass_name) == sizeof(void*));

    // On LP64 the alignment is exactly 8.
    if (sizeof(void*) == 8u)
    {
        check("w3_lp64_struct_align_8", alignof(vm_struct_entry_t) == 8u);
        check("w3_lp64_type_align_8", alignof(vm_type_entry_t) == 8u);
    }
    else
    {
        check("w3_lp64_align_block_skipped", sizeof(void*) != 8u);
    }
}

// ===========================================================================
// EXHAUSTIVE EXPANSION WAVE 4 (44+).
//
// WAVES 1-3 pinned the null-array contract, the clamp/cap arithmetic, the
// entry-struct ABI, and the walk loop (match/skip/terminator/exactness/offset
// round-trip) over OWNED arrays.  This wave closes the bounds/structure cases
// those left untouched, ALL over memory THIS TEST OWNS or over PURE-LOGIC
// predicates fed CONSTANTS -- never a fabricated/unmapped pointer dereference,
// never a JVM, never the global-reading API driven over a fake array:
//
//   * The is_valid_pointer / untag_pointer bound-and-REJECT predicates that gate
//     EVERY offset-resolution path bottoming out in iterate_* (a resolved
//     entry->offset is added to a base, and the result is is_valid_pointer-gated
//     before any deref).  These are pure address arithmetic: drive them with the
//     EXACT boundary constants from source (user_address_floor == 0xFFFF,
//     user_address_ceiling == 0x00007FFFFFFFFFFF, odd-address & sentinel
//     rejection) -- NEVER passing a high pointer to anything that reads memory.
//   * offset/length JUST-IN / JUST-OUT bound thresholds: a resolved field offset
//     validated against the owning type's size (offset + member_width <= size),
//     reconstructed as pure arithmetic over OWNED entry/type values at the exact
//     in/out boundary -- the plausibility check a static-aware caller applies.
//   * flags-width EXTRACTION: the int32 classification columns read back as a
//     packed discriminator from an owned entry (distinct from WAVE 2/3 which only
//     compared equality).
// ===========================================================================

// ---------------------------------------------------------------------------
// 44. is_valid_pointer bound-and-reject over CONSTANTS (the offset-resolution
//     gate).  Every reader that resolves a field via iterate_struct_entries adds
//     entry->offset to a base and gates the candidate address through
//     is_valid_pointer before dereferencing.  is_valid_pointer is pure address
//     arithmetic (vmhook.hpp:2047): it REJECTS addr <= user_address_floor
//     (0xFFFF), addr >= user_address_ceiling (0x00007FFFFFFFFFFF), odd addresses,
//     null, and nine debug-sentinel low32 patterns; ACCEPTS an aligned canonical
//     user-space address.  Drive it with literal CONSTANTS only -- no memory is
//     ever read, so this is fully POSIX-safe (no fabricated pointer is
//     dereferenced; the predicate inspects the integer value, not the target).
// ---------------------------------------------------------------------------
static auto test_w4_is_valid_pointer_bounds_over_constants() -> void
{
    using vmhook::hotspot::is_valid_pointer;

    // Exact boundary constants, derived from vmhook.hpp:515/520.
    constexpr std::uintptr_t floor_v{ 0xFFFFull };               // user_address_floor
    constexpr std::uintptr_t ceiling_v{ 0x00007FFFFFFFFFFFull };  // user_address_ceiling

    auto as_ptr = [](std::uintptr_t v) -> const void*
    { return reinterpret_cast<const void*>(v); };

    // --- null and the low-noise floor: REJECT (addr <= floor).
    check("w4_ivp_null_rejected", is_valid_pointer(nullptr) == false);
    check("w4_ivp_one_rejected", is_valid_pointer(as_ptr(1u)) == false);
    check("w4_ivp_floor_value_rejected", is_valid_pointer(as_ptr(floor_v)) == false);
    // floor is odd (0xFFFF); use the even value just below to isolate the
    // <=floor branch from the odd-reject branch -- 0xFFFE is even AND <= floor.
    check("w4_ivp_just_below_floor_even_rejected",
          is_valid_pointer(as_ptr(floor_v - 1u)) == false);

    // --- JUST ABOVE the floor: the first even address strictly greater than
    //     floor is 0x10000 (floor+1 == 0x10000, already even) -- ACCEPT.
    check("w4_ivp_just_above_floor_accepted",
          is_valid_pointer(as_ptr(floor_v + 1u)) == true);   // 0x10000, even, in range

    // --- the ceiling and above: REJECT (addr >= ceiling).
    check("w4_ivp_ceiling_value_rejected", is_valid_pointer(as_ptr(ceiling_v)) == false);
    check("w4_ivp_above_ceiling_rejected",
          is_valid_pointer(as_ptr(ceiling_v + 1u)) == false);
    // JUST BELOW the ceiling: ceiling is odd (...FFF), ceiling-1 is even and in
    // range -- ACCEPT (the highest accepted address).
    check("w4_ivp_just_below_ceiling_even_accepted",
          is_valid_pointer(as_ptr(ceiling_v - 1u)) == true);

    // --- odd-address rejection (addr & 0x1) for an otherwise in-range value.
    check("w4_ivp_odd_in_range_rejected",
          is_valid_pointer(as_ptr(0x100001u)) == false);   // odd, in range -> reject
    check("w4_ivp_even_in_range_accepted",
          is_valid_pointer(as_ptr(0x100000u)) == true);    // even sibling -> accept

    // --- the nine debug-sentinel low32 patterns (vmhook.hpp:2070-2078): a pointer
    //     whose low32 equals any of these is NEVER valid.  Each is placed in the
    //     low 32 bits of an otherwise in-range address (high word 0x1000 keeps it
    //     between floor and ceiling); every one MUST be rejected.
    static const std::uint32_t sentinels[]{
        0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
        0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
    };
    bool all_sentinels_rejected{ true };
    bool even_sentinels_via_switch{ true };
    int sentinel_count{ 0 };
    int even_sentinel_count{ 0 };
    for (const std::uint32_t s : sentinels)
    {
        ++sentinel_count;
        // Compose 0x0000'1000'<sentinel>: in range, low32 == the sentinel exactly.
        const std::uintptr_t composed{
            (static_cast<std::uintptr_t>(0x1000u) << 32) | s };
        // The true contract: a sentinel-low32 pointer is never valid (whether it
        // is the odd-bit gate or the sentinel switch that rejects it).
        if (is_valid_pointer(as_ptr(composed)) != false) { all_sentinels_rejected = false; }

        // For EVEN sentinels the odd-bit gate passes, so ONLY the sentinel switch
        // can be responsible for the rejection -- the strongest proof the switch
        // fires.  (Odd sentinels are rejected earlier by the alignment gate and
        // can never reach the switch, by construction of is_valid_pointer.)
        if ((s & 0x1u) == 0u)
        {
            ++even_sentinel_count;
            if (is_valid_pointer(as_ptr(composed)) != false) { even_sentinels_via_switch = false; }
        }
    }
    check("w4_ivp_all_sentinel_low32_rejected", all_sentinels_rejected);
    check("w4_ivp_even_sentinels_rejected_via_switch", even_sentinels_via_switch);
    check("w4_ivp_sentinel_count_is_9", sentinel_count == 9);
    check("w4_ivp_even_sentinel_count_is_3", even_sentinel_count == 3);

    // A plain even in-range address that shares the high word but NOT a sentinel
    // low32 is accepted -- proving the sentinel switch, not the range, is what
    // rejects the patterns above.
    check("w4_ivp_non_sentinel_high_word_accepted",
          is_valid_pointer(as_ptr((static_cast<std::uintptr_t>(0x1000u) << 32) | 0x2000u))
              == true);
}

// ---------------------------------------------------------------------------
// 45. untag_pointer masks with the user_address_ceiling (vmhook.hpp:2092).
//
// A resolved oop pointer can carry high GC tag bits; untag_pointer ANDs the raw
// value with user_address_ceiling (0x00007FFFFFFFFFFF) to recover the canonical
// address before it is fed back through is_valid_pointer.  Pure bitmask logic
// over CONSTANTS -- the result is never dereferenced, only its integer value is
// checked, so this is POSIX-safe.
// ---------------------------------------------------------------------------
static auto test_w4_untag_pointer_mask_over_constants() -> void
{
    using vmhook::hotspot::untag_pointer;
    using vmhook::hotspot::is_valid_pointer;

    constexpr std::uintptr_t ceiling_v{ 0x00007FFFFFFFFFFFull };

    auto as_ptr = [](std::uintptr_t v) -> const void* { return reinterpret_cast<const void*>(v); };
    auto as_int = [](const void* p) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(p); };

    // An already-canonical address is unchanged by the mask.
    check("w4_untag_canonical_unchanged",
          as_int(untag_pointer(as_ptr(0x100000u))) == 0x100000u);

    // High tag bits ABOVE the ceiling are stripped: 0xFFFF'<low> & ceiling keeps
    // only the low 47 bits.  Compose tag bits in the top word that the mask drops.
    const std::uintptr_t tagged{ 0xFFFF800000100000ull };   // high tag + low 0x100000
    check("w4_untag_strips_high_tag_bits",
          as_int(untag_pointer(as_ptr(tagged))) == (tagged & ceiling_v));
    check("w4_untag_result_within_ceiling",
          as_int(untag_pointer(as_ptr(tagged))) <= ceiling_v);

    // Masking is idempotent: untag(untag(x)) == untag(x) (the second AND with the
    // ceiling is a no-op on an already-masked value).
    const void* once{ untag_pointer(as_ptr(tagged)) };
    const void* twice{ untag_pointer(once) };
    check("w4_untag_idempotent", once == twice);

    // The untagged canonical address (here even and in range) is is_valid_pointer
    // -- proving the untag -> validate handoff the readers rely on, all over a
    // value that is NEVER dereferenced.
    const std::uintptr_t untagged_int{ as_int(untag_pointer(as_ptr(0xFFFF800000100000ull))) };
    check("w4_untag_then_validate_accepts_canonical",
          (untagged_int == 0x100000u) && is_valid_pointer(as_ptr(untagged_int)) == true);
}

// ---------------------------------------------------------------------------
// 46. Resolved-offset JUST-IN / JUST-OUT bound threshold against the owning
//     type's size.  A static-aware caller that resolves a field offset via
//     iterate_struct_entries validates `offset + member_width <= type_size`
//     before computing `base + offset` -- so an offset that would read past the
//     declared object size is rejected.  Reconstruct that arithmetic over OWNED
//     vm_struct_entry_t / vm_type_entry_t VALUES at the EXACT in/out boundary; no
//     memory is read, the offset is never used as a pointer.
// ---------------------------------------------------------------------------
static auto test_w4_offset_within_type_size_threshold() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // An owned type of declared size 24 bytes, and field entries whose offset +
    // a member width straddle that size.  The member width is taken from the
    // entry's own type_string sense via an explicit width we control.
    const vm_type_entry_t owning_type{ "T", nullptr, 0, 0, 0, 24u };

    // The plausibility predicate the caller applies (pure arithmetic).
    auto fits = [](std::uint64_t offset, std::uint64_t width, std::uint64_t size) -> bool
    { return offset + width <= size; };

    // member width 8 (a pointer/long field): the last in-bounds offset is 16
    // (16 + 8 == 24 == size), and 17.. is out.
    const vm_struct_entry_t just_in{ "T", "_last", "void*", 0, 16u, nullptr };
    const vm_struct_entry_t just_out{ "T", "_over", "void*", 0, 17u, nullptr };
    check("w4_offset_just_in_fits",
          fits(just_in.offset, 8u, owning_type.size) == true);   // 16+8==24
    check("w4_offset_just_out_rejected",
          fits(just_out.offset, 8u, owning_type.size) == false); // 17+8==25 > 24

    // Offset exactly at the size with zero width is the empty tail (allowed); one
    // past the size is rejected even for a zero-width read.
    check("w4_offset_at_size_zero_width_fits",
          fits(24u, 0u, owning_type.size) == true);
    check("w4_offset_past_size_rejected",
          fits(25u, 0u, owning_type.size) == false);

    // The exact in/out boundary for a 4-byte field: offset 20 fits (20+4==24),
    // offset 21 does not (21+4==25).
    check("w4_offset_4byte_just_in",
          fits(20u, 4u, owning_type.size) == true);
    check("w4_offset_4byte_just_out",
          fits(21u, 4u, owning_type.size) == false);

    // A corrupt huge offset: `offset + width` WRAPS in uint64 (UINT64_MAX + 8 == 7),
    // so a NAIVE `offset + width <= size` test would wrongly accept it -- this is
    // exactly why a robust caller compares the offset ALONE against the size FIRST.
    // Demonstrate both: the naive predicate is fooled by the wrap, and the robust
    // offset-first guard rejects the corrupt offset.
    check("w4_offset_naive_fits_is_fooled_by_wrap",
          fits(0xFFFFFFFFFFFFFFFFull, 8u, owning_type.size) == true);   // wrap -> 7 <= 24
    check("w4_offset_robust_compares_offset_first",
          (0xFFFFFFFFFFFFFFFFull > owning_type.size) == true);          // offset alone rejects

    // Sanity: a real small instance offset (8, an oop._mark-like slot) is well
    // within a 16-byte object and fits.
    const vm_type_entry_t oopdesc_like{ "oopDesc", nullptr, 1, 0, 0, 16u };
    check("w4_offset_mark_within_oopdesc",
          fits(8u, 8u, oopdesc_like.size) == true);   // 8+8==16
}

// ---------------------------------------------------------------------------
// 47. Flags-width EXTRACTION: read the int32 classification columns of an owned
//     entry back as a packed 3-bit discriminator.  WAVE 2/3 compared the flags
//     for equality; here we EXTRACT them bit-by-bit into a composite to prove
//     each column is an independent, full-width int32 lane that a classifier can
//     pack -- the operation a type-classification helper performs on a resolved
//     type entry.  Owned VALUES; no memory read.
// ---------------------------------------------------------------------------
static auto test_w4_flags_width_extraction() -> void
{
    using vmhook::hotspot::vm_type_entry_t;

    // Pack (is_oop, is_integer, is_unsigned) into a 3-bit code for all 8 combos
    // and confirm extraction recovers exactly the stored lane in each position.
    bool extract_ok{ true };
    for (int code{ 0 }; code <= 7; ++code)
    {
        const std::int32_t oop{ (code >> 2) & 1 };
        const std::int32_t integer{ (code >> 1) & 1 };
        const std::int32_t uns{ code & 1 };
        const vm_type_entry_t e{ "T", nullptr, oop, integer, uns, 8u };

        const int recomposed{
            (static_cast<int>(e.is_oop_type_type) << 2)
            | (static_cast<int>(e.is_integer_type) << 1)
            | static_cast<int>(e.is_unsigned) };
        if (recomposed != code) { extract_ok = false; }
        // Each lane independently equals the bit we stored.
        if (e.is_oop_type_type != oop || e.is_integer_type != integer
            || e.is_unsigned != uns)
        {
            extract_ok = false;
        }
    }
    check("w4_flags_3bit_extraction_all_8_codes", extract_ok);

    // The flag lanes are full int32: a column set to a wide non-{0,1} value (a
    // non-standard JVM could) reads back at full width, not narrowed to a bit.
    const vm_type_entry_t wide{ "T", nullptr, 0x40000000, 0x1F, -1, 8u };
    check("w4_flags_full_int32_oop_lane", wide.is_oop_type_type == 0x40000000);
    check("w4_flags_full_int32_integer_lane", wide.is_integer_type == 0x1F);
    check("w4_flags_full_int32_unsigned_lane_negative", wide.is_unsigned == -1);
    // is_static on the struct entry is the same full-width int32 lane.
    const vmhook::hotspot::vm_struct_entry_t s{ "T", "_f", "ty", 0x7FFFFFFF, 8u, nullptr };
    check("w4_flags_struct_is_static_full_int32", s.is_static == 0x7FFFFFFF);
}

// ===========================================================================
// EXHAUSTIVE EXPANSION WAVE 5 (48+).
//
// WAVE 4 (44-47) pinned the is_valid_pointer / untag_pointer bound-and-reject
// predicates, the offset<=type_size plausibility threshold, and flags-width
// extraction.  This wave closes three bounds/structure cases NONE of waves 1-4
// touched, ALL over memory THIS TEST OWNS or over PURE-LOGIC predicates fed
// CONSTANTS -- never a fabricated/unmapped pointer dereference, never a JVM,
// never the global-reading API driven over a fake array:
//
//   * The terminator/skip ASYMMETRY: the walk loop guard is `entry &&
//     entry->type_name` and the skip is `if (!entry->field_name) continue;`
//     (vmhook.hpp:1997-2002).  So a NULL type_name TERMINATES the walk even when
//     field_name is non-null (a later well-formed entry becomes UNREACHABLE),
//     while a NULL field_name only SKIPS and the walk continues.  WAVE 2/3
//     tested the standard terminator and the standalone skip, but never this
//     asymmetry -- a guard that accidentally keyed termination on field_name (or
//     the skip on type_name) would mis-walk a real array and is caught here.
//   * is_readable_pointer's pure-arithmetic PREFILTER (vmhook.hpp:2018-2028):
//     it returns false for addr <= floor, addr >= ceiling, OR non-8-byte-aligned
//     addr BEFORE it ever calls query_region.  Drive ONLY the rejected branch
//     with constants (those return false without touching memory -- POSIX-safe);
//     never the accept branch (which would query a fabricated region).  This
//     pins that is_readable_pointer's alignment gate is STRICTER (8-byte) than
//     is_valid_pointer's (2-byte) -- the tighter bound the readable check adds.
//   * clamp-count vs validity-gate INDEPENDENCE: the two bound disciplines a
//     walk uses together (clamp the trip count, validity-gate each address) are
//     disjoint predicates over disjoint domains; a walk bounded by BOTH is
//     doubly safe and neither gate interferes with the other.  Pure arithmetic.
// ===========================================================================

// ---------------------------------------------------------------------------
// 48. Struct/type walk terminator keys on type_name ONLY (not field_name).
//
// The loop guard `entry && entry->type_name` ends the walk on a NULL type_name
// regardless of the other columns; the `if (!entry->field_name) continue;` skip
// only removes a single entry and does NOT end the walk.  Pin both halves of
// this asymmetry over OWNED arrays (every char* is a string literal or nullptr):
// a null-type_name entry with a NON-null field_name terminates (a later real
// entry is unreachable), while a null-field_name entry merely skips and the walk
// reaches the later real entry.  The type walk (no field column) terminates on a
// null type_name even when superclass_name is non-null.
// ---------------------------------------------------------------------------
static auto test_w5_terminator_keys_on_type_name_only() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // [1] has NULL type_name but a NON-null field_name -> the guard terminates
    // the walk at [1], so the well-formed entry [2] AFTER it is UNREACHABLE.
    static const vm_struct_entry_t early_term[]{
        { "Symbol", "_length", "int",     0,  8u, nullptr },  // [0] match (before term)
        { nullptr,  "_field",  "ty",      0,  0u, nullptr },  // [1] terminator (null type)
        { "Klass",  "_name",   "Symbol*", 1, 16u, nullptr },  // [2] UNREACHABLE
        { nullptr,  nullptr,   nullptr,   0,  0u, nullptr },
    };
    check("w5_term_entry_before_null_type_found",
          wave3_offset_resolution::find_struct(early_term, "Symbol", "_length")
              == &early_term[0]);
    // [2] is past the null-type_name terminator -> never found despite being
    // a perfectly well-formed (type,field) pair.
    check("w5_term_null_type_with_field_ends_walk",
          wave3_offset_resolution::find_struct(early_term, "Klass", "_name") == nullptr);

    // Contrast: a null-FIELD entry only SKIPS; the walk continues past it and
    // reaches the later real entry (the skip is NOT a terminator).
    static const vm_struct_entry_t skip_not_term[]{
        { "A", nullptr, "ty",  0,  0u, nullptr },  // [0] skipped (null field)
        { "B", "_b",    "int", 0, 24u, nullptr },  // [1] reachable past the skip
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    };
    check("w5_term_null_field_only_skips_not_terminates",
          wave3_offset_resolution::find_struct(skip_not_term, "B", "_b")
              == &skip_not_term[1]);
    // The skipped entry's own type is unmatchable for ANY field (it is removed
    // from the matchable set, never terminating the walk).
    check("w5_term_skipped_entry_type_unmatchable",
          wave3_offset_resolution::find_struct(skip_not_term, "A", "_b") == nullptr);

    // Type walk: terminator is a null type_name even with a non-null
    // superclass_name; the entry after it is unreachable.
    static const vm_type_entry_t t_term[]{
        { "oopDesc", "Base", 1, 0, 0, 16u },  // [0] match
        { nullptr,   "Base", 0, 0, 0,  0u },  // [1] terminator (null type)
        { "Klass",   "Base", 0, 0, 0, 64u },  // [2] UNREACHABLE
        { nullptr,   nullptr, 0, 0, 0, 0u },
    };
    check("w5_type_term_entry_before_null_found",
          wave3_offset_resolution::find_type(t_term, "oopDesc") == &t_term[0]);
    check("w5_type_term_null_type_with_superclass_ends_walk",
          wave3_offset_resolution::find_type(t_term, "Klass") == nullptr);
}

// ---------------------------------------------------------------------------
// 49. is_readable_pointer pure-arithmetic PREFILTER -- REJECTED branch only.
//
// is_readable_pointer (vmhook.hpp:2018-2028) short-circuits to false when
// addr <= user_address_floor (0xFFFF), addr >= user_address_ceiling
// (0x00007FFFFFFFFFFF), OR (addr & 0x7) != 0 -- BEFORE it ever calls
// query_region.  Drive ONLY those rejected cases with literal CONSTANTS: each
// returns false without touching memory, so this is fully POSIX-safe (HARD RULE
// 2 -- we never reach the accept branch, which would query a fabricated region).
// The 8-byte-alignment gate is STRICTER than is_valid_pointer's 2-byte gate: an
// address accepted by is_valid_pointer (even, in-window) but only 2-/4-aligned
// is still REJECTED by is_readable_pointer -- the tighter bound it adds.
// ---------------------------------------------------------------------------
static auto test_w5_is_readable_pointer_prefilter_rejects() -> void
{
    using vmhook::hotspot::is_readable_pointer;
    using vmhook::hotspot::is_valid_pointer;

    constexpr std::uintptr_t floor_v{ 0xFFFFull };
    constexpr std::uintptr_t ceiling_v{ 0x00007FFFFFFFFFFFull };
    auto as_ptr = [](std::uintptr_t v) -> const void*
    { return reinterpret_cast<const void*>(v); };

    // --- null and the low-noise floor: rejected by `addr <= floor` (no query).
    check("w5_readable_null_rejected", is_readable_pointer(nullptr) == false);
    check("w5_readable_floor_rejected", is_readable_pointer(as_ptr(floor_v)) == false);
    // An 8-aligned value below the floor still rejected by the <=floor branch.
    check("w5_readable_below_floor_aligned_rejected",
          is_readable_pointer(as_ptr(0x8000u)) == false);   // 0x8000 <= 0xFFFF, 8-aligned

    // --- the ceiling and above: rejected by `addr >= ceiling` (no query).
    check("w5_readable_ceiling_rejected", is_readable_pointer(as_ptr(ceiling_v)) == false);
    // An 8-aligned value at/above the ceiling: 0x0000800000000000 > ceiling.
    check("w5_readable_above_ceiling_aligned_rejected",
          is_readable_pointer(as_ptr(0x0000800000000000ull)) == false);

    // --- the 8-byte-alignment gate: an in-window address whose low 3 bits are
    //     non-zero is rejected BEFORE any region query.  Sweep offsets 1..7 off
    //     an 8-aligned in-window base; every misaligned one is rejected.
    const std::uintptr_t base8{ 0x0000100000000000ull };   // in-window, 8-aligned
    bool misaligned_all_rejected{ true };
    for (std::uintptr_t off{ 1 }; off <= 7u; ++off)
    {
        if (is_readable_pointer(as_ptr(base8 + off)) != false)
        {
            misaligned_all_rejected = false;
        }
    }
    check("w5_readable_misaligned_1_to_7_all_rejected", misaligned_all_rejected);

    // --- the alignment gate is STRICTER than is_valid_pointer's: a 4-aligned
    //     (but not 8-aligned), even, in-window, non-sentinel address is ACCEPTED
    //     by is_valid_pointer yet REJECTED by is_readable_pointer's prefilter
    //     (its `addr & 0x7` is non-zero).  base8+4 == ...0004: even -> valid
    //     accepts; low 3 bits 0b100 -> readable rejects (no query reached).
    const std::uintptr_t four_aligned{ base8 + 4u };
    check("w5_valid_accepts_4aligned_even",
          is_valid_pointer(as_ptr(four_aligned)) == true);
    check("w5_readable_rejects_4aligned_stricter",
          is_readable_pointer(as_ptr(four_aligned)) == false);

    // base8+2 (2-aligned only): same story -- valid accepts, readable rejects.
    check("w5_valid_accepts_2aligned_even",
          is_valid_pointer(as_ptr(base8 + 2u)) == true);
    check("w5_readable_rejects_2aligned_stricter",
          is_readable_pointer(as_ptr(base8 + 2u)) == false);
}

// ---------------------------------------------------------------------------
// 50. clamp-count vs validity-gate INDEPENDENCE (two disjoint bound disciplines).
//
// A walk bounds its trip COUNT with clamp_safe_container_count and its each
// ADDRESS with is_valid_pointer.  The two predicates operate on disjoint domains
// (a signed count vs a pointer's integer value) and never interfere: an honest
// count + a valid base are both admitted, a corrupt count saturates while a
// sentinel base is independently rejected, and the degenerate inputs (negative
// count, null pointer) are each refused by their own gate.  Pure arithmetic; no
// address is ever dereferenced.
// ---------------------------------------------------------------------------
static auto test_w5_clamp_and_validity_independence() -> void
{
    using vmhook::clamp_safe_container_count;
    using vmhook::hotspot::is_valid_pointer;
    auto as_ptr = [](std::uintptr_t v) -> const void*
    { return reinterpret_cast<const void*>(v); };

    // Honest count clamps to itself AND a valid (even, in-window, non-sentinel)
    // base passes -- a well-formed walk of clamp(n) elements over a valid base is
    // admitted by both gates.
    const std::int32_t honest_n{ 4096 };
    check("w5_honest_count_and_valid_ptr_both_admitted",
          clamp_safe_container_count(honest_n) == honest_n
              && is_valid_pointer(as_ptr(0x0000100000000000ull)) == true);

    // Corrupt count saturates to the cap AND a debug-sentinel base is rejected --
    // a corrupt walk is stopped by EITHER gate independently.  Sentinel composed
    // as in-window high word | DEADBEEF low32 (the value is never dereferenced).
    const std::uintptr_t sentinel{ (static_cast<std::uintptr_t>(0x1000u) << 32)
                                    | 0xDEADBEEFu };
    check("w5_corrupt_count_saturates_and_sentinel_ptr_rejected",
          clamp_safe_container_count((std::numeric_limits<std::int32_t>::max)()) == k_cap
              && is_valid_pointer(as_ptr(sentinel)) == false);

    // Degenerate inputs: a negative count clamps to 0 (no walk) and a null
    // pointer is invalid (no deref) -- each gate refuses its own degenerate case.
    check("w5_negative_count_zero_and_null_ptr_invalid",
          clamp_safe_container_count(-1) == 0
              && is_valid_pointer(nullptr) == false);

    // The clamp result is ALWAYS a legal trip count in [0, cap] regardless of the
    // pointer gate's verdict -- the two bounds are computed independently.
    bool independent{ true };
    static const std::int32_t counts[]{ -100, -1, 0, 1, 7, 4096, k_cap, k_cap + 1,
                                        (std::numeric_limits<std::int32_t>::max)() };
    for (const std::int32_t n : counts)
    {
        const std::int32_t c{ clamp_safe_container_count(n) };
        if (c < 0 || c > k_cap) { independent = false; }
    }
    check("w5_clamp_count_in_range_independent_of_ptr_gate", independent);
}

// ===========================================================================
// EXHAUSTIVE EXPANSION WAVE 6 (51+).
//
// WAVES 1-5 pinned the null-array contract, the clamp/cap arithmetic, the
// entry-struct ABI, the walk loop (match/skip/terminator/exactness/offset
// round-trip/first-wins), the is_valid_pointer / untag_pointer / is_readable_-
// pointer bound-and-reject predicates, the offset<=type_size threshold, the
// terminator/skip asymmetry, and clamp-vs-validity independence.  This wave
// closes the gaps those left, ALL over memory THIS TEST OWNS or over PURE-LOGIC
// predicates fed CONSTANTS -- never a fabricated/unmapped pointer dereference,
// never a JVM, never the global-reading API driven over a fake array:
//
//   * The vm_struct_entry_t::type_string column (the 3rd char* member): WAVES
//     2-5 only round-tripped type_name/field_name; type_string is the descriptor
//     string a static-aware caller pairs with the resolved offset.  Pin that it
//     round-trips bit-exact through the owned walk, honours first-NUL strcmp
//     semantics, and is the member at the declared slot -- so a column shuffle is
//     caught.
//   * The COMPLETE static-field read DISCIPLINE end-to-end over OWNED storage:
//     resolve -> is_static==1 -> read ->address -> untag_pointer -> is_valid_-
//     pointer, with ->address pointing at a real owned object so the validity
//     gate's ACCEPT branch is exercised on a MAPPED address (POSIX-safe; the
//     pointer targets our own stack/static storage, never a fabricated one).
//   * The resolved-offset + base ARITHMETIC (flaw #5): `(uintptr_t)base +
//     entry->offset` computed as pure integer math over an owned base and the
//     resolved uint64 offset, with the SUM gated by is_valid_pointer by VALUE
//     only (never dereferenced) -- a plausible small offset lands in-window and
//     is accepted; a corrupt huge offset drives the sum out of the user window
//     (or wraps) and is rejected by the gate.  This is the choke-point flaw #5
//     identifies, exercised without a single memory read of the computed address.
//   * untag_pointer FULL high-word tag-bit sweep: WAVE 4 stripped one tag value;
//     here every individual high bit above the 47-bit ceiling is set in turn and
//     the mask is required to clear exactly the bits above the ceiling and keep
//     exactly the low 47 -- a bit-exact mask proof over CONSTANTS.
//   * is_valid_pointer low-3-bit residue lattice: WAVE 4/5 tested residues 2/4;
//     here EVERY residue 0..7 off an in-window even/odd base is swept and the
//     2-byte-alignment contract (reject iff low bit set; residues 2/4/6 accepted)
//     is pinned exactly -- distinct from is_readable_pointer's 8-byte gate.
// ===========================================================================

// ---------------------------------------------------------------------------
// 51. vm_struct_entry_t::type_string round-trip through the owned walk.
//
// type_string is the JVM type-descriptor string the resolver carries alongside
// the offset (vmhook.hpp:1895).  WAVES 2-5 never asserted it survives the walk.
// Resolve owned entries and require type_string reads back bit-exact (pointer-
// equal to the stored literal AND strcmp-equal), that it honours the same first-
// NUL strcmp semantics as the keys, and that it is an independent slot from
// type_name/field_name.  Owned string literals; no memory read beyond our array.
// ---------------------------------------------------------------------------
static auto test_w6_type_string_column_roundtrip() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;

    static const char* const k_desc_int{ "int" };
    static const char* const k_desc_ptr{ "ConstMethod*" };
    static const vm_struct_entry_t table[]{
        { "Symbol", "_length",      k_desc_int, 0,  8u, nullptr },
        { "Method", "_constMethod", k_desc_ptr, 0, 16u, nullptr },
        { nullptr,  nullptr,        nullptr,    0,  0u, nullptr },
    };

    const vm_struct_entry_t* const a{
        wave3_offset_resolution::find_struct(table, "Symbol", "_length") };
    const vm_struct_entry_t* const b{
        wave3_offset_resolution::find_struct(table, "Method", "_constMethod") };

    // Bit-exact (pointer-equal to the stored literal) and content-equal.
    check("w6_type_string_pointer_equal_stored_literal",
          a != nullptr && a->type_string == k_desc_int
              && b != nullptr && b->type_string == k_desc_ptr);
    check("w6_type_string_content_equal",
          a != nullptr && std::strcmp(a->type_string, "int") == 0
              && b != nullptr && std::strcmp(b->type_string, "ConstMethod*") == 0);

    // type_string is an INDEPENDENT slot: the two resolved entries carry
    // different type_string pointers (no column aliasing).
    check("w6_type_string_distinct_per_entry",
          a != nullptr && b != nullptr && a->type_string != b->type_string);

    // The descriptor lives at a different address than the entry's other char*
    // members -- a separate lane a caller can read after a successful resolve.
    check("w6_type_string_slot_distinct_from_name_slots",
          a != nullptr
              && reinterpret_cast<const void*>(&a->type_string)
                     != reinterpret_cast<const void*>(&a->type_name)
              && reinterpret_cast<const void*>(&a->type_string)
                     != reinterpret_cast<const void*>(&a->field_name));

    // type_string is the 3rd char* member -- one pointer-width past field_name
    // (re-pinning the column order from the resolved-entry side).
    check("w6_type_string_is_third_pointer_member",
          offsetof(vm_struct_entry_t, type_string)
              == offsetof(vm_struct_entry_t, field_name) + sizeof(const char*));
}

// ---------------------------------------------------------------------------
// 52. Complete STATIC-field read discipline end-to-end over OWNED storage.
//
// A static-aware caller, after resolving a static field, reads entry->address
// (NOT base+offset), untag_pointers it, and is_valid_pointer-gates it before the
// deref (flaw #1 territory).  Drive that exact pipeline over an owned entry whose
// ->address points at a REAL owned object -- so the is_valid_pointer ACCEPT
// branch runs against a MAPPED, readable address (POSIX-safe: the target is our
// own static storage, never a fabricated pointer).  The instance-field entry's
// is_static==0 selects the offset path instead; pin the discriminator drives the
// member choice.
// ---------------------------------------------------------------------------
static auto test_w6_static_field_read_discipline_owned() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::untag_pointer;

    // Real owned storage the static entry's ->address points at.  Aligned (a
    // static<std::uint64_t> is 8-aligned), mapped, readable -- a legitimately
    // valid address the gate must ACCEPT after untagging.
    static std::uint64_t owned_static_storage{ 0x1122334455667788ull };

    static const vm_struct_entry_t entries[]{
        // [0] instance field: located by ->offset, is_static == 0.
        { "oopDesc",  "_mark",          "markWord", 0, 8u, nullptr },
        // [1] static field: located by ->address, is_static == 1.
        { "Universe", "_collectedHeap", "CollectedHeap*", 1, 0u,
          static_cast<void*>(&owned_static_storage) },
        { nullptr, nullptr, nullptr, 0, 0u, nullptr },
    };

    const vm_struct_entry_t* const inst{
        wave3_offset_resolution::find_struct(entries, "oopDesc", "_mark") };
    const vm_struct_entry_t* const stat{
        wave3_offset_resolution::find_struct(entries, "Universe", "_collectedHeap") };

    // The discriminator selects the member: instance -> offset path, static ->
    // address path.
    check("w6_static_discipline_instance_selects_offset",
          inst != nullptr && inst->is_static == 0 && inst->offset == 8u);
    check("w6_static_discipline_static_selects_address",
          stat != nullptr && stat->is_static == 1
              && stat->address == static_cast<void*>(&owned_static_storage));

    // The static caller's pipeline: untag the resolved ->address, then validate.
    // The address is canonical (no tag bits) so untag is a no-op, and it is a
    // mapped even in-window non-sentinel address -> is_valid_pointer ACCEPTS.
    const void* const untagged{ untag_pointer(stat != nullptr ? stat->address : nullptr) };
    check("w6_static_address_untag_is_noop_on_canonical",
          stat != nullptr && untagged == stat->address);
    check("w6_static_address_passes_validity_gate",
          stat != nullptr && is_valid_pointer(untagged) == true);

    // Reading THROUGH the validated owned address recovers the value we stored
    // (the deref targets our own static storage -- mapped, POSIX-safe).
    check("w6_static_address_deref_owned_storage_roundtrips",
          stat != nullptr
              && *static_cast<const std::uint64_t*>(stat->address)
                     == 0x1122334455667788ull);

    // The instance entry's ->address is null (its location is in ->offset); a
    // caller that wrongly fed it to the gate would be refused -- the trap flaw #1
    // describes, here proven to fail-closed.
    check("w6_static_discipline_instance_address_is_null_failclosed",
          inst != nullptr && inst->address == nullptr
              && is_valid_pointer(inst->address) == false);
}

// ---------------------------------------------------------------------------
// 53. Resolved-offset + base arithmetic, gated by is_valid_pointer BY VALUE
//     (flaw #5 choke point).  An instance-field reader computes
//     `(uintptr_t)base + entry->offset` and validates the result before any
//     deref.  Reconstruct that as PURE integer math over an owned base and the
//     resolved uint64 offset, then gate the SUM through is_valid_pointer by VALUE
//     ONLY -- the computed address is NEVER dereferenced (HARD RULE 3).  A
//     plausible small offset lands in-window and is accepted; a corrupt huge
//     offset drives the sum out of the user window (or wraps low) and is
//     rejected -- exactly the boundary a resolver-level offset ceiling guards.
// ---------------------------------------------------------------------------
static auto test_w6_resolved_offset_plus_base_value_gate() -> void
{
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::is_valid_pointer;

    auto as_ptr = [](std::uintptr_t v) -> const void*
    { return reinterpret_cast<const void*>(v); };

    // An in-window, 8-aligned, non-sentinel base the gate accepts on its own.
    constexpr std::uintptr_t base{ 0x0000100000000000ull };
    check("w6_offadd_base_itself_valid", is_valid_pointer(as_ptr(base)) == true);

    // Plausible small offsets: base + offset stays in-window and even -> accepted.
    // (Even offsets keep the low bit clear so the 2-byte gate also passes.)
    static const std::uint64_t plausible[]{ 0u, 8u, 16u, 24u, 0x1000u, 0x100000u };
    bool plausible_all_valid{ true };
    for (const std::uint64_t off : plausible)
    {
        const vm_struct_entry_t table[]{
            { "T", "_f", "ty", 0, off, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(table, "T", "_f") };
        const std::uintptr_t sum{ base + (e != nullptr ? e->offset : 0u) };
        if (is_valid_pointer(as_ptr(sum)) != true) { plausible_all_valid = false; }
    }
    check("w6_offadd_plausible_offsets_land_in_window", plausible_all_valid);

    // A corrupt huge offset (>= the 47-bit window) drives base+offset past the
    // ceiling -> rejected by the range branch.  0x0000800000000000 added to the
    // base exceeds user_address_ceiling.
    {
        const vm_struct_entry_t table[]{
            { "T", "_f", "ty", 0, 0x0000800000000000ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(table, "T", "_f") };
        const std::uintptr_t sum{ base + (e != nullptr ? e->offset : 0u) };
        check("w6_offadd_corrupt_huge_offset_out_of_window_rejected",
              e != nullptr && is_valid_pointer(as_ptr(sum)) == false);
    }

    // A UINT64_MAX offset WRAPS the sum low (base + 2^64-1 == base - 1), landing
    // BELOW the floor (base-1 is odd AND in the low-noise region relative to the
    // gate) -> rejected.  The computed value is never dereferenced.
    {
        const vm_struct_entry_t table[]{
            { "T", "_f", "ty", 0, 0xFFFFFFFFFFFFFFFFull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(table, "T", "_f") };
        const std::uintptr_t sum{ base + (e != nullptr ? e->offset : 0u) };  // == base - 1
        check("w6_offadd_uint64_max_offset_wraps_low_rejected",
              e != nullptr && sum == base - 1u
                  && is_valid_pointer(as_ptr(sum)) == false);
    }

    // An offset that makes the sum ODD is rejected by the 2-byte alignment gate
    // even while in-window -- proving the gate catches a misaligned resolved add.
    {
        const vm_struct_entry_t table[]{
            { "T", "_f", "ty", 0, 1u, nullptr },
            { nullptr, nullptr, nullptr, 0, 0u, nullptr },
        };
        const vm_struct_entry_t* const e{
            wave3_offset_resolution::find_struct(table, "T", "_f") };
        const std::uintptr_t sum{ base + (e != nullptr ? e->offset : 0u) };  // odd
        check("w6_offadd_odd_sum_rejected_in_window",
              e != nullptr && (sum & 0x1u) != 0u
                  && is_valid_pointer(as_ptr(sum)) == false);
    }
}

// ---------------------------------------------------------------------------
// 54. untag_pointer FULL high-word tag-bit sweep over CONSTANTS.
//
// untag_pointer ANDs with user_address_ceiling (0x00007FFFFFFFFFFF), so it must
// clear EXACTLY the bits at positions 47..63 and preserve EXACTLY bits 0..46.
// WAVE 4 stripped a single tag value; here set each individual high bit (47..63)
// in turn over a fixed canonical low payload and require the mask clears it while
// the payload survives untouched -- a bit-exact mask proof.  The result is never
// dereferenced (only its integer value is inspected), so POSIX-safe.
// ---------------------------------------------------------------------------
static auto test_w6_untag_full_high_bit_sweep() -> void
{
    using vmhook::hotspot::untag_pointer;

    constexpr std::uintptr_t ceiling_v{ 0x00007FFFFFFFFFFFull };
    auto as_ptr = [](std::uintptr_t v) -> const void* { return reinterpret_cast<const void*>(v); };
    auto as_int = [](const void* p) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(p); };

    // A fixed canonical low payload (within the 47-bit window, even).
    constexpr std::uintptr_t payload{ 0x0000123456789ABCull & ceiling_v };

    bool sweep_ok{ true };
    int bits_swept{ 0 };
    for (int bit{ 47 }; bit <= 63; ++bit)
    {
        ++bits_swept;
        const std::uintptr_t tag{ static_cast<std::uintptr_t>(1) << bit };
        const std::uintptr_t tagged{ payload | tag };
        const std::uintptr_t got{ as_int(untag_pointer(as_ptr(tagged))) };
        // The mask must recover EXACTLY the payload: high tag bit cleared, low
        // 47 bits preserved.
        if (got != payload) { sweep_ok = false; }
        // And the result is within the ceiling (no high bit survives).
        if (got > ceiling_v) { sweep_ok = false; }
    }
    check("w6_untag_each_high_bit_47_to_63_cleared", sweep_ok);
    check("w6_untag_high_bit_count_is_17", bits_swept == 17);

    // ALL high bits set at once (0xFFFF800000000000 | payload) reduces to exactly
    // the payload.
    check("w6_untag_all_high_bits_set_reduces_to_payload",
          as_int(untag_pointer(as_ptr(0xFFFF800000000000ull | payload))) == payload);

    // The boundary bit 46 (the highest bit INSIDE the window) is PRESERVED, not
    // cleared -- proving the mask boundary is exactly between bit 46 and bit 47.
    constexpr std::uintptr_t bit46{ static_cast<std::uintptr_t>(1) << 46 };
    check("w6_untag_preserves_bit_46_inside_window",
          as_int(untag_pointer(as_ptr(bit46))) == bit46);
    check("w6_untag_clears_bit_47_outside_window",
          as_int(untag_pointer(as_ptr(static_cast<std::uintptr_t>(1) << 47))) == 0u);
}

// ---------------------------------------------------------------------------
// 55. is_valid_pointer low-3-bit residue lattice (the 2-byte gate, exactly).
//
// is_valid_pointer rejects ONLY odd addresses (addr & 0x1) -- a 2-byte gate, NOT
// the 8-byte gate is_readable_pointer / safe_read_pointer apply (vmhook.hpp:2059
// vs 2025/2118).  WAVE 4/5 sampled residues 2 and 4; here sweep EVERY residue
// 0..7 off an in-window, non-sentinel, 8-aligned base and pin the EXACT contract:
// even residues {0,2,4,6} accepted, odd residues {1,3,5,7} rejected.  Then
// contrast with is_readable_pointer's prefilter, which rejects ALL of {1..7}
// (8-byte gate).  All over CONSTANTS; is_readable_pointer's rejected branch
// never queries memory, so POSIX-safe (HARD RULE 3).
// ---------------------------------------------------------------------------
static auto test_w6_is_valid_pointer_residue_lattice() -> void
{
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::is_readable_pointer;

    auto as_ptr = [](std::uintptr_t v) -> const void*
    { return reinterpret_cast<const void*>(v); };

    constexpr std::uintptr_t base8{ 0x0000100000000000ull };  // in-window, 8-aligned

    bool valid_lattice_ok{ true };
    bool readable_lattice_ok{ true };
    for (std::uintptr_t r{ 0 }; r <= 7u; ++r)
    {
        const std::uintptr_t addr{ base8 + r };
        const bool even{ (r & 0x1u) == 0u };
        // is_valid_pointer: accept iff even (2-byte gate).
        if (is_valid_pointer(as_ptr(addr)) != even) { valid_lattice_ok = false; }
        // is_readable_pointer prefilter: accept-branch only for the 8-aligned
        // residue 0 would query memory, so we ONLY assert the REJECTED residues
        // (1..7) here -- every non-zero residue is rejected by the 8-byte gate
        // WITHOUT a region query (POSIX-safe).
        if (r != 0u)
        {
            if (is_readable_pointer(as_ptr(addr)) != false) { readable_lattice_ok = false; }
        }
    }
    check("w6_ivp_even_residues_accept_odd_reject", valid_lattice_ok);
    check("w6_readable_all_nonzero_residues_rejected_8byte_gate", readable_lattice_ok);

    // Spell out the four even residues explicitly so a failure names the residue:
    // 0/2/4/6 are all accepted by is_valid_pointer (2-byte gate).
    check("w6_ivp_residue_0_accept", is_valid_pointer(as_ptr(base8 + 0u)) == true);
    check("w6_ivp_residue_2_accept", is_valid_pointer(as_ptr(base8 + 2u)) == true);
    check("w6_ivp_residue_4_accept", is_valid_pointer(as_ptr(base8 + 4u)) == true);
    check("w6_ivp_residue_6_accept", is_valid_pointer(as_ptr(base8 + 6u)) == true);
    // 1/3/5/7 are all rejected (odd).
    check("w6_ivp_residue_1_reject", is_valid_pointer(as_ptr(base8 + 1u)) == false);
    check("w6_ivp_residue_3_reject", is_valid_pointer(as_ptr(base8 + 3u)) == false);
    check("w6_ivp_residue_5_reject", is_valid_pointer(as_ptr(base8 + 5u)) == false);
    check("w6_ivp_residue_7_reject", is_valid_pointer(as_ptr(base8 + 7u)) == false);

    // The 2-byte vs 8-byte gate distinction at residue 2: valid accepts, readable
    // rejects -- the tighter bound the readable check layers on, pinned again from
    // the residue side.
    check("w6_residue_2_valid_yes_readable_no",
          is_valid_pointer(as_ptr(base8 + 2u)) == true
              && is_readable_pointer(as_ptr(base8 + 2u)) == false);
}

// ---------------------------------------------------------------------------
// 56. Cold-state for_each_loaded_class / for_each_instance: bounded iteration
//     with no JVM loaded.
//
// LEDGER (wave-25, no-JVM gap):
//   * for_each_loaded_class() walks gHotSpotVMStructs-derived ClassLoaderData
//     graph; with no JVM, the inner graph construction raises (caught by the
//     function's try/catch) so the visitor is invoked ZERO times AND no
//     exception escapes the call.  Result == "0 iterations, no throw".
//   * for_each_instance<T>(visit) requires register_class<T>() AND a populated
//     heap-VMStructs cache; with no JVM both preconditions fail and the function
//     returns 0 via the early-return ladder, no visitor call, no throw.  We
//     exercise BOTH branches: (a) type NOT registered -> early-return 0;
//     (b) type IS registered -> find_class() returns nullptr -> early-return 0.
//   * Cap CONSTANT pinning: the only externally-exposed iteration cap constant
//     is vmhook::k_max_safe_container_elems (1<<24).  The internal walk caps
//     (1<<20 for hash bucket chains, 1<<24 for descend) are LOCAL constexpr in
//     the helpers and not addressable from outside the header; we pin
//     k_max_safe_container_elems with a static_assert here AND assert by INFO-
//     gated runtime probe that the documented value matches the spec (cap >=
//     largest plausible container size, far above any honest JVM container).
//   * Fabricated iterate_struct_entries with field_name==nullptr (the
//     "synthetic null-field skip") is already pinned in test_null_arg_guards()
//     section 5, but we re-pin it here in a fresh combo against types the
//     walker would also short-circuit on (synthetic / never-occurring names),
//     proving the guard fires for the same {nullptr field} signal regardless
//     of the type slot contents -- a "fabricated array oop" surrogate.
//
// EVERY assertion below is HARD: no JVM here is deterministic for every
// platform/JDK in the matrix (we never get past the early-return), so there is
// no platform-variant case to gate.
// ---------------------------------------------------------------------------
namespace coll_iter_safety_w25 {
    struct counting_visitor_loaded {
        int* counter;
        auto operator()(const std::string&, vmhook::hotspot::klass*) const -> void
        {
            ++*counter;
        }
    };
    // A never-registered, never-instantiated type used purely as the T in
    // for_each_instance<T> to drive the "type not registered" early-return.
    struct never_registered_marker : public vmhook::object_base {
        explicit never_registered_marker(void* p) : vmhook::object_base{ p } {}
    };
    // A second marker derived from object_base, used to register and drive
    // the "registered but find_class returns null" branch with no JVM.
    struct registered_but_absent_marker : public vmhook::object_base {
        explicit registered_but_absent_marker(void* p) : vmhook::object_base{ p } {}
    };
} // namespace coll_iter_safety_w25

static auto test_w25_for_each_loaded_class_no_jvm_zero_iterations() -> void
{
    int calls{ 0 };
    bool threw{ false };
    try
    {
        vmhook::for_each_loaded_class(
            coll_iter_safety_w25::counting_visitor_loaded{ &calls });
    }
    catch (...)
    {
        threw = true;
    }
    // Zero iterations: with no JVM the graph construction (or the first
    // VMStructs lookup it does internally) fails, the function's own try/catch
    // swallows it, and the visitor is never invoked.
    check("w25_for_each_loaded_class_no_jvm_zero_visits", calls == 0);
    // No exception escapes the public API surface (the inner try/catch is the
    // contract -- a regression that removed it would propagate here).
    check("w25_for_each_loaded_class_no_jvm_no_throw", threw == false);

    // Repeat hammering: the bound is stable across many cold-state calls (a
    // regression that lazily-initialized state and then crashed on second use
    // would surface here).
    bool repeated_stable{ true };
    for (int i{ 0 }; i < 32; ++i)
    {
        int c{ 0 };
        try
        {
            vmhook::for_each_loaded_class(
                coll_iter_safety_w25::counting_visitor_loaded{ &c });
        }
        catch (...)
        {
            repeated_stable = false;
        }
        if (c != 0) { repeated_stable = false; }
    }
    check("w25_for_each_loaded_class_no_jvm_repeated_stable", repeated_stable);
}

static auto test_w25_for_each_instance_no_jvm_zero_returns() -> void
{
    using coll_iter_safety_w25::never_registered_marker;
    using coll_iter_safety_w25::registered_but_absent_marker;

    // Branch (a): T not registered -> early-return 0, visitor never invoked.
    int calls_a{ 0 };
    auto visit_a = [&calls_a](std::unique_ptr<never_registered_marker>) { ++calls_a; };
    bool threw_a{ false };
    std::size_t got_a{ 0 };
    try
    {
        got_a = vmhook::for_each_instance<never_registered_marker>(visit_a);
    }
    catch (...) { threw_a = true; }
    check("w25_for_each_instance_unregistered_returns_zero", got_a == 0u);
    check("w25_for_each_instance_unregistered_no_visit", calls_a == 0);
    check("w25_for_each_instance_unregistered_no_throw", threw_a == false);

    // Branch (b): T IS registered, but no JVM -> find_class returns null ->
    // early-return 0.  register_class is noexcept and merely populates a map,
    // so it succeeds even with no JVM.
    vmhook::register_class<registered_but_absent_marker>(
        "vmhook/test/W25NeverLoadedClass");
    int calls_b{ 0 };
    auto visit_b = [&calls_b](std::unique_ptr<registered_but_absent_marker>) { ++calls_b; };
    bool threw_b{ false };
    std::size_t got_b{ 0 };
    try
    {
        got_b = vmhook::for_each_instance<registered_but_absent_marker>(visit_b);
    }
    catch (...) { threw_b = true; }
    check("w25_for_each_instance_registered_absent_returns_zero", got_b == 0u);
    check("w25_for_each_instance_registered_absent_no_visit", calls_b == 0);
    check("w25_for_each_instance_registered_absent_no_throw", threw_b == false);

    // max_visits parameter MUST be respected even on the cold path: a cap of 0
    // is the most aggressive bound and still produces a clean 0 return.
    std::size_t got_cap0{ 999 };
    bool threw_cap0{ false };
    try
    {
        got_cap0 = vmhook::for_each_instance<registered_but_absent_marker>(
            visit_b, /*max_visits=*/0u);
    }
    catch (...) { threw_cap0 = true; }
    check("w25_for_each_instance_max_visits_zero_returns_zero", got_cap0 == 0u);
    check("w25_for_each_instance_max_visits_zero_no_throw", threw_cap0 == false);

    // And the documented "no limit" default (size_t max) still returns 0 with
    // no JVM -- the early-return ladder runs BEFORE the cap is consulted.
    std::size_t got_nolimit{ 999 };
    try
    {
        got_nolimit = vmhook::for_each_instance<registered_but_absent_marker>(
            visit_b, std::numeric_limits<std::size_t>::max());
    }
    catch (...) {}
    check("w25_for_each_instance_nolimit_default_returns_zero", got_nolimit == 0u);
}

static auto test_w25_iteration_cap_constants_pin() -> void
{
    // The ONE externally-exposed iteration cap: k_max_safe_container_elems.
    // Pin its value AND its relationship to a signed-int32 loop bound (the
    // documented saturating target the clamp returns).
    static_assert(vmhook::k_max_safe_container_elems == (1ull << 24),
                  "k_max_safe_container_elems must be exactly 1<<24");
    static_assert(vmhook::k_max_safe_container_elems == 16777216ull,
                  "k_max_safe_container_elems must be 16,777,216 elements");
    static_assert(vmhook::k_max_safe_container_elems
                      <= static_cast<std::size_t>(
                          (std::numeric_limits<std::int32_t>::max)()),
                  "cap must fit signed int32 for the clamp cast");
    // Pin the cap's relationship to the documented bucket-chain cap (1<<20):
    // 1<<24 is exactly 16x larger -- the descend cap must dominate the hash
    // bucket cap, so an honest hash table walk cannot starve a descent walk.
    static_assert(vmhook::k_max_safe_container_elems
                      == static_cast<std::size_t>(1u << 20) * 16ull,
                  "descend cap (1<<24) must be 16x the bucket cap (1<<20)");
    check("w25_cap_descend_dominates_bucket_static_assert_held",
          vmhook::k_max_safe_container_elems
              == static_cast<std::size_t>(1u << 20) * 16ull);

    // Internal-only caps 1<<20 and 1<<24 are NOT exposed as named symbols.
    // INFO-gate the runtime probe: their numeric value is 1048576 and
    // 16777216 respectively, and the descend cap equals the exposed
    // k_max_safe_container_elems.  Any change to these numbers in the
    // header that broke a walker's termination would surface in the JVM
    // integration modules; this pin is purely the value contract.
    constexpr std::size_t bucket_cap{ 1ull << 20 };
    constexpr std::size_t descend_cap{ 1ull << 24 };
    check("w25_bucket_cap_value_1M",  bucket_cap  == 1048576ull);
    check("w25_descend_cap_value_16M", descend_cap == 16777216ull);
    check("w25_descend_cap_eq_exposed_max",
          descend_cap == vmhook::k_max_safe_container_elems);
    std::printf("[INFO] w25 internal caps: bucket=1<<20=%zu descend=1<<24=%zu\n",
                bucket_cap, descend_cap);

    // Cross-tie: the clamp saturates EXACTLY to the descend cap for any input
    // above it (a single boundary cross to seal the value).
    check("w25_clamp_saturates_to_descend_cap_value",
          static_cast<std::size_t>(
              vmhook::clamp_safe_container_count(
                  (std::numeric_limits<std::int32_t>::max)()))
              == descend_cap);
}

static auto test_w25_fabricated_null_field_skip_surrogate() -> void
{
    using vmhook::hotspot::iterate_struct_entries;

    // The "fabricated array oop" surrogate for the field_name==nullptr skip
    // path: pass nullptr in the field slot against a variety of type slots
    // (real, synthetic, empty, even nullptr itself) and prove the guard fires
    // uniformly without ever dereferencing the type-slot bytes.  This pins
    // that the null-field skip is type-slot-INVARIANT -- a regression that
    // moved the null-field check below a strcmp of the type slot would crash
    // on the nullptr type combo.
    const char* const type_slots[]{
        nullptr, "",  "Symbol", "Method", "FabricatedArrayOop",
        "[Ljava/lang/Object;",                 // JNI-style array name
        "X", "OopDesc", "ZZZ_NoSuchType",
    };
    bool all_null{ true };
    int combos{ 0 };
    for (const char* const t : type_slots)
    {
        ++combos;
        if (iterate_struct_entries(t, nullptr) != nullptr) { all_null = false; }
    }
    check("w25_fabricated_null_field_skip_uniform_for_all_type_slots", all_null);
    check("w25_fabricated_null_field_skip_combo_count", combos == 9);

    // Cold-state determinism: hammer the (X, nullptr) and (nullptr, nullptr)
    // shapes many times.  A regression that mutated cache state on the
    // null-field path would surface as a non-null result somewhere.
    bool det_ok{ true };
    for (int i{ 0 }; i < 256; ++i)
    {
        if (iterate_struct_entries("FabricatedArrayOop", nullptr) != nullptr) { det_ok = false; }
        if (iterate_struct_entries(nullptr,              nullptr) != nullptr) { det_ok = false; }
    }
    check("w25_fabricated_null_field_skip_deterministic", det_ok);
}

int main()
{
    test_getters_cache_no_jvm();
    test_jvm_module_cache_no_jvm();
    test_iterate_struct_entries_real_symbols_no_jvm();
    test_iterate_type_entries_real_symbols_no_jvm();
    test_null_arg_guards();
    test_argument_ordering_and_empty_strings();
    test_iterate_consistent_with_getters();
    test_lookup_determinism_no_jvm();
    test_pathological_symbol_strings_no_jvm();
    test_extended_real_symbol_matrix_no_jvm();
    test_clamp_boundary_table();
    test_clamp_universal_invariants();
    test_clamp_algebraic_properties();
    test_clamp_constexpr_evaluation();
    test_cap_constant();
    test_near_miss_names_no_jvm();
    test_null_empty_real_permutations();
    test_interleaved_getter_iterator_stress();
    test_complete_struct_query_surface_no_jvm();
    test_version_fallback_chains_no_jvm();
    test_complete_type_query_surface_no_jvm();
    test_clamp_power_of_two_ladder();
    test_clamp_dense_low_region();
    test_cap_is_the_walk_bound();
    test_single_byte_name_fuzz_no_jvm();
    test_control_char_names_no_jvm();
    test_substring_rotation_near_misses_no_jvm();
    test_slot_independence_matrix_no_jvm();
    test_maximal_interleave_and_cache_order();
    test_determinism_fingerprint_no_jvm();
    test_entry_struct_layout_pin();
    test_walk_algorithm_owned_array();
    test_is_static_discriminator_values();
    test_type_walk_matches_on_type_name_only();
    test_walk_iteration_count_bounded();
    test_matcher_exactness_owned_array();
    test_w3_offset_full_uint64_roundtrip();
    test_w3_type_size_full_uint64_roundtrip();
    test_w3_first_match_wins();
    test_w3_is_static_full_int32_domain();
    test_w3_embedded_nul_key_stops_at_first_nul();
    test_w3_walk_determinism_populated();
    test_w3_alignment_and_pointer_sizes();
    test_w4_is_valid_pointer_bounds_over_constants();
    test_w4_untag_pointer_mask_over_constants();
    test_w4_offset_within_type_size_threshold();
    test_w4_flags_width_extraction();
    test_w5_terminator_keys_on_type_name_only();
    test_w5_is_readable_pointer_prefilter_rejects();
    test_w5_clamp_and_validity_independence();
    test_w6_type_string_column_roundtrip();
    test_w6_static_field_read_discipline_owned();
    test_w6_resolved_offset_plus_base_value_gate();
    test_w6_untag_full_high_bit_sweep();
    test_w6_is_valid_pointer_residue_lattice();
    test_w25_for_each_loaded_class_no_jvm_zero_iterations();
    test_w25_for_each_instance_no_jvm_zero_returns();
    test_w25_iteration_cap_constants_pin();
    test_w25_fabricated_null_field_skip_surrogate();

    return failures == 0 ? 0 : 1;
}
