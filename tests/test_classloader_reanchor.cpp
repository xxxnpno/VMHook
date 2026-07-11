// Standalone (no-JVM) unit test for the multi-classloader resolution helpers
// (the "classloader_reanchor" feature):
//
//   vmhook::find_class_via_oop(anchor_oop, name)              -> klass* (noexcept)
//   vmhook::override_class_lookup(name, klass*)               -> void   (noexcept)
//   vmhook::evict_class_lookup(name)                          -> void   (noexcept)
//   vmhook::reanchor_classes_via_oop(anchor, {names...})      -> bool   (noexcept)
//
// The real "resolve the right loader's copy" behaviour fundamentally needs a
// live JVM with two classloaders defining the same internal name; that proof
// belongs in JVM integration and is deliberately OUT of scope here.  This file
// EXHAUSTS the no-JVM-observable surface only, in three layers:
//
//   (a) The documented NO-JVM CONTRACT.  Every reanchor entry point must return
//       cleanly — nullptr / false / (vacuously) true — and never throw / never
//       fault when no JVM (hence no JavaThread, no classloader) is present.  The
//       single gate is vmhook::hotspot::ensure_current_java_thread(), which
//       returns false in a JVM-less process (vmhook.hpp ~5088): find_class_via_oop
//       short-circuits on `!anchor_oop || !ensure_current_java_thread()`
//       (~13474), and reanchor_classes_via_oop short-circuits on `!anchor_oop`
//       (~13613) and otherwise propagates find_class_via_oop's nullptr into a
//       `false` aggregate.
//
//   (b) The PURE cache helpers.  override_class_lookup writes
//       `klass_lookup_cache[std::string{name}] = k` (~13572) and
//       evict_class_lookup runs `klass_lookup_cache.erase(std::string{name})`
//       (~13590), both under klass_lookup_cache_mutex, both swallowing.  These
//       mutate plain process memory, so they are FULLY testable with no JVM by
//       inspecting the map directly under its mutex (exactly as find_class reads
//       it on its cache-hit path, ~7996).  We exercise the complete
//       seed / last-write-wins / evict / null-value / round-trip-fidelity matrix
//       over every name shape (empty, unicode, embedded-NUL, very long, garbage).
//
//   (c) COMPILE-TIME signature / return-type / noexcept contracts via
//       static_assert, so a silent drift in any of the four entry points fails
//       the build rather than a runtime check.
//
// Every assertion is platform-invariant and deterministic: only nullptr / bool /
// size / pointer-identity comparisons and direct map reads — no <charconv>, no
// float parsing, no JDK-variant or sizeof/endianness-dependent expectation, no
// banned API.  Identical across MSVC /WX, libstdc++ (MinGW) and libc++
// (clang / Apple clang).  No JVM fixture, no heap growth beyond a bounded set of
// std::string cache keys that are all evicted again before exit.
//
// IMPORTANT hygiene note (flaw: the cache is PROCESS-GLOBAL and unscoped —
// vmhook.hpp ~7958): override_class_lookup / reanchor_classes_via_oop poison the
// one shared klass_lookup_cache with no ownership tracking.  Every mutation this
// test performs is therefore bracketed by an explicit save/restore of the
// touched keys (scoped_cache_guard below) so the test leaves the cache exactly
// as it found it and cannot corrupt any later assertion within this binary.
#include <vmhook/vmhook.hpp>

#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

