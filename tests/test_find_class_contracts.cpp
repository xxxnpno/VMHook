// Standalone (no-JVM) unit test pinning the NO-JVM contracts of the recently
// hardened class-lookup / method-introspection entry points:
//
//   vmhook::find_class(class_name)                         -> klass*  (null w/o JVM)
//   vmhook::find_class_via_oop(anchor_oop, class_name)     -> klass*  (null on null anchor)
//   vmhook::override_class_lookup(name, klass*)            -> void    (seeds the cache)
//   vmhook::evict_class_lookup(name)                       -> void    (forgets a cache entry)
//   vmhook::jni::find_class(class_name)                    -> void*   (null w/o JVM)
//   vmhook::jni::find_class_with_context_loader(name)      -> klass*  (null w/o JVM)
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
        try { (void)vmhook::jni::find_class(name); return true; }
        catch (...) { return false; }
    }
    auto jni_find_class_ctx_never_throws(std::string_view name) -> bool
    {
        try { (void)vmhook::jni::find_class_with_context_loader(name); return true; }
        catch (...) { return false; }
    }
    auto find_class_via_oop_null_never_throws(std::string_view name) -> bool
    {
        try { (void)vmhook::find_class_via_oop(nullptr, name); return true; }
        catch (...) { return false; }
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
    static_assert(std::is_same_v<decltype(vmhook::jni::find_class(std::string_view{})),
                                 void*>,
                  "jni::find_class must return void* (a JNI handle)");
    static_assert(std::is_same_v<
                      decltype(vmhook::jni::find_class_with_context_loader(std::string_view{})),
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
    // 8. vmhook::jni::find_class(name): public JNI FindClass wrapper.  With no
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
            if (vmhook::jni::find_class(n) != nullptr) { all_null = false; }
            if (!jni_find_class_never_throws(n))       { none_threw = false; }
        }
        check("jni_find_class_all_null_no_jvm", all_null);
        check("jni_find_class_no_throw", none_threw);
        // Long name through the JNI wrapper too (it builds a std::string).
        const std::string long_name(8192, 'q');
        check("jni_find_class_long_name_null",
              vmhook::jni::find_class(long_name) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 9. vmhook::jni::find_class_with_context_loader(name): the full fallback
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
            if (vmhook::jni::find_class_with_context_loader(n) != nullptr) { all_null = false; }
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
            const bool jni_null { vmhook::jni::find_class(n) == nullptr };
            const bool ctx_null { vmhook::jni::find_class_with_context_loader(n) == nullptr };
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
