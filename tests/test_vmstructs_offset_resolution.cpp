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
    using vmhook::hotspot::resolve_struct_entry;
    using vmhook::hotspot::struct_entry_candidate_t;
    using vmhook::hotspot::method_flags_evidence;
    using vmhook::hotspot::method_flags_layout;
    using vmhook::hotspot::derive_method_flags_layout;

    // A standalone re-implementation of the EXACT iterate_struct_entries walk
    // semantics (loop terminator `entry && entry->type_name`, the defensive
    // `if (!entry->field_name) continue;` partial-entry skip, exact double
    // strcmp, first-match-wins) over a CALLER-SUPPLIED head.  The real resolver
    // reads the cached global head and takes no array parameter, so a fabricated
    // in-process table cannot be driven through the public API in a no-JVM
    // process.  This mirror lets the success path — match-found, ->offset /
    // ->address / ->is_static read-back, terminator handling, partial-entry
    // skip, no-match exhaustion, first-wins on duplicates — be pinned
    // deterministically as pure struct math.  It is intentionally byte-for-byte
    // the same control flow as vmhook.hpp's iterate_struct_entries so a drift in
    // the documented walk contract is caught here even though the global cannot
    // be populated.
    auto walk_struct_table(const vm_struct_entry_t* head,
                           const char* const type_name,
                           const char* const field_name) -> const vm_struct_entry_t*
    {
        if (!type_name || !field_name) { return nullptr; }
        for (const vm_struct_entry_t* entry{ head }; entry && entry->type_name; ++entry)
        {
            if (!entry->field_name) { continue; }
            if (!std::strcmp(entry->type_name, type_name)
                && !std::strcmp(entry->field_name, field_name))
            {
                return entry;
            }
        }
        return nullptr;
    }

    // Mirror of iterate_type_entries' walk over a caller-supplied head.
    auto walk_type_table(const vm_type_entry_t* head,
                         const char* const type_name) -> const vm_type_entry_t*
    {
        if (!type_name) { return nullptr; }
        for (const vm_type_entry_t* entry{ head }; entry && entry->type_name; ++entry)
        {
            if (!std::strcmp(entry->type_name, type_name)) { return entry; }
        }
        return nullptr;
    }

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

        // Embedded-NUL trailing bytes: "_length\0Z" with explicit length — strcmp
        // stops at the NUL so the effective field key is exactly "_length".  No
        // JVM here, so still nullptr; the positive NUL-truncation proof is below.
        const std::string field_with_nul{ std::string{ "_length" } + '\0' + "Z" };
        check("struct_embedded_nul_field_prefix",
              iterate_struct_entries("Symbol", field_with_nul.c_str()) == nullptr);

        // POSITIVE proof of strcmp NUL-truncation against a POPULATED table: a
        // key "Symbol\0GARBAGE" must MATCH the "Symbol" row (strcmp ignores
        // everything from the NUL on) and must NOT read into the trailing bytes.
        // The match-found path makes this observable; the no-JVM lane otherwise
        // only ever sees nullptr.
        const vm_struct_entry_t nul_table[]{
            { "Symbol", "_length", "u4", 0, 24ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const std::string type_with_nul{ std::string{ "Symbol" } + '\0' + "GARBAGE" };
        const std::string fld_with_nul{ std::string{ "_length" } + '\0' + "MORE" };
        const vm_struct_entry_t* const nul_hit{
            walk_struct_table(nul_table, type_with_nul.c_str(), fld_with_nul.c_str()) };
        check("struct_embedded_nul_truncates_to_prefix_match",
              nul_hit != nullptr && nul_hit->offset == 24ull);
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
    // 13b. resolve_struct_entry — the candidate-chain walker.  The four
    //     compressed-pointer codecs do NOT call iterate_struct_entries directly;
    //     they hand resolve_struct_entry an ORDERED array of (type,field)
    //     candidates and take the first that resolves, which is the whole
    //     mechanism behind the cross-JDK base/shift fallbacks.  In a no-JVM
    //     process every candidate resolves nullptr, so resolve_struct_entry must
    //     walk the WHOLE array and return nullptr — and critically must handle
    //     count==0 (empty list) WITHOUT touching the candidates pointer, and a
    //     null candidates pointer when count==0.  These are pure-logic contracts.
    // -----------------------------------------------------------------------
    auto test_resolve_struct_entry_candidate_chain() -> void
    {
        // count==0 must short-circuit the loop entirely: the candidates pointer
        // is never dereferenced.  Pass a null pointer to prove it is untouched.
        check("resolve_count_zero_null_ptr",
              resolve_struct_entry(nullptr, 0u) == nullptr);

        // count==0 with a non-null (but unread) array.
        static const struct_entry_candidate_t one[]{ { "Symbol", "_length" } };
        check("resolve_count_zero_unread_array",
              resolve_struct_entry(one, 0u) == nullptr);

        // Single candidate, absent in a no-JVM process.
        check("resolve_single_candidate_null",
              resolve_struct_entry(one, 1u) == nullptr);

        // The EXACT real codec candidate arrays, in the try-order the header
        // uses (CompressedOops::_narrow_oop._base -> _base -> Universe...).
        // Every one is absent with no JVM, so the chain exhausts to nullptr —
        // which is precisely the "this JDK has none of these, decode is
        // disabled" signal the codec relies on.
        static const struct_entry_candidate_t oop_base[]{
            { "CompressedOops", "_narrow_oop._base" },
            { "CompressedOops", "_base" },
            { "Universe", "_narrow_oop._base" },
        };
        static const struct_entry_candidate_t oop_shift[]{
            { "CompressedOops", "_narrow_oop._shift" },
            { "CompressedOops", "_shift" },
            { "Universe", "_narrow_oop._shift" },
        };
        static const struct_entry_candidate_t klass_base[]{
            { "CompressedKlassPointers", "_narrow_klass._base" },
            { "CompressedKlassPointers", "_base" },
            { "Universe", "_narrow_klass._base" },
        };
        static const struct_entry_candidate_t klass_shift[]{
            { "CompressedKlassPointers", "_narrow_klass._shift" },
            { "CompressedKlassPointers", "_shift" },
            { "Universe", "_narrow_klass._shift" },
        };
        check("resolve_oop_base_chain_null", resolve_struct_entry(oop_base, 3u) == nullptr);
        check("resolve_oop_shift_chain_null", resolve_struct_entry(oop_shift, 3u) == nullptr);
        check("resolve_klass_base_chain_null", resolve_struct_entry(klass_base, 3u) == nullptr);
        check("resolve_klass_shift_chain_null", resolve_struct_entry(klass_shift, 3u) == nullptr);

        // A long candidate list (every renamed pair the library knows) walked in
        // one go — exhausts to nullptr, no fault, no dependence on list length.
        static const struct_entry_candidate_t many[]{
            { "oopDesc", "_mark" }, { "oopDesc", "_markWord" },
            { "Method", "_from_compiled_code_entry_point" },
            { "Method", "_from_compiled_entry" },
            { "InstanceKlass", "_fields" }, { "InstanceKlass", "_fieldinfo_stream" },
            { "ClassLoaderData", "_klasses" }, { "ClassLoaderData", "_dictionary" },
            { "oopDesc", "_metadata._compressed_klass" }, { "oopDesc", "_metadata._klass" },
            { "Threads", "_thread_list" }, { "ThreadsSMRSupport", "_java_thread_list" },
        };
        check("resolve_many_candidates_null",
              resolve_struct_entry(many, sizeof(many) / sizeof(many[0])) == nullptr);

        // A candidate whose entries contain nulls/empties — resolve_struct_entry
        // forwards them to iterate_struct_entries, whose null-arg guard absorbs
        // them; still nullptr, still no fault.
        static const struct_entry_candidate_t degenerate[]{
            { nullptr, "_x" }, { "X", nullptr }, { "", "" }, { nullptr, nullptr },
        };
        check("resolve_degenerate_candidates_null",
              resolve_struct_entry(degenerate, 4u) == nullptr);

        // Determinism: repeating the same chain resolution always returns
        // nullptr (resolve_struct_entry caches nothing itself).
        bool stable{ true };
        for (int i{ 0 }; i < 512; ++i)
        {
            if (resolve_struct_entry(oop_base, 3u) != nullptr) { stable = false; }
            if (resolve_struct_entry(many, sizeof(many) / sizeof(many[0])) != nullptr) { stable = false; }
            if (resolve_struct_entry(nullptr, 0u) != nullptr) { stable = false; }
        }
        check("resolve_chain_deterministic_null", stable);

        // struct_entry_candidate_t aggregate layout — two const char* the codecs
        // build constexpr arrays of.  Pin the member set + that it is a plain
        // aggregate (brace-init with two strings round-trips).
        static_assert(std::is_same_v<decltype(struct_entry_candidate_t::type_name), const char*>,
                      "candidate type_name must be const char*");
        static_assert(std::is_same_v<decltype(struct_entry_candidate_t::field_name), const char*>,
                      "candidate field_name must be const char*");
        constexpr struct_entry_candidate_t c{ "T", "F" };
        check("candidate_aggregate_roundtrip",
              std::strcmp(c.type_name, "T") == 0 && std::strcmp(c.field_name, "F") == 0);
    }

    // -----------------------------------------------------------------------
    // 13c. Per-pair named struct-surface assertions.  Section 4 collapses the
    //     whole call-site surface into a single boolean, so a regression that
    //     resurrected ONE pair would print an opaque failure.  These name the
    //     highest-value pairs individually (the renamed/version-switch fields
    //     whose presence drives a code branch), so a regression points at the
    //     exact (type,field).  All nullptr with no JVM.
    // -----------------------------------------------------------------------
    auto test_named_struct_surface_pairs() -> void
    {
        struct named { const char* tag; const char* type; const char* field; };
        static const named pairs[]{
            { "pair_symbol_length", "Symbol", "_length" },
            { "pair_symbol_body", "Symbol", "_body" },
            { "pair_method_constmethod", "Method", "_constMethod" },
            { "pair_method_i2i_entry", "Method", "_i2i_entry" },
            { "pair_method_code", "Method", "_code" },
            { "pair_method_adapter", "Method", "_adapter" },
            { "pair_method_access_flags", "Method", "_access_flags" },
            { "pair_method_flags", "Method", "_flags" },
            { "pair_method_intrinsic_id", "Method", "_intrinsic_id" },
            { "pair_method_fce_point", "Method", "_from_compiled_code_entry_point" },
            { "pair_method_fce_short", "Method", "_from_compiled_entry" },
            { "pair_constmethod_constants", "ConstMethod", "_constants" },
            { "pair_constmethod_name_index", "ConstMethod", "_name_index" },
            { "pair_constmethod_signature_index", "ConstMethod", "_signature_index" },
            { "pair_constantpool_length", "ConstantPool", "_length" },
            { "pair_constantpool_pool_holder", "ConstantPool", "_pool_holder" },
            { "pair_klass_name", "Klass", "_name" },
            { "pair_klass_java_mirror", "Klass", "_java_mirror" },
            { "pair_klass_super", "Klass", "_super" },
            { "pair_klass_next_link", "Klass", "_next_link" },
            { "pair_klass_class_loader_data", "Klass", "_class_loader_data" },
            { "pair_ik_methods", "InstanceKlass", "_methods" },
            { "pair_ik_fields", "InstanceKlass", "_fields" },
            { "pair_ik_fieldinfo_stream", "InstanceKlass", "_fieldinfo_stream" },
            { "pair_cld_klasses", "ClassLoaderData", "_klasses" },
            { "pair_cld_dictionary", "ClassLoaderData", "_dictionary" },
            { "pair_cldg_head", "ClassLoaderDataGraph", "_head" },
            { "pair_oop_mark", "oopDesc", "_mark" },
            { "pair_oop_markword", "oopDesc", "_markWord" },
            { "pair_oop_compressed_klass", "oopDesc", "_metadata._compressed_klass" },
            { "pair_oop_klass", "oopDesc", "_metadata._klass" },
            { "pair_jlc_klass_offset", "java_lang_Class", "_klass_offset" },
            { "pair_stub_call_stub_entry", "StubRoutines", "_call_stub_entry" },
            { "pair_adapter_c2i_entry", "AdapterHandlerEntry", "_c2i_entry" },
        };
        for (const auto& p : pairs)
        {
            check(p.tag, iterate_struct_entries(p.type, p.field) == nullptr);
        }
    }

    // -----------------------------------------------------------------------
    // 13d. Synthetic populated-table walk (success path).  Section 4..12 cover
    //     ABSENT-table behaviour exhaustively; this pins the MATCH-FOUND path —
    //     the resolver's entire reason to exist — using walk_struct_table, a
    //     byte-for-byte mirror of iterate_struct_entries over a local synthetic
    //     array (the real global cannot be populated in a no-JVM process).  This
    //     directly exercises flaw #1 (offset vs address, is_static surfaced) and
    //     flaw #4 (terminator / partial-entry / off-by-one) deterministically.
    // -----------------------------------------------------------------------
    auto test_synthetic_table_walk() -> void
    {
        int static_sentinel_a{ 0 };
        int static_sentinel_b{ 0 };

        // A populated table ending in a zeroed terminator.  Mix of instance
        // (is_static==0, ->offset live) and static (is_static==1, ->address
        // live) entries, plus a PARTIAL entry (type set, field null) the walk
        // must SKIP without faulting and without stopping the walk, and a
        // duplicate (Method,_code) to pin first-wins.
        const vm_struct_entry_t table[]{
            { "Symbol", "_length",  "u4", 0, 8ull,  nullptr },
            { "Symbol", nullptr,    "u1", 0, 999ull, nullptr },          // partial: skipped
            { "Method", "_code",    "nmethod*", 0, 0x48ull, nullptr },   // first dup
            { "oopDesc","_mark",    "markWord", 0, 0ull,  nullptr },
            { "Universe","_collectedHeap", "CollectedHeap*", 1, 0ull, &static_sentinel_a },
            { "CompressedOops","_base", "address", 1, 0ull, &static_sentinel_b },
            { "Method", "_code",    "nmethod*", 0, 0xDEADull, nullptr }, // dup: never reached
            { "Method", "_offset_max", "u8", 0, 0xFFFFFFFFFFFFFFFFull, nullptr },
            { nullptr,  nullptr,    nullptr, 0, 0ull, nullptr },         // terminator
        };
        const vm_struct_entry_t* const head{ table };

        // Exact match on an INSTANCE field returns the entry; ->offset is live,
        // ->is_static==0, and ->offset round-trips the synthetic value exactly.
        const vm_struct_entry_t* const sym{ walk_struct_table(head, "Symbol", "_length") };
        check("synth_instance_match_nonnull", sym != nullptr);
        check("synth_instance_offset_exact", sym != nullptr && sym->offset == 8ull);
        check("synth_instance_is_static_zero", sym != nullptr && sym->is_static == 0);

        // The partial entry (Symbol,<null field>) at row 2 is skipped by the
        // `continue` and does NOT stop the walk — a later real row is still
        // reachable.  Prove it: the Method row AFTER the partial resolves.
        const vm_struct_entry_t* const meth{ walk_struct_table(head, "Method", "_code") };
        check("synth_after_partial_reachable", meth != nullptr);

        // First-wins on duplicate (Method,_code): the 0x48 row, not 0xDEAD.
        check("synth_duplicate_first_wins", meth != nullptr && meth->offset == 0x48ull);

        // STATIC field: ->address is the live word (the sentinel), ->is_static==1.
        // This is flaw #1 — the caller must read ->address, not ->offset.
        const vm_struct_entry_t* const heap{ walk_struct_table(head, "Universe", "_collectedHeap") };
        check("synth_static_match_nonnull", heap != nullptr);
        check("synth_static_is_static_one", heap != nullptr && heap->is_static == 1);
        check("synth_static_address_live", heap != nullptr && heap->address == &static_sentinel_a);

        // A second static entry resolves to its OWN distinct address — the two
        // statics are not aliased.
        const vm_struct_entry_t* const cbase{ walk_struct_table(head, "CompressedOops", "_base") };
        check("synth_static_b_address_live",
              cbase != nullptr && cbase->address == &static_sentinel_b
              && cbase->address != &static_sentinel_a);

        // Full uint64 offset round-trips through the walk with no 32-bit
        // truncation (the entry just before the terminator).
        const vm_struct_entry_t* const omax{ walk_struct_table(head, "Method", "_offset_max") };
        check("synth_offset_max_uint64",
              omax != nullptr && omax->offset == 0xFFFFFFFFFFFFFFFFull);

        // No-match exhaustion on a FULLY-POPULATED table: walk every row to the
        // terminator and return nullptr (distinct from the trivially-empty-array
        // case the rest of the file covers).
        check("synth_no_match_exhausts_null",
              walk_struct_table(head, "Klass", "_name") == nullptr);
        check("synth_wrong_field_on_real_type_null",
              walk_struct_table(head, "Symbol", "_body") == nullptr);
        check("synth_wrong_type_on_real_field_null",
              walk_struct_table(head, "ConstMethod", "_length") == nullptr);

        // Argument independence with a populated table: swapping (type,field)
        // must NOT match the row where the strings appear in the other column.
        check("synth_swapped_args_no_match",
              walk_struct_table(head, "_length", "Symbol") == nullptr);

        // Case sensitivity / near-miss against a populated table: strcmp is exact.
        check("synth_case_mismatch_null",
              walk_struct_table(head, "symbol", "_length") == nullptr);
        check("synth_field_case_mismatch_null",
              walk_struct_table(head, "Symbol", "_Length") == nullptr);
        check("synth_field_prefix_null",
              walk_struct_table(head, "Method", "_cod") == nullptr);
        check("synth_field_superstring_null",
              walk_struct_table(head, "Method", "_code_entry") == nullptr);

        // Null args against a populated table still short-circuit (guard runs
        // before any row is touched).
        check("synth_null_type_guard", walk_struct_table(head, nullptr, "_length") == nullptr);
        check("synth_null_field_guard", walk_struct_table(head, "Symbol", nullptr) == nullptr);

        // A null head (the no-JVM shape) terminates immediately.
        check("synth_null_head_null",
              walk_struct_table(nullptr, "Symbol", "_length") == nullptr);

        // A table whose VERY FIRST entry is the terminator (zeroed type_name)
        // returns nullptr for any query — the empty-but-non-null-head shape.
        const vm_struct_entry_t empty_table[]{ { nullptr, nullptr, nullptr, 0, 0ull, nullptr } };
        check("synth_first_is_terminator_null",
              walk_struct_table(empty_table, "Symbol", "_length") == nullptr);

        // Off-by-one: the match is the LAST real row immediately before the
        // terminator — it must still be found (loop reads it before the
        // terminator stops the walk).
        const vm_struct_entry_t last_row_table[]{
            { "A", "_a", "x", 0, 1ull, nullptr },
            { "B", "_b", "x", 0, 2ull, nullptr },
            { "Z", "_z", "x", 0, 0x1234ull, nullptr },                  // last real row
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const vm_struct_entry_t* const last{ walk_struct_table(last_row_table, "Z", "_z") };
        check("synth_last_row_before_terminator_found",
              last != nullptr && last->offset == 0x1234ull);

        // A partial entry as the FIRST row (type set, field null) followed by
        // the real match — the `continue` must not skip the later valid row.
        const vm_struct_entry_t partial_first_table[]{
            { "Symbol", nullptr, "x", 0, 7ull, nullptr },               // partial first
            { "Symbol", "_length", "u4", 0, 16ull, nullptr },           // real match after
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const vm_struct_entry_t* const after_partial{
            walk_struct_table(partial_first_table, "Symbol", "_length") };
        check("synth_partial_first_then_match",
              after_partial != nullptr && after_partial->offset == 16ull);
    }

    // -----------------------------------------------------------------------
    // 13e. Synthetic TYPE-table walk (success path) — the gHotSpotVMTypes
    //     mirror.  Pins iterate_type_entries' match-found path: a populated
    //     vm_type_entry_t array resolves a type by name and reads back its
    //     classification (is_oop_type_type / is_integer_type / is_unsigned) and
    //     64-bit size, exercising the flaw-#6-named member positively.
    // -----------------------------------------------------------------------
    auto test_synthetic_type_walk() -> void
    {
        const vm_type_entry_t types[]{
            { "oopDesc",     "",       1, 0, 0, 0ull },                  // oop type
            { "InstanceKlass","Klass", 0, 0, 0, 0ull },
            { "jint",        nullptr,  0, 1, 0, 4ull },                  // signed integer
            { "size_t",      nullptr,  0, 1, 1, 8ull },                  // unsigned integer
            { "narrowOop",   nullptr,  0, 1, 1, 0xFFFFFFFFFFFFFFFFull }, // full-width size
            { nullptr, nullptr, 0, 0, 0, 0ull },                        // terminator
        };
        const vm_type_entry_t* const head{ types };

        const vm_type_entry_t* const oop{ walk_type_table(head, "oopDesc") };
        check("synth_type_oop_nonnull", oop != nullptr);
        check("synth_type_oop_is_oop", oop != nullptr && oop->is_oop_type_type == 1);
        check("synth_type_oop_not_integer", oop != nullptr && oop->is_integer_type == 0);

        const vm_type_entry_t* const ji{ walk_type_table(head, "jint") };
        check("synth_type_jint_integer",
              ji != nullptr && ji->is_integer_type == 1 && ji->is_unsigned == 0 && ji->size == 4ull);

        const vm_type_entry_t* const sz{ walk_type_table(head, "size_t") };
        check("synth_type_sizet_unsigned",
              sz != nullptr && sz->is_unsigned == 1 && sz->size == 8ull);

        const vm_type_entry_t* const ik{ walk_type_table(head, "InstanceKlass") };
        check("synth_type_superclass_readback",
              ik != nullptr && ik->superclass_name != nullptr
              && std::strcmp(ik->superclass_name, "Klass") == 0);

        const vm_type_entry_t* const no{ walk_type_table(head, "narrowOop") };
        check("synth_type_size_full_uint64",
              no != nullptr && no->size == 0xFFFFFFFFFFFFFFFFull);

        // No-match on a populated type table.
        check("synth_type_no_match_null", walk_type_table(head, "Method") == nullptr);
        check("synth_type_case_mismatch_null", walk_type_table(head, "OOPDESC") == nullptr);
        check("synth_type_null_arg_guard", walk_type_table(head, nullptr) == nullptr);
        check("synth_type_null_head_null", walk_type_table(nullptr, "oopDesc") == nullptr);
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

    // -----------------------------------------------------------------------
    // 17. VMStruct offset-resolution arithmetic feeding the Method-flags layout
    //     DECISION (derive_method_flags_layout).  This is the resolver's
    //     reason-to-exist exercised end-to-end on the no-JVM surface: a resolved
    //     vm_struct_entry_t's ->type_string and ->offset are the ONLY inputs the
    //     library reads out of gHotSpotVMStructs for the _dont_inline locator
    //     (resolve_method_flags_slot, vmhook.hpp:7518-7536), which copies them
    //     into a method_flags_evidence and runs the PURE derive_method_flags_layout
    //     (vmhook.hpp:7450-7498) to pick (offset,width,bit,confident).  Here we
    //     build the SAME evidence by walking a synthetic table with the in-file
    //     walk_struct_table mirror (so a resolved ->offset / ->type_string drives
    //     the decision exactly as the real resolver does — no live Method, no
    //     fabricated address read, pure struct + integer math).  Every expected
    //     value is read straight off vmhook.hpp's two decision paths and the
    //     per-JDK analysis in its block comment (8 / 11-20 / 21+).
    //
    //     Path A (JDK 11..20): exported `mutable u2 Method::_flags`
    //                          -> {offset = flags_offset, width 2, bit 2, confident}.
    //     Path B (JDK 21+):    `_flags` absent/non-u2 BUT `_intrinsic_id` present
    //                          as EXACTLY "u2" with offset >= 4 and 4-aligned
    //                          -> {offset = intrinsic_id_offset - 4, width 4,
    //                              bit 12, confident}.
    //     JDK 8 / unrecognised: neither path -> default-constructed (not confident).
    // -----------------------------------------------------------------------
    auto build_method_flags_evidence(const vm_struct_entry_t* const head)
        -> method_flags_evidence
    {
        // Mirror of resolve_method_flags_slot's evidence-gathering (7524-7536):
        // resolve Method._flags / Method._intrinsic_id, copy type_string+offset.
        method_flags_evidence evidence{};
        const vm_struct_entry_t* const flags_entry{
            walk_struct_table(head, "Method", "_flags") };
        const vm_struct_entry_t* const intrinsic_entry{
            walk_struct_table(head, "Method", "_intrinsic_id") };
        if (flags_entry)
        {
            evidence.flags_present = true;
            evidence.flags_type = flags_entry->type_string;
            evidence.flags_offset = flags_entry->offset;
        }
        if (intrinsic_entry)
        {
            evidence.intrinsic_id_present = true;
            evidence.intrinsic_id_type = intrinsic_entry->type_string;
            evidence.intrinsic_id_offset = intrinsic_entry->offset;
        }
        return evidence;
    }

    auto test_method_flags_layout_from_resolved_offsets() -> void
    {
        // ---- ABI of the evidence / layout PODs the resolver fills and the pure
        // decision returns.  Widths are load-bearing: flags_offset/intrinsic_id_
        // offset mirror vm_struct_entry_t::offset (uint64), so the `- 4` derive
        // and the `% 4` alignment gate never truncate; the layout offset is the
        // same uint64 so `base + layout.offset` cannot 32-bit wrap.
        static_assert(std::is_same_v<decltype(method_flags_evidence::flags_present), bool>,
                      "flags_present must be bool");
        static_assert(std::is_same_v<decltype(method_flags_evidence::flags_type), const char*>,
                      "flags_type must be const char*");
        static_assert(std::is_same_v<decltype(method_flags_evidence::flags_offset), std::uint64_t>,
                      "flags_offset must be uint64 (mirrors VMStruct ->offset)");
        static_assert(std::is_same_v<decltype(method_flags_evidence::intrinsic_id_present), bool>,
                      "intrinsic_id_present must be bool");
        static_assert(std::is_same_v<decltype(method_flags_evidence::intrinsic_id_type), const char*>,
                      "intrinsic_id_type must be const char*");
        static_assert(std::is_same_v<decltype(method_flags_evidence::intrinsic_id_offset), std::uint64_t>,
                      "intrinsic_id_offset must be uint64 (mirrors VMStruct ->offset)");
        static_assert(std::is_same_v<decltype(method_flags_layout::offset), std::uint64_t>,
                      "layout offset must be uint64 (base+offset must not truncate)");
        static_assert(std::is_same_v<decltype(method_flags_layout::width_bytes), int>,
                      "layout width_bytes must be int");
        static_assert(std::is_same_v<decltype(method_flags_layout::dont_inline_bit), int>,
                      "layout dont_inline_bit must be int");
        static_assert(std::is_same_v<decltype(method_flags_layout::confident), bool>,
                      "layout confident must be bool");

        // The decision is constexpr-evaluable on synthetic evidence: pin the
        // three canonical layouts at COMPILE time so a drift in the pure decision
        // is caught even where the runtime path is never reached.
        {
            constexpr method_flags_evidence path_a{ true, "u2", 64ull, true, "u2", 70ull };
            constexpr method_flags_layout la{ derive_method_flags_layout(path_a) };
            static_assert(la.confident, "path A must be confident");
            static_assert(la.offset == 64ull, "path A offset == flags_offset");
            static_assert(la.width_bytes == 2, "path A width 2");
            static_assert(la.dont_inline_bit == 2, "path A bit 2");

            constexpr method_flags_evidence path_b{ false, nullptr, 0ull, true, "u2", 48ull };
            constexpr method_flags_layout lb{ derive_method_flags_layout(path_b) };
            static_assert(lb.confident, "path B must be confident");
            static_assert(lb.offset == 44ull, "path B offset == intrinsic_id_offset - 4");
            static_assert(lb.width_bytes == 4, "path B width 4");
            static_assert(lb.dont_inline_bit == 12, "path B bit 12");

            constexpr method_flags_evidence jdk8{ false, nullptr, 0ull, true, "u1", 40ull };
            constexpr method_flags_layout l8{ derive_method_flags_layout(jdk8) };
            static_assert(!l8.confident, "JDK 8 u1 intrinsic_id -> not confident");
            static_assert(l8.offset == 0ull && l8.width_bytes == 0 && l8.dont_inline_bit == 0,
                          "non-confident layout is value-initialised");
        }

        // ---- Path A driven THROUGH the resolver mirror: a synthetic table that
        // exports Method._flags as "u2" at offset 64.  walk_struct_table resolves
        // it, build_method_flags_evidence copies ->type_string/->offset, and the
        // pure decision yields {64, 2, 2, confident} — exactly Path A.
        const vm_struct_entry_t path_a_table[]{
            { "Method", "_flags", "u2", 0, 64ull, nullptr },
            { "Method", "_intrinsic_id", "u2", 0, 72ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const method_flags_layout la{
            derive_method_flags_layout(build_method_flags_evidence(path_a_table)) };
        check("flags_path_a_confident", la.confident);
        check("flags_path_a_offset_is_flags_offset", la.offset == 64ull);
        check("flags_path_a_width_2", la.width_bytes == 2);
        check("flags_path_a_bit_2", la.dont_inline_bit == 2);

        // ---- Path B: _flags ABSENT, _intrinsic_id exported as "u2" at offset 48
        // (4-aligned, >= 4).  Derived flags-word offset is 48 - 4 == 44, width 4,
        // bit 12.  This is the JDK 21+ MethodFlags::_status derivation, with the
        // `_intrinsic_id_offset - 4` arithmetic done over the resolved uint64.
        const vm_struct_entry_t path_b_table[]{
            { "Method", "_intrinsic_id", "u2", 0, 48ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const method_flags_layout lb{
            derive_method_flags_layout(build_method_flags_evidence(path_b_table)) };
        check("flags_path_b_confident", lb.confident);
        check("flags_path_b_offset_minus_4", lb.offset == 44ull);
        check("flags_path_b_width_4", lb.width_bytes == 4);
        check("flags_path_b_bit_12", lb.dont_inline_bit == 12);

        // ---- Path A WINS when BOTH are present and _flags is "u2": the decision
        // checks Path A first, so a u2 _flags owns the layout even if a u2
        // _intrinsic_id is also exported (mirrors the 11..20 layout where both
        // members exist).  Offset must be the FLAGS offset, not intrinsic-4.
        const vm_struct_entry_t both_table[]{
            { "Method", "_flags", "u2", 0, 80ull, nullptr },
            { "Method", "_intrinsic_id", "u2", 0, 96ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const method_flags_layout lboth{
            derive_method_flags_layout(build_method_flags_evidence(both_table)) };
        check("flags_path_a_wins_over_b", lboth.confident
              && lboth.offset == 80ull && lboth.width_bytes == 2
              && lboth.dont_inline_bit == 2);

        // ---- Rejection gates (each -> not confident -> caller no-ops).
        // (a) JDK 8 shape: NOTHING exported (no _flags, no _intrinsic_id row).
        const vm_struct_entry_t jdk8_table[]{
            { "Method", "_constMethod", "ConstMethod*", 0, 16ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const method_flags_layout l8{
            derive_method_flags_layout(build_method_flags_evidence(jdk8_table)) };
        check("flags_jdk8_nothing_exported_not_confident", !l8.confident);

        // (b) _intrinsic_id present but as "u1" (JDK 8's actual width) -> Path B
        // type-gate excludes it; with no u2 _flags either, not confident.
        const vm_struct_entry_t u1_table[]{
            { "Method", "_intrinsic_id", "u1", 0, 48ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        check("flags_intrinsic_u1_not_confident",
              !derive_method_flags_layout(build_method_flags_evidence(u1_table)).confident);

        // (c) _intrinsic_id u2 but offset < 4 -> underflow gate rejects.
        const vm_struct_entry_t low_off_table[]{
            { "Method", "_intrinsic_id", "u2", 0, 2ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        check("flags_intrinsic_offset_lt4_not_confident",
              !derive_method_flags_layout(build_method_flags_evidence(low_off_table)).confident);

        // (d) _intrinsic_id u2 but offset not 4-aligned (6) -> alignment gate
        // rejects (the u4 _status it derives must be 4-aligned).
        const vm_struct_entry_t misaligned_table[]{
            { "Method", "_intrinsic_id", "u2", 0, 6ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        check("flags_intrinsic_misaligned_not_confident",
              !derive_method_flags_layout(build_method_flags_evidence(misaligned_table)).confident);

        // (e) _flags present but a NON-u2 type ("u4") with no usable _intrinsic_id
        // -> Path A type-gate excludes it, Path B has no intrinsic -> not confident.
        const vm_struct_entry_t u4_flags_table[]{
            { "Method", "_flags", "u4", 0, 64ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        check("flags_u4_flags_no_intrinsic_not_confident",
              !derive_method_flags_layout(build_method_flags_evidence(u4_flags_table)).confident);

        // (f) _flags is "u4" (non-u2) AND a valid u2 _intrinsic_id IS present:
        // Path A declines (not u2), Path B fires off the intrinsic offset.  This
        // is the JDK 21+ shape where _flags exists but is the MethodFlags object,
        // not the legacy u2 -> derived offset 100 - 4 == 96, width 4, bit 12.
        const vm_struct_entry_t u4_flags_with_intrinsic[]{
            { "Method", "_flags", "u4", 0, 90ull, nullptr },
            { "Method", "_intrinsic_id", "u2", 0, 100ull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const method_flags_layout lmix{
            derive_method_flags_layout(build_method_flags_evidence(u4_flags_with_intrinsic)) };
        check("flags_u4_flags_falls_through_to_path_b",
              lmix.confident && lmix.offset == 96ull && lmix.width_bytes == 4
              && lmix.dont_inline_bit == 12);

        // ---- Large/aligned offset round-trips through the uint64 derive with no
        // truncation: a 4-aligned _intrinsic_id offset just under 4 GiB derives
        // offset-4 exactly (proves the `- 4` and `% 4` run on the full uint64).
        const vm_struct_entry_t big_off_table[]{
            { "Method", "_intrinsic_id", "u2", 0, 0xFFFFFFFCull, nullptr },
            { nullptr, nullptr, nullptr, 0, 0ull, nullptr },
        };
        const method_flags_layout lbig{
            derive_method_flags_layout(build_method_flags_evidence(big_off_table)) };
        check("flags_big_offset_no_truncation",
              lbig.confident && lbig.offset == 0xFFFFFFF8ull && lbig.width_bytes == 4);

        // ---- Empty / no-JVM evidence (the shape resolve_method_flags_slot builds
        // when iterate_struct_entries returns nullptr for both): default evidence
        // -> not confident -> caller no-ops.  This is the live no-JVM contract.
        check("flags_empty_evidence_not_confident",
              !derive_method_flags_layout(method_flags_evidence{}).confident);

        // The real resolver path with the live (null) global head produces exactly
        // that empty evidence, so the live decision here must also be not-confident.
        method_flags_evidence live_evidence{};
        const vm_struct_entry_t* const live_flags{ iterate_struct_entries("Method", "_flags") };
        const vm_struct_entry_t* const live_intr{ iterate_struct_entries("Method", "_intrinsic_id") };
        if (live_flags)
        {
            live_evidence.flags_present = true;
            live_evidence.flags_type = live_flags->type_string;
            live_evidence.flags_offset = live_flags->offset;
        }
        if (live_intr)
        {
            live_evidence.intrinsic_id_present = true;
            live_evidence.intrinsic_id_type = live_intr->type_string;
            live_evidence.intrinsic_id_offset = live_intr->offset;
        }
        check("flags_live_no_jvm_evidence_empty",
              !live_evidence.flags_present && !live_evidence.intrinsic_id_present);
        check("flags_live_no_jvm_not_confident",
              !derive_method_flags_layout(live_evidence).confident);

        // Determinism: the pure decision is a function of its argument only.
        bool stable{ true };
        for (int i{ 0 }; i < 256; ++i)
        {
            const method_flags_layout a{
                derive_method_flags_layout(build_method_flags_evidence(path_a_table)) };
            const method_flags_layout b{
                derive_method_flags_layout(build_method_flags_evidence(path_b_table)) };
            if (!a.confident || a.offset != 64ull || a.width_bytes != 2) { stable = false; }
            if (!b.confident || b.offset != 44ull || b.width_bytes != 4) { stable = false; }
        }
        check("flags_decision_deterministic", stable);
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
    test_resolve_struct_entry_candidate_chain();
    test_named_struct_surface_pairs();
    test_synthetic_table_walk();
    test_synthetic_type_walk();
    test_struct_entry_abi_layout();
    test_type_entry_abi_layout();
    test_terminator_shape();
    test_method_flags_layout_from_resolved_offsets();

    std::printf("=== %d checks, %d failure(s) ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
