// Standalone (no-JVM) unit test pinning the NO-JVM contracts of the recently
// hardened class-lookup / method-introspection entry points:
//
//   vmhook::find_class(class_name)                  -> klass*  (null w/o JVM)
//   vmhook::get_class_methods(class_name)           -> vector  (empty w/o JVM)
//   vmhook::get_class_methods<T>()                  -> vector  (empty w/o JVM)
//   vmhook::find_methods_by_signature<T>(desc)      -> vector  (empty w/o JVM)
//
// In this process there is NO JVM:
//   * find_class first short-circuits the empty name (vmhook.hpp ~6439), then
//     for any non-empty name runs the ClassLoaderDataGraph walk.  With no JVM
//     gHotSpotVMStructs is absent so iterate_struct_entries() returns null,
//     class_loader_data_graph::get_head() throws-internally-and-returns-null,
//     and the walk yields null.  The JNI fallback
//     (jni_find_class_with_context_loader) then bails immediately because
//     ensure_current_java_thread() / current_jni_env are null.  Net result:
//     find_class returns nullptr for EVERY name, never throws, never crashes.
//   * collect_klass_methods(nullptr) early-returns an empty vector, so every
//     get_class_methods* overload is empty, and find_methods_by_signature
//     (which iterates get_class_methods<T>()) is empty too.
//
// The "real klass / real method list" behaviour requires a live JVM and is
// covered by JVM integration in example.cpp.  test_method_enumeration.cpp
// already pins the empty-list contract for the registered/unregistered wrapper
// split and the by-name path; this file is the dedicated, EXHAUSTIVE-input
// home for find_class itself (which otherwise has only indirect coverage via
// test_object_factory / register_class) plus a throwaway-wrapper sweep of the
// templated method-introspection forms across many descriptor shapes.
//
// Every assertion's expected value (nullptr / empty / no-throw) is derived from
// the implementations cited above, not guessed.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // Throwaway wrapper for the templated get_class_methods<T>() /
    // find_methods_by_signature<T>() forms.  Left UNregistered on purpose so
    // the type_to_class_map lookup misses and the empty-on-miss branch is
    // exercised (vmhook.hpp ~7158).
    struct unreg_w : vmhook::object<unreg_w>
    {
        using vmhook::object<unreg_w>::object;
    };

    // A second throwaway wrapper that WILL be registered below.  Registration
    // with no JVM still populates type_to_class_map (register_class inserts the
    // mapping; it only returns false because find_class can't verify the
    // class), so get_class_methods<reg_w>() takes the map-HIT branch and then
    // walks a null klass -> still empty.  This distinguishes the two empty
    // paths (map-miss vs. null-klass) for the templated form.
    struct reg_w : vmhook::object<reg_w>
    {
        using vmhook::object<reg_w>::object;
    };

    // find_class never throws; run a call inside a try/catch and report whether
    // it stayed noexcept-in-practice (returns true) so a regression that starts
    // throwing is caught as a [FAIL] rather than an aborted test binary.
    auto find_class_never_throws(std::string_view name) -> bool
    {
        try
        {
            (void)vmhook::find_class(name);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
}

int main()
{
    // ---------------------------------------------------------------------
    // 0. Precondition: confirm there is genuinely no JVM in this process, so
    //    the null/empty contracts below are the no-JVM contracts (not an
    //    accidental real-class miss).  current_jni_env is null until a JVM
    //    attaches; every fallback path keys off it.
    // ---------------------------------------------------------------------
    check("precondition_no_jvm_env_is_null",
          vmhook::hotspot::current_jni_env == nullptr);

    // ---------------------------------------------------------------------
    // 1. find_class("") -> nullptr (the explicit empty-name fast-reject).
    //    This is the dedicated empty-string guard, distinct from a normal
    //    not-found miss: it returns before any graph walk / JNI call.
    // ---------------------------------------------------------------------
    check("find_class_empty_name_is_null",
          vmhook::find_class("") == nullptr);
    check("find_class_empty_name_never_throws",
          find_class_never_throws(""));
    // A default-constructed string_view (null data, zero size) is also "empty"
    // and must hit the same guard without dereferencing the null data pointer.
    check("find_class_default_string_view_is_null",
          vmhook::find_class(std::string_view{}) == nullptr);

    // ---------------------------------------------------------------------
    // 2. find_class on a REAL, always-present bootstrap class name -> nullptr
    //    with no JVM.  These names WOULD resolve against a live JVM; here they
    //    must not, proving the no-JVM path returns null rather than fabricating
    //    a klass.
    // ---------------------------------------------------------------------
    check("find_class_java_lang_Object_is_null",
          vmhook::find_class("java/lang/Object") == nullptr);
    check("find_class_java_lang_String_is_null",
          vmhook::find_class("java/lang/String") == nullptr);
    check("find_class_java_lang_Thread_is_null",
          vmhook::find_class("java/lang/Thread") == nullptr);
    check("find_class_java_util_HashMap_is_null",
          vmhook::find_class("java/util/HashMap") == nullptr);

    // ---------------------------------------------------------------------
    // 3. find_class on junk / non-existent names -> nullptr, never throws.
    //    Covers a variety of malformed-but-plausible internal-name shapes.
    // ---------------------------------------------------------------------
    {
        const char* const junk_names[]{
            "definitely/Not/A/Class",
            "a",                              // single char
            "NoSlashAtAll",                  // unqualified
            "trailing/slash/",               // trailing separator
            "/leading/slash",                // leading separator
            "double//slash",                 // empty path segment
            "with spaces/in it",             // spaces (never a valid binary name)
            "weird$Inner$Names",             // $-nested (valid chars, unloaded)
            "java.lang.Object",              // dotted form (find_class wants '/')
            "[Ljava/lang/Object;",           // array descriptor, not a plain name
            "[I",                            // primitive-array descriptor
            "Ljava/lang/Object;",            // field descriptor, not a class name
            "\x01\x02\x03",                  // control bytes
            "tab\tnewline\nname",            // embedded whitespace controls
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const char* n : junk_names)
        {
            if (vmhook::find_class(n) != nullptr) { all_null = false; }
            if (!find_class_never_throws(n))      { none_threw = false; }
        }
        check("find_class_all_junk_names_null", all_null);
        check("find_class_all_junk_names_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 4. find_class on names embedding an interior NUL.  string_view carries
    //    an explicit length, so "java/lang/Object\0extra" is a 22-char name
    //    that can never match a loaded class; must be null, no over-read.
    // ---------------------------------------------------------------------
    {
        const std::string with_nul{ std::string{ "java/lang/Object" } + '\0' + "extra" };
        const std::string_view view_with_nul{ with_nul.data(), with_nul.size() };
        check("find_class_interior_nul_name_is_null",
              vmhook::find_class(view_with_nul) == nullptr);
        check("find_class_interior_nul_name_no_throw",
              find_class_never_throws(view_with_nul));
        // A leading NUL makes a non-empty view whose first byte is 0; it is NOT
        // the empty-name guard (size != 0), so it travels the normal path and
        // must still be null.
        const std::string leading_nul{ std::string{ '\0' } + "java/lang/Object" };
        const std::string_view leading_nul_view{ leading_nul.data(), leading_nul.size() };
        check("find_class_leading_nul_name_is_null",
              vmhook::find_class(leading_nul_view) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 5. find_class on a VERY long name -> nullptr, never throws.  Stresses
    //    the std::string{class_name} cache-key construction and the walk with
    //    an oversized input; no fixed-size buffer should be involved.
    // ---------------------------------------------------------------------
    {
        const std::string long_pkg(4096, 'a');                 // 4 KiB of 'a'
        const std::string long_name{ long_pkg + "/" + long_pkg };
        check("find_class_very_long_name_is_null",
              vmhook::find_class(long_name) == nullptr);
        check("find_class_very_long_name_no_throw",
              find_class_never_throws(long_name));

        // A deeply nested package path (many '/' segments).
        std::string deep{};
        for (int i{ 0 }; i < 512; ++i) { deep += "pkg/"; }
        deep += "Leaf";
        check("find_class_deeply_nested_name_is_null",
              vmhook::find_class(deep) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 6. find_class is idempotent for a miss: a repeated lookup of the same
    //    missing name still returns null (a not-found result is not cached as
    //    a poisoned hit) and does not throw on the second call.
    // ---------------------------------------------------------------------
    {
        const char* const name{ "repeat/Miss/Probe" };
        const auto first{ vmhook::find_class(name) };
        const auto second{ vmhook::find_class(name) };
        check("find_class_repeat_miss_first_null",  first == nullptr);
        check("find_class_repeat_miss_second_null", second == nullptr);
        check("find_class_repeat_miss_stable", first == second);
    }

    // ---------------------------------------------------------------------
    // 7. get_class_methods(name) by internal name -> empty for every shape of
    //    input (collect_klass_methods(nullptr) returns {}), never throws.
    // ---------------------------------------------------------------------
    {
        const char* const names[]{
            "",                              // empty name -> find_class null
            "java/lang/Object",              // real-but-no-JVM
            "java/util/List",
            "definitely/Not/A/Class",        // miss
            "[Ljava/lang/Object;",           // descriptor-shaped
            "weird$Inner",                   // unloaded
        };
        bool all_empty{ true };
        for (const char* n : names)
        {
            if (!vmhook::get_class_methods(n).empty()) { all_empty = false; }
        }
        check("get_class_methods_by_name_all_empty_no_jvm", all_empty);

        // Spot-check the explicit empty-name case and assert size()==0 too.
        check("get_class_methods_empty_name_size0",
              vmhook::get_class_methods("").size() == 0);

        // Never throws on a long name either.
        try
        {
            const std::string long_name(8192, 'x');
            const bool empty{ vmhook::get_class_methods(long_name).empty() };
            check("get_class_methods_long_name_empty_no_throw", empty);
        }
        catch (...)
        {
            check("get_class_methods_long_name_empty_no_throw", false);
        }
    }

    // ---------------------------------------------------------------------
    // 8. Templated get_class_methods<T>().
    //    * UNregistered T  -> type-map MISS -> empty (the catch-all empty).
    //    * Registered   T  -> type-map HIT  -> find_class null -> empty.
    //    Both empty with no JVM; never throws.  The two distinct empty paths
    //    are what this file adds over the by-name coverage.
    // ---------------------------------------------------------------------
    {
        // Before registration, reg_w is also a map miss -> empty.
        check("get_class_methods_T_unregistered_before_reg_empty",
              vmhook::get_class_methods<reg_w>().empty());

        // register_class returns false (no JVM to verify), but STILL records
        // the type->name mapping, so the next call takes the map-HIT branch.
        const bool registered{ vmhook::register_class<reg_w>("test/find_class/Reg") };
        check("register_class_returns_false_no_jvm", registered == false);

        check("get_class_methods_T_registered_hit_empty",
              vmhook::get_class_methods<reg_w>().empty());
        check("get_class_methods_T_registered_hit_size0",
              vmhook::get_class_methods<reg_w>().size() == 0);

        // A never-registered wrapper stays a map miss -> empty.
        check("get_class_methods_T_never_registered_empty",
              vmhook::get_class_methods<unreg_w>().empty());

        // Return type is exactly vector<pair<string,string>> (documents the
        // data channel callers iterate).
        using ret_t = decltype(vmhook::get_class_methods<reg_w>());
        static_assert(std::is_same_v<ret_t,
                          std::vector<std::pair<std::string, std::string>>>,
                      "get_class_methods<T>() must return vector<pair<name,descriptor>>");
        check("get_class_methods_T_return_type_is_pair_vector", true);
    }

    // ---------------------------------------------------------------------
    // 9. find_methods_by_signature<T>(descriptor): empty for every descriptor
    //    shape, for both registered and unregistered T, with no JVM.  It
    //    iterates get_class_methods<T>() (empty) so the result is always empty
    //    regardless of the descriptor; never throws.
    // ---------------------------------------------------------------------
    {
        const char* const descriptors[]{
            "",                              // empty descriptor
            "()V",                           // no-arg void
            "(I)I",                          // int -> int
            "()Ljava/util/Collection;",      // ref return
            "(Ljava/lang/String;)V",         // ref arg
            "([B)[B",                        // array arg + return
            "garbage-not-a-descriptor",      // malformed
        };
        bool reg_all_empty{ true };
        bool unreg_all_empty{ true };
        for (const char* d : descriptors)
        {
            if (!vmhook::find_methods_by_signature<reg_w>(d).empty())   { reg_all_empty = false; }
            if (!vmhook::find_methods_by_signature<unreg_w>(d).empty()) { unreg_all_empty = false; }
        }
        check("find_methods_by_signature_registered_all_empty", reg_all_empty);
        check("find_methods_by_signature_unregistered_all_empty", unreg_all_empty);

        // Return type is exactly vector<string> (the names channel).
        using names_t = decltype(vmhook::find_methods_by_signature<reg_w>(""));
        static_assert(std::is_same_v<names_t, std::vector<std::string>>,
                      "find_methods_by_signature<T>() must return vector<string>");
        check("find_methods_by_signature_return_type_is_string_vector", true);
    }

    // ---------------------------------------------------------------------
    // 10. Cross-check: a name that resolves to null in find_class produces an
    //     empty method list through the by-name overload — the two contracts
    //     are consistent (collect_klass_methods is fed exactly find_class's
    //     null result).
    // ---------------------------------------------------------------------
    {
        const char* const name{ "consistency/Probe/Klass" };
        const bool klass_null{ vmhook::find_class(name) == nullptr };
        const bool methods_empty{ vmhook::get_class_methods(name).empty() };
        check("find_class_null_implies_methods_empty",
              klass_null && methods_empty);
    }

    if (failures == 0)
    {
        std::printf("vmhook find_class contracts: OK\n");
    }
    else
    {
        std::printf("vmhook find_class contracts: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
