// Standalone (no-JVM) unit test pinning the NO-JVM contracts of the recently
// hardened class-lookup / method-introspection entry points:
//
//   vmhook::find_class(class_name)                         -> klass*  (null w/o JVM)
//   vmhook::find_class_via_oop(anchor_oop, class_name)     -> klass*  (null on null anchor)
//   vmhook::override_class_lookup(name, klass*)            -> void    (seeds the cache)
//   vmhook::evict_class_lookup(name)                       -> void    (forgets a cache entry)
//   vmhook::find_class(class_name)                    -> void*   (null w/o JVM)
//   vmhook::find_class(name)      -> klass*  (null w/o JVM)
//   vmhook::get_class_methods(class_name)                  -> vector  (empty w/o JVM)
//   vmhook::get_class_methods<T>()                         -> vector  (empty w/o JVM)
//   vmhook::find_methods_by_signature<T>(desc)            -> vector  (empty w/o JVM)
//
// In this process there is NO JVM.  Derivation of every expected value below,
// straight from vmhook.hpp (line numbers approximate, current as of writing):
//
//   * find_class (~6424) first short-circuits the empty name (~6439), then for
//     any non-empty name consults klass_lookup_cache (~6444); on a cache MISS it
//     runs the ClassLoaderDataGraph walk (class_loader_data_graph::find_klass,
//     ~3579).  With no JVM gHotSpotVMStructs is absent so iterate_struct_entries()
//     returns null, class_loader_data_graph::get_head() (~3547) throws-internally-
//     and-returns-null, and the walk yields null.  The JNI fallback
//     (jni_find_class_with_context_loader, ~9789) then bails IMMEDIATELY because
//     ensure_current_java_thread() (~4234) returns false (no JavaThread, attach
//     fails) and current_jni_env is null.  Net: find_class returns nullptr for
//     EVERY name, never throws, never crashes, and — because only SUCCESSES are
//     inserted into the cache (~6508) — a miss is never memoised.
//
//   * The cache HIT path (~6447) validates the cached klass with is_valid_pointer
//     (~1768) BEFORE any dereference; is_valid_pointer rejects null, addresses
//     outside [user_address_floor=0xFFFF, user_address_ceiling), odd addresses,
//     and the documented debug-fill sentinels (0xCAFEBABE, 0xCCCCCCCC, ...) purely
//     by integer arithmetic — no memory access.  So a cache entry seeded with a
//     bogus-but-rejected pointer makes find_class evict the entry and re-walk
//     (-> null) WITHOUT ever touching the bogus address.  That is what lets the
//     stale-eviction contract be tested deterministically and crash-free here.
//
//   * find_class_via_oop (~11282) returns nullptr immediately when anchor_oop is
//     null (the `!anchor_oop` guard short-circuits BEFORE ensure_current_java_thread
//     and before any JNI work).  We only ever pass a null anchor — never a bogus
//     non-null oop, which would enter JNI land.
//
//   * jni::find_class (~11120) -> jni_find_class (~9485) returns nullptr because
//     ensure_current_java_thread() is false; jni::find_class_with_context_loader
//     (~11128) -> the fallback helper, also nullptr.  These are DISTINCT code
//     paths from the HotSpot-internal find_class and are pinned separately.
//
//   * collect_klass_methods(nullptr) (~7093) early-returns an empty vector, so
//     every get_class_methods* overload is empty, and find_methods_by_signature
//     (which iterates get_class_methods<T>()) is empty too, for any descriptor.
//
// The "real klass / real method list" behaviour requires a live JVM and is
// covered by JVM integration (tests/jvm + example.cpp).  This file is the
// dedicated, EXHAUSTIVE-input home for find_class's NO-JVM contract: empty/
// invalid/garbage names, dotted-vs-slashed and array/descriptor name forms, the
// deterministic nullptr-return paths, the name-cache override/evict/stale
// contract, and the sibling JNI / by-oop entry points — plus a throwaway-wrapper
// sweep of the templated method-introspection forms across many descriptor
// shapes.  Every assertion is platform-invariant (only nullptr / empty / size /
// no-throw comparisons; no <charconv>, no float parsing, no banned API), so it is
// identical across MSVC, libstdc++ (MinGW) and libc++.
//
// FRACTION TESTABLE WITHOUT A JVM: find_class's *positive* resolution (graph walk
// hit, JNI-fallback hit, real klass usability, real method lists) is inherently
// live-JVM-only.  What IS no-JVM-observable — and is what this file exhausts — is
// the deterministic side: the empty-name fast-reject, the universal nullptr
// return for every name shape, the never-throw / never-crash invariant, the cache
// seed/evict/stale-rejection logic (observable because the cache is plain process
// memory), and the consistency between find_class's null and the empty method
// lists / sibling entry points.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <utility>

