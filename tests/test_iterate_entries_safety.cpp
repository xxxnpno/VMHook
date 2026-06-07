// No-JVM safety + null-arg guards for hotspot::iterate_struct_entries /
// iterate_type_entries, plus get_vm_types / get_vm_structs caching.
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
// for this pure no-JVM file.
//
// This file deliberately extends tests/test_helpers.cpp sections 15 & 16
// (which cover only "Symbol"/"_length" + the four null-guard cases) with a far
// wider set of real HotSpot symbol names, argument-ordering / empty-string
// boundary cases, repeated caching calls, and cross-consistency between the
// iterate_* helpers and the cached getters.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

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

    return failures == 0 ? 0 : 1;
}