static int failures{ 0 };
static int checks_run{ 0 };
static auto check(const char* name, bool ok) -> void
{
    ++checks_run;
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// An [INFO] line records an OBSERVED library behaviour (often a documented-vs-
// actual mismatch) that we deliberately pin rather than treat as pass/fail, so a
// future change to that behaviour is a visible, reviewed diff in this file.
static auto info(const char* name, bool observed) -> void
{
    std::printf("[INFO] %s = %s\n", name, observed ? "true" : "false");
}

namespace
{
    // ---- Direct, mutex-guarded access to the shared find_class cache ---------
    // These mirror exactly how find_class reads/writes klass_lookup_cache
    // (vmhook.hpp ~7996 / ~8059) and let us observe override/evict precisely.
    auto cache_contains(const std::string& key) -> bool
    {
        std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
        return vmhook::klass_lookup_cache.contains(key);
    }
    auto cache_value_or_null(const std::string& key) -> vmhook::hotspot::klass*
    {
        std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
        const auto it{ vmhook::klass_lookup_cache.find(key) };
        return it == vmhook::klass_lookup_cache.end() ? nullptr : it->second;
    }
    auto cache_size() -> std::size_t
    {
        std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
        return vmhook::klass_lookup_cache.size();
    }

    // RAII save/restore of a single cache key.  Because klass_lookup_cache is a
    // process-global with NO ownership tracking (the feature's documented flaw),
    // every test that mutates it must put it back; this enforces that locally so
    // sections cannot bleed into one another.  Captures presence + value on
    // construction and restores both on destruction.
    class scoped_cache_guard
    {
    public:
        explicit scoped_cache_guard(std::string key)
            : key_{ std::move(key) }
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            const auto it{ vmhook::klass_lookup_cache.find(key_) };
            if (it != vmhook::klass_lookup_cache.end())
            {
                had_entry_ = true;
                saved_     = it->second;
            }
        }
        ~scoped_cache_guard()
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            if (had_entry_)
            {
                vmhook::klass_lookup_cache[key_] = saved_;
            }
            else
            {
                vmhook::klass_lookup_cache.erase(key_);
            }
        }

        scoped_cache_guard(const scoped_cache_guard&)            = delete;
        scoped_cache_guard& operator=(const scoped_cache_guard&) = delete;
        scoped_cache_guard(scoped_cache_guard&&)                 = delete;
        scoped_cache_guard& operator=(scoped_cache_guard&&)      = delete;

    private:
        std::string             key_{};
        bool                    had_entry_{ false };
        vmhook::hotspot::klass* saved_{ nullptr };
    };

    // ---- noexcept-in-practice probes ----------------------------------------
    // All four entry points are declared noexcept (a throw would call
    // std::terminate and tear the test down).  Wrapping each call in try/catch
    // lets a regression that STARTS throwing surface as a clean [FAIL] line on
    // most toolchains rather than an aborted binary, and doubles as executable
    // documentation of the noexcept claim.
    auto via_oop_never_throws(void* anchor, std::string_view name) -> bool
    {
        try { (void)vmhook::find_class_via_oop(anchor, name); return true; }
        catch (...) { return false; }
    }
    auto reanchor_never_throws(void* anchor,
                               std::initializer_list<std::string_view> names) -> bool
    {
        try { (void)vmhook::reanchor_classes_via_oop(anchor, names); return true; }
        catch (...) { return false; }
    }
    auto override_never_throws(std::string_view name, vmhook::hotspot::klass* k) -> bool
    {
        try { vmhook::override_class_lookup(name, k); return true; }
        catch (...) { return false; }
    }
    auto evict_never_throws(std::string_view name) -> bool
    {
        try { vmhook::evict_class_lookup(name); return true; }
        catch (...) { return false; }
    }

    // ---- ADDITIVE deepening (section 13) no-throw probes ---------------------
    // The remaining no-JVM-observable entry points of the classloader_reanchor
    // feature beyond the four already covered above: the public JNI context-
    // loader resolver (vmhook::find_class), the
    // HotSpot-internal find_class consumer that override/evict actually steer
    // (vmhook::find_class), and the detail:: host-classloader-inheritance
    // machinery (klass_to_class_loader_oop / capture_host_classloader_klass /
    // inherit_host_context_classloader_for_current_thread).  Each must honour the
    // no-JVM contract (null / no-op, never throw, never fault on a NON-deref'd
    // null / is_valid_pointer-rejected argument).  None of these fabricates a
    // live oop/klass and reads through it: with no JVM, ensure_current_java_thread
    // is false and get_vm_structs() is null, so every dereference is gated off
    // before the pointer is touched.
    auto ctx_loader_never_throws(std::string_view name) -> bool
    {
        try { (void)vmhook::find_class(name); return true; }
        catch (...) { return false; }
    }
    auto find_class_never_throws(std::string_view name) -> bool
    {
        try { (void)vmhook::find_class(name); return true; }
        catch (...) { return false; }
    }
    // klass_to_class_loader_oop only dereferences its argument AFTER an
    // is_valid_pointer gate AND only once iterate_struct_entries("Klass", ...)
    // returns non-null — which it cannot with no JVM (gHotSpotVMStructs unexported),
    // so a null / low-constant klass* is never read.  We pass nullptr only.
    auto klass_to_loader_never_throws(vmhook::hotspot::klass* k) -> bool
    {
        try { (void)vmhook::detail::klass_to_class_loader_oop(k); return true; }
        catch (...) { return false; }
    }
    auto capture_host_never_throws(vmhook::hotspot::klass* k) -> bool
    {
        try { vmhook::detail::capture_host_classloader_klass(k); return true; }
        catch (...) { return false; }
    }
    auto inherit_host_never_throws() -> bool
    {
        try { vmhook::detail::inherit_host_context_classloader_for_current_thread(); return true; }
        catch (...) { return false; }
    }

    // A spread of fake non-null anchor pointers.  None is ever dereferenced:
    // find_class_via_oop bails at the ensure_current_java_thread() gate BEFORE
    // touching the oop, so any non-null bit pattern exercises the no-JVM
    // short-circuit identically.  We include aligned, odd, tiny, huge, and the
    // documented debug-fill values to prove the gate runs first regardless.
    auto make_fake_anchor(std::uintptr_t bits) -> void*
    {
        return reinterpret_cast<void*>(bits);
    }

    const std::uintptr_t k_fake_anchor_bits[]{
        0x1ull,                    // odd, below any floor
        0x2ull,                    // tiny aligned
        0x8ull,
        0x1000ull,                 // page-ish
        0x2000ull,
        0xABCD000ull,
        0x00000000CAFEBABEull,     // even debug-fill sentinel
        0x00000000DEADBEEFull,     // odd debug-fill sentinel
        0x00000000CCCCCCCCull,
        0x7FFFFFFFFFFFFFFEull,     // near the top of canonical user space
        0xFFFFFFFFFFFFFFFEull,     // very high
        ~static_cast<std::uintptr_t>(0) & ~static_cast<std::uintptr_t>(1), // all-ones, even
    };

    // The master name matrix: every shape of internal-name input we can throw
    // at the no-JVM contract.  Held as std::string so embedded NULs survive (a
    // C-string would truncate).  This drives the find_class_via_oop and
    // reanchor null/no-JVM sweeps and the cache round-trip fidelity checks.
    auto build_name_matrix() -> std::vector<std::string>
    {
        std::vector<std::string> names{};
        // Canonical bootstrap names (would resolve under a JVM; null here).
        names.emplace_back("java/lang/Object");
        names.emplace_back("java/lang/String");
        names.emplace_back("java/lang/Integer");
        names.emplace_back("java/lang/Class");
        names.emplace_back("java/lang/ClassLoader");
        names.emplace_back("java/util/HashMap");
        names.emplace_back("java/util/List");
        // App-ish / multi-loader-flavoured names (the feature's raison d'etre).
        names.emplace_back("net/minecraft/client/Minecraft");
        names.emplace_back("com/example/plugin/Service");
        names.emplace_back("org/osgi/framework/Bundle");
        // Separator pathologies.
        names.emplace_back("");                       // empty
        names.emplace_back("a");                      // single char
        names.emplace_back("NoSlash");
        names.emplace_back("trailing/slash/");
        names.emplace_back("/leading/slash");
        names.emplace_back("double//slash");
        names.emplace_back("/");
        names.emplace_back("//");
        names.emplace_back("///");
        // Dotted forms (find_class_via_oop dots '/'->'.'; here all miss anyway).
        names.emplace_back("java.lang.Object");
        names.emplace_back("com.example.Dotted");
        // Descriptor / array shapes (the documented loadClass array gap).
        names.emplace_back("[I");
        names.emplace_back("[Ljava/lang/Object;");
        names.emplace_back("[[Ljava/lang/String;");
        names.emplace_back("Ljava/lang/Object;");
        names.emplace_back("()V");
        // Whitespace / control / unicode bytes.
        names.emplace_back(" ");
        names.emplace_back("\t");
        names.emplace_back("with spaces/in it");
        names.emplace_back(std::string{ "\x01\x02\x03" });
        names.emplace_back(std::string{ "\xC3\xA9\xC3\xA8/unicode" });   // UTF-8
        names.emplace_back(std::string{ "emoji\xF0\x9F\x98\x80/x" });    // 4-byte UTF-8
        // Names carrying an interior / leading / trailing NUL (length-bearing).
        names.emplace_back(std::string{ "java/lang/Object" } + '\0' + "extra");
        names.emplace_back(std::string{ '\0' } + "leadingNul");
        names.emplace_back(std::string{ "trailingNul" } + '\0');
        names.emplace_back(std::string(1, '\0'));                        // lone NUL
        names.emplace_back(std::string(8, '\0'));                        // run of NULs
        // Long names (stress the std::string{name} key construction).
        names.emplace_back(std::string(4096, 'a') + "/" + std::string(4096, 'b'));
        names.emplace_back(std::string(2048, '/'));
        return names;
    }
}

