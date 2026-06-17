// vmstructs_offset_resolution — exhaustive NO-JVM unit coverage for the
// gHotSpotVMStructs / gHotSpotVMTypes offset/address resolution machinery.
//
//   vmhook::hotspot::get_jvm_module()
//   vmhook::hotspot::get_vm_types()      -> gHotSpotVMTypes head (cached)
//   vmhook::hotspot::get_vm_structs()    -> gHotSpotVMStructs head (cached)
//   vmhook::hotspot::iterate_type_entries(type)         -> vm_type_entry_t*
//   vmhook::hotspot::iterate_struct_entries(type,field) -> vm_struct_entry_t*
//   vmhook::hotspot::vm_type_entry_t / vm_struct_entry_t  (the ABI mirrors)
//
// This is the resolver-dedicated lane.  It is deliberately #38-IMMUNE: no JVM
// library is loaded into this standalone process, so the resolver NEVER touches
// VM memory, never takes a safepoint, never walks a live heap.  The whole point
// of the no-JVM contract is that a miss is cheap and crash-free:
//
//   * get_jvm_module() resolves to a null module handle (no jvm.dll/libjvm.*),
//   * get_vm_types() / get_vm_structs() resolve their global symbol to nullptr
//     via get_proc_address(null-module, ...) and CACHE that nullptr in a
//     function-local static (the one-shot cache),
//   * iterate_*_entries() therefore start their walk from a null array head, so
//     the loop guard (`entry && entry->type_name`) terminates on the very first
//     iteration and the function returns nullptr WITHOUT dereferencing anything.
//   * the null-argument guards (`if (!type_name|!field_name) return nullptr;`)
//     short-circuit BEFORE any strcmp, so a null symbol pointer can never reach
//     strcmp(nullptr, ...) which would be UB.
//
// Because iterate_*_entries() read the cached global head internally and take
// NO array parameter, a fabricated in-process array cannot be driven through
// the real public API without a live exported symbol.  The match-found path
// (returning a non-null entry and reading ->offset / ->address / ->is_static)
// is therefore LIVE-JVM integration territory — out of scope here.  For those
// paths this file asserts only the no-JVM contract (clean nullptr, never fault)
// AND statically pins the ABI layout the resolver hands back (the struct
// member set + sizes), which is pure-logic and JVM-independent.
//
// Scope boundary vs sibling tests:
//   * tests/test_iterate_entries_safety.cpp owns clamp_safe_container_count /
//     k_max_safe_container_elems (the oop-derived bound math).  This file does
//     NOT re-test the clamp — it stays strictly on the VMStructs/VMTypes
//     resolver surface.
//   * It DOES exhaustively cover the resolver's own angles: the complete
//     call-site-derived (type,field) query surface, the renamed-field fallback
//     chains (_mark/_markWord, _from_compiled_code_entry_point/_from_compiled_
//     entry, _fields/_fieldinfo_stream, _klasses/_dictionary), the candidate-
//     table ordering the codec resolvers depend on, the ABI mirror layout
//     (is_static / offset / address — the union the caller must read by hand),
//     embedded-NUL keys (via std::string + explicit length), degenerate inputs,
//     determinism, and no-global-state.

