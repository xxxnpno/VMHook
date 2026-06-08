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

    return failures == 0 ? 0 : 1;
}