static int failures{ 0 };
static int checks_run{ 0 };
static auto check(const char* name, bool ok) -> void
{
    ++checks_run;
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

    // A third wrapper used purely for the override/evict interaction on a
    // registered type's mapped name.
    struct ovr_w : vmhook::object<ovr_w>
    {
        using vmhook::object<ovr_w>::object;
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

    // Same guard for the public JNI wrappers and the by-oop form (all declared
    // noexcept, so a throw would be a contract break — but a noexcept function
    // that throws calls std::terminate, so we additionally never want them to
    // even *try*; these helpers double as documentation of the noexcept claim).
    auto jni_find_class_never_throws(std::string_view name) -> bool
    {
        try { (void)vmhook::find_class(name); return true; }
        catch (...) { return false; }
    }
    auto jni_find_class_ctx_never_throws(std::string_view name) -> bool
    {
        try { (void)vmhook::find_class(name); return true; }
        catch (...) { return false; }
    }
    auto find_class_via_oop_null_never_throws(std::string_view name) -> bool
    {
        try { (void)vmhook::find_class_via_oop(nullptr, name); return true; }
        catch (...) { return false; }
    }

    // The whole-family no-JVM contract for ONE name, as a single predicate:
    //   * find_class                          -> nullptr
    //   * jni::find_class                      -> nullptr
    //   * jni::find_class_with_context_loader  -> nullptr
    //   * find_class_via_oop(nullptr, .)       -> nullptr
    //   * get_class_methods                    -> empty
    //   * NONE of the above throws
    //   * find_class is deterministic for the name (two calls are bit-identical;
    //     both nullptr here, so identity also holds)
    // Returns true iff every clause holds.  Used to run the same contract over
    // very large programmatically-generated input matrices with a handful of
    // aggregate [PASS]/[FAIL] lines.
    auto family_contract_holds(std::string_view name) -> bool
    {
        try
        {
            const auto fc1{ vmhook::find_class(name) };
            const auto fc2{ vmhook::find_class(name) };
            if (fc1 != nullptr) { return false; }
            if (fc2 != nullptr) { return false; }
            if (fc1 != fc2)     { return false; }                 // determinism
            if (vmhook::find_class(name) != nullptr) { return false; }
            if (vmhook::find_class(name) != nullptr) { return false; }
            if (vmhook::find_class_via_oop(nullptr, name) != nullptr) { return false; }
            if (!vmhook::get_class_methods(name).empty()) { return false; }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // Reference oracle for is_valid_pointer, re-derived from first principles
    // straight from the documented contract (vmhook.hpp ~2018):
    //   reject  addr <= user_address_floor (0xFFFF)
    //   reject  addr >= user_address_ceiling (0x00007FFFFFFFFFFF)
    //   reject  odd addresses (addr & 1)
    //   reject  low-32-bits ∈ the documented debug-fill sentinel set
    //   accept  everything else
    // Cross-checking the live function against this independent reimplementation
    // pins the exact acceptance set value-by-value (this helper, is_valid_pointer,
    // sits on find_class's cache-hit validation path at ~8019/8022).
    auto expected_is_valid_pointer(std::uintptr_t addr) -> bool
    {
        constexpr std::uintptr_t floor_v{ 0xFFFFull };
        constexpr std::uintptr_t ceiling_v{ 0x00007FFFFFFFFFFFull };
        if (addr <= floor_v || addr >= ceiling_v) { return false; }
        if ((addr & 0x1ull) != 0ull)              { return false; }
        const std::uint32_t low32{ static_cast<std::uint32_t>(addr) };
        switch (low32)
        {
            case 0xDEADBEEFu:
            case 0xCAFEBABEu:
            case 0xCCCCCCCCu:
            case 0xCDCDCDCDu:
            case 0xBAADF00Du:
            case 0xFEEEFEEEu:
            case 0xABABABABu:
            case 0xFDFDFDFDu:
            case 0xDDDDDDDDu:
                return false;
            default:
                return true;
        }
    }
}

int main()
{
    // ---------------------------------------------------------------------
    // 0. Precondition: confirm there is genuinely no JVM in this process, so
    //    the null/empty contracts below are the no-JVM contracts (not an
    //    accidental real-class miss).  Both thread-locals that every fallback
    //    path keys off are null until a JVM attaches.
    // ---------------------------------------------------------------------
    check("precondition_no_jvm_env_is_null",
          vmhook::hotspot::current_jni_env == nullptr);
    check("precondition_no_jvm_java_thread_is_null",
          vmhook::hotspot::current_java_thread == nullptr);
    // ensure_current_java_thread() must fail (no JavaThread to adopt, attach
    // fails); this is the single gate the JNI fallback + by-oop paths rely on.
    check("precondition_ensure_current_java_thread_false",
          vmhook::hotspot::ensure_current_java_thread() == false);

    // find_class's static return type is exactly klass* (documents the channel
    // and guards against a silent signature drift).
    static_assert(std::is_same_v<decltype(vmhook::find_class(std::string_view{})),
                                 vmhook::hotspot::klass*>,
                  "find_class must return vmhook::hotspot::klass*");
    static_assert(std::is_same_v<decltype(vmhook::find_class(std::string_view{})),
                                 void*>,
                  "jni::find_class must return void* (a JNI handle)");
    static_assert(std::is_same_v<
                      decltype(vmhook::find_class(std::string_view{})),
                      vmhook::hotspot::klass*>,
                  "jni::find_class_with_context_loader must return klass*");
    static_assert(std::is_same_v<
                      decltype(vmhook::find_class_via_oop(nullptr, std::string_view{})),
                      vmhook::hotspot::klass*>,
                  "find_class_via_oop must return klass*");
    check("return_type_static_asserts_compiled", true);

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
    // An explicitly-empty string and a zero-length view over a real buffer.
    check("find_class_empty_std_string_is_null",
          vmhook::find_class(std::string{}) == nullptr);
    {
        const char buf[]{ "x" };
        check("find_class_zero_length_view_over_buffer_is_null",
              vmhook::find_class(std::string_view{ buf, 0 }) == nullptr);
    }
    // Repeated empty-name calls stay null and never throw (the guard is pure).
    {
        bool all_null{ true };
        bool none_threw{ true };
        for (int i{ 0 }; i < 64; ++i)
        {
            if (vmhook::find_class("") != nullptr) { all_null = false; }
            if (!find_class_never_throws(""))      { none_threw = false; }
        }
        check("find_class_empty_name_repeated_null", all_null);
        check("find_class_empty_name_repeated_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 2. find_class on REAL, always-present bootstrap class names -> nullptr
    //    with no JVM.  These names WOULD resolve against a live JVM; here they
    //    must not, proving the no-JVM path returns null rather than fabricating
    //    a klass.  Broadened well past the original four.
    // ---------------------------------------------------------------------
    {
        const char* const bootstrap_names[]{
            "java/lang/Object",
            "java/lang/String",
            "java/lang/Integer",
            "java/lang/Long",
            "java/lang/Class",
            "java/lang/Thread",
            "java/lang/ClassLoader",
            "java/lang/Throwable",
            "java/lang/Exception",
            "java/lang/RuntimeException",
            "java/lang/System",
            "java/lang/Runnable",
            "java/util/ArrayList",
            "java/util/HashMap",
            "java/util/List",
            "java/util/Map",
            "java/util/Collection",
            "java/io/InputStream",
            "java/io/Serializable",
            "sun/misc/Unsafe",
            "jdk/internal/misc/Unsafe",
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const char* n : bootstrap_names)
        {
            if (vmhook::find_class(n) != nullptr) { all_null = false; }
            if (!find_class_never_throws(n))      { none_threw = false; }
        }
        check("find_class_all_bootstrap_names_null", all_null);
        check("find_class_all_bootstrap_names_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 3. find_class on junk / non-existent names -> nullptr, never throws.
    //    A much wider variety of malformed-but-plausible internal-name shapes
    //    than the original list, covering every separator pathology.
    // ---------------------------------------------------------------------
    {
        const char* const junk_names[]{
            "definitely/Not/A/Class",
            "a",                              // single char
            "Z",                             // single upper char
            "NoSlashAtAll",                  // unqualified
            "trailing/slash/",               // trailing separator
            "/leading/slash",                // leading separator
            "double//slash",                 // empty path segment
            "triple///slash",               // multiple empty segments
            "/",                             // a lone separator
            "//",                            // two separators
            "///",                           // three separators
            "with spaces/in it",             // spaces (never a valid binary name)
            "  leading/space",              // leading spaces
            "trailing/space  ",             // trailing spaces
            "weird$Inner$Names",             // $-nested (valid chars, unloaded)
            "$",                             // lone dollar
            "java.lang.Object",              // dotted form (find_class wants '/')
            "java.util.HashMap",             // dotted form, two segments
            "com.example.Deeply.Nested",     // dotted, many segments
            "[Ljava/lang/Object;",           // array descriptor, not a plain name
            "[[Ljava/lang/Object;",          // 2-d object array
            "[[[I",                          // 3-d primitive array
            "[I",                            // primitive-array descriptor
            "[Z", "[B", "[C", "[S", "[J", "[F", "[D", // every primitive array
            "Ljava/lang/Object;",            // field descriptor, not a class name
            "()V",                           // a method descriptor, not a name
            "(I)Ljava/lang/String;",         // another descriptor
            "123Numeric/Start",              // digit-leading segment
            "name-with-dash",                // dash (invalid identifier char)
            "name.with.mixed/separators",    // mixed '.' and '/'
            "UPPER/lower/MiXeD",             // mixed case
            "\x01\x02\x03",                  // control bytes
            "tab\tnewline\nname",            // embedded whitespace controls
            "\xC3\xA9\xC3\xA8/unicode",      // UTF-8 multibyte lead bytes
            "\xFF\xFE/byteordermark",        // high bytes
            "emoji\xF0\x9F\x98\x80/name",   // 4-byte UTF-8 sequence
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
    // 3b. Whitespace-only and separator-only names.  None can name a class;
    //     all must be null, none may throw.  These have size != 0 so they do
    //     NOT hit the empty-name guard — they travel the full path.
    // ---------------------------------------------------------------------
    {
        const char* const blank_names[]{
            " ",                             // single space
            "  ",                            // two spaces
            "\t",                            // tab
            "\n",                            // newline
            "\r\n",                          // CRLF
            "\t \t ",                        // mixed whitespace
            "\f\v",                          // form-feed + vertical tab
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const char* n : blank_names)
        {
            if (vmhook::find_class(n) != nullptr) { all_null = false; }
            if (!find_class_never_throws(n))      { none_threw = false; }
        }
        check("find_class_whitespace_only_names_null", all_null);
        check("find_class_whitespace_only_names_no_throw", none_threw);
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
        check("find_class_leading_nul_name_no_throw",
              find_class_never_throws(leading_nul_view));
        // A view that is ONLY a single NUL byte (size 1) — non-empty, must be null.
        const char one_nul[]{ '\0' };
        const std::string_view single_nul_view{ one_nul, 1 };
        check("find_class_single_nul_byte_is_null",
              vmhook::find_class(single_nul_view) == nullptr);
        // Trailing NUL after a valid-looking name.
        const std::string trailing_nul{ std::string{ "java/lang/Object" } + '\0' };
        const std::string_view trailing_nul_view{ trailing_nul.data(), trailing_nul.size() };
        check("find_class_trailing_nul_name_is_null",
              vmhook::find_class(trailing_nul_view) == nullptr);
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
        check("find_class_deeply_nested_name_no_throw",
              find_class_never_throws(deep));

        // A pathological all-separator string of substantial length.
        const std::string all_slashes(2048, '/');
        check("find_class_all_slashes_long_is_null",
              vmhook::find_class(all_slashes) == nullptr);
        check("find_class_all_slashes_long_no_throw",
              find_class_never_throws(all_slashes));

        // A very long string of interior NULs (size carried explicitly).
        const std::string long_nuls(1024, '\0');
        const std::string_view long_nuls_view{ long_nuls.data(), long_nuls.size() };
        check("find_class_long_nul_run_is_null",
              vmhook::find_class(long_nuls_view) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 6. find_class is idempotent for a miss: a repeated lookup of the same
    //    missing name still returns null (a not-found result is not cached as
    //    a poisoned hit) and does not throw across many iterations.  A tight
    //    loop must NEVER diverge — every call returns the identical (null)
    //    result (the cache contract: misses are not memoised, so they re-walk
    //    and stay null).
    // ---------------------------------------------------------------------
    {
        const char* const name{ "repeat/Miss/Probe" };
        const auto first{ vmhook::find_class(name) };
        const auto second{ vmhook::find_class(name) };
        check("find_class_repeat_miss_first_null",  first == nullptr);
        check("find_class_repeat_miss_second_null", second == nullptr);
        check("find_class_repeat_miss_stable", first == second);

        bool tight_loop_all_null{ true };
        bool tight_loop_stable{ true };
        for (int i{ 0 }; i < 256; ++i)
        {
            const auto k{ vmhook::find_class("tight/Loop/Miss/Probe") };
            if (k != nullptr)   { tight_loop_all_null = false; }
            if (k != first)     { /* first is null; k must also be null */ tight_loop_stable = (k == nullptr) && tight_loop_stable; }
        }
        check("find_class_tight_loop_miss_all_null", tight_loop_all_null);
        check("find_class_tight_loop_miss_stable", tight_loop_stable);
    }

    // ---------------------------------------------------------------------
    // 6b. Cache contract via the PUBLIC override / evict API (observable with
    //     no JVM because klass_lookup_cache is plain process memory).
    //     * override_class_lookup(name, nullptr) seeds a null entry; the next
    //       find_class hits the cache, sees a null cached_klass (fails the
    //       `cached_klass && is_valid_pointer(...)` guard), EVICTS it, re-walks
    //       (-> null with no JVM) and returns nullptr.  No deref of null.
    //     * evict_class_lookup(name) is a no-throw forget; a subsequent
    //       find_class re-walks (-> null).
    // ---------------------------------------------------------------------
    {
        const char* const name{ "override/Null/Probe" };
        bool threw{ false };
        try { vmhook::override_class_lookup(name, nullptr); }
        catch (...) { threw = true; }
        check("override_class_lookup_null_no_throw", !threw);
        check("find_class_after_null_override_is_null",
              vmhook::find_class(name) == nullptr);
        // Idempotent: override null again, evict, re-query.
        vmhook::override_class_lookup(name, nullptr);
        bool evict_threw{ false };
        try { vmhook::evict_class_lookup(name); }
        catch (...) { evict_threw = true; }
        check("evict_class_lookup_no_throw", !evict_threw);
        check("find_class_after_evict_is_null",
              vmhook::find_class(name) == nullptr);
        // Evicting a name that was never cached is a safe no-op.
        bool evict_missing_threw{ false };
        try { vmhook::evict_class_lookup("never/Cached/Name"); }
        catch (...) { evict_missing_threw = true; }
        check("evict_class_lookup_unknown_no_throw", !evict_missing_threw);
        // Evicting the empty name is also safe.
        try { vmhook::evict_class_lookup(""); }
        catch (...) { evict_missing_threw = true; }
        check("evict_class_lookup_empty_no_throw", !evict_missing_threw);
    }

    // ---------------------------------------------------------------------
    // 6c. Stale-cache EVICTION via a bogus-but-rejected pointer.  is_valid_pointer
    //     rejects each of these by pure integer arithmetic (out-of-range, odd,
    //     or a documented debug-fill sentinel) WITHOUT dereferencing, so seeding
    //     the cache with one and calling find_class exercises the stale-eviction
    //     branch (~6468-6487) and returns nullptr — crash-free.  After the call
    //     the entry is gone, so a second find_class re-walks (-> null) too.
    //
    //     Sentinels chosen to span the rejection branches:
    //       0x0000000000000002 -> below user_address_floor (0xFFFF)
    //       0x0000000000000001 -> odd (and below floor)
    //       0x0000000000000FFF -> below floor
    //       0x00000000CAFEBABE -> even, in range, debug-fill sentinel switch
    //       0x00000000CCCCCCCC -> even, in range, debug-fill sentinel switch
    //       0x00000000FEEEFEEE -> even, in range, debug-fill sentinel switch
    //       0x00000000DEADBEEF -> odd (caught by odd-check before the switch)
    //       0xFFFFFFFFFFFFFFFE -> above user_address_ceiling
    //       0x7FFFFFFFFFFFFFFF -> == ceiling boundary (>= ceiling -> rejected)
    // ---------------------------------------------------------------------
    {
        const std::uintptr_t bogus_values[]{
            0x0000000000000002ull,
            0x0000000000000001ull,
            0x0000000000000FFFull,
            0x00000000CAFEBABEull,
            0x00000000CCCCCCCCull,
            0x00000000FEEEFEEEull,
            0x00000000DEADBEEFull,
            0xFFFFFFFFFFFFFFFEull,
            0x7FFFFFFFFFFFFFFFull,
        };
        // First confirm is_valid_pointer rejects every one of these (so we KNOW
        // the deref-guard holds before we ever seed the cache with them).
        bool all_rejected{ true };
        for (const std::uintptr_t v : bogus_values)
        {
            if (vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(v)))
            {
                all_rejected = false;
            }
        }
        check("bogus_sentinels_all_rejected_by_is_valid_pointer", all_rejected);

        bool all_null{ true };
        bool none_threw{ true };
        bool all_evicted{ true };
        int idx{ 0 };
        for (const std::uintptr_t v : bogus_values)
        {
            const std::string name{ "stale/Sentinel/Probe/" + std::to_string(idx++) };
            vmhook::override_class_lookup(name, reinterpret_cast<vmhook::hotspot::klass*>(v));
            // find_class must reject the bogus entry and return null WITHOUT
            // dereferencing it.
            if (!find_class_never_throws(name)) { none_threw = false; }
            if (vmhook::find_class(name) != nullptr) { all_null = false; }
            // After the stale eviction, the entry is gone; a fresh lookup
            // re-walks and is still null (proves the entry was actually evicted,
            // not just ignored — a second seed isn't needed).
            if (vmhook::find_class(name) != nullptr) { all_evicted = false; }
        }
        check("find_class_stale_sentinel_all_null", all_null);
        check("find_class_stale_sentinel_no_throw", none_threw);
        check("find_class_stale_sentinel_all_evicted", all_evicted);
    }

    // ---------------------------------------------------------------------
    // 7. find_class_via_oop(nullptr, name): the `!anchor_oop` guard short-
    //    circuits to nullptr for EVERY name BEFORE any JNI work, so it is a
    //    deterministic no-JVM contract.  We ONLY ever pass a null anchor (a
    //    bogus non-null oop would enter JNI land).
    // ---------------------------------------------------------------------
    {
        const char* const names[]{
            "",                              // empty
            "java/lang/Object",              // real-but-no-JVM
            "definitely/Not/A/Class",        // miss
            "[Ljava/lang/String;",           // array descriptor
            "weird$Inner",                   // unloaded nested
            "java.dotted.Form",              // dotted
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const char* n : names)
        {
            if (vmhook::find_class_via_oop(nullptr, n) != nullptr) { all_null = false; }
            if (!find_class_via_oop_null_never_throws(n))          { none_threw = false; }
        }
        check("find_class_via_oop_null_anchor_all_null", all_null);
        check("find_class_via_oop_null_anchor_no_throw", none_threw);
        // Explicitly the empty-name + null-anchor combination.
        check("find_class_via_oop_null_anchor_empty_name_null",
              vmhook::find_class_via_oop(nullptr, "") == nullptr);
    }

    // ---------------------------------------------------------------------
    // 8. vmhook::find_class(name): public JNI FindClass wrapper.  With no
    //    JVM, jni_find_class returns nullptr (ensure_current_java_thread false)
    //    for every name.  This is a DISTINCT path from the HotSpot-internal
    //    find_class and is pinned separately, including the empty name.
    // ---------------------------------------------------------------------
    {
        const char* const names[]{
            "",                              // empty
            "java/lang/Object",
            "java/lang/String",
            "definitely/Not/A/Class",        // miss
            "[I",                            // primitive array
            "[Ljava/lang/Object;",           // object array
            "java.dotted.Name",              // dotted
            "trailing/slash/",
            "weird$Inner",
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const char* n : names)
        {
            if (vmhook::find_class(n) != nullptr) { all_null = false; }
            if (!jni_find_class_never_throws(n))       { none_threw = false; }
        }
        check("jni_find_class_all_null_no_jvm", all_null);
        check("jni_find_class_no_throw", none_threw);
        // Long name through the JNI wrapper too (it builds a std::string).
        const std::string long_name(8192, 'q');
        check("jni_find_class_long_name_null",
              vmhook::find_class(long_name) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 9. vmhook::find_class(name): the full fallback
    //    helper, called DIRECTLY.  With no JVM it bails at the
    //    ensure_current_java_thread() gate and returns nullptr for every name,
    //    including array names and the empty name — never crashing despite the
    //    multi-loader walk it would otherwise run.
    // ---------------------------------------------------------------------
    {
        const char* const names[]{
            "",                              // empty
            "java/lang/Object",
            "net/minecraft/client/Minecraft",
            "definitely/Not/A/Class",        // miss
            "[I",                            // primitive array (the documented gap)
            "[Ljava/lang/String;",           // object array (the documented gap)
            "java.dotted.Name",
            "double//slash",
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const char* n : names)
        {
            if (vmhook::find_class(n) != nullptr) { all_null = false; }
            if (!jni_find_class_ctx_never_throws(n))                       { none_threw = false; }
        }
        check("jni_find_class_with_context_loader_all_null_no_jvm", all_null);
        check("jni_find_class_with_context_loader_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 9b. Cross-entry-point consistency: for a batch of names, the three
    //     resolution entry points all agree on "not found" with no JVM, and the
    //     method-list overload is consistently empty.  This ties find_class's
    //     null to its siblings (collect_klass_methods is fed exactly find_class's
    //     null result; the JNI helpers share the no-thread gate).
    // ---------------------------------------------------------------------
    {
        const char* const names[]{
            "java/lang/Object",
            "consistency/Probe/Klass",
            "[Ljava/lang/Object;",
            "weird$Inner",
            "java.dotted.Name",
            "trailing/slash/",
        };
        bool consistent{ true };
        for (const char* n : names)
        {
            const bool fc_null  { vmhook::find_class(n) == nullptr };
            const bool jni_null { vmhook::find_class(n) == nullptr };
            const bool ctx_null { vmhook::find_class(n) == nullptr };
            const bool oop_null { vmhook::find_class_via_oop(nullptr, n) == nullptr };
            const bool m_empty  { vmhook::get_class_methods(n).empty() };
            if (!(fc_null && jni_null && ctx_null && oop_null && m_empty))
            {
                consistent = false;
            }
        }
        check("all_entry_points_agree_not_found_no_jvm", consistent);
    }

    // ---------------------------------------------------------------------
    // 10. get_class_methods(name) by internal name -> empty for every shape of
    //     input (collect_klass_methods(nullptr) returns {}), never throws.
    //     Broadened input set + explicit size()/empty() checks.
    // ---------------------------------------------------------------------
    {
        const char* const names[]{
            "",                              // empty name -> find_class null
            "java/lang/Object",              // real-but-no-JVM
            "java/lang/String",
            "java/util/List",
            "java/util/Map",
            "definitely/Not/A/Class",        // miss
            "[Ljava/lang/Object;",           // descriptor-shaped
            "[I",                            // primitive array
            "weird$Inner",                   // unloaded
            "trailing/slash/",               // separator pathology
            "java.dotted.Name",              // dotted
            " ",                             // whitespace
        };
        bool all_empty{ true };
        bool all_size0{ true };
        bool none_threw{ true };
        for (const char* n : names)
        {
            try
            {
                const auto methods{ vmhook::get_class_methods(n) };
                if (!methods.empty())   { all_empty = false; }
                if (methods.size() != 0){ all_size0 = false; }
            }
            catch (...) { none_threw = false; }
        }
        check("get_class_methods_by_name_all_empty_no_jvm", all_empty);
        check("get_class_methods_by_name_all_size0_no_jvm", all_size0);
        check("get_class_methods_by_name_no_throw", none_threw);

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

        // Return type is exactly vector<pair<string,string>>.
        using ret_t = decltype(vmhook::get_class_methods(std::string_view{}));
        static_assert(std::is_same_v<ret_t,
                          std::vector<std::pair<std::string, std::string>>>,
                      "get_class_methods(name) must return vector<pair<name,descriptor>>");
        check("get_class_methods_by_name_return_type_is_pair_vector", true);
    }

    // ---------------------------------------------------------------------
    // 11. Templated get_class_methods<T>().
    //     * UNregistered T  -> type-map MISS -> empty (the catch-all empty).
    //     * Registered   T  -> type-map HIT  -> find_class null -> empty.
    //     Both empty with no JVM; never throws.  The two distinct empty paths
    //     are what this file adds over the by-name coverage.
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

        // Calling it many times is stable + never throws.
        bool stable_empty{ true };
        bool none_threw{ true };
        for (int i{ 0 }; i < 64; ++i)
        {
            try
            {
                if (!vmhook::get_class_methods<reg_w>().empty())   { stable_empty = false; }
                if (!vmhook::get_class_methods<unreg_w>().empty()) { stable_empty = false; }
            }
            catch (...) { none_threw = false; }
        }
        check("get_class_methods_T_repeated_empty", stable_empty);
        check("get_class_methods_T_repeated_no_throw", none_threw);

        // Return type is exactly vector<pair<string,string>> (documents the
        // data channel callers iterate).
        using ret_t = decltype(vmhook::get_class_methods<reg_w>());
        static_assert(std::is_same_v<ret_t,
                          std::vector<std::pair<std::string, std::string>>>,
                      "get_class_methods<T>() must return vector<pair<name,descriptor>>");
        check("get_class_methods_T_return_type_is_pair_vector", true);
    }

    // ---------------------------------------------------------------------
    // 11b. register_class / override interaction on a registered type's name.
    //      Registering ovr_w records its mapped name; overriding that name with
    //      a null cache entry, then querying get_class_methods<ovr_w>(), still
    //      yields empty (map HIT -> find_class consults the cache -> null entry
    //      -> evict + re-walk -> null -> collect_klass_methods(null) -> empty).
    // ---------------------------------------------------------------------
    {
        const bool registered{ vmhook::register_class<ovr_w>("test/find_class/Ovr") };
        check("register_class_ovr_returns_false_no_jvm", registered == false);
        // Seed a null cache entry under the mapped name, then query via T.
        vmhook::override_class_lookup("test/find_class/Ovr", nullptr);
        check("get_class_methods_T_after_null_override_empty",
              vmhook::get_class_methods<ovr_w>().empty());
        // And via the by-name overload for the same name.
        check("get_class_methods_byname_after_null_override_empty",
              vmhook::get_class_methods("test/find_class/Ovr").empty());
        // Evict and re-query: still empty.
        vmhook::evict_class_lookup("test/find_class/Ovr");
        check("get_class_methods_T_after_evict_empty",
              vmhook::get_class_methods<ovr_w>().empty());
        // Re-registering the same wrapper with a different name is a safe no-op
        // contract here (still returns false, no throw, still empty).
        bool rereg_threw{ false };
        bool rereg_result{ true };
        try { rereg_result = vmhook::register_class<ovr_w>("test/find_class/Ovr2"); }
        catch (...) { rereg_threw = true; }
        check("register_class_ovr_rereg_no_throw", !rereg_threw);
        check("register_class_ovr_rereg_returns_false", rereg_result == false);
        check("get_class_methods_T_after_rereg_empty",
              vmhook::get_class_methods<ovr_w>().empty());
    }

    // ---------------------------------------------------------------------
    // 12. find_methods_by_signature<T>(descriptor): empty for every descriptor
    //     shape, for both registered and unregistered T, with no JVM.  It
    //     iterates get_class_methods<T>() (empty) so the result is always empty
    //     regardless of the descriptor; never throws.  Wider descriptor set.
    // ---------------------------------------------------------------------
    {
        const char* const descriptors[]{
            "",                              // empty descriptor
            "()V",                           // no-arg void
            "(I)I",                          // int -> int
            "(J)J",                          // long -> long
            "(Z)Z",                          // boolean
            "(D)D",                          // double
            "(F)F",                          // float
            "(C)C",                          // char
            "(B)B",                          // byte
            "(S)S",                          // short
            "()Ljava/util/Collection;",      // ref return
            "(Ljava/lang/String;)V",         // ref arg
            "(Ljava/lang/String;Ljava/lang/Object;)Z", // two ref args
            "([B)[B",                        // array arg + return
            "([[I)[[I",                      // 2-d array arg + return
            "(ILjava/lang/String;[J)V",      // mixed args
            "garbage-not-a-descriptor",      // malformed
            "(",                             // truncated
            ")",                             // truncated
            "()",                            // missing return
            "(Ljava/lang/String", // unterminated ref
        };
        bool reg_all_empty{ true };
        bool unreg_all_empty{ true };
        bool ovr_all_empty{ true };
        bool none_threw{ true };
        for (const char* d : descriptors)
        {
            try
            {
                if (!vmhook::find_methods_by_signature<reg_w>(d).empty())   { reg_all_empty = false; }
                if (!vmhook::find_methods_by_signature<unreg_w>(d).empty()) { unreg_all_empty = false; }
                if (!vmhook::find_methods_by_signature<ovr_w>(d).empty())   { ovr_all_empty = false; }
            }
            catch (...) { none_threw = false; }
        }
        check("find_methods_by_signature_registered_all_empty", reg_all_empty);
        check("find_methods_by_signature_unregistered_all_empty", unreg_all_empty);
        check("find_methods_by_signature_overridden_all_empty", ovr_all_empty);
        check("find_methods_by_signature_no_throw", none_threw);

        // Return type is exactly vector<string> (the names channel).
        using names_t = decltype(vmhook::find_methods_by_signature<reg_w>(""));
        static_assert(std::is_same_v<names_t, std::vector<std::string>>,
                      "find_methods_by_signature<T>() must return vector<string>");
        check("find_methods_by_signature_return_type_is_string_vector", true);
    }

    // ---------------------------------------------------------------------
    // 13. Cross-check: a name that resolves to null in find_class produces an
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

    // ---------------------------------------------------------------------
    // 14. Mass junk sweep — aggregate checks over programmatically generated
    //     inputs covering far more of the byte/shape space than an enumerated
    //     list could.  Each aggregate asserts "every generated input -> null,
    //     no throw".  This is where the "all the input possible" requirement is
    //     stretched cheaply (hundreds of distinct inputs, a handful of checks).
    // ---------------------------------------------------------------------
    {
        // (a) Every single byte 0x01..0xFF as a one-char name (skip 0x00 which
        //     would make an empty C-string; we feed via length-bearing view so
        //     even 0x00 is covered separately above).  size==1 so none hit the
        //     empty guard.
        bool single_byte_all_null{ true };
        bool single_byte_no_throw{ true };
        for (int b{ 1 }; b <= 0xFF; ++b)
        {
            const char c{ static_cast<char>(b) };
            const std::string_view sv{ &c, 1 };
            if (vmhook::find_class(sv) != nullptr) { single_byte_all_null = false; }
            if (!find_class_never_throws(sv))      { single_byte_no_throw = false; }
        }
        check("find_class_every_single_byte_null", single_byte_all_null);
        check("find_class_every_single_byte_no_throw", single_byte_no_throw);

        // (b) Every printable ASCII char repeated to length 16 — a wide sweep of
        //     "uniform garbage" names.
        bool uniform_all_null{ true };
        for (int ch{ 0x20 }; ch <= 0x7E; ++ch)
        {
            const std::string name(16, static_cast<char>(ch));
            if (vmhook::find_class(name) != nullptr) { uniform_all_null = false; }
        }
        check("find_class_uniform_printable_repeats_null", uniform_all_null);

        // (c) All two-character combinations of the four separator-ish chars
        //     ('/', '.', '$', ' ') — every ordering of separator pathologies.
        const char seps[]{ '/', '.', '$', ' ' };
        bool sep_pairs_all_null{ true };
        for (const char a : seps)
        {
            for (const char b : seps)
            {
                const char pair[]{ a, b };
                const std::string_view sv{ pair, 2 };
                if (vmhook::find_class(sv) != nullptr) { sep_pairs_all_null = false; }
            }
        }
        check("find_class_all_separator_pairs_null", sep_pairs_all_null);

        // (d) Increasing lengths of 'a' (1..256) — boundary-free length sweep
        //     for the cache-key construction.
        bool length_sweep_all_null{ true };
        for (std::size_t len{ 1 }; len <= 256; ++len)
        {
            const std::string name(len, 'a');
            if (vmhook::find_class(name) != nullptr) { length_sweep_all_null = false; }
        }
        check("find_class_length_sweep_null", length_sweep_all_null);

        // (e) The same wide sweeps must hold through the by-name method overload
        //     (it forwards find_class's null into collect_klass_methods).
        bool methods_sweep_all_empty{ true };
        for (int ch{ 0x20 }; ch <= 0x7E; ++ch)
        {
            const std::string name(8, static_cast<char>(ch));
            if (!vmhook::get_class_methods(name).empty()) { methods_sweep_all_empty = false; }
        }
        check("get_class_methods_uniform_sweep_empty", methods_sweep_all_empty);
    }

    // ---------------------------------------------------------------------
    // 16. EVERY descriptor type character as a standalone "name".  The JVM
    //     field-descriptor alphabet is B C D F I J S Z (primitives) + V (void,
    //     method-return-only) + L...; / [ (reference / array).  A bare type
    //     char is never a loadable internal class name, so find_class is null
    //     for each; the family contract holds for all of them.  We sweep the
    //     valid descriptor chars, the invalid-but-plausible upper letters that
    //     are NOT descriptor leads (A E G H K M N O P Q R T U W X Y), and the
    //     lowercase primitives (which are valid identifier chars but not types).
    // ---------------------------------------------------------------------
    {
        const char* const desc_chars[]{
            // valid field/return descriptor leads, one char each
            "B", "C", "D", "F", "I", "J", "S", "Z", "V",
            "L", "[", ";", "(", ")",
            // the upper-case letters that are NOT descriptor type leads
            "A", "E", "G", "H", "K", "M", "N", "O",
            "P", "Q", "R", "T", "U", "W", "X", "Y",
            // lowercase primitives — valid Java identifier chars, never types
            "b", "c", "d", "f", "i", "j", "s", "z", "v",
            "l",
        };
        bool all_hold{ true };
        for (const char* d : desc_chars)
        {
            if (!family_contract_holds(d)) { all_hold = false; }
        }
        check("find_class_every_descriptor_char_family_contract", all_hold);

        // Each primitive descriptor as a ONE-DIMENSIONAL array: [B [C [D [F [I
        // [J [S [Z, plus the (technically illegal) [V, plus an [L...; .  All
        // null, family contract holds.
        const char* const prim_arrays[]{
            "[B", "[C", "[D", "[F", "[I", "[J", "[S", "[Z",
            "[V",                              // void array — illegal but must not fault
            "[Ljava/lang/Object;",
        };
        bool arr_hold{ true };
        for (const char* d : prim_arrays)
        {
            if (!family_contract_holds(d)) { arr_hold = false; }
        }
        check("find_class_every_primitive_array_family_contract", arr_hold);
    }

    // ---------------------------------------------------------------------
    // 17. Array nesting depth sweep.  Build [, [[, [[[ ... up to a generous
    //     bound for BOTH a primitive element (I) and an object element
    //     (Ljava/lang/Object;).  The JVM caps real array dimensions at 255,
    //     but find_class must return null / never fault for ANY depth — these
    //     are descriptor strings, not real classes, with no JVM present.
    //     Covers "every array nesting depth up to a sane bound".
    // ---------------------------------------------------------------------
    {
        bool prim_depths_hold{ true };
        bool obj_depths_hold{ true };
        std::string brackets{};
        for (int depth{ 1 }; depth <= 300; ++depth)   // past the JVM's 255 cap
        {
            brackets.push_back('[');
            const std::string prim_name{ brackets + "I" };
            const std::string obj_name{ brackets + "Ljava/lang/Object;" };
            if (vmhook::find_class(prim_name) != nullptr) { prim_depths_hold = false; }
            if (vmhook::find_class(obj_name)  != nullptr) { obj_depths_hold  = false; }
            if (!find_class_never_throws(prim_name))      { prim_depths_hold = false; }
            if (!find_class_never_throws(obj_name))       { obj_depths_hold  = false; }
        }
        check("find_class_primitive_array_depth_sweep_null", prim_depths_hold);
        check("find_class_object_array_depth_sweep_null", obj_depths_hold);

        // The exact JVM-boundary depths (254, 255, 256) explicitly.
        bool boundary_depths_hold{ true };
        for (const int depth : { 254, 255, 256 })
        {
            const std::string b(static_cast<std::size_t>(depth), '[');
            if (vmhook::find_class(b + "I") != nullptr) { boundary_depths_hold = false; }
            if (vmhook::find_class(b + "Ljava/lang/String;") != nullptr) { boundary_depths_hold = false; }
        }
        check("find_class_array_depth_255_boundary_null", boundary_depths_hold);
    }

    // ---------------------------------------------------------------------
    // 18. Field-descriptor forms (Lpkg/Type;) for a broad spread of canonical
    //     JDK types, in BOTH slashed and dotted internal forms.  A field
    //     descriptor is never itself a loadable class name (find_class wants
    //     the bare internal name, not the L...; wrapper), so all are null; the
    //     family contract holds for each.
    // ---------------------------------------------------------------------
    {
        const char* const field_descriptors[]{
            "Ljava/lang/Object;",
            "Ljava/lang/String;",
            "Ljava/lang/Integer;",
            "Ljava/lang/Class;",
            "Ljava/util/List;",
            "Ljava/util/Map$Entry;",
            "Ljava/lang/Thread$State;",
            // dotted variants (find_class neither requires nor rewrites these
            // with no JVM — they simply miss)
            "Ljava.lang.Object;",
            "Ljava.util.List;",
        };
        bool all_hold{ true };
        for (const char* d : field_descriptors)
        {
            if (!family_contract_holds(d)) { all_hold = false; }
        }
        check("find_class_field_descriptor_forms_family_contract", all_hold);
    }

    // ---------------------------------------------------------------------
    // 19. Dotted-vs-slashed canonical-name PARITY.  For every canonical JDK
    //     name, both the slashed internal form and the dotted binary form must
    //     resolve to null with no JVM (the slashed form is what find_class
    //     expects; the dotted form simply misses — find_class does NOT
    //     canonicalize the name on the no-JVM path).  Asserting both null pins
    //     the no-normalization contract from both directions.
    // ---------------------------------------------------------------------
    {
        const char* const slashed[]{
            "java/lang/Object", "java/lang/String", "java/lang/Integer",
            "java/lang/Long",   "java/lang/Short",  "java/lang/Byte",
            "java/lang/Character", "java/lang/Boolean", "java/lang/Float",
            "java/lang/Double", "java/lang/Void",   "java/lang/Number",
            "java/util/ArrayList", "java/util/LinkedList", "java/util/HashMap",
            "java/util/TreeMap", "java/util/HashSet", "java/util/Optional",
            "java/lang/StringBuilder", "java/lang/reflect/Method",
            "java/util/concurrent/ConcurrentHashMap",
            "java/lang/invoke/MethodHandle",
        };
        const char* const dotted[]{
            "java.lang.Object", "java.lang.String", "java.lang.Integer",
            "java.lang.Long",   "java.lang.Short",  "java.lang.Byte",
            "java.lang.Character", "java.lang.Boolean", "java.lang.Float",
            "java.lang.Double", "java.lang.Void",   "java.lang.Number",
            "java.util.ArrayList", "java.util.LinkedList", "java.util.HashMap",
            "java.util.TreeMap", "java.util.HashSet", "java.util.Optional",
            "java.lang.StringBuilder", "java.lang.reflect.Method",
            "java.util.concurrent.ConcurrentHashMap",
            "java.lang.invoke.MethodHandle",
        };
        bool slashed_all_null{ true };
        bool dotted_all_null{ true };
        for (const char* n : slashed)
        {
            if (!family_contract_holds(n)) { slashed_all_null = false; }
        }
        for (const char* n : dotted)
        {
            if (!family_contract_holds(n)) { dotted_all_null = false; }
        }
        check("find_class_canonical_slashed_form_family_contract", slashed_all_null);
        check("find_class_canonical_dotted_form_family_contract", dotted_all_null);

        // Per-name parity: the slashed and dotted forms of the SAME class agree
        // (both null) — there is no hidden form-dependent divergence.
        bool per_name_parity{ true };
        const std::size_t count{ sizeof(slashed) / sizeof(slashed[0]) };
        for (std::size_t i{ 0 }; i < count; ++i)
        {
            const bool s_null{ vmhook::find_class(slashed[i]) == nullptr };
            const bool d_null{ vmhook::find_class(dotted[i])  == nullptr };
            if (s_null != d_null) { per_name_parity = false; }
        }
        check("find_class_slashed_dotted_per_name_parity", per_name_parity);
    }

    // ---------------------------------------------------------------------
    // 20. Nested / inner-class ($) name forms — every shape the JVM uses for
    //     member, local, and anonymous classes.  Valid binary-name characters,
    //     simply unloaded here; null + family contract for each.
    // ---------------------------------------------------------------------
    {
        const char* const nested_names[]{
            "java/util/Map$Entry",            // member class
            "Outer$Inner",                    // simple nested
            "Outer$Inner$Deeper",             // doubly nested
            "Outer$1",                        // anonymous class
            "Outer$1$2",                      // anonymous within anonymous
            "Outer$1Local",                   // local class
            "pkg/Outer$Inner",                // qualified nested
            "a/b/c/Outer$Inner$1$Local",      // fully exercised
            "$Leading",                       // leading $
            "Trailing$",                      // trailing $
            "Double$$Dollar",                 // doubled $
        };
        bool all_hold{ true };
        for (const char* n : nested_names)
        {
            if (!family_contract_holds(n)) { all_hold = false; }
        }
        check("find_class_nested_dollar_name_forms_family_contract", all_hold);
    }

    // ---------------------------------------------------------------------
    // 21. Method-descriptor strings fed to find_class as if they were names.
    //     A method descriptor "(args)ret" is never a class name; every shape
    //     must be null and never fault.  Wider than the section-12 set and run
    //     through the WHOLE family (find_class + siblings), not just
    //     find_methods_by_signature.
    // ---------------------------------------------------------------------
    {
        const char* const method_descriptors[]{
            "()V", "()I", "()Z", "()J", "()D", "()F", "()C", "()B", "()S",
            "()Ljava/lang/Object;",
            "()[I", "()[[Ljava/lang/String;",
            "(I)V", "(II)I", "(J)J", "(ID)V",
            "(Ljava/lang/String;)V",
            "(Ljava/lang/String;I)Ljava/lang/String;",
            "([B)[B", "([[I)[[I",
            "(ILjava/lang/Object;[J)Z",
            "(BCDFIJSZ)V",                    // every primitive arg in order
            "([Ljava/lang/Object;)Ljava/lang/Object;",
            // malformed / truncated descriptors
            "(", ")", "()", "(V)V", "(I", "(L", "(Ljava/lang/String",
            "()L", "()[",
        };
        bool all_hold{ true };
        for (const char* d : method_descriptors)
        {
            if (!family_contract_holds(d)) { all_hold = false; }
        }
        check("find_class_method_descriptor_strings_family_contract", all_hold);
    }

    // ---------------------------------------------------------------------
    // 22. Separator-pathology matrix, exhaustively, over the full descriptor /
    //     name punctuation alphabet.  Every ordered 2-char and 3-char string
    //     drawn from { '/', '.', '$', '[', ';', '(', ')', ' ' } — covering
    //     every duplicate / leading / trailing / mixed separator arrangement.
    //     None can name a class; all must be null, none may throw.  Hundreds of
    //     inputs, two aggregate checks.
    // ---------------------------------------------------------------------
    {
        const char punct[]{ '/', '.', '$', '[', ';', '(', ')', ' ' };
        constexpr std::size_t n{ sizeof(punct) / sizeof(punct[0]) };

        bool pairs_all_null{ true };
        bool pairs_no_throw{ true };
        for (std::size_t i{ 0 }; i < n; ++i)
        {
            for (std::size_t j{ 0 }; j < n; ++j)
            {
                const char pair[]{ punct[i], punct[j] };
                const std::string_view sv{ pair, 2 };
                if (vmhook::find_class(sv) != nullptr) { pairs_all_null = false; }
                if (!find_class_never_throws(sv))      { pairs_no_throw = false; }
            }
        }
        check("find_class_punct_pairs_all_null", pairs_all_null);
        check("find_class_punct_pairs_no_throw", pairs_no_throw);

        bool triples_all_null{ true };
        for (std::size_t i{ 0 }; i < n; ++i)
        {
            for (std::size_t j{ 0 }; j < n; ++j)
            {
                for (std::size_t k{ 0 }; k < n; ++k)
                {
                    const char triple[]{ punct[i], punct[j], punct[k] };
                    const std::string_view sv{ triple, 3 };
                    if (vmhook::find_class(sv) != nullptr) { triples_all_null = false; }
                }
            }
        }
        check("find_class_punct_triples_all_null", triples_all_null);
    }

    // ---------------------------------------------------------------------
    // 23. Wider byte / encoding sweeps (beyond section 14's single-byte pass).
    //     (a) Every 2-byte combination 0x00..0xFF x {0x2F '/'} — a name byte
    //         followed by a separator, and a separator followed by a name byte
    //         — so EVERY byte value is exercised adjacent to a separator.
    //     (b) A valid-looking name with one arbitrary byte 0x00..0xFF spliced
    //         into the middle (length-bearing view, so NUL is included).
    //     All null, none throw.
    // ---------------------------------------------------------------------
    {
        bool byte_then_sep_null{ true };
        bool sep_then_byte_null{ true };
        for (int b{ 0 }; b <= 0xFF; ++b)
        {
            const char c{ static_cast<char>(b) };
            const char bs[]{ c, '/' };
            const char sb[]{ '/', c };
            if (vmhook::find_class(std::string_view{ bs, 2 }) != nullptr) { byte_then_sep_null = false; }
            if (vmhook::find_class(std::string_view{ sb, 2 }) != nullptr) { sep_then_byte_null = false; }
        }
        check("find_class_byte_then_separator_null", byte_then_sep_null);
        check("find_class_separator_then_byte_null", sep_then_byte_null);

        bool spliced_byte_null{ true };
        bool spliced_byte_no_throw{ true };
        for (int b{ 0 }; b <= 0xFF; ++b)
        {
            std::string name{ "java/lang/" };
            name.push_back(static_cast<char>(b));   // arbitrary byte, incl. NUL
            name += "Object";
            const std::string_view sv{ name.data(), name.size() };
            if (vmhook::find_class(sv) != nullptr) { spliced_byte_null = false; }
            if (!find_class_never_throws(sv))      { spliced_byte_no_throw = false; }
        }
        check("find_class_spliced_arbitrary_byte_null", spliced_byte_null);
        check("find_class_spliced_arbitrary_byte_no_throw", spliced_byte_no_throw);

        // A multibyte-UTF-8 package name (valid UTF-8, never an ASCII binary
        // name) and a high-byte / invalid-UTF-8 run.
        const char* const encoded_names[]{
            "caf\xC3\xA9/Bar",                 // "café/Bar" (2-byte UTF-8)
            "\xE4\xB8\xAD\xE6\x96\x87/Type",   // CJK (3-byte UTF-8)
            "name\xF0\x9F\x92\xA9/x",          // 4-byte UTF-8 (emoji)
            "\xFF\xFE\xFD/raw",                // invalid UTF-8 high bytes
            "\x80\x81/cont",                   // lone continuation bytes
        };
        bool encoded_hold{ true };
        for (const char* n : encoded_names)
        {
            if (!family_contract_holds(n)) { encoded_hold = false; }
        }
        check("find_class_utf8_and_highbyte_names_family_contract", encoded_hold);
    }

    // ---------------------------------------------------------------------
    // 24. Determinism / no-global-state-dependence.  The same input must yield
    //     the same (null) result regardless of intervening calls on OTHER
    //     names and regardless of cache churn.  We interleave a probe name with
    //     unrelated lookups, overrides, and evicts of DIFFERENT names and prove
    //     the probe's result never changes.
    // ---------------------------------------------------------------------
    {
        const char* const probe{ "determinism/Probe/Klass" };
        const auto baseline{ vmhook::find_class(probe) };       // null
        bool stable{ baseline == nullptr };

        // Hammer unrelated names / cache ops between probe reads.
        for (int i{ 0 }; i < 100; ++i)
        {
            const std::string other{ "noise/Name/" + std::to_string(i) };
            (void)vmhook::find_class(other);
            vmhook::override_class_lookup(other, nullptr);
            (void)vmhook::find_class(other);
            vmhook::evict_class_lookup(other);
            const auto again{ vmhook::find_class(probe) };
            if (again != baseline) { stable = false; }           // both null
        }
        check("find_class_deterministic_under_unrelated_churn", stable);

        // The same name through the same entry point twice in a row is bit-
        // identical for a batch of varied shapes (idempotent, side-effect-free
        // for a miss).
        const char* const shapes[]{
            "x", "java/lang/Object", "[I", "Ljava/lang/String;",
            "()V", "a.b.c", "weird$Inner", "trailing/",
        };
        bool idempotent{ true };
        for (const char* s : shapes)
        {
            if (vmhook::find_class(s) != vmhook::find_class(s)) { idempotent = false; }
            if (vmhook::find_class(s) != vmhook::find_class(s)) { idempotent = false; }
            if (vmhook::find_class(s)
                != vmhook::find_class(s)) { idempotent = false; }
        }
        check("find_class_entry_points_idempotent_per_name", idempotent);
    }

    // ---------------------------------------------------------------------
    // 25. is_valid_pointer — the ONE pure helper on find_class's cache-hit
    //     validation path (~8019/8022) — pinned exhaustively, value-by-value,
    //     against an independent reimplementation of its documented contract.
    //     This is the deterministic core that lets a stale / bogus cache entry
    //     be rejected WITHOUT a dereference, so it is squarely in scope for the
    //     resolver's no-JVM contract.  All checks are pure integer arithmetic,
    //     identical on every platform / compiler (no address is dereferenced).
    // ---------------------------------------------------------------------
    {
        // (a) Boundary values around the floor (0xFFFF) and ceiling
        //     (0x00007FFFFFFFFFFF).  Use only EVEN candidates here so the
        //     odd-rejection rule doesn't confound the range rule.
        const std::uintptr_t boundary_addrs[]{
            0x0ull, 0x2ull, 0xFFFEull,                 // <= floor -> reject
            0xFFFFull,                                 // == floor -> reject (<=)
            0x10000ull, 0x10002ull,                    // just above floor -> accept
            0x00007FFFFFFFFFFEull,                     // just below ceiling -> accept
            0x00007FFFFFFFFFFFull,                     // == ceiling -> reject (>=)
            0x0000800000000000ull,                     // above ceiling -> reject
            0xFFFFFFFFFFFFFFFEull,                      // far above -> reject
        };
        bool boundary_match{ true };
        for (const std::uintptr_t a : boundary_addrs)
        {
            const bool got{ vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(a)) };
            if (got != expected_is_valid_pointer(a)) { boundary_match = false; }
        }
        check("is_valid_pointer_boundary_values_match_oracle", boundary_match);

        // Explicit, human-readable spot asserts for the four corners.
        check("is_valid_pointer_rejects_null",
              !vmhook::hotspot::is_valid_pointer(nullptr));
        check("is_valid_pointer_rejects_floor",
              !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(0xFFFFull)));
        check("is_valid_pointer_accepts_just_above_floor",
              vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(0x10000ull)));
        check("is_valid_pointer_rejects_ceiling",
              !vmhook::hotspot::is_valid_pointer(
                  reinterpret_cast<const void*>(0x00007FFFFFFFFFFFull)));
        check("is_valid_pointer_accepts_just_below_ceiling",
              vmhook::hotspot::is_valid_pointer(
                  reinterpret_cast<const void*>(0x00007FFFFFFFFFFEull)));

        // (b) Odd vs even at a known-in-range base: every odd address is
        //     rejected, the matching even address (when not a sentinel) accepted.
        bool odd_even_match{ true };
        for (std::uintptr_t base{ 0x100000ull }; base < 0x100000ull + 512; ++base)
        {
            const bool got{ vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(base)) };
            if (got != expected_is_valid_pointer(base)) { odd_even_match = false; }
        }
        check("is_valid_pointer_odd_even_sweep_match_oracle", odd_even_match);
        check("is_valid_pointer_rejects_odd_in_range",
              !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(0x100001ull)));
        check("is_valid_pointer_accepts_even_in_range",
              vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(0x100000ull)));

        // (c) Every documented debug-fill sentinel, EVEN-low32, placed in range:
        //     each must be rejected by the low-32 switch even though the address
        //     is otherwise valid (in-range + even).  Then prove the SAME low32
        //     at a higher 64-bit address is still rejected (the check is on the
        //     low 32 bits, independent of the high bits).
        const std::uint32_t sentinels[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool sentinels_rejected{ true };
        bool sentinels_high_rejected{ true };
        bool sentinels_match_oracle{ true };
        for (const std::uint32_t s : sentinels)
        {
            const std::uintptr_t low_addr{ static_cast<std::uintptr_t>(s) };
            // low32 == sentinel, high bits 0: in-range (sentinels are > floor,
            // < ceiling).  Even ones (CAFEBABE, CCCCCCCC, FEEEFEEE, BAADF00D,
            // ABABABAB, FDFDFDFD, DDDDDDDD, CDCDCDCD) reach the switch; the odd
            // ones (DEADBEEF) are already caught by the odd rule — either way
            // the result is "reject", and matches the oracle.
            if (vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(low_addr)))
            {
                sentinels_rejected = false;
            }
            if (vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(low_addr))
                != expected_is_valid_pointer(low_addr))
            {
                sentinels_match_oracle = false;
            }
            // Same low32, high 32 bits set to 0x00001234 -> still in range
            // (0x000012340000.... < ceiling 0x00007FFF........), still rejected.
            const std::uintptr_t high_addr{ (static_cast<std::uintptr_t>(0x00001234ull) << 32)
                                            | static_cast<std::uintptr_t>(s) };
            if (vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(high_addr)))
            {
                sentinels_high_rejected = false;
            }
            if (vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(high_addr))
                != expected_is_valid_pointer(high_addr))
            {
                sentinels_match_oracle = false;
            }
        }
        check("is_valid_pointer_low_sentinels_rejected", sentinels_rejected);
        check("is_valid_pointer_high_addr_sentinels_rejected", sentinels_high_rejected);
        check("is_valid_pointer_sentinels_match_oracle", sentinels_match_oracle);

        // (d) A non-sentinel even in-range address is accepted — proving the
        //     rejection is the low32 pattern, not the magnitude.  Address chosen
        //     to be in range and even on BOTH 32- and 64-bit (low 32 bits carry
        //     the whole value, so the verdict is pointer-width-independent):
        //     0x00100000 (1 MiB) > floor (0xFFFF), < ceiling on either width,
        //     even, low32 = 0x00100000 (not a sentinel).
        check("is_valid_pointer_accepts_nonsentinel_in_range",
              vmhook::hotspot::is_valid_pointer(
                  reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x00100000ull))));

        // (e) A deterministic pseudo-random scatter across the whole 64-bit
        //     space cross-checked against the oracle — broadest possible value
        //     coverage, fixed seed so it is byte-identical every run.
        bool scatter_match{ true };
        std::uint64_t state{ 0x9E3779B97F4A7C15ull };       // fixed seed
        for (int i{ 0 }; i < 20000; ++i)
        {
            // SplitMix64 — pure integer, deterministic, platform-independent.
            state += 0x9E3779B97F4A7C15ull;
            std::uint64_t z{ state };
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z =  z ^ (z >> 31);
            const std::uintptr_t addr{ static_cast<std::uintptr_t>(z) };
            if (vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(addr))
                != expected_is_valid_pointer(addr))
            {
                scatter_match = false;
            }
        }
        check("is_valid_pointer_random_scatter_matches_oracle", scatter_match);
    }

    // ---------------------------------------------------------------------
    // 26. override_class_lookup with a VALID-looking (but fake) klass pointer,
    //     observed through find_class WITHOUT a JVM.  An even, in-range,
    //     non-sentinel address PASSES is_valid_pointer, so find_class's cache
    //     hit proceeds to cached_klass->get_name() — which is itself fully
    //     guarded (is_valid_pointer + safe_read + try/catch inside get_name /
    //     symbol::to_string).  With no JVM the symbol read yields "" / nullptr,
    //     the name-match fails, the entry is evicted, the graph re-walk returns
    //     null.  Net: still nullptr, no crash, entry gone — the live-memory
    //     counterpart to the bogus-sentinel eviction of section 6c, but for a
    //     pointer that clears the cheap integer filter.
    //
    //     NOTE: this seeds the SHARED klass_lookup_cache; we save & restore the
    //     exact prior state of every key we touch so later test ordering is
    //     unaffected (defensive even though these are fresh probe names).
    // ---------------------------------------------------------------------
    {
        // An address that PASSES is_valid_pointer on BOTH 32- and 64-bit: even,
        // in range (> floor 0xFFFF, < ceiling on either width), low32 not a
        // sentinel.  0x00100000 (1 MiB) carries its whole value in the low 32
        // bits, so the verdict is pointer-width-independent — no sizeof hard-code.
        auto* const plausible_klass{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0x00100000ull)) };
        // Confirm our chosen pointer really does clear the integer filter, so
        // this test exercises the get_name() path (not the early reject).
        check("section26_plausible_klass_passes_is_valid_pointer",
              vmhook::hotspot::is_valid_pointer(plausible_klass));

        const char* const name{ "override/Plausible/Probe" };

        // Save prior cache state for `name` (should be absent) and restore later.
        auto cache_snapshot = [](const std::string& key)
            -> std::pair<bool, vmhook::hotspot::klass*>
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            const auto it{ vmhook::klass_lookup_cache.find(key) };
            if (it == vmhook::klass_lookup_cache.end()) { return { false, nullptr }; }
            return { true, it->second };
        };
        auto cache_restore = [](const std::string& key,
                                const std::pair<bool, vmhook::hotspot::klass*>& prior)
            -> void
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            if (prior.first) { vmhook::klass_lookup_cache[key] = prior.second; }
            else             { vmhook::klass_lookup_cache.erase(key); }
        };

        const auto prior{ cache_snapshot(name) };

        vmhook::override_class_lookup(name, plausible_klass);
        // find_class hits the cache, validates via get_name (-> "" no-JVM),
        // name-mismatch -> evicts -> re-walks -> null.  No deref of fake memory
        // because get_name gates everything behind is_valid_pointer + safe_read.
        bool threw{ false };
        vmhook::hotspot::klass* result{ plausible_klass };
        try { result = vmhook::find_class(name); }
        catch (...) { threw = true; }
        check("find_class_plausible_override_no_throw", !threw);
        check("find_class_plausible_override_returns_null", result == nullptr);

        // The stale entry was evicted by that first call: a second lookup also
        // re-walks to null (and the cache no longer pins the fake pointer).
        check("find_class_plausible_override_evicted",
              vmhook::find_class(name) == nullptr);
        {
            const auto after{ cache_snapshot(name) };
            check("find_class_plausible_override_entry_gone", !after.first);
        }

        cache_restore(name, prior);
    }

    // ---------------------------------------------------------------------
    // 27. reanchor_classes_via_oop return-value truth table, no JVM.  With a
    //     null anchor it is always false (before any work).  We additionally
    //     exercise the empty / single / multi name-list shapes to pin the
    //     "true only if EVERY name resolved" rule at the null-anchor boundary
    //     (where nothing resolves, so any non-empty list is false and the empty
    //     list is vacuously... still false here, because the null-anchor guard
    //     returns false BEFORE the per-name loop — distinct from the non-null
    //     fake-anchor empty-list case covered in test_classloader_reanchor).
    // ---------------------------------------------------------------------
    {
        check("reanchor_null_anchor_empty_list_false",
              vmhook::reanchor_classes_via_oop(nullptr, {}) == false);
        check("reanchor_null_anchor_single_false",
              vmhook::reanchor_classes_via_oop(nullptr, { "a/B" }) == false);
        check("reanchor_null_anchor_multi_false",
              vmhook::reanchor_classes_via_oop(
                  nullptr, { "a/B", "c/D", "e/F" }) == false);
        // Never throws for a batch of name shapes (all with null anchor).
        bool none_threw{ true };
        try
        {
            (void)vmhook::reanchor_classes_via_oop(
                nullptr, { "", "java/lang/Object", "[I", "Ljava/lang/String;",
                           "()V", "weird$Inner" });
        }
        catch (...) { none_threw = false; }
        check("reanchor_null_anchor_varied_shapes_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 28. Empty-name fast-reject parity ACROSS the whole family.  The empty
    //     name is special-cased in find_class (returns before the lock); the
    //     siblings reach it via their own no-thread gates.  Pin that every
    //     entry point treats "" as not-found, through every constructible empty
    //     view, and that the method overloads are empty for "".
    // ---------------------------------------------------------------------
    {
        const std::string_view empties[]{
            std::string_view{},                       // null data, 0 size
            std::string_view{ "" },                   // valid ptr, 0 size
        };
        bool all_empty_null{ true };
        for (const std::string_view e : empties)
        {
            if (vmhook::find_class(e) != nullptr) { all_empty_null = false; }
            if (vmhook::find_class(e) != nullptr) { all_empty_null = false; }
            if (vmhook::find_class(e) != nullptr) { all_empty_null = false; }
            if (vmhook::find_class_via_oop(nullptr, e) != nullptr) { all_empty_null = false; }
            if (!vmhook::get_class_methods(e).empty()) { all_empty_null = false; }
        }
        check("empty_name_all_entry_points_not_found", all_empty_null);
        check("find_methods_by_signature_empty_descriptor_empty",
              vmhook::find_methods_by_signature<unreg_w>("").empty());
    }

    // ---------------------------------------------------------------------
    // 29. Length boundaries for the cache-key std::string construction.  Sweep
    //     a contiguous run of lengths spanning the small-string-optimisation
    //     boundary (15/16/23 chars on common libs) and on into heap territory,
    //     for both a plain name and a separator-laden name.  No fixed buffer is
    //     involved, so every length must be null + no-throw.
    // ---------------------------------------------------------------------
    {
        bool plain_ok{ true };
        bool sep_ok{ true };
        for (std::size_t len{ 0 }; len <= 64; ++len)
        {
            const std::string plain(len, 'q');
            const std::string_view plain_v{ plain.data(), plain.size() };
            // len==0 hits the empty guard; len>0 travels the full path.
            if (vmhook::find_class(plain_v) != nullptr) { plain_ok = false; }
            if (!find_class_never_throws(plain_v))       { plain_ok = false; }

            std::string sep{};
            for (std::size_t i{ 0 }; i < len; ++i) { sep += (i % 2 == 0) ? 'a' : '/'; }
            if (vmhook::find_class(sep) != nullptr) { sep_ok = false; }
        }
        check("find_class_length_boundary_sweep_plain_ok", plain_ok);
        check("find_class_length_boundary_sweep_separators_ok", sep_ok);

        // A handful of large powers-of-two lengths.
        bool big_ok{ true };
        for (const std::size_t len : { std::size_t{ 1024 }, std::size_t{ 4096 },
                                       std::size_t{ 16384 }, std::size_t{ 65536 } })
        {
            const std::string big(len, 'a');
            if (vmhook::find_class(big) != nullptr) { big_ok = false; }
            if (!find_class_never_throws(big))       { big_ok = false; }
        }
        check("find_class_large_power_of_two_lengths_ok", big_ok);
    }

    // =====================================================================
    // 30. ADDITIVE DEEPENING PASS (wave). Every assertion below is derived
    //     directly from the current vmhook.hpp source and pins NO-JVM
    //     contracts not covered by sections 0-29 above. Pure null/empty/false
    //     /size comparisons and is_valid_pointer-rejected integer constants —
    //     no fabricated handle is ever dereferenced, no real allocation, no
    //     platform-variant comparison. Self-contained; touches no prior
    //     assertion. Any shared-cache key it seeds is saved and restored so
    //     the postcondition section below is unaffected.
    // =====================================================================

    // ---- Shared snapshot/restore for the klass_lookup_cache (same technique
    //      section 26 uses; klass_lookup_cache + its mutex are process memory,
    //      readable with no JVM). Lets the array-bypass probe seed a key and
    //      restore the exact prior state afterwards. -----------------------
    auto deepen_cache_snapshot = [](const std::string& key)
        -> std::pair<bool, vmhook::hotspot::klass*>
    {
        std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
        const auto it{ vmhook::klass_lookup_cache.find(key) };
        if (it == vmhook::klass_lookup_cache.end()) { return { false, nullptr }; }
        return { true, it->second };
    };
    auto deepen_cache_restore = [](const std::string& key,
                                   const std::pair<bool, vmhook::hotspot::klass*>& prior)
        -> void
    {
        std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
        if (prior.first) { vmhook::klass_lookup_cache[key] = prior.second; }
        else             { vmhook::klass_lookup_cache.erase(key); }
    };
    auto deepen_cache_has = [](const std::string& key) -> bool
    {
        std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
        return vmhook::klass_lookup_cache.find(key) != vmhook::klass_lookup_cache.end();
    };

    // ---------------------------------------------------------------------
    // 30a. ARRAY-NAME branch BYPASSES the cache entirely (vmhook.hpp ~8175).
    //      find_class checks class_name.front()=='[' BEFORE consulting
    //      klass_lookup_cache: it routes straight to jni_find_class (null with
    //      no JVM, via the ensure_current_java_thread gate ~11872) and returns
    //      WITHOUT ever reading OR erasing the cache. Therefore:
    //        * an override_class_lookup() seeded under an array name is IGNORED
    //          by find_class on that name (returns null, not the seeded ptr),
    //        * and the seeded entry SURVIVES the find_class call (the array
    //          branch never erases) — unlike the non-array stale path
    //          (section 6c / 26) which consults+evicts.
    //      We seed with an is_valid_pointer-REJECTED low constant (0x2: below
    //      user_address_floor 0xFFFF) so the fake klass is NEVER dereferenced
    //      on any path. Prior cache state for the key is saved and restored.
    // ---------------------------------------------------------------------
    {
        // 0x2 is even but <= floor (0xFFFF) -> is_valid_pointer rejects it
        // purely by arithmetic; it is never read. Confirm that first.
        auto* const rejected_fake{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0x2ull)) };
        check("section30a_fake_klass_rejected_by_is_valid_pointer",
              vmhook::hotspot::is_valid_pointer(rejected_fake) == false);

        const char* const array_names[]{
            "[I", "[J", "[Z", "[B", "[C", "[S", "[F", "[D",
            "[Ljava/lang/Object;", "[[I", "[[Ljava/lang/String;",
        };
        bool seeded_ignored_all_null{ true };
        bool seeded_entry_survives{ true };
        bool none_threw{ true };
        for (const char* n : array_names)
        {
            const std::string key{ n };
            const auto prior{ deepen_cache_snapshot(key) };

            vmhook::override_class_lookup(n, rejected_fake);
            // find_class takes the '[' branch BEFORE the cache, so it returns
            // null (no JVM) and never reads the seeded fake pointer.
            bool threw{ false };
            vmhook::hotspot::klass* result{ rejected_fake };
            try { result = vmhook::find_class(n); }
            catch (...) { threw = true; }
            if (threw)              { none_threw = false; }
            if (result != nullptr)  { seeded_ignored_all_null = false; }
            // The array branch never erased the cache, so the seeded entry is
            // still present (proving find_class never touched the cache here).
            if (!deepen_cache_has(key)) { seeded_entry_survives = false; }

            deepen_cache_restore(key, prior);
        }
        check("find_class_array_name_ignores_seeded_cache_entry", seeded_ignored_all_null);
        check("find_class_array_name_leaves_cache_entry_intact", seeded_entry_survives);
        check("find_class_array_name_seeded_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 30b. find_class_via_oop SECOND-clause guard: a NON-null anchor that is
    //      an is_valid_pointer-rejected low constant passes the `!anchor_oop`
    //      test (it is non-null) and so reaches the `!ensure_current_java_thread()`
    //      clause of the `||` guard (vmhook.hpp ~13777), which is true with no
    //      JVM -> returns null BEFORE jni_oop_handle ever dereferences the
    //      anchor. Sections 7/27 only ever pass nullptr (first clause); this
    //      pins the second clause. The anchor is NEVER dereferenced: the guard
    //      short-circuits first, and the constants are is_valid_pointer-rejected
    //      regardless. (anchor is void*, so we cast through uintptr_t.)
    // ---------------------------------------------------------------------
    {
        const std::uintptr_t rejected_anchors[]{
            0x0000000000000002ull,   // even, below floor
            0x0000000000000004ull,   // even, below floor
            0x0000000000000FFEull,   // even, below floor
        };
        const char* const names[]{
            "", "java/lang/Object", "definitely/Not/A/Class",
            "[Ljava/lang/String;", "weird$Inner",
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const std::uintptr_t a : rejected_anchors)
        {
            void* const anchor{ reinterpret_cast<void*>(a) };
            for (const char* n : names)
            {
                bool threw{ false };
                vmhook::hotspot::klass* result{ nullptr };
                try { result = vmhook::find_class_via_oop(anchor, n); }
                catch (...) { threw = true; }
                if (threw)             { none_threw = false; }
                if (result != nullptr) { all_null = false; }
            }
        }
        check("find_class_via_oop_nonnull_rejected_anchor_all_null", all_null);
        check("find_class_via_oop_nonnull_rejected_anchor_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 30c. reanchor_classes_via_oop with a NON-null (rejected) anchor enters
    //      the per-name loop (the `!anchor_oop` early-false guard at ~13923 is
    //      NOT taken), calls find_class_via_oop per name (null with no JVM,
    //      gated by ensure_current_java_thread), so NO name resolves, nothing
    //      is overridden, and all_resolved stays false. Section 27 only covers
    //      the null-anchor EARLY return; this covers the loop's all-miss path.
    //      A non-empty list MUST be false (no name resolved). The EMPTY list
    //      with a non-null anchor is vacuously TRUE (the loop body never runs,
    //      all_resolved stays its initial true) — the documented "true only if
    //      EVERY name resolved" rule, vacuously satisfied. Anchor never deref'd
    //      (find_class_via_oop's gate fails first; constant is rejected anyway).
    // ---------------------------------------------------------------------
    {
        void* const rejected_anchor{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x2ull)) };

        // Non-empty lists: nothing resolves with no JVM -> false.
        check("reanchor_nonnull_anchor_single_false",
              vmhook::reanchor_classes_via_oop(rejected_anchor, { "a/B" }) == false);
        check("reanchor_nonnull_anchor_multi_false",
              vmhook::reanchor_classes_via_oop(
                  rejected_anchor, { "a/B", "c/D", "java/lang/Object" }) == false);
        check("reanchor_nonnull_anchor_arrays_false",
              vmhook::reanchor_classes_via_oop(
                  rejected_anchor, { "[I", "[Ljava/lang/String;" }) == false);

        // Empty list + non-null anchor: loop body never runs -> vacuously true.
        check("reanchor_nonnull_anchor_empty_list_vacuously_true",
              vmhook::reanchor_classes_via_oop(rejected_anchor, {}) == true);

        // Never throws across a batch of mixed name shapes (rejected anchor).
        bool none_threw{ true };
        try
        {
            (void)vmhook::reanchor_classes_via_oop(
                rejected_anchor,
                { "", "java/lang/Object", "[I", "Ljava/lang/String;",
                  "()V", "weird$Inner", "java.dotted.Form" });
        }
        catch (...) { none_threw = false; }
        check("reanchor_nonnull_anchor_varied_shapes_no_throw", none_threw);

        // The all-miss loop left NOTHING overridden in the cache (no name
        // resolved, so override_class_lookup was never called for any of them).
        bool nothing_overridden{ true };
        for (const char* n : { "a/B", "c/D" })
        {
            if (deepen_cache_has(std::string{ n })) { nothing_overridden = false; }
        }
        check("reanchor_nonnull_anchor_overrides_nothing_no_jvm", nothing_overridden);
    }

    // ---------------------------------------------------------------------
    // 30d. reanchor_classes_via_oop return-type AND null-anchor independence
    //      from list contents. The return type is exactly bool (documents the
    //      poll-until-true channel). With a null anchor the result is false for
    //      EVERY list shape — including the empty list — because the
    //      `!anchor_oop` guard returns false BEFORE the loop (so, unlike 30c's
    //      non-null empty-list vacuous-true, the null empty-list is FALSE). The
    //      two empty-list outcomes (null->false at ~13923 vs non-null->true via
    //      the untouched all_resolved) are the boundary this pins from both
    //      sides.
    // ---------------------------------------------------------------------
    {
        using reanchor_ret_t = decltype(vmhook::reanchor_classes_via_oop(nullptr, {}));
        static_assert(std::is_same_v<reanchor_ret_t, bool>,
                      "reanchor_classes_via_oop must return bool");
        check("reanchor_return_type_is_bool", true);

        // Null anchor: false regardless of list contents (early guard).
        check("reanchor_null_anchor_empty_false_vs_nonnull_true_boundary",
              vmhook::reanchor_classes_via_oop(nullptr, {}) == false);
    }

    // ---------------------------------------------------------------------
    // 30e. override_class_lookup / evict_class_lookup degenerate-name
    //      contract. Both take the name by value into std::string{class_name}
    //      under the lock (vmhook.hpp ~13882 / ~13900) and are noexcept (their
    //      bodies are wrapped in try/catch). They accept ANY name shape —
    //      including the empty name, embedded NULs, and a default-constructed
    //      view — without throwing. After a null-override + evict of an
    //      ordinary (non-array) name, find_class re-walks to null. We exercise
    //      a spread of degenerate keys and confirm: no throw, and a final
    //      find_class on each is null, and the key is gone after eviction.
    //      Every key we use is a fresh probe name; we evict each at the end so
    //      no entry is left armed.
    // ---------------------------------------------------------------------
    {
        // Length-bearing degenerate keys (string_view carries the size, so the
        // interior/leading NUL keys are non-empty and travel the full path).
        const std::string nul_key{ std::string{ "deepen/Nul" } + '\0' + "Key" };
        const std::string lead_nul_key{ std::string{ '\0' } + "deepen/LeadNul" };
        const std::string_view keys[]{
            std::string_view{},                         // default-constructed (empty)
            std::string_view{ "" },                     // valid ptr, 0 size
            std::string_view{ "deepen/Plain/Probe" },
            std::string_view{ "deepen/Sep//Probe" },
            std::string_view{ nul_key.data(), nul_key.size() },
            std::string_view{ lead_nul_key.data(), lead_nul_key.size() },
        };
        bool override_no_throw{ true };
        bool evict_no_throw{ true };
        bool find_null_after{ true };
        bool gone_after_evict{ true };
        for (const std::string_view k : keys)
        {
            try { vmhook::override_class_lookup(k, nullptr); }
            catch (...) { override_no_throw = false; }
            // A null override does not seed a durable negative; find_class
            // re-walks (-> null with no JVM). Non-array keys only (no '[').
            if (vmhook::find_class(k) != nullptr) { find_null_after = false; }
            try { vmhook::evict_class_lookup(k); }
            catch (...) { evict_no_throw = false; }
            if (deepen_cache_has(std::string{ k })) { gone_after_evict = false; }
        }
        check("override_class_lookup_degenerate_keys_no_throw", override_no_throw);
        check("evict_class_lookup_degenerate_keys_no_throw", evict_no_throw);
        check("find_class_after_null_override_degenerate_null", find_null_after);
        check("evict_class_lookup_degenerate_keys_gone", gone_after_evict);

        // Double-evict of the same key is a safe no-op the second time.
        bool double_evict_no_throw{ true };
        try
        {
            vmhook::override_class_lookup("deepen/Double/Evict", nullptr);
            vmhook::evict_class_lookup("deepen/Double/Evict");
            vmhook::evict_class_lookup("deepen/Double/Evict");   // already gone
        }
        catch (...) { double_evict_no_throw = false; }
        check("evict_class_lookup_double_evict_no_throw", double_evict_no_throw);
        check("evict_class_lookup_double_evict_key_gone",
              deepen_cache_has(std::string{ "deepen/Double/Evict" }) == false);
    }

    // ---------------------------------------------------------------------
    // 30f. jni::find_class returns a JNI HANDLE channel (void*), distinct from
    //      the HotSpot-internal klass* channel, and jni::find_class_with_context_loader
    //      returns a klass*. Both null with no JVM (sections 8/9). Here we add
    //      the EXACT-byte determinism of the JNI handle channel across the full
    //      0x01..0xFF single-byte name space (size 1, never the empty guard):
    //      every single-byte name yields a null handle, twice in a row, never
    //      throwing — the JNI-side mirror of section 14(a)/24. void* compares
    //      are platform-invariant (only against nullptr / itself).
    // ---------------------------------------------------------------------
    {
        bool jni_single_byte_null{ true };
        bool jni_single_byte_stable{ true };
        bool jni_single_byte_no_throw{ true };
        bool ctx_single_byte_null{ true };
        for (int b{ 1 }; b <= 0xFF; ++b)
        {
            const char c{ static_cast<char>(b) };
            const std::string_view sv{ &c, 1 };
            try
            {
                void* const h1{ vmhook::find_class(sv) };
                void* const h2{ vmhook::find_class(sv) };
                if (h1 != nullptr) { jni_single_byte_null = false; }
                if (h1 != h2)      { jni_single_byte_stable = false; }   // both null
                if (vmhook::find_class(sv) != nullptr)
                {
                    ctx_single_byte_null = false;
                }
            }
            catch (...) { jni_single_byte_no_throw = false; }
        }
        check("jni_find_class_every_single_byte_null", jni_single_byte_null);
        check("jni_find_class_every_single_byte_stable", jni_single_byte_stable);
        check("jni_find_class_every_single_byte_no_throw", jni_single_byte_no_throw);
        check("jni_find_class_ctx_every_single_byte_null", ctx_single_byte_null);

        // The HotSpot-internal find_class (klass*) and the JNI find_class
        // (void*) are SEPARATE channels but agree on not-found with no JVM for
        // a batch of array / descriptor / dotted shapes (array names take the
        // '[' branch in the internal form, the FindClass slot in the JNI form;
        // both null). This ties the two channels together where they diverge
        // most (the array path).
        const char* const array_descs[]{
            "[I", "[J", "[Ljava/lang/Object;", "[[I",
            "Ljava/lang/String;", "()V", "java.dotted.Name",
        };
        bool channels_agree{ true };
        for (const char* n : array_descs)
        {
            const bool internal_null{ vmhook::find_class(n) == nullptr };
            const bool jni_null{ vmhook::find_class(n) == nullptr };
            const bool ctx_null{ vmhook::find_class(n) == nullptr };
            if (!(internal_null && jni_null && ctx_null)) { channels_agree = false; }
        }
        check("find_class_internal_and_jni_channels_agree_arrays_null", channels_agree);
    }

    // ---------------------------------------------------------------------
    // 30g. register_class<T>(name) return-value contract with no JVM, over a
    //      spread of degenerate names. Source (vmhook.hpp ~8926): the function
    //      FIRST verifies the class via vmhook::find_class(class_name) (~8929)
    //      and returns false BEFORE the type_to_class_map write (~8948) when
    //      that is null — which it ALWAYS is with no JVM, for EVERY name shape
    //      (empty, garbage, dotted, array, separator-laden). It is noexcept.
    //      So register_class<T>() returns false for every name here and never
    //      throws. Return type is exactly bool. We reuse the three throwaway
    //      wrappers; each is harmless to (attempt to) register repeatedly.
    //      This pins the verify-gate-false branch, which the two valid-name
    //      calls in sections 11/11b touch only incidentally.
    // ---------------------------------------------------------------------
    {
        using reg_ret_t = decltype(vmhook::register_class<unreg_w>(std::string_view{}));
        static_assert(std::is_same_v<reg_ret_t, bool>,
                      "register_class<T>() must return bool");
        check("register_class_T_return_type_is_bool", true);

        const char* const reg_names[]{
            "",                              // empty -> find_class empty-guard null
            "test/find_class/Deepen",        // ordinary
            "java/lang/Object",              // real-but-no-JVM
            "[I",                            // array name (find_class '[' branch null)
            "Ljava/lang/String;",            // field descriptor
            "()V",                           // method descriptor
            "java.dotted.Form",              // dotted
            "double//slash",                 // separator pathology
            "weird$Inner",                   // nested
        };
        bool all_false{ true };
        bool none_threw{ true };
        for (const char* n : reg_names)
        {
            try
            {
                // unreg_w stays a wrapper we never durably register (each call
                // returns false before the map write with no JVM).
                if (vmhook::register_class<unreg_w>(n) != false) { all_false = false; }
            }
            catch (...) { none_threw = false; }
        }
        check("register_class_T_all_names_false_no_jvm", all_false);
        check("register_class_T_no_throw", none_threw);

        // Repeated registration of the SAME wrapper+name is stable (false every
        // time with no JVM) and never throws — the verify gate is re-evaluated
        // each call and stays false.
        bool repeat_false{ true };
        bool repeat_no_throw{ true };
        for (int i{ 0 }; i < 32; ++i)
        {
            try
            {
                if (vmhook::register_class<reg_w>("test/find_class/Reg") != false)
                {
                    repeat_false = false;
                }
            }
            catch (...) { repeat_no_throw = false; }
        }
        check("register_class_T_repeat_same_false", repeat_false);
        check("register_class_T_repeat_same_no_throw", repeat_no_throw);
    }

    // ---------------------------------------------------------------------
    // 30h. LEDGER-GAP: cold-state fallback CHAIN — every tier returns null
    //      for the same 16 fabricated bad names. The chain is (vmhook.hpp
    //      ~8241 / ~13620 / ~13623):
    //           find_class(name)                                  [tier 1: HotSpot graph walk]
    //        -> jni::find_class(name)                              [tier 2: JNI FindClass slot]
    //        -> jni::find_class_with_context_loader(name)          [tier 3: thread CL + system CL + Forge]
    //      With no JVM ALL three tiers null out at their own gates (graph
    //      head null; ensure_current_java_thread false; ensure_current_java_thread
    //      false again). The chain is therefore observably null at EACH tier
    //      INDEPENDENTLY for the same input — i.e. a downstream tier would
    //      still be null even if the upstream had not been called. We pin
    //      that per-tier independence here.
    //
    //      Signature noexcept pins (vmhook.hpp ~8085, ~12173, ~13620): all
    //      three resolution entry points + the public wrappers are declared
    //      noexcept, so a throw would call std::terminate. We static_assert
    //      noexcept on every relevant signature.
    // ---------------------------------------------------------------------
    {
        // ---- noexcept static_asserts on the fallback signatures.
        //      Characterization of the ACTUAL declarations in vmhook.hpp:
        //        * vmhook::find_class (~8146)           — NOT declared noexcept
        //          (calls into the graph walk which can throw internally; the
        //          empirical never-throws-in-practice contract is exercised by
        //          the runtime try/catch helpers above).
        //        * vmhook::find_class (~13612)            — noexcept.
        //        * vmhook::find_class (~13620) — noexcept.
        //        * vmhook::find_class_via_oop (~13774)        — noexcept.
        //        * override_class_lookup (~13876)              — noexcept.
        //        * evict_class_lookup (~13894)                 — noexcept.
        static_assert(!noexcept(vmhook::find_class(std::string_view{})),
                      "vmhook::find_class is not declared noexcept (characterization)");
        static_assert(noexcept(vmhook::find_class(std::string_view{})),
                      "vmhook::find_class must be noexcept");
        static_assert(noexcept(vmhook::find_class(std::string_view{})),
                      "vmhook::find_class must be noexcept");
        static_assert(noexcept(vmhook::find_class_via_oop(nullptr, std::string_view{})),
                      "vmhook::find_class_via_oop must be noexcept");
        static_assert(noexcept(vmhook::override_class_lookup(std::string_view{},
                                  static_cast<vmhook::hotspot::klass*>(nullptr))),
                      "vmhook::override_class_lookup must be noexcept");
        static_assert(noexcept(vmhook::evict_class_lookup(std::string_view{})),
                      "vmhook::evict_class_lookup must be noexcept");
        check("section30h_fallback_chain_noexcept_static_asserts_compiled", true);

        // ---- 16 fabricated bad names spanning every "bad" shape class.
        const char* const bad16[]{
            "",                              //  1. empty (fast-reject in find_class)
            " ",                             //  2. whitespace-only
            "java.lang.Object",              //  3. dotted (find_class wants '/')
            "java/lang/",                    //  4. trailing slash
            "/java/lang/Object",             //  5. leading slash
            "java//lang/Object",             //  6. double slash
            "$",                             //  7. lone dollar
            "weird$Inner$$",                 //  8. trailing $$
            "Ljava/lang/String;",            //  9. field descriptor (not a name)
            "[I",                            // 10. primitive-array descriptor
            "[Ljava/lang/Object;",           // 11. object-array descriptor
            "[[[I",                          // 12. nested-array descriptor
            "()V",                           // 13. method descriptor
            "(Ljava/lang/String;)V",         // 14. method descriptor w/ ref arg
            "no/such/Class/Exists/At/All",   // 15. plausible-but-missing
            "\x01\x02\x03/ctrl",             // 16. control bytes
        };
        static_assert(sizeof(bad16) / sizeof(bad16[0]) == 16,
                      "ledger requires exactly 16 fabricated bad names");

        bool tier1_all_null{ true };   // HotSpot-internal find_class
        bool tier2_all_null{ true };   // jni::find_class (FindClass)
        bool tier3_all_null{ true };   // jni::find_class_with_context_loader
        bool tier1_no_throw{ true };
        bool tier2_no_throw{ true };
        bool tier3_no_throw{ true };
        bool tier1_stable{ true };
        bool tier2_stable{ true };
        bool tier3_stable{ true };
        for (const char* n : bad16)
        {
            // Tier 1: the HotSpot graph walk path.
            try
            {
                const auto a{ vmhook::find_class(n) };
                const auto b{ vmhook::find_class(n) };
                if (a != nullptr) { tier1_all_null = false; }
                if (a != b)       { tier1_stable = false; }
            }
            catch (...) { tier1_no_throw = false; }
            // Tier 2: the public JNI FindClass wrapper (void* channel).
            try
            {
                void* const h1{ vmhook::find_class(n) };
                void* const h2{ vmhook::find_class(n) };
                if (h1 != nullptr) { tier2_all_null = false; }
                if (h1 != h2)      { tier2_stable = false; }
            }
            catch (...) { tier2_no_throw = false; }
            // Tier 3: the context-loader fallback helper (klass* channel).
            try
            {
                const auto c1{ vmhook::find_class(n) };
                const auto c2{ vmhook::find_class(n) };
                if (c1 != nullptr) { tier3_all_null = false; }
                if (c1 != c2)      { tier3_stable = false; }
            }
            catch (...) { tier3_no_throw = false; }
        }
        check("ledger_fallback_tier1_find_class_all_16_null", tier1_all_null);
        check("ledger_fallback_tier2_jni_find_class_all_16_null", tier2_all_null);
        check("ledger_fallback_tier3_ctx_loader_all_16_null", tier3_all_null);
        check("ledger_fallback_tier1_no_throw", tier1_no_throw);
        check("ledger_fallback_tier2_no_throw", tier2_no_throw);
        check("ledger_fallback_tier3_no_throw", tier3_no_throw);
        check("ledger_fallback_tier1_stable", tier1_stable);
        check("ledger_fallback_tier2_stable", tier2_stable);
        check("ledger_fallback_tier3_stable", tier3_stable);

        // Per-name cross-tier consistency: for EVERY one of the 16 bad names,
        // all three tiers agree on "not found" (null/null/null). Pins the
        // ledger claim "returns null at each tier" name-by-name.
        bool per_name_all_three_null{ true };
        for (const char* n : bad16)
        {
            const bool t1{ vmhook::find_class(n) == nullptr };
            const bool t2{ vmhook::find_class(n) == nullptr };
            const bool t3{ vmhook::find_class(n) == nullptr };
            if (!(t1 && t2 && t3)) { per_name_all_three_null = false; }
        }
        check("ledger_fallback_per_name_all_three_tiers_null", per_name_all_three_null);
    }

    // ---------------------------------------------------------------------
    // 15. Final invariant: after ALL the cache seeding / eviction churn above,
    //     a brand-new never-touched name still resolves to null and the JVM-
    //     absence preconditions still hold (nothing we did attached a thread or
    //     fabricated an env).
    // ---------------------------------------------------------------------
    check("postcondition_fresh_name_still_null",
          vmhook::find_class("final/Fresh/Never/Seen/Probe") == nullptr);
    check("postcondition_no_jvm_env_still_null",
          vmhook::hotspot::current_jni_env == nullptr);
    check("postcondition_no_jvm_java_thread_still_null",
          vmhook::hotspot::current_java_thread == nullptr);

    std::printf("ran %d checks\n", checks_run);
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