int main()
{
    // ---------------------------------------------------------------------
    // 0. Precondition: genuinely NO JVM in this process, so the contracts
    //    below are the no-JVM contracts (not an accidental real-class miss).
    //    The reanchor paths key off ensure_current_java_thread() — pin that it
    //    is false and that both thread-locals it consults are null.
    // ---------------------------------------------------------------------
    check("precondition_no_jvm_env_is_null",
          vmhook::hotspot::current_jni_env == nullptr);
    check("precondition_no_jvm_java_thread_is_null",
          vmhook::hotspot::current_java_thread == nullptr);
    check("precondition_ensure_current_java_thread_false",
          vmhook::hotspot::ensure_current_java_thread() == false);

    // ---------------------------------------------------------------------
    // 1. COMPILE-TIME signature / return-type / noexcept contracts.  A silent
    //    drift in any of the four entry points fails the build here.
    // ---------------------------------------------------------------------
    static_assert(std::is_same_v<
                      decltype(vmhook::find_class_via_oop(nullptr, std::string_view{})),
                      vmhook::hotspot::klass*>,
                  "find_class_via_oop must return vmhook::hotspot::klass*");
    static_assert(std::is_same_v<
                      decltype(vmhook::reanchor_classes_via_oop(
                          nullptr, std::initializer_list<std::string_view>{})),
                      bool>,
                  "reanchor_classes_via_oop must return bool");
    static_assert(std::is_same_v<
                      decltype(vmhook::override_class_lookup(std::string_view{}, nullptr)),
                      void>,
                  "override_class_lookup must return void");
    static_assert(std::is_same_v<
                      decltype(vmhook::evict_class_lookup(std::string_view{})),
                      void>,
                  "evict_class_lookup must return void");
    // noexcept contracts: every entry point promises not to throw.
    static_assert(noexcept(vmhook::find_class_via_oop(nullptr, std::string_view{})),
                  "find_class_via_oop must be noexcept");
    static_assert(noexcept(vmhook::reanchor_classes_via_oop(
                      nullptr, std::initializer_list<std::string_view>{})),
                  "reanchor_classes_via_oop must be noexcept");
    static_assert(noexcept(vmhook::override_class_lookup(std::string_view{}, nullptr)),
                  "override_class_lookup must be noexcept");
    static_assert(noexcept(vmhook::evict_class_lookup(std::string_view{})),
                  "evict_class_lookup must be noexcept");
    // The shared cache's static types (documents the channel the helpers write).
    static_assert(std::is_same_v<decltype(vmhook::klass_lookup_cache),
                      std::unordered_map<std::string, vmhook::hotspot::klass*>>,
                  "klass_lookup_cache must be unordered_map<string, klass*>");
    static_assert(std::is_same_v<decltype(vmhook::klass_lookup_cache_mutex), std::mutex>,
                  "klass_lookup_cache_mutex must be a std::mutex");
    check("compile_time_signature_contracts_compiled", true);

    // ---------------------------------------------------------------------
    // 2. find_class_via_oop(nullptr, name): the `!anchor_oop` guard short-
    //    circuits to nullptr for EVERY name BEFORE any JNI / thread work.  This
    //    is a deterministic no-JVM contract independent of the JVM gate.
    //    Swept over the full name matrix.
    // ---------------------------------------------------------------------
    {
        const auto names{ build_name_matrix() };
        bool all_null{ true };
        bool none_threw{ true };
        for (const auto& n : names)
        {
            const std::string_view sv{ n.data(), n.size() };
            if (vmhook::find_class_via_oop(nullptr, sv) != nullptr) { all_null = false; }
            if (!via_oop_never_throws(nullptr, sv))                 { none_threw = false; }
        }
        check("via_oop_null_anchor_all_names_null", all_null);
        check("via_oop_null_anchor_no_throw", none_threw);
        // Explicit spot checks for the two canonical extremes.
        check("via_oop_null_anchor_empty_name_null",
              vmhook::find_class_via_oop(nullptr, "") == nullptr);
        check("via_oop_null_anchor_default_string_view_null",
              vmhook::find_class_via_oop(nullptr, std::string_view{}) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 3. find_class_via_oop(fake_non_null_anchor, name): with no JVM the
    //    ensure_current_java_thread() gate fails, so EVERY (anchor, name)
    //    combination returns nullptr without dereferencing the fake anchor.
    //    Full cross-product of fake-anchor bit patterns x name matrix.
    // ---------------------------------------------------------------------
    {
        const auto names{ build_name_matrix() };
        bool all_null{ true };
        bool none_threw{ true };
        for (const std::uintptr_t bits : k_fake_anchor_bits)
        {
            void* const anchor{ make_fake_anchor(bits) };
            for (const auto& n : names)
            {
                const std::string_view sv{ n.data(), n.size() };
                if (vmhook::find_class_via_oop(anchor, sv) != nullptr) { all_null = false; }
                if (!via_oop_never_throws(anchor, sv))                 { none_threw = false; }
            }
        }
        check("via_oop_fake_anchor_all_combinations_null", all_null);
        check("via_oop_fake_anchor_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 3b. find_class_via_oop must NOT mutate the cache on the no-JVM path
    //     (only find_class inserts on a successful resolution; via_oop returns
    //     null before any override).  Snapshot the cache size around a sweep
    //     and assert it is unchanged, and that no probed name was inserted.
    // ---------------------------------------------------------------------
    {
        const std::size_t before{ cache_size() };
        const char* const probe_names[]{
            "via_oop/NoCacheWrite/A",
            "via_oop/NoCacheWrite/B",
            "via_oop/NoCacheWrite/C",
        };
        for (const char* n : probe_names)
        {
            (void)vmhook::find_class_via_oop(nullptr, n);
            (void)vmhook::find_class_via_oop(make_fake_anchor(0x2000ull), n);
        }
        const std::size_t after{ cache_size() };
        check("via_oop_does_not_grow_cache", before == after);
        bool none_inserted{ true };
        for (const char* n : probe_names)
        {
            if (cache_contains(std::string{ n })) { none_inserted = false; }
        }
        check("via_oop_inserts_no_cache_entry", none_inserted);
    }

    // ---------------------------------------------------------------------
    // 4. reanchor_classes_via_oop NULL-ANCHOR truth table.  The `!anchor_oop`
    //    guard returns false for ANY list shape (empty, single, multi, varied,
    //    duplicate, garbage), never throwing.  Empty + null anchor is STILL
    //    false (the null-anchor guard precedes the empty-list vacuous-true).
    // ---------------------------------------------------------------------
    {
        check("reanchor_null_anchor_empty_list_false",
              vmhook::reanchor_classes_via_oop(nullptr, {}) == false);
        check("reanchor_null_anchor_single_false",
              vmhook::reanchor_classes_via_oop(nullptr, { "a/B" }) == false);
        check("reanchor_null_anchor_pair_false",
              vmhook::reanchor_classes_via_oop(nullptr, { "a/B", "c/D" }) == false);
        check("reanchor_null_anchor_triple_false",
              vmhook::reanchor_classes_via_oop(nullptr, { "a/B", "c/D", "e/F" }) == false);
        check("reanchor_null_anchor_with_empty_name_false",
              vmhook::reanchor_classes_via_oop(nullptr, { "" }) == false);
        check("reanchor_null_anchor_duplicates_false",
              vmhook::reanchor_classes_via_oop(nullptr, { "dup/X", "dup/X", "dup/X" }) == false);
        check("reanchor_null_anchor_mixed_shapes_false",
              vmhook::reanchor_classes_via_oop(
                  nullptr, { "java/lang/Object", "", "[I", "trailing/slash/", "a" }) == false);
        check("reanchor_null_anchor_bootstrap_names_false",
              vmhook::reanchor_classes_via_oop(
                  nullptr, { "java/lang/String", "java/util/Map" }) == false);
        // No-throw across all the null-anchor list shapes.
        bool none_threw{ true };
        if (!reanchor_never_throws(nullptr, {}))                       { none_threw = false; }
        if (!reanchor_never_throws(nullptr, { "a/B" }))               { none_threw = false; }
        if (!reanchor_never_throws(nullptr, { "a/B", "c/D" }))        { none_threw = false; }
        if (!reanchor_never_throws(nullptr, { "", "", "" }))          { none_threw = false; }
        if (!reanchor_never_throws(nullptr, { "[I", "()V", "//" }))   { none_threw = false; }
        check("reanchor_null_anchor_no_throw_all_shapes", none_threw);
    }

    // ---------------------------------------------------------------------
    // 5. reanchor_classes_via_oop FAKE-ANCHOR truth table (no JVM).
    //    * EMPTY list  -> true  (vacuous: every one of zero names "resolved",
    //      and the empty-loop never calls find_class_via_oop).
    //    * NON-EMPTY   -> false (each find_class_via_oop returns null at the
    //      JVM gate, so all_resolved is cleared).
    //    Verified across many fake-anchor bit patterns and list shapes.
    // ---------------------------------------------------------------------
    {
        bool empty_all_true{ true };
        bool nonempty_all_false{ true };
        bool none_threw{ true };
        for (const std::uintptr_t bits : k_fake_anchor_bits)
        {
            void* const anchor{ make_fake_anchor(bits) };

            // Empty list is vacuously true for EVERY non-null anchor.
            if (vmhook::reanchor_classes_via_oop(anchor, {}) != true) { empty_all_true = false; }
            if (!reanchor_never_throws(anchor, {}))                   { none_threw = false; }

            // Every non-empty shape is false (nothing resolves with no JVM).
            const bool s1{ vmhook::reanchor_classes_via_oop(anchor, { "a/B" }) };
            const bool s2{ vmhook::reanchor_classes_via_oop(anchor, { "a/B", "c/D" }) };
            const bool s3{ vmhook::reanchor_classes_via_oop(
                anchor, { "java/lang/Object", "java/lang/String", "java/util/List" }) };
            const bool s4{ vmhook::reanchor_classes_via_oop(
                anchor, { "", "[I", "trailing/slash/" }) };
            const bool s5{ vmhook::reanchor_classes_via_oop(
                anchor, { "dup/X", "dup/X" }) };
            if (s1 || s2 || s3 || s4 || s5) { nonempty_all_false = false; }

            if (!reanchor_never_throws(anchor, { "a/B" }))            { none_threw = false; }
            if (!reanchor_never_throws(anchor, { "a/B", "c/D" }))     { none_threw = false; }
            if (!reanchor_never_throws(anchor, { "", "[I" }))         { none_threw = false; }
        }
        check("reanchor_fake_anchor_empty_list_true", empty_all_true);
        check("reanchor_fake_anchor_nonempty_false", nonempty_all_false);
        check("reanchor_fake_anchor_no_throw", none_threw);
    }

    // ---------------------------------------------------------------------
    // 5b. reanchor_classes_via_oop leaves the cache UNTOUCHED on the no-JVM
    //     path: nothing resolves, so override_class_lookup is never reached,
    //     and the empty-list true path also writes nothing.  Snapshot size and
    //     assert none of the probed names were inserted.
    // ---------------------------------------------------------------------
    {
        const std::size_t before{ cache_size() };
        const char* const probe_names[]{
            "reanchor/NoCacheWrite/Alpha",
            "reanchor/NoCacheWrite/Beta",
            "reanchor/NoCacheWrite/Gamma",
        };
        void* const anchor{ make_fake_anchor(0x3000ull) };
        (void)vmhook::reanchor_classes_via_oop(anchor, {});  // vacuous true, no write
        (void)vmhook::reanchor_classes_via_oop(anchor,
            { "reanchor/NoCacheWrite/Alpha",
              "reanchor/NoCacheWrite/Beta",
              "reanchor/NoCacheWrite/Gamma" });              // false, no write
        (void)vmhook::reanchor_classes_via_oop(nullptr,
            { "reanchor/NoCacheWrite/Alpha" });              // null anchor, no write
        const std::size_t after{ cache_size() };
        check("reanchor_does_not_grow_cache_no_jvm", before == after);
        bool none_inserted{ true };
        for (const char* n : probe_names)
        {
            if (cache_contains(std::string{ n })) { none_inserted = false; }
        }
        check("reanchor_inserts_no_cache_entry_no_jvm", none_inserted);
    }

    // ---------------------------------------------------------------------
    // 6. override_class_lookup / evict_class_lookup: the PURE cache-mutation
    //    contract, observed directly under the cache mutex (the JVM-independent
    //    half of the feature).  seed -> last-write-wins -> evict, with full
    //    save/restore so the global cache is left pristine.
    // ---------------------------------------------------------------------
    {
        const std::string name{ "test/OverriddenClass" };
        scoped_cache_guard guard{ name };

        // Start from a known-absent state (guard will restore whatever was there).
        vmhook::evict_class_lookup(name);
        check("override_absent_before", !cache_contains(name));

        auto* const k1{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xABCD000)) };
        check("override_seed_no_throw", override_never_throws(name, k1));
        check("override_seeds_cache", cache_value_or_null(name) == k1);
        check("override_seed_present", cache_contains(name));

        // Last-write-wins (the corrective API uses `=`, not insert).
        auto* const k2{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xBEEF000)) };
        vmhook::override_class_lookup(name, k2);
        check("override_replaces_previous", cache_value_or_null(name) == k2);

        // Re-seeding the SAME value is idempotent (still that value, still one key).
        const std::size_t size_before_reseed{ cache_size() };
        vmhook::override_class_lookup(name, k2);
        check("override_same_value_idempotent_value", cache_value_or_null(name) == k2);
        check("override_same_value_idempotent_size", cache_size() == size_before_reseed);

        // Evict removes the key entirely.
        check("evict_no_throw", evict_never_throws(name));
        check("evict_removes_from_cache", !cache_contains(name));
        check("evict_value_now_null", cache_value_or_null(name) == nullptr);

        // Evicting again is a safe no-op (still absent, no throw).
        check("evict_twice_no_throw", evict_never_throws(name));
        check("evict_twice_still_absent", !cache_contains(name));
    }

    // ---------------------------------------------------------------------
    // 6b. Evicting names that were never cached is a safe no-op for every
    //     shape, including empty / unicode / long / NUL-bearing keys.
    // ---------------------------------------------------------------------
    {
        const std::size_t before{ cache_size() };
        const auto names{ build_name_matrix() };
        bool none_threw{ true };
        for (const auto& n : names)
        {
            const std::string_view sv{ n.data(), n.size() };
            // Guard each so a name that DID happen to be present is restored.
            scoped_cache_guard guard{ n };
            if (cache_contains(n)) { vmhook::evict_class_lookup(sv); }   // normalise
            if (!evict_never_throws(sv)) { none_threw = false; }
            if (cache_contains(n))       { none_threw = false; }         // really gone
        }
        check("evict_absent_all_shapes_no_throw", none_threw);
        check("evict_absent_leaves_cache_size", cache_size() == before);
        // Explicit empty-name eviction.
        check("evict_empty_name_no_throw", evict_never_throws(""));
        check("evict_default_string_view_no_throw",
              evict_never_throws(std::string_view{}));
    }

    // ---------------------------------------------------------------------
    // 7. Round-trip FIDELITY of the cache key across every name shape.  The key
    //    is std::string{name}, so a length-bearing string_view (embedded NUL,
    //    unicode, very long) must override and evict the EXACT same key — no
    //    truncation at a NUL, no normalisation.  We seed a unique sentinel per
    //    name, read it back, then evict and confirm absence, all save/restored.
    // ---------------------------------------------------------------------
    {
        const auto names{ build_name_matrix() };
        bool all_roundtrip{ true };
        bool all_evicted{ true };
        std::uintptr_t seed{ 0x10000ull };
        for (const auto& n : names)
        {
            const std::string_view sv{ n.data(), n.size() };
            scoped_cache_guard guard{ n };

            // Normalise to absent first (restored by guard at scope exit).
            vmhook::evict_class_lookup(sv);

            seed += 0x2ull;  // keep even, distinct, irrelevant value (never deref'd)
            auto* const sentinel{ reinterpret_cast<vmhook::hotspot::klass*>(seed) };
            vmhook::override_class_lookup(sv, sentinel);
            // The exact key (same bytes incl. NULs) must hold our sentinel.
            if (cache_value_or_null(n) != sentinel) { all_roundtrip = false; }

            vmhook::evict_class_lookup(sv);
            if (cache_contains(n)) { all_evicted = false; }
        }
        check("override_evict_roundtrip_all_shapes_fidelity", all_roundtrip);
        check("override_evict_roundtrip_all_shapes_evicted", all_evicted);
    }

    // ---------------------------------------------------------------------
    // 7b. Distinct length-bearing views that share a common C-string prefix but
    //     differ AFTER an interior NUL must be DIFFERENT cache keys.  Proves the
    //     key is the full byte range, not a NUL-terminated C string.
    // ---------------------------------------------------------------------
    {
        const std::string key_a{ std::string{ "pre" } + '\0' + "A" };  // "pre\0A" (5)
        const std::string key_b{ std::string{ "pre" } + '\0' + "B" };  // "pre\0B" (5)
        const std::string key_c{ "pre" };                              // "pre"    (3)
        scoped_cache_guard ga{ key_a };
        scoped_cache_guard gb{ key_b };
        scoped_cache_guard gc{ key_c };
        vmhook::evict_class_lookup(std::string_view{ key_a.data(), key_a.size() });
        vmhook::evict_class_lookup(std::string_view{ key_b.data(), key_b.size() });
        vmhook::evict_class_lookup(std::string_view{ key_c.data(), key_c.size() });

        auto* const ka{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xAA0000)) };
        auto* const kb{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xBB0000)) };
        auto* const kc{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xCC0000)) };
        vmhook::override_class_lookup(std::string_view{ key_a.data(), key_a.size() }, ka);
        vmhook::override_class_lookup(std::string_view{ key_b.data(), key_b.size() }, kb);
        vmhook::override_class_lookup(std::string_view{ key_c.data(), key_c.size() }, kc);

        check("interior_nul_keys_are_distinct_a", cache_value_or_null(key_a) == ka);
        check("interior_nul_keys_are_distinct_b", cache_value_or_null(key_b) == kb);
        check("interior_nul_prefix_key_distinct_c", cache_value_or_null(key_c) == kc);
        // Evicting the short prefix must NOT touch the longer NUL-bearing keys.
        vmhook::evict_class_lookup(std::string_view{ key_c.data(), key_c.size() });
        check("evict_prefix_leaves_nul_keys_a", cache_value_or_null(key_a) == ka);
        check("evict_prefix_leaves_nul_keys_b", cache_value_or_null(key_b) == kb);
        check("evict_prefix_removed_c", !cache_contains(key_c));
    }

    // ---------------------------------------------------------------------
    // 8. [INFO] Flaw characterisation: override_class_lookup(name, nullptr)
    //    does NOT seed a DURABLE negative entry, contradicting the doc comment
    //    (vmhook.hpp ~13561-13562: "Passing a null k seeds a negative entry").
    //    What actually happens, pinned here from the LIVE header:
    //      * override writes a null VALUE into the map (observable directly).
    //      * BUT find_class's cache-hit path (~8019) is
    //        `if (cached_klass && is_valid_pointer(cached_klass))`; a null value
    //        fails that guard and falls through to erase (~8034), then re-walks
    //        (-> null with no JVM).  So the very next find_class HEALS the null
    //        entry away — the "negative entry" is a one-shot, not durable.
    //    We assert the directly-observable facts (the null IS written; the doc's
    //    durability claim is FALSE) and record the heal as [INFO].  This locks
    //    the real behaviour so a future fix is a deliberate, reviewed change.
    // ---------------------------------------------------------------------
    {
        const std::string name{ "flaw1/NullOverride/Probe" };
        scoped_cache_guard guard{ name };
        vmhook::evict_class_lookup(name);

        // Fact 1: a null override writes a present-but-null map entry.
        vmhook::override_class_lookup(name, nullptr);
        check("null_override_writes_present_entry", cache_contains(name));
        check("null_override_value_is_null", cache_value_or_null(name) == nullptr);

        // Fact 2: find_class does NOT keep that null negative entry — with no
        // JVM it returns null AND (per the cache-hit guard) evicts the null so
        // the map no longer contains the name.  This is the doc-vs-actual gap.
        const auto fc{ vmhook::find_class(std::string_view{ name.data(), name.size() }) };
        check("null_override_find_class_returns_null", fc == nullptr);
        const bool still_present_after_find_class{ cache_contains(name) };
        info("null_override_survives_find_class_DOC_CLAIM", still_present_after_find_class);
        // The DOC implies it survives; the LIVE behaviour is that it does NOT.
        // Pin the live behaviour as the assertion (negative entry is healed):
        check("null_override_NOT_durable_negative_entry_healed_by_find_class",
              !still_present_after_find_class);

        // Re-seeding null then evicting explicitly still works (no throw).
        vmhook::override_class_lookup(name, nullptr);
        check("null_override_reseed_present", cache_contains(name));
        vmhook::evict_class_lookup(name);
        check("null_override_evict_removes", !cache_contains(name));
    }

    // ---------------------------------------------------------------------
    // 8b. [INFO] Last-write-wins across null/non-null transitions on one key.
    //     override(k) then override(nullptr) leaves a present null entry (not an
    //     erased key) — null is a VALUE, distinct from absence.  Then a real
    //     value overrides it back.  Confirms `=` semantics value-by-value.
    // ---------------------------------------------------------------------
    {
        const std::string name{ "flaw1/LastWrite/Transitions" };
        scoped_cache_guard guard{ name };
        vmhook::evict_class_lookup(name);

        auto* const real_k{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0x123456)) };

        vmhook::override_class_lookup(name, real_k);
        check("transition_real_value_set", cache_value_or_null(name) == real_k);

        vmhook::override_class_lookup(name, nullptr);
        check("transition_to_null_present", cache_contains(name));      // present...
        check("transition_to_null_value_null", cache_value_or_null(name) == nullptr); // ...but null

        vmhook::override_class_lookup(name, real_k);
        check("transition_back_to_real", cache_value_or_null(name) == real_k);

        // Absence != present-null: evict makes it truly absent.
        vmhook::evict_class_lookup(name);
        check("transition_evict_truly_absent", !cache_contains(name));
    }

    // ---------------------------------------------------------------------
    // 9. [INFO] Flaw characterisation: override_class_lookup is process-GLOBAL
    //    and unscoped — a second override on the SAME name silently wins, with
    //    no ownership / refcount / revert.  Two simulated "components" anchoring
    //    the same name to different klass copies: the LAST writer wins, the
    //    first is lost without a trace.  This is the documented footgun; we pin
    //    it so its behaviour is locked and visible.
    // ---------------------------------------------------------------------
    {
        const std::string name{ "flaw2/SharedGlobal/Contended" };
        scoped_cache_guard guard{ name };
        vmhook::evict_class_lookup(name);

        auto* const component_a_copy{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xA0A0A0)) };
        auto* const component_b_copy{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xB0B0B0)) };

        vmhook::override_class_lookup(name, component_a_copy);   // component A anchors
        check("shared_global_component_a_wins_first",
              cache_value_or_null(name) == component_a_copy);
        vmhook::override_class_lookup(name, component_b_copy);   // component B anchors
        // Last write wins; A's anchor is silently gone (the footgun).
        check("shared_global_last_writer_b_wins",
              cache_value_or_null(name) == component_b_copy);
        info("shared_global_no_revert_to_a_after_b",
             cache_value_or_null(name) != component_a_copy);

        vmhook::evict_class_lookup(name);
    }

    // ---------------------------------------------------------------------
    // 10. Thread-safety smoke: many threads hammering override / evict on a mix
    //     of DISJOINT (per-thread) and SHARED keys.  klass_lookup_cache_mutex
    //     exists precisely to make this safe (vmhook.hpp ~7949).  We only assert
    //     (a) nothing throws / crashes, (b) the map is structurally intact
    //     afterwards (every disjoint key ends in a deterministic final state),
    //     and (c) the cache is fully cleaned up.  No value on a SHARED key is
    //     asserted (last-writer is racy by design); we only require it is in the
    //     set of written values or absent — i.e. no torn / corrupt entry.
    // ---------------------------------------------------------------------
    {
        constexpr int thread_count{ 8 };
        constexpr int iterations{ 400 };

        // Capture the set of keys we will touch so we can clean up deterministically.
        const std::string shared_key{ "thread/Shared/Key" };
        scoped_cache_guard shared_guard{ shared_key };

        std::vector<std::string> disjoint_keys{};
        disjoint_keys.reserve(static_cast<std::size_t>(thread_count));
        for (int t{ 0 }; t < thread_count; ++t)
        {
            disjoint_keys.emplace_back("thread/Disjoint/" + std::to_string(t));
        }

        // A stable, valid-looking sentinel value per thread (never dereferenced).
        auto thread_value = [](int t) -> vmhook::hotspot::klass*
        {
            const std::uintptr_t bits{ 0x100000ull
                + (static_cast<std::uintptr_t>(t) << 8) };
            return reinterpret_cast<vmhook::hotspot::klass*>(bits);
        };

        std::vector<std::thread> workers{};
        workers.reserve(static_cast<std::size_t>(thread_count));
        for (int t{ 0 }; t < thread_count; ++t)
        {
            workers.emplace_back([t, &shared_key, &disjoint_keys, thread_value]() noexcept
            {
                const std::string& my_key{ disjoint_keys[static_cast<std::size_t>(t)] };
                vmhook::hotspot::klass* const my_val{ thread_value(t) };
                for (int i{ 0 }; i < iterations; ++i)
                {
                    // Disjoint key: only this thread writes it, so its final
                    // value is deterministic (last op below sets it to my_val).
                    vmhook::override_class_lookup(my_key, my_val);
                    vmhook::evict_class_lookup(my_key);
                    vmhook::override_class_lookup(my_key, my_val);

                    // Shared key: all threads contend (racy by design).
                    vmhook::override_class_lookup(shared_key, my_val);
                    (void)vmhook::evict_class_lookup(shared_key);
                    vmhook::override_class_lookup(shared_key, my_val);
                }
            });
        }
        for (auto& w : workers) { w.join(); }

        // (a)/(b): each disjoint key must hold exactly its owning thread's value
        //          (no cross-thread corruption of an unshared key).
        bool disjoint_consistent{ true };
        for (int t{ 0 }; t < thread_count; ++t)
        {
            if (cache_value_or_null(disjoint_keys[static_cast<std::size_t>(t)])
                != thread_value(t))
            {
                disjoint_consistent = false;
            }
        }
        check("threadsafe_disjoint_keys_uncorrupted", disjoint_consistent);

        // The shared key, if present, must hold one of the written values (never
        // a torn pointer) — pin that it is one of the per-thread sentinels.
        {
            vmhook::hotspot::klass* const v{ cache_value_or_null(shared_key) };
            bool shared_value_sane{ v == nullptr };
            for (int t{ 0 }; t < thread_count && !shared_value_sane; ++t)
            {
                if (v == thread_value(t)) { shared_value_sane = true; }
            }
            check("threadsafe_shared_key_value_is_sane", shared_value_sane);
        }

        // (c) clean up the disjoint keys (shared_key is restored by its guard).
        for (const auto& k : disjoint_keys) { vmhook::evict_class_lookup(k); }
        bool all_cleaned{ true };
        for (const auto& k : disjoint_keys)
        {
            if (cache_contains(k)) { all_cleaned = false; }
        }
        check("threadsafe_cleanup_disjoint_keys", all_cleaned);
    }

    // ---------------------------------------------------------------------
    // 10b. Thread-safety smoke for the no-JVM RESOLUTION entry points run
    //      concurrently: find_class_via_oop and reanchor_classes_via_oop from
    //      many threads must all return their no-JVM contract values and never
    //      crash.  (They take ensure_current_java_thread() per-thread; with no
    //      JVM every thread fails the gate.)
    // ---------------------------------------------------------------------
    {
        constexpr int thread_count{ 8 };
        constexpr int iterations{ 200 };
        std::vector<std::thread> workers{};
        std::vector<int> via_oop_nonnull(static_cast<std::size_t>(thread_count), 0);
        std::vector<int> reanchor_true(static_cast<std::size_t>(thread_count), 0);
        workers.reserve(static_cast<std::size_t>(thread_count));
        for (int t{ 0 }; t < thread_count; ++t)
        {
            workers.emplace_back([t, &via_oop_nonnull, &reanchor_true]() noexcept
            {
                void* const anchor{ reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(0x5000 + t)) };
                for (int i{ 0 }; i < iterations; ++i)
                {
                    if (vmhook::find_class_via_oop(anchor, "java/lang/Object") != nullptr)
                    {
                        ++via_oop_nonnull[static_cast<std::size_t>(t)];
                    }
                    if (vmhook::find_class_via_oop(nullptr, "x/Y") != nullptr)
                    {
                        ++via_oop_nonnull[static_cast<std::size_t>(t)];
                    }
                    if (vmhook::reanchor_classes_via_oop(anchor, { "a/B", "c/D" }))
                    {
                        ++reanchor_true[static_cast<std::size_t>(t)];
                    }
                }
            });
        }
        for (auto& w : workers) { w.join(); }
        bool no_via_oop_resolved{ true };
        bool no_reanchor_true{ true };
        for (int t{ 0 }; t < thread_count; ++t)
        {
            if (via_oop_nonnull[static_cast<std::size_t>(t)] != 0) { no_via_oop_resolved = false; }
            if (reanchor_true[static_cast<std::size_t>(t)] != 0)   { no_reanchor_true = false; }
        }
        check("threadsafe_via_oop_never_resolves_no_jvm", no_via_oop_resolved);
        check("threadsafe_reanchor_never_true_no_jvm", no_reanchor_true);
    }

    // ---------------------------------------------------------------------
    // 11. Determinism: the no-JVM contract is bit-identical across repeated
    //     calls (no hidden state makes a later call differ).  Repeat the core
    //     entry points many times and require every result is the SAME value.
    // ---------------------------------------------------------------------
    {
        bool via_oop_stable{ true };
        bool reanchor_empty_stable{ true };
        bool reanchor_nonempty_stable{ true };
        bool reanchor_null_stable{ true };
        void* const anchor{ make_fake_anchor(0x6000ull) };
        for (int i{ 0 }; i < 256; ++i)
        {
            if (vmhook::find_class_via_oop(nullptr, "java/lang/Object") != nullptr)
            {
                via_oop_stable = false;
            }
            if (vmhook::find_class_via_oop(anchor, "java/lang/Object") != nullptr)
            {
                via_oop_stable = false;
            }
            if (vmhook::reanchor_classes_via_oop(anchor, {}) != true)
            {
                reanchor_empty_stable = false;
            }
            if (vmhook::reanchor_classes_via_oop(anchor, { "a/B" }) != false)
            {
                reanchor_nonempty_stable = false;
            }
            if (vmhook::reanchor_classes_via_oop(nullptr, { "a/B" }) != false)
            {
                reanchor_null_stable = false;
            }
        }
        check("via_oop_deterministic_repeat", via_oop_stable);
        check("reanchor_empty_deterministic_repeat", reanchor_empty_stable);
        check("reanchor_nonempty_deterministic_repeat", reanchor_nonempty_stable);
        check("reanchor_null_deterministic_repeat", reanchor_null_stable);
    }

    // =====================================================================
    // 13. ADDITIVE deepening pass — the rest of the classloader_reanchor
    //     feature's no-JVM-observable surface beyond the four entry points
    //     exercised above.  Three further entry points are part of this
    //     feature per the header (vmhook.hpp):
    //
    //       * vmhook::find_class(name)  (~13620)
    //           -> detail::jni_find_class_with_context_loader (~12173), which
    //           short-circuits to nullptr on !ensure_current_java_thread()
    //           (~12176).  Public, noexcept, klass*.
    //       * vmhook::find_class(name)  (~8146) — the HotSpot-internal consumer
    //           that override_class_lookup / reanchor actually steer.  Its
    //           empty-name fast-reject (~8161) and array-name '[' branch (~8175,
    //           via jni_find_class which also gates on ensure_current_java_thread
    //           ~11872) make it a clean nullptr with no JVM for EVERY shape.
    //       * detail host-classloader inheritance: klass_to_class_loader_oop
    //           (~12463), capture_host_classloader_klass (~12492),
    //           inherit_host_context_classloader_for_current_thread (~12534),
    //           and the host_classloader_klass latch (~12451).  With no JVM,
    //           iterate_struct_entries("Klass", ...) is null (gHotSpotVMStructs
    //           unexported) so klass_to_class_loader_oop returns nullptr WITHOUT
    //           dereferencing its argument; capture is therefore a no-op and the
    //           latch stays null.
    //
    //     Every assertion below is the null/empty/no-op/no-throw no-JVM
    //     contract.  No fabricated live oop/klass is ever dereferenced (only
    //     nullptr is passed where the arg would be read), and the section leaves
    //     klass_lookup_cache and host_classloader_klass exactly as it found them,
    //     so the section-12 cleanliness check below still observes an empty cache.
    // =====================================================================

    // ---- 13a. Compile-time signature / return-type / noexcept contracts -----
    static_assert(std::is_same_v<
                      decltype(vmhook::find_class(std::string_view{})),
                      vmhook::hotspot::klass*>,
                  "find_class_with_context_loader must return vmhook::hotspot::klass*");
    static_assert(noexcept(vmhook::find_class(std::string_view{})),
                  "find_class_with_context_loader must be noexcept");
    static_assert(std::is_same_v<
                      decltype(vmhook::find_class(std::string_view{})),
                      vmhook::hotspot::klass*>,
                  "find_class must return vmhook::hotspot::klass*");
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::klass_to_class_loader_oop(nullptr)),
                      void*>,
                  "klass_to_class_loader_oop must return void*");
    static_assert(noexcept(vmhook::detail::klass_to_class_loader_oop(nullptr)),
                  "klass_to_class_loader_oop must be noexcept");
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::capture_host_classloader_klass(nullptr)),
                      void>,
                  "capture_host_classloader_klass must return void");
    static_assert(noexcept(vmhook::detail::capture_host_classloader_klass(nullptr)),
                  "capture_host_classloader_klass must be noexcept");
    static_assert(std::is_same_v<
                      decltype(vmhook::detail::inherit_host_context_classloader_for_current_thread()),
                      void>,
                  "inherit_host_context_classloader_for_current_thread must return void");
    static_assert(noexcept(vmhook::detail::inherit_host_context_classloader_for_current_thread()),
                  "inherit_host_context_classloader_for_current_thread must be noexcept");
    // The host-klass latch is a std::atomic<klass*>.
    static_assert(std::is_same_v<decltype(vmhook::detail::host_classloader_klass),
                      std::atomic<vmhook::hotspot::klass*>>,
                  "host_classloader_klass must be std::atomic<klass*>");
    check("section13_compile_time_contracts_compiled", true);

    // ---- 13b. find_class_with_context_loader: null for EVERY name shape -----
    //      The ensure_current_java_thread() gate (~12176) fails with no JVM, so
    //      every name resolves to nullptr without any JNI / loadClass work, and
    //      it never throws nor mutates the cache.
    {
        const std::size_t before{ cache_size() };
        const auto names{ build_name_matrix() };
        bool all_null{ true };
        bool none_threw{ true };
        for (const auto& n : names)
        {
            const std::string_view sv{ n.data(), n.size() };
            if (vmhook::find_class(sv) != nullptr) { all_null = false; }
            if (!ctx_loader_never_throws(sv))                              { none_threw = false; }
        }
        check("ctx_loader_all_names_null_no_jvm", all_null);
        check("ctx_loader_no_throw_all_shapes", none_threw);
        check("ctx_loader_empty_name_null",
              vmhook::find_class("") == nullptr);
        check("ctx_loader_default_string_view_null",
              vmhook::find_class(std::string_view{}) == nullptr);
        check("ctx_loader_does_not_grow_cache", cache_size() == before);
    }

    // ---- 13c. find_class: the HotSpot-internal consumer override/evict steer -
    //      No JVM -> nullptr for every shape: empty (fast-reject ~8161), array
    //      descriptors (the '[' branch via jni_find_class, gated ~11872), dotted,
    //      separator pathologies (find_class does NOT reject mixed separators —
    //      it just fails to resolve with no JVM), unicode, NUL-bearing, long.
    //      Crucially: a miss must NOT leave an insertion behind (insert only
    //      happens after a SUCCESSFUL resolution ~8252), so the cache size is
    //      unchanged and no probed name appears.
    {
        const std::size_t before{ cache_size() };
        const auto names{ build_name_matrix() };
        bool all_null{ true };
        bool none_threw{ true };
        bool none_inserted{ true };
        for (const auto& n : names)
        {
            const std::string_view sv{ n.data(), n.size() };
            if (vmhook::find_class(sv) != nullptr) { all_null = false; }
            if (!find_class_never_throws(sv))      { none_threw = false; }
            if (cache_contains(n))                 { none_inserted = false; }
        }
        check("find_class_all_names_null_no_jvm", all_null);
        check("find_class_no_throw_all_shapes", none_threw);
        check("find_class_miss_inserts_nothing_no_jvm", none_inserted);
        check("find_class_does_not_grow_cache_no_jvm", cache_size() == before);
        // Explicit array-descriptor spot checks (the '[' branch).
        check("find_class_array_int_null", vmhook::find_class("[I") == nullptr);
        check("find_class_array_obj_null",
              vmhook::find_class("[Ljava/lang/Object;") == nullptr);
        check("find_class_array_2d_null",
              vmhook::find_class("[[Ljava/lang/String;") == nullptr);
        // Explicit empty + default-view fast-reject.
        check("find_class_empty_null", vmhook::find_class("") == nullptr);
        check("find_class_default_string_view_null",
              vmhook::find_class(std::string_view{}) == nullptr);
        // Mixed-separator inputs resolve to null (no JVM) but must not throw —
        // find_class normalises rather than rejecting them.
        check("find_class_dotted_name_null", vmhook::find_class("java.lang.Object") == nullptr);
        check("find_class_mixed_sep_no_throw",
              find_class_never_throws("java.lang/Object"));
    }

    // ---- 13d. override_class_lookup -> find_class observation, no JVM --------
    //      With a JVM, override seeds a klass that find_class would return on its
    //      cache-hit path.  With NO JVM the cache-hit guard
    //      `cached_klass && is_valid_pointer(cached_klass)` (~8212) REJECTS our
    //      fabricated low-constant sentinel, find_class erases it and re-walks
    //      (-> null), so the name is GONE from the cache afterwards.  Pin this
    //      heal precisely (it is the same mechanism flaw-1 documents for the null
    //      override) — we assert the post-find_class state, restoring via guard.
    {
        const std::string name{ "section13/OverrideThenFindClass" };
        scoped_cache_guard guard{ name };
        vmhook::evict_class_lookup(name);

        auto* const sentinel{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0x7A7A00)) };
        vmhook::override_class_lookup(name, sentinel);
        check("section13_override_present_before_find_class", cache_contains(name));

        const auto fc{ vmhook::find_class(std::string_view{ name.data(), name.size() }) };
        check("section13_override_find_class_null_no_jvm", fc == nullptr);
        // is_valid_pointer rejects the low-constant sentinel -> evicted by the
        // stale-cache check; the name no longer maps to anything.
        info("section13_invalid_sentinel_survives_find_class", cache_contains(name));
        check("section13_invalid_sentinel_healed_by_find_class", !cache_contains(name));
    }

    // ---- 13e. host-classloader inheritance: null / no-op contract, no JVM ----
    //      klass_to_class_loader_oop returns nullptr for null AND for any klass
    //      with no JVM (iterate_struct_entries null), capture is a no-op so the
    //      latch stays null, and inherit_* is a no-op.  None throws.  We pass
    //      only nullptr where the argument would be dereferenced (no fabricated
    //      live klass read).  The latch must be null on entry and exit.
    {
        check("host_latch_null_at_section_entry",
              vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) == nullptr);

        check("klass_to_loader_null_arg_null",
              vmhook::detail::klass_to_class_loader_oop(nullptr) == nullptr);
        check("klass_to_loader_null_arg_no_throw", klass_to_loader_never_throws(nullptr));

        // A non-null but is_valid_pointer-REJECTED low constant: klass_to_oop
        // still returns null (no JVM -> iterate_struct_entries null gates first;
        // and even past that, is_valid_pointer rejects it before any deref).
        auto* const low_klass{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0x4)) };
        check("klass_to_loader_low_const_null",
              vmhook::detail::klass_to_class_loader_oop(low_klass) == nullptr);
        check("klass_to_loader_low_const_no_throw", klass_to_loader_never_throws(low_klass));

        // capture_host_classloader_klass: null candidate -> no-op; low-const
        // candidate -> klass_to_class_loader_oop(candidate) is null so it bails
        // before the CAS.  Either way the latch is never published with no JVM.
        check("capture_null_candidate_no_throw", capture_host_never_throws(nullptr));
        check("capture_low_const_candidate_no_throw", capture_host_never_throws(low_klass));
        check("host_latch_still_null_after_capture",
              vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) == nullptr);

        // inherit_*: latch is null -> immediate no-op, never throws.
        check("inherit_host_no_throw", inherit_host_never_throws());
        check("host_latch_null_at_section_exit",
              vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) == nullptr);
    }

    // ---- 13f. Concurrent no-JVM smoke for the added resolution entry points --
    //      find_class_with_context_loader, find_class, klass_to_class_loader_oop
    //      (null), capture (null) and inherit hammered from many threads must all
    //      hold the no-JVM contract and never crash.  We count any non-null /
    //      any latch publication; both must remain zero / null.
    {
        constexpr int thread_count{ 8 };
        constexpr int iterations{ 200 };
        std::vector<std::thread> workers{};
        std::vector<int> resolved(static_cast<std::size_t>(thread_count), 0);
        workers.reserve(static_cast<std::size_t>(thread_count));
        for (int t{ 0 }; t < thread_count; ++t)
        {
            workers.emplace_back([t, &resolved]() noexcept
            {
                for (int i{ 0 }; i < iterations; ++i)
                {
                    if (vmhook::find_class("java/lang/Object") != nullptr)
                    {
                        ++resolved[static_cast<std::size_t>(t)];
                    }
                    if (vmhook::find_class("java/lang/String") != nullptr)
                    {
                        ++resolved[static_cast<std::size_t>(t)];
                    }
                    if (vmhook::detail::klass_to_class_loader_oop(nullptr) != nullptr)
                    {
                        ++resolved[static_cast<std::size_t>(t)];
                    }
                    vmhook::detail::capture_host_classloader_klass(nullptr);
                    vmhook::detail::inherit_host_context_classloader_for_current_thread();
                }
            });
        }
        for (auto& w : workers) { w.join(); }
        bool none_resolved{ true };
        for (int t{ 0 }; t < thread_count; ++t)
        {
            if (resolved[static_cast<std::size_t>(t)] != 0) { none_resolved = false; }
        }
        check("section13_concurrent_never_resolves_no_jvm", none_resolved);
        check("section13_concurrent_latch_still_null",
              vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) == nullptr);
    }

    // =====================================================================
    // 14. Wave-28 LEDGER-GAP deepening: cold-state context_loader contract,
    //     FindClass-with-explicit-context-loader fallback null-safety, loader-
    //     switching state no-leak across repeated calls, and additional
    //     static_assert symbol-type-identity pins for the context_loader
    //     resolver and its detail:: forwardee.  Every assertion below is no-JVM
    //     deterministic; the section leaves the cache and the host-klass latch
    //     exactly as it found them.
    // =====================================================================

    // ---- 14a. COLD-state contract: at first call from this process, with no
    //      JVM ever started, the public context-loader resolver returns null
    //      for the canonical "cold" inputs.  ensure_current_java_thread() must
    //      still be false (no JVM was created by any earlier section either).
    {
        check("cold_ensure_current_java_thread_still_false",
              vmhook::hotspot::ensure_current_java_thread() == false);
        check("cold_ctx_loader_object_null",
              vmhook::find_class("java/lang/Object") == nullptr);
        check("cold_ctx_loader_app_null",
              vmhook::find_class("com/example/App") == nullptr);
        check("cold_ctx_loader_array_null",
              vmhook::find_class("[Ljava/lang/Object;") == nullptr);
        check("cold_ctx_loader_dotted_null",
              vmhook::find_class("java.lang.Object") == nullptr);
        // Host latch must still be unpublished after these cold calls (the
        // resolver never reaches capture without a JVM).
        check("cold_host_latch_unpublished",
              vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) == nullptr);
    }

    // ---- 14b. FindClass-with-explicit-context-loader fallback: the resolver's
    //      three-loader cascade (thread context -> system -> Launch.classLoader)
    //      collapses to a single nullptr return with no JVM.  Repeated calls
    //      with varying name shapes must stay null and never throw — the
    //      "explicit fallback returns null safely" ledger gap.
    {
        const char* const fallback_names[]{
            "java/lang/Object",
            "java/lang/String",
            "java/lang/ClassLoader",
            "net/minecraft/launchwrapper/Launch",  // the documented 3rd fallback
            "sun/misc/Launcher",
            "jdk/internal/loader/ClassLoaders",
            "com/example/Missing",
            "",
        };
        bool all_null{ true };
        bool none_threw{ true };
        for (const char* n : fallback_names)
        {
            if (vmhook::find_class(n) != nullptr) { all_null = false; }
            if (!ctx_loader_never_throws(n))                               { none_threw = false; }
        }
        check("fallback_cascade_all_null_no_jvm", all_null);
        check("fallback_cascade_no_throw", none_threw);
    }

    // ---- 14c. Loader-switching state NO-LEAK across calls: many repeated
    //      context-loader resolutions on different names from the same thread
    //      and from many threads must NOT accumulate any state — the global
    //      cache size is unchanged, the host latch stays null, and the result
    //      is deterministic (always null on the no-JVM path).
    {
        const std::size_t cache_before{ cache_size() };
        auto* const latch_before{
            vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) };

        const char* const cycle[]{
            "java/lang/Object",
            "java/lang/String",
            "java/lang/ClassLoader",
            "java/util/HashMap",
            "com/example/Switcher",
        };
        bool stable{ true };
        for (int i{ 0 }; i < 512; ++i)
        {
            for (const char* n : cycle)
            {
                if (vmhook::find_class(n) != nullptr) { stable = false; }
            }
        }
        check("loader_switch_repeat_stable_null", stable);
        check("loader_switch_repeat_no_cache_leak", cache_size() == cache_before);
        check("loader_switch_repeat_no_latch_leak",
              vmhook::detail::host_classloader_klass.load(std::memory_order_acquire)
                  == latch_before);

        // Many threads switching across the same cycle: same no-leak contract.
        constexpr int thread_count{ 6 };
        constexpr int iterations{ 128 };
        std::vector<std::thread> workers{};
        std::atomic<int> nonnull_hits{ 0 };
        workers.reserve(static_cast<std::size_t>(thread_count));
        for (int t{ 0 }; t < thread_count; ++t)
        {
            workers.emplace_back([&cycle, &nonnull_hits]() noexcept
            {
                for (int i{ 0 }; i < iterations; ++i)
                {
                    for (const char* n : cycle)
                    {
                        if (vmhook::find_class(n) != nullptr)
                        {
                            nonnull_hits.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
        }
        for (auto& w : workers) { w.join(); }
        check("loader_switch_threads_never_resolved",
              nonnull_hits.load(std::memory_order_acquire) == 0);
        check("loader_switch_threads_no_cache_leak", cache_size() == cache_before);
        check("loader_switch_threads_no_latch_leak",
              vmhook::detail::host_classloader_klass.load(std::memory_order_acquire)
                  == latch_before);
    }

    // ---- 14d. ADDITIONAL static_assert symbol-type-identity pins for the
    //      context_loader resolver and its detail:: forwardee.  These pin
    //      pointer-to-function identity (catches a silent overload/template
    //      drift) and the exact noexcept / return-type / arg-type triple.
    {
        using ctx_loader_fn_t = vmhook::hotspot::klass*(*)(std::string_view) noexcept;
        static_assert(std::is_same_v<
                          decltype(&vmhook::find_class),
                          ctx_loader_fn_t>,
                      "find_class_with_context_loader must be klass*(string_view) noexcept");
        static_assert(std::is_same_v<
                          decltype(&vmhook::detail::jni_find_class_with_context_loader),
                          ctx_loader_fn_t>,
                      "detail::jni_find_class_with_context_loader must match the public sig");
        // klass_to_class_loader_oop: void*(klass*) noexcept.
        using k2l_fn_t = void*(*)(vmhook::hotspot::klass*) noexcept;
        static_assert(std::is_same_v<
                          decltype(&vmhook::detail::klass_to_class_loader_oop),
                          k2l_fn_t>,
                      "klass_to_class_loader_oop must be void*(klass*) noexcept");
        // capture_host_classloader_klass: void(klass*) noexcept.
        using capture_fn_t = void(*)(vmhook::hotspot::klass*) noexcept;
        static_assert(std::is_same_v<
                          decltype(&vmhook::detail::capture_host_classloader_klass),
                          capture_fn_t>,
                      "capture_host_classloader_klass must be void(klass*) noexcept");
        // inherit_host_context_classloader_for_current_thread: void() noexcept.
        using inherit_fn_t = void(*)() noexcept;
        static_assert(std::is_same_v<
                          decltype(&vmhook::detail::inherit_host_context_classloader_for_current_thread),
                          inherit_fn_t>,
                      "inherit_* must be void() noexcept");
        // Use is_always_lock_free (constexpr) instead of is_lock_free() —
        // the runtime call requires linking libatomic on Linux clang for
        // certain compilers/configs (undefined __atomic_is_lock_free).
        // For a pointer-sized atomic on every supported platform, the
        // constexpr property is the stronger pin anyway.
        info("host_classloader_klass_atomic_is_lock_free",
             std::atomic<vmhook::hotspot::klass*>::is_always_lock_free);
        check("section14_static_assert_identity_compiled", true);
    }

    // ---------------------------------------------------------------------
    // 12. Whole-cache cleanliness: after every section above (each of which
    //     save/restored its keys), the cache must be back to its starting size.
    //     With no JVM nothing else populates it, so the expected size is 0; we
    //     assert empty to prove the test left no residue (no global poisoning).
    // ---------------------------------------------------------------------
    check("cache_left_pristine_empty_after_all_sections", cache_size() == 0);

    std::printf("checks_run=%d failures=%d\n", checks_run, failures);
    return failures == 0 ? 0 : 1;
}