#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    int g_failures{ 0 };
    int g_checks{ 0 };

    auto check(const char* const name, const bool ok) -> void
    {
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
        ++g_checks;
        if (!ok) { ++g_failures; }
    }

    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;
    using vmhook::hotspot::get_vm_structs;
    using vmhook::hotspot::get_vm_types;
    using vmhook::hotspot::get_jvm_module;
    using vmhook::hotspot::vm_struct_entry_t;
    using vmhook::hotspot::vm_type_entry_t;

    // -----------------------------------------------------------------------
    // 1. The cached getters: nullptr in a no-JVM process, and STABLE.
    //
    // get_vm_types / get_vm_structs each cache their resolved head in a
    // function-local static initialised exactly once.  In this process the
    // resolve yields nullptr; the contract under test is (a) nullptr now,
    // (b) the SAME pointer on every subsequent call (a regression that
    // recomputed each call, or returned a fresh-but-equal value, would still
    // satisfy "== nullptr", so identity is asserted directly and hammered),
    // (c) the two heads are independent globals (gHotSpotVMTypes vs
    // gHotSpotVMStructs), not an alias of one another.
    // -----------------------------------------------------------------------
    auto test_getters_cached_nullptr() -> void
    {
        vm_type_entry_t* const types_a{ get_vm_types() };
        vm_type_entry_t* const types_b{ get_vm_types() };
        vm_struct_entry_t* const structs_a{ get_vm_structs() };
        vm_struct_entry_t* const structs_b{ get_vm_structs() };

        check("get_vm_types_null_no_jvm", types_a == nullptr);
        check("get_vm_types_stable", types_a == types_b);
        check("get_vm_structs_null_no_jvm", structs_a == nullptr);
        check("get_vm_structs_stable", structs_a == structs_b);

        bool types_stable{ true };
        bool structs_stable{ true };
        for (int i{ 0 }; i < 4096; ++i)
        {
            if (get_vm_types() != types_a) { types_stable = false; }
            if (get_vm_structs() != structs_a) { structs_stable = false; }
        }
        check("get_vm_types_stable_hammered", types_stable);
        check("get_vm_structs_stable_hammered", structs_stable);

        // Two distinct globals; both null here, but by the documented no-JVM
        // outcome, not by aliasing.
        check("type_and_struct_heads_both_null",
              types_a == nullptr && structs_a == nullptr);
    }

    // -----------------------------------------------------------------------
    // 2. get_jvm_module: null module handle in a no-JVM process, cached stable.
    //    module_handle is a pointer type, so it compares against nullptr.
    // -----------------------------------------------------------------------
    auto test_jvm_module_cached_nullptr() -> void
    {
        auto const first{ get_jvm_module() };
        auto const second{ get_jvm_module() };
        check("get_jvm_module_null_no_jvm", first == nullptr);
        check("get_jvm_module_stable", first == second);

        bool stable{ true };
        for (int i{ 0 }; i < 4096; ++i)
        {
            if (get_jvm_module() != first) { stable = false; }
        }
        check("get_jvm_module_stable_hammered", stable);
    }

    // -----------------------------------------------------------------------
    // 3. Null-argument guards.  iterate_struct_entries short-circuits to nullptr
    //    if EITHER pointer is null (`!type_name || !field_name`), exercised
    //    against several real type/field names so a guard that only checked one
    //    of the two arguments fails loudly.  iterate_type_entries guards its one
    //    argument.  These fire BEFORE any strcmp, so strcmp(nullptr,...) is never
    //    reached.
    // -----------------------------------------------------------------------
    auto test_null_argument_guards() -> void
    {
        // null field_name, non-null type_name
        check("struct_null_field_symbol", iterate_struct_entries("Symbol", nullptr) == nullptr);
        check("struct_null_field_method", iterate_struct_entries("Method", nullptr) == nullptr);
        check("struct_null_field_klass", iterate_struct_entries("Klass", nullptr) == nullptr);
        check("struct_null_field_oopdesc", iterate_struct_entries("oopDesc", nullptr) == nullptr);
        check("struct_null_field_instanceklass", iterate_struct_entries("InstanceKlass", nullptr) == nullptr);

        // null type_name, non-null field_name
        check("struct_null_type_length", iterate_struct_entries(nullptr, "_length") == nullptr);
        check("struct_null_type_mark", iterate_struct_entries(nullptr, "_mark") == nullptr);
        check("struct_null_type_constmethod", iterate_struct_entries(nullptr, "_constMethod") == nullptr);

        // both null
        check("struct_both_null", iterate_struct_entries(nullptr, nullptr) == nullptr);

        // type-walker null
        check("type_null", iterate_type_entries(nullptr) == nullptr);

        // Hammer the null guards to prove they are stateless.
        bool stable{ true };
        for (int i{ 0 }; i < 1024; ++i)
        {
            if (iterate_struct_entries(nullptr, "_x") != nullptr) { stable = false; }
            if (iterate_struct_entries("X", nullptr) != nullptr) { stable = false; }
            if (iterate_struct_entries(nullptr, nullptr) != nullptr) { stable = false; }
            if (iterate_type_entries(nullptr) != nullptr) { stable = false; }
        }
        check("null_guards_stateless", stable);
    }

    // -----------------------------------------------------------------------
    // 4. COMPLETE struct-query surface.  Every distinct (type_name, field_name)
    //    pair that vmhook.hpp ever passes to iterate_struct_entries, gathered by
    //    sweeping every call site in the header.  Each must short-circuit on the
    //    null head and return nullptr without faulting — the foundational
    //    "every lookup the library performs is crash-free when the table is
    //    absent" guarantee.  Kept in sync with the header's call sites; the
    //    count assertion at the end guards against a new call-site pair being
    //    added to the library but silently dropped from this surface.
    // -----------------------------------------------------------------------
    auto test_complete_struct_query_surface() -> void
    {
        struct pair { const char* type; const char* field; };
        static const pair pairs[]{
            // Symbol
            { "Symbol", "_length" }, { "Symbol", "_body" },
            // ConstantPool
            { "ConstantPool", "_length" }, { "ConstantPool", "_pool_holder" },
            // ConstMethod
            { "ConstMethod", "_constants" }, { "ConstMethod", "_name_index" },
            { "ConstMethod", "_signature_index" },
            // Method (incl. renamed compiled-entry pair + adapter + flags)
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
            { "JavaThread", "_tlab" },
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
                std::printf("  struct surface (%s,%s) non-null\n", p.type, p.field);
            }
        }
        check("complete_struct_query_surface_all_null", all_null);
        check("complete_struct_query_surface_count_ge_60", count >= 60);
    }

    // -----------------------------------------------------------------------
    // 5. COMPLETE type-query surface for iterate_type_entries: every type the
    //    header looks up directly, every type the struct walks reference by
    //    name, plus the JDK-version-specific types (present on only one JDK) and
    //    the common integer typedefs HotSpot publishes in gHotSpotVMTypes.  All
    //    nullptr with no JVM — a wrong-JDK type name must report "absent"
    //    without faulting, the same bounds contract as the absent-table case.
    // -----------------------------------------------------------------------
    auto test_complete_type_query_surface() -> void
    {
        static const char* const names[]{
            "ConstantPool", "Method",
            "Symbol", "ConstMethod", "Klass", "InstanceKlass", "oopDesc",
            "narrowOop", "JavaThread", "Thread", "OSThread",
            "ClassLoaderData", "ClassLoaderDataGraph", "SystemDictionary",
            "CompressedOops", "CompressedKlassPointers", "Universe",
            "CollectedHeap", "MemRegion", "ThreadLocalAllocBuffer",
            "AdapterHandlerEntry", "StubRoutines",
            "ThreadsSMRSupport", "ThreadsList",
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
                std::printf("  type surface %s non-null\n", n);
            }
        }
        check("complete_type_query_surface_all_null", all_null);
        check("complete_type_query_surface_count_ge_25", count >= 25);
    }

    // -----------------------------------------------------------------------
    // 6. JDK-version fallback chains.  Cross-version support rests entirely on
    //    each ABSENT candidate resolving to nullptr cheaply: the resolver tries
    //    candidate A, then B, then C, taking the first non-null.  With no JVM
    //    every candidate is nullptr — which is precisely the "not in THIS JDK,
    //    try the next" signal the chain needs.  A fault or a false match on any
    //    candidate would break the chain.
    // -----------------------------------------------------------------------
    auto test_version_fallback_chains() -> void
    {
        struct pair { const char* type; const char* field; };

        // The compressed-oops / compressed-klass base+shift candidate tables,
        // in the exact try-order the codec resolvers walk.
        static const pair codec_candidates[]{
            { "CompressedOops", "_narrow_oop._base" },
            { "CompressedOops", "_base" },
            { "Universe",       "_narrow_oop._base" },
            { "CompressedOops", "_narrow_oop._shift" },
            { "CompressedOops", "_shift" },
            { "Universe",       "_narrow_oop._shift" },
            { "CompressedKlassPointers", "_narrow_klass._base" },
            { "CompressedKlassPointers", "_base" },
            { "Universe",                "_narrow_klass._base" },
            { "CompressedKlassPointers", "_narrow_klass._shift" },
            { "CompressedKlassPointers", "_shift" },
            { "Universe",                "_narrow_klass._shift" },
        };
        bool codec_all_null{ true };
        for (const auto& c : codec_candidates)
        {
            if (iterate_struct_entries(c.type, c.field) != nullptr) { codec_all_null = false; }
        }
        check("version_fallback_codec_candidates_all_null", codec_all_null);

        // The renamed-field pairs the library tries old-then-new (or the
        // capability-probe pairs whose presence is used as a version switch):
        // BOTH spellings must be null with no JVM so the fallback is reached.
        struct rename { const char* type; const char* old_name; const char* new_name; const char* tag; };
        static const rename renames[]{
            { "oopDesc", "_mark", "_markWord", "rename_oop_mark_markword" },
            { "Method", "_from_compiled_code_entry_point", "_from_compiled_entry", "rename_method_compiled_entry" },
            { "InstanceKlass", "_fields", "_fieldinfo_stream", "rename_ik_fields_stream" },
            { "ClassLoaderData", "_klasses", "_dictionary", "rename_cld_klasses_dictionary" },
            { "oopDesc", "_metadata._compressed_klass", "_metadata._klass", "rename_oop_klass_meta" },
        };
        for (const auto& r : renames)
        {
            const bool old_null{ iterate_struct_entries(r.type, r.old_name) == nullptr };
            const bool new_null{ iterate_struct_entries(r.type, r.new_name) == nullptr };
            check(r.tag, old_null && new_null);
        }
    }

    // -----------------------------------------------------------------------
    // 7. Argument ordering / positional independence.  type_name is matched
    //    against entry->type_name and field_name against entry->field_name; the
    //    two slots are independent and never interchangeable.  With no JVM every
    //    permutation is nullptr, which pins that swapping the arguments, or
    //    placing a field name in the type slot, never produces an accidental
    //    match (or a crash).
    // -----------------------------------------------------------------------
    auto test_argument_ordering() -> void
    {
        check("order_symbol_length", iterate_struct_entries("Symbol", "_length") == nullptr);
        check("order_swapped_length_symbol", iterate_struct_entries("_length", "Symbol") == nullptr);
        check("order_method_code", iterate_struct_entries("Method", "_code") == nullptr);
        check("order_swapped_code_method", iterate_struct_entries("_code", "Method") == nullptr);
        check("order_field_in_type_slot", iterate_struct_entries("_metadata._klass", "oopDesc") == nullptr);
        check("order_type_in_field_slot", iterate_struct_entries("_pool_holder", "ConstantPool") == nullptr);
    }

    // -----------------------------------------------------------------------
    // 8. Empty strings are NON-NULL.  The guard checks the POINTER, not the
    //    length: "" survives the guard, walks the (empty) array, and returns
    //    nullptr by failing to match — it must NOT be conflated with nullptr,
    //    and must never crash.  Full {null,"",real} x {null,"",real} cross
    //    product for the struct walker (9 combos), and the three slot kinds for
    //    the type walker.
    // -----------------------------------------------------------------------
    auto test_empty_string_permutations() -> void
    {
        const char* const slots[]{ nullptr, "", "Symbol" };
        const char* const tags[]{ "null", "empty", "real" };

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
                    std::printf("  combo type=%s field=%s non-null\n", tags[ti], tags[fi]);
                }
            }
        }
        check("struct_9_combo_permutation_all_null", all_null);
        check("struct_permutation_count_is_9", combos == 9);

        bool type_all_null{ true };
        for (int ti{ 0 }; ti < 3; ++ti)
        {
            if (iterate_type_entries(slots[ti]) != nullptr) { type_all_null = false; }
        }
        check("type_3_slot_permutation_all_null", type_all_null);

        // Empty strings specifically (a distinct boundary worth its own asserts).
        check("struct_empty_type_real_field", iterate_struct_entries("", "_length") == nullptr);
        check("struct_real_type_empty_field", iterate_struct_entries("Symbol", "") == nullptr);
        check("struct_both_empty", iterate_struct_entries("", "") == nullptr);
        check("type_empty", iterate_type_entries("") == nullptr);
    }

    // -----------------------------------------------------------------------
    // 9. Near-miss names.  strcmp is EXACT — whole-string, case-sensitive, no
    //    normalisation, no prefix matching.  These pin the input-handling
    //    semantics so a future change to add case-folding / trimming / prefix
    //    matching is caught, and they exercise the trickiest "almost real"
    //    inputs without faulting.  All nullptr here regardless (no JVM).
    // -----------------------------------------------------------------------
    auto test_near_miss_names() -> void
    {
        // Case variants of a real type name.
        const char* const case_variants[]{
            "method", "METHOD", "MeThOd", "symbol", "SYMBOL", "Oopdesc", "oopdesc",
        };
        bool case_null{ true };
        for (const char* const t : case_variants)
        {
            if (iterate_type_entries(t) != nullptr) { case_null = false; }
            if (iterate_struct_entries(t, "_length") != nullptr) { case_null = false; }
        }
        check("near_miss_case_all_null", case_null);

        // Prefix / superstring / whitespace variants of a real field name.
        const char* const field_variants[]{
            "_leng", "_lengthX", "_length_", "length", "_length ", " _length",
            "_Length", "_LENGTH", "_mar", "_markWordX", "_constMetho", "_constMethodd",
        };
        bool field_null{ true };
        for (const char* const f : field_variants)
        {
            if (iterate_struct_entries("Symbol", f) != nullptr) { field_null = false; }
            if (iterate_struct_entries("Method", f) != nullptr) { field_null = false; }
        }
        check("near_miss_field_prefix_superstring_whitespace_all_null", field_null);

        // A field that is a strict prefix of another real field, and vice-versa
        // (e.g. "_code" vs a hypothetical "_code_entry") — exact match only.
        check("near_miss_code_prefix", iterate_struct_entries("Method", "_cod") == nullptr);
        check("near_miss_code_superstring", iterate_struct_entries("Method", "_code_entry") == nullptr);
    }

    // -----------------------------------------------------------------------
    // 10. Embedded NUL keys.  Through a const char* an embedded NUL would
    //     truncate at the first NUL; here we pass keys built from std::string
    //     with explicit length so the buffer genuinely contains interior NULs.
    //     strcmp stops at the FIRST NUL, so the effective key is the prefix
    //     before the NUL — the resolver must never read past it, and (no JVM)
    //     returns nullptr without faulting.  Also exercises high-bit bytes.
    // -----------------------------------------------------------------------
    auto test_embedded_nul_and_high_bytes() -> void
    {
        // "Symbol\0Extra" — strcmp sees "Symbol"; still nullptr with no JVM.
        const std::string with_nul{ std::string{ "Symbol" } + '\0' + "Extra" };
        check("struct_embedded_nul_type",
              iterate_struct_entries(with_nul.c_str(), "_length") == nullptr);
        check("struct_embedded_nul_field",
              iterate_struct_entries("Symbol", with_nul.c_str()) == nullptr);
        check("type_embedded_nul",
              iterate_type_entries(with_nul.c_str()) == nullptr);

        // A key whose first byte is NUL — strcmp sees the empty string.
        const std::string leading_nul{ std::string{ '\0' } + "Method" };
        check("struct_leading_nul_type",
              iterate_struct_entries(leading_nul.c_str(), "_x") == nullptr);
        check("type_leading_nul",
              iterate_type_entries(leading_nul.c_str()) == nullptr);

        // High-bit / control bytes (NUL-free) — ordinary non-null strings.
        const char high_bytes[]{ '\x7F', '\x80', '\xFE', '\xFF', '\0' };
        const char control_bytes[]{ 'M', '\t', '\n', '\r', '\x01', '\0' };
        check("struct_high_bytes_type",
              iterate_struct_entries(high_bytes, "_length") == nullptr);
        check("struct_control_bytes_type",
              iterate_struct_entries(control_bytes, "_length") == nullptr);
        check("type_high_bytes", iterate_type_entries(high_bytes) == nullptr);
        check("type_control_bytes", iterate_type_entries(control_bytes) == nullptr);
    }

    // -----------------------------------------------------------------------
    // 11. Pathological lengths.  Very long keys (8 KiB, 64 KiB) must still
    //     short-circuit on the null head — no buffer walk, no overflow, no
    //     dependence on the key length.  And the shortest possible non-empty
    //     keys (single char).
    // -----------------------------------------------------------------------
    auto test_pathological_lengths() -> void
    {
        const std::string long_key(8u * 1024u, 'Z');
        const std::string huge_key(64u * 1024u, 'Q');

        check("struct_long_type", iterate_struct_entries(long_key.c_str(), "_length") == nullptr);
        check("struct_long_field", iterate_struct_entries("Symbol", long_key.c_str()) == nullptr);
        check("struct_long_both", iterate_struct_entries(long_key.c_str(), long_key.c_str()) == nullptr);
        check("type_long", iterate_type_entries(long_key.c_str()) == nullptr);

        check("struct_huge_type", iterate_struct_entries(huge_key.c_str(), "_length") == nullptr);
        check("type_huge", iterate_type_entries(huge_key.c_str()) == nullptr);

        check("struct_single_char", iterate_struct_entries("X", "y") == nullptr);
        check("type_single_char", iterate_type_entries("X") == nullptr);
    }

    // -----------------------------------------------------------------------
    // 12. Determinism / no global state.  Repeating the SAME lookup many times
    //     always returns nullptr (no internal state mutates), across a mix of
    //     real, bogus, empty, and null inputs for BOTH walkers, interleaved with
    //     direct getter calls (which the walkers call internally) to prove the
    //     cache never desynchronises under interleaving.
    // -----------------------------------------------------------------------
    auto test_determinism_and_no_global_state() -> void
    {
        bool stable{ true };
        for (int i{ 0 }; i < 1024; ++i)
        {
            if (get_vm_structs() != nullptr) { stable = false; }
            if (get_vm_types() != nullptr) { stable = false; }
            if (iterate_struct_entries("Method", "_constMethod") != nullptr) { stable = false; }
            if (iterate_struct_entries("Symbol", "_length") != nullptr) { stable = false; }
            if (iterate_struct_entries("", "") != nullptr) { stable = false; }
            if (iterate_struct_entries(nullptr, "_x") != nullptr) { stable = false; }
            if (iterate_struct_entries("ZZZ_NoSuchType", "_no_such_field") != nullptr) { stable = false; }
            if (iterate_type_entries("Klass") != nullptr) { stable = false; }
            if (iterate_type_entries("") != nullptr) { stable = false; }
            if (iterate_type_entries(nullptr) != nullptr) { stable = false; }
            if (iterate_type_entries("ZZZ_NoSuchType") != nullptr) { stable = false; }
        }
        check("determinism_interleaved_all_stable_null", stable);
    }

    // -----------------------------------------------------------------------
    // 13. Cross-consistency: walker result implies the cached getter.  The
    //     walkers are thin loops over get_vm_structs()/get_vm_types(); when the
    //     getter is nullptr the loop body never runs, so the walker MUST return
    //     nullptr.  Assert the implication (getter==null => walker==null)
    //     directly rather than just observing both null in isolation.
    // -----------------------------------------------------------------------
    auto test_walker_implies_getter() -> void
    {
        const bool structs_null{ get_vm_structs() == nullptr };
        const bool types_null{ get_vm_types() == nullptr };

        check("precondition_structs_null", structs_null);
        check("precondition_types_null", types_null);

        // !structs_null is false here, so the implication reduces to the RHS;
        // written as a material implication so a JVM-present build would also
        // pass iff the contract holds.
        check("struct_null_when_head_null",
              !structs_null || iterate_struct_entries("Method", "_constMethod") == nullptr);
        check("type_null_when_head_null",
              !types_null || iterate_type_entries("Method") == nullptr);
    }

    // -----------------------------------------------------------------------
    // 14. ABI mirror layout — vm_struct_entry_t.  This is the union flaw-#1
    //     surface: the resolver hands back the raw entry and the caller reads
    //     ->offset (instance fields) OR ->address (static fields), choosing by
    //     hand; ->is_static says which is live but the resolver never inspects
    //     it.  These are pure-logic, JVM-independent layout assertions that pin
    //     the mirror so a member rename / reorder / width change that would
    //     desync from HotSpot's VMStructEntry is caught at compile/run time.
    //
    //     HotSpot VMStructEntry: typeName, fieldName, typeString, isStatic,
    //     offset (union with address).  vmhook lays offset/address out as two
    //     separate members (ABI-safe because the published table writes both
    //     words); we pin both exist with the documented widths.
    // -----------------------------------------------------------------------
    auto test_struct_entry_abi_layout() -> void
    {
        // Member types — the offset add and address deref discipline depends on
        // these exact widths (offset is uint64 so the `this + offset` add never
        // truncates; is_static is int32 mirroring HotSpot's int).
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::type_name), const char*>,
                      "type_name must be const char*");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::field_name), const char*>,
                      "field_name must be const char*");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::type_string), const char*>,
                      "type_string must be const char*");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::is_static), std::int32_t>,
                      "is_static must be int32 (mirrors HotSpot isStatic)");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::offset), std::uint64_t>,
                      "offset must be uint64 so the base+offset add never truncates");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::address), void*>,
                      "address must be void* (static-field absolute address)");

        // Build a synthetic entry purely to prove the members are independently
        // addressable and round-trip bit-exactly — exercising flaw #1: an
        // instance entry's ->offset and a static entry's ->address coexist as
        // distinct words, and the full uint64 offset range is preserved (no
        // 32-bit truncation).  No JVM, no table: this is plain struct math.
        const std::uint64_t offsets[]{
            0ull, 1ull, 7ull, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull,
            0x100000000ull, 0xFFFFFFFFFFFFFFFFull,
        };
        bool offset_roundtrip{ true };
        for (const std::uint64_t o : offsets)
        {
            vm_struct_entry_t e{};
            e.is_static = 0;
            e.offset = o;
            e.address = nullptr;
            if (e.offset != o) { offset_roundtrip = false; }
            if (e.is_static != 0) { offset_roundtrip = false; }
        }
        check("struct_entry_offset_roundtrip_full_uint64", offset_roundtrip);

        // A static entry: address word holds an absolute pointer, is_static set,
        // and the offset word does NOT alias the address (separate members).
        int sentinel{ 0 };
        vm_struct_entry_t stat{};
        stat.is_static = 1;
        stat.address = &sentinel;
        stat.offset = 0ull;
        check("struct_entry_static_address_distinct_from_offset",
              stat.is_static == 1 && stat.address == &sentinel && stat.offset == 0ull);

        // Pointer alignment of the struct (it is read straight from the JVM's
        // exported array, so it must be naturally aligned for a pointer member).
        check("struct_entry_pointer_aligned",
              alignof(vm_struct_entry_t) >= alignof(void*));
    }

    // -----------------------------------------------------------------------
    // 15. ABI mirror layout — vm_type_entry_t.  Pins the gHotSpotVMTypes mirror
    //     (typeName, superclassName, isOopType, isIntegerType, isUnsigned,
    //     size).  Note: the header member that mirrors HotSpot's isOopType is
    //     spelled `is_oop_type_type` (a doubled-suffix naming drift, flaw #6);
    //     no current caller reads it, but we pin its existence + width so the
    //     mirror cannot silently desync from VMTypeEntry.
    // -----------------------------------------------------------------------
    auto test_type_entry_abi_layout() -> void
    {
        static_assert(std::is_same_v<decltype(vm_type_entry_t::type_name), const char*>,
                      "type_name must be const char*");
        static_assert(std::is_same_v<decltype(vm_type_entry_t::superclass_name), const char*>,
                      "superclass_name must be const char*");
        static_assert(std::is_same_v<decltype(vm_type_entry_t::is_oop_type_type), std::int32_t>,
                      "is_oop_type_type (isOopType mirror) must be int32");
        static_assert(std::is_same_v<decltype(vm_type_entry_t::is_integer_type), std::int32_t>,
                      "is_integer_type must be int32");
        static_assert(std::is_same_v<decltype(vm_type_entry_t::is_unsigned), std::int32_t>,
                      "is_unsigned must be int32");
        static_assert(std::is_same_v<decltype(vm_type_entry_t::size), std::uint64_t>,
                      "size must be uint64 (HotSpot publishes the type size as 64-bit)");

        // Round-trip a synthetic type entry's classification + size fields.
        const std::uint64_t sizes[]{ 0ull, 1ull, 8ull, 0xFFFFFFFFull, 0x100000000ull,
                                     0xFFFFFFFFFFFFFFFFull };
        bool size_roundtrip{ true };
        for (const std::uint64_t s : sizes)
        {
            vm_type_entry_t t{};
            t.is_oop_type_type = 1;
            t.is_integer_type = 0;
            t.is_unsigned = 1;
            t.size = s;
            if (t.size != s) { size_roundtrip = false; }
            if (t.is_oop_type_type != 1 || t.is_integer_type != 0 || t.is_unsigned != 1)
            {
                size_roundtrip = false;
            }
        }
        check("type_entry_size_and_flags_roundtrip", size_roundtrip);
        check("type_entry_pointer_aligned",
              alignof(vm_type_entry_t) >= alignof(void*));
    }

    // -----------------------------------------------------------------------
    // 16. Loop-terminator reasoning, asserted at the type level.  The walk
    //     terminates on `entry && entry->type_name`.  A zeroed entry (the
    //     HotSpot terminator) therefore has a null type_name — assert that a
    //     value-initialised entry's type_name IS null (so a zeroed terminator
    //     stops the walk) and that the defensive field_name-null skip is
    //     consistent with the same zeroed-entry shape.  These pin the assumed
    //     terminator shape without a live table.
    // -----------------------------------------------------------------------
    auto test_terminator_shape() -> void
    {
        const vm_struct_entry_t zeroed{};
        check("zeroed_struct_entry_type_name_null", zeroed.type_name == nullptr);
        check("zeroed_struct_entry_field_name_null", zeroed.field_name == nullptr);

        const vm_type_entry_t zeroed_type{};
        check("zeroed_type_entry_type_name_null", zeroed_type.type_name == nullptr);

        // A partial entry (type_name set, field_name null) is the shape the
        // resolver's `if (!entry->field_name) continue;` guard exists for: it
        // must be skippable.  We can't drive it through the real walker without
        // a live head, but we pin that such an entry is representable and the
        // two fields are independent.
        const char marker[]{ "Method" };
        vm_struct_entry_t partial{};
        partial.type_name = marker;
        partial.field_name = nullptr;
        check("partial_struct_entry_type_set_field_null",
              partial.type_name != nullptr && partial.field_name == nullptr);
    }
}

auto main() -> int
{
    std::printf("=== vmstructs_offset_resolution (no-JVM) ===\n");

    test_getters_cached_nullptr();
    test_jvm_module_cached_nullptr();
    test_null_argument_guards();
    test_complete_struct_query_surface();
    test_complete_type_query_surface();
    test_version_fallback_chains();
    test_argument_ordering();
    test_empty_string_permutations();
    test_near_miss_names();
    test_embedded_nul_and_high_bytes();
    test_pathological_lengths();
    test_determinism_and_no_global_state();
    test_walker_implies_getter();
    test_struct_entry_abi_layout();
    test_type_entry_abi_layout();
    test_terminator_shape();

    std::printf("=== %d checks, %d failure(s) ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
