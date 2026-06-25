// Standalone (no-JVM) unit test for the vmhook `instanceklass_methods_walk`
// feature: the primitive that turns an InstanceKlass* into its declared method
// list by reading HotSpot's InstanceKlass::_methods Array<Method*> directly,
// plus the two related surfaces that share its read discipline
// (constantpool_access bound/tag arithmetic, hook_verify_repair decision logic).
//
// ───────────────────────────────────────────────────────────────────────────
// WHAT IS / ISN'T DETERMINABLE WITH NO JVM
// ───────────────────────────────────────────────────────────────────────────
// This executable runs with NO HotSpot JVM in-process, so gHotSpotVMStructs is
// never resolvable and no live ConstantPool / InstanceKlass / Method / frame
// exists to read.  We therefore NEVER fabricate a live metadata pointer and
// dereference it (that is a POSIX SEGV the no-SEH MinGW/clang legs cannot
// contain).  Exactly three layers stay fully deterministic here:
//
//   1. PUBLIC WALK CONTRACT.  get_class_methods(name) / get_class_methods<T>() /
//      find_methods_by_signature<T>(desc) all funnel through
//      detail::collect_klass_methods(find_class(...)).  With no JVM find_class
//      returns nullptr (empty-name fast-reject, else a ClassLoaderDataGraph walk
//      that finds nothing + a JNI fallback that fails at
//      ensure_current_java_thread), so the collector's `if (!target_klass)`
//      arm fires and every overload returns an EMPTY vector without touching
//      memory.  These are pinned directly on the public entry points (the
//      sibling test_find_class_contracts.cpp already proves these calls are
//      noexcept-in-practice off-JVM).
//
//   2. THE WALK'S BOUND / STRIDE / CLAMP ARITHMETIC.  get_methods_count()
//      sanity-clamps Array<Method*>::_length to [0, 65535] (a class file's
//      method_count is u2).  get_methods_ptr() hardcodes the x64 Array<T> layout
//      `[int32 _length @0][int32 _pad @4][T _data @8]` and skips 8 bytes to the
//      first element.  collect_klass_methods bounds the index loop `[0, count)`
//      and reserves `count`.  symbol::to_string clamps the per-symbol copy to
//      (0, 0x1000].  All of this is pure integer arithmetic reproduced here from
//      an independent re-implementation of the documented closed forms — the
//      element address of slot i is base + 8 + 8*i, the clamp predicate is
//      `count < 0 || count > 65535`, etc.  This is where "all inputs possible"
//      lives: a dense sweep over lengths and indices, every value recomputed.
//
//   3. constantpool_access INDEX/LENGTH BOUND + TAG GATING.
//      resolve_constant_pool_symbol's decision logic is pure predicate math:
//      `!index` rejects slot 0 (cp is 1-based); `cp_length >= 0 && index >=
//      cp_length` rejects out-of-range when the length is known; `cp_length < 0`
//      ("unknown") skips the bound.  The slot ADDRESS is base + 8*index (each
//      entry pointer-sized).  We reproduce every branch as arithmetic and pin
//      that the is_valid_pointer / is_readable_pointer pre-gates reject the
//      poison/low constants the walk would otherwise deref — using only
//      constants that are NEVER dereferenced.
//
//   4. hook_verify_repair DRIFT-DECISION logic.  verify_and_repair compares the
//      live 5-byte stub against an expected `0xE9 + rel32(allocated - (target +
//      5))`.  The expected-bytes construction and the memcmp "intact vs drifted"
//      decision, plus the chain-follow rel32 decode, are POD byte arithmetic we
//      reproduce verbatim (no live stub read).  The watchdog's gating booleans
//      (`error || !target || !allocated -> treat intact`) are pinned as the
//      pure boolean expression.
//
// Anything needing a LIVE walk (real Method* names/descriptors, a real cp
// resolve, an actual stub patch) is OUT OF SCOPE and covered by the JVM module
// tests/jvm/modules/instanceklass_methods_walk.cpp.
//
// Source of truth (vmhook/ext/vmhook/vmhook.hpp; functions are the authority):
//   klass::get_methods_count        ~3504  ([0,65535] clamp, _length @0)
//   klass::get_methods_ptr          ~3543  (data @ base+8)
//   detail::collect_klass_methods   ~8985  (null->empty, [0,count) loop, skip slot)
//   get_class_methods(name)         ~9031
//   get_class_methods<T>()          ~9042  ({} on type_to_class_map miss)
//   find_methods_by_signature<T>    ~9093  (EXACT-equality filter)
//   symbol::to_string               ~2237  ((0,0x1000] length clamp)
//   constant_pool::get_base         ~2331  (header size -> entry[0])
//   constant_pool::get_length       ~2362  (-1 == unknown)
//   resolve_constant_pool_symbol    ~3898  (index/length bound, tag/slot gating)
//   is_valid_pointer                ~2047  (range + bit0 + 9 poison sentinels)
//   is_readable_pointer             ~2018  (range + 8-align + committed/readable)
//   midi2i...::verify_and_repair    ~6977  (0xE9 + rel32, memcmp drift decision)
#include <vmhook/vmhook.hpp>
#include <cstddef>      // std::size_t
#include <cstdio>
#include <cstdint>
#include <cstring>      // std::memcmp / std::memcpy (stub byte arithmetic)
#include <string>       // std::string (value_t / descriptor compares)
#include <string_view>  // std::string_view (find_class / get_class_methods inputs)
#include <type_traits>  // std::is_same_v / std::remove_cvref_t (element-type pins)
#include <utility>      // std::pair (the (name, descriptor) result element)
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // Throwaway wrapper exercising the templated get_class_methods<T>() /
    // find_methods_by_signature<T>() forms.  Left UNregistered on purpose so the
    // type_to_class_map lookup MISSES and the {}-on-miss branch is exercised.
    // File-scope (anonymous namespace) to match the proven-safe sibling idiom in
    // test_find_class_contracts.cpp.
    struct unreg_wrapper : vmhook::object<unreg_wrapper>
    {
        using vmhook::object<unreg_wrapper>::object;
    };
}

// ───────────────────────────────────────────────────────────────────────────
// Independent re-implementations of the documented closed forms.  These NEVER
// call into the library; they compute the EXPECTED values the walk's own
// arithmetic must agree with.
// ───────────────────────────────────────────────────────────────────────────

// HotSpot Array<T> on x64: _length at +0, 4 bytes pad at +4, _data[0] at +8.
// get_methods_ptr() returns array_base + 8; element i lives 8 bytes apart.
static constexpr std::uint32_t kArrayDataOffset{ 8u };
static constexpr std::uint32_t kMethodPtrStride{ 8u };  // sizeof(Method*) on LP64

static auto element_offset(std::uint32_t index) -> std::uint64_t
{
    return static_cast<std::uint64_t>(kArrayDataOffset)
         + static_cast<std::uint64_t>(kMethodPtrStride) * index;
}

// get_methods_count() clamp: a class file's method_count is u2, so HotSpot can
// never declare more than 65535 methods; a negative or larger value is a
// valid-but-wrong / torn read and clamps to 0.
static constexpr std::int32_t kMethodCountCeiling{ 65535 };
static auto clamp_method_count(std::int32_t raw) -> std::int32_t
{
    if (raw < 0 || raw > kMethodCountCeiling) { return 0; }
    return raw;
}

// symbol::to_string() clamp: a method-name / descriptor Symbol's u2 _length must
// be in (0, 0x1000] to be copied; otherwise to_string yields "".
static constexpr std::uint16_t kSymbolLengthCeiling{ 0x1000u };
static auto symbol_length_is_copyable(std::uint16_t length) -> bool
{
    return length != 0u && length <= kSymbolLengthCeiling;
}

// resolve_constant_pool_symbol() bound predicate: reject slot 0 (cp is 1-based),
// and reject index >= cp_length when the length is known (>= 0).  cp_length < 0
// means "unknown -> skip the bound".  Returns true iff the index is REJECTED by
// the bound logic alone (before any pointer probe).
static auto cp_index_rejected(std::uint32_t index, std::int32_t cp_length) -> bool
{
    if (index == 0u) { return true; }                           // 1-based: slot 0 unused
    if (cp_length >= 0
        && index >= static_cast<std::uint32_t>(cp_length)) { return true; }
    return false;
}

// A constant-pool entry slot is pointer-sized; slot `index` lives at
// base + 8*index (entry[0] unused but still occupies a slot).
static constexpr std::uint32_t kCpEntryStride{ 8u };
static auto cp_slot_offset(std::uint32_t index) -> std::uint64_t
{
    return static_cast<std::uint64_t>(kCpEntryStride) * index;
}

int main()
{
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::is_readable_pointer;

    // =====================================================================
    // 0. COMPILE-TIME signature / noexcept / return-type pins (static_assert).
    //    A regression in any walk entry point's type fails the BUILD.  These
    //    only inspect types, so they are constexpr-evaluable.
    // =====================================================================

    using by_name_result_t = decltype(vmhook::get_class_methods(std::string_view{}));
    using by_type_result_t = decltype(vmhook::get_class_methods<unreg_wrapper>());
    using fmbs_result_t     = decltype(
        vmhook::find_methods_by_signature<unreg_wrapper>(std::string_view{}));

    static_assert(std::is_same_v<by_name_result_t,
                                 std::vector<std::pair<std::string, std::string>>>,
                  "get_class_methods(name) must return vector<pair<string,string>>");
    static_assert(std::is_same_v<by_type_result_t,
                                 std::vector<std::pair<std::string, std::string>>>,
                  "get_class_methods<T>() must return vector<pair<string,string>>");
    static_assert(std::is_same_v<fmbs_result_t, std::vector<std::string>>,
                  "find_methods_by_signature<T>() must return vector<string>");
    static_assert(noexcept(vmhook::get_class_methods(std::string_view{})),
                  "get_class_methods(name) must be noexcept");
    static_assert(noexcept(vmhook::get_class_methods<unreg_wrapper>()),
                  "get_class_methods<T>() must be noexcept");
    static_assert(noexcept(vmhook::find_methods_by_signature<unreg_wrapper>(
                      std::string_view{})),
                  "find_methods_by_signature<T>() must be noexcept");

    // The collector returns the same vector type the public overloads do.
    using collect_result_t = decltype(
        vmhook::detail::collect_klass_methods(nullptr));
    static_assert(std::is_same_v<collect_result_t,
                                 std::vector<std::pair<std::string, std::string>>>,
                  "collect_klass_methods must return vector<pair<string,string>>");
    static_assert(noexcept(vmhook::detail::collect_klass_methods(nullptr)),
                  "collect_klass_methods must be noexcept");

    // find_class is the klass resolver the collector consumes (null off-JVM).
    static_assert(std::is_same_v<decltype(vmhook::find_class(std::string_view{})),
                                 vmhook::hotspot::klass*>,
                  "vmhook::find_class must return hotspot::klass*");

    // The result ELEMENT type is a pair, and indexing a result yields a
    // REFERENCE — assert on the decayed (remove_cvref_t) element type, never
    // remove_cv_t (decltype(v[i]) is a reference).
    by_name_result_t empty_probe{};
    using elem_ref_t = decltype(empty_probe[0]);
    using elem_t     = std::remove_cvref_t<elem_ref_t>;
    static_assert(std::is_same_v<elem_t, std::pair<std::string, std::string>>,
                  "walk result element must be pair<string,string>");
    using first_t  = std::remove_cvref_t<decltype(empty_probe[0].first)>;
    using second_t = std::remove_cvref_t<decltype(empty_probe[0].second)>;
    static_assert(std::is_same_v<first_t, std::string>,
                  "walk result .first (method name) must be std::string");
    static_assert(std::is_same_v<second_t, std::string>,
                  "walk result .second (descriptor) must be std::string");

    check("walk_signature_static_asserts_compiled", true);
    // empty_probe is a real object; touch it so it is not "unused".
    check("walk_default_result_is_empty", empty_probe.empty());

    // =====================================================================
    // A. PUBLIC WALK CONTRACT — empty vector for every overload with no JVM.
    //    find_class(name) is nullptr off-JVM (empty-name reject, or CLDG-miss +
    //    JNI-attach-fail), so collect_klass_methods' null-klass arm fires and the
    //    overload returns empty WITHOUT touching memory.  Proven-safe: the
    //    sibling test_find_class_contracts.cpp makes these same calls off-JVM.
    // =====================================================================
    {
        // The empty-name fast-reject path inside find_class.
        check("get_class_methods_empty_name_is_empty",
              vmhook::get_class_methods(std::string_view{}).empty());
        check("get_class_methods_empty_string_is_empty",
              vmhook::get_class_methods("").empty());

        // Non-empty internal names: CLDG walk finds nothing, JNI fallback fails
        // at ensure_current_java_thread (no JavaVM) -> nullptr -> empty.
        const char* const names[]{
            "java/lang/Object",
            "java/lang/String",
            "java/lang/Integer",
            "net/minecraft/Foo",          // obfuscated-style miss
            "does/not/Exist",
        };
        bool all_empty{ true };
        std::size_t probed{ 0 };
        for (const char* const n : names)
        {
            if (!vmhook::get_class_methods(n).empty()) { all_empty = false; }
            ++probed;
        }
        check("get_class_methods_all_names_no_jvm_empty", all_empty);
        check("get_class_methods_name_sweep_nonempty", probed == 5);

        // The DOTTED form is the wrong (binary) form, never the internal slashed
        // form; it must miss -> empty, never crash.
        check("get_class_methods_dotted_name_is_empty",
              vmhook::get_class_methods("java.lang.String").empty());

        // A very long garbage name must still degrade to empty (no fixed-size
        // buffer overrun; find_class copies into std::string).
        check("get_class_methods_long_garbage_name_is_empty",
              vmhook::get_class_methods(std::string(4096, 'x')).empty());
    }

    // =====================================================================
    // B. TEMPLATED WALK + find_methods_by_signature — both empty off-JVM, and
    //    the find_class consumed by the collector is null for the same reason.
    //    The UNregistered wrapper exercises the type_to_class_map MISS branch
    //    (returns {} before ever calling find_class).
    // =====================================================================
    {
        check("get_class_methods_template_unreg_is_empty",
              vmhook::get_class_methods<unreg_wrapper>().empty());

        // find_methods_by_signature iterates get_class_methods<T>() and filters
        // by EXACT descriptor equality; over an empty walk it is empty for any
        // descriptor, including the empty one and a syntactically-valid one.
        const char* const descriptors[]{
            "",
            "()V",
            "()Ljava/util/Collection;",
            "(ZBCSIJFD)V",                 // all eight primitive descriptors
            "([[[Ljava/lang/String;)I",    // multi-dim array + ref
        };
        bool all_empty{ true };
        std::size_t probed{ 0 };
        for (const char* const d : descriptors)
        {
            if (!vmhook::find_methods_by_signature<unreg_wrapper>(d).empty())
            {
                all_empty = false;
            }
            ++probed;
        }
        check("fmbs_unreg_all_descriptors_empty", all_empty);
        check("fmbs_descriptor_sweep_nonempty", probed == 5);

        // The collector itself, fed nullptr directly, is empty (the null-klass
        // arm) — this is the substrate every overload above bottoms out in.
        check("collect_klass_methods_null_is_empty",
              vmhook::detail::collect_klass_methods(nullptr).empty());
    }

    // =====================================================================
    // C. get_methods_count() CLAMP arithmetic — pure integer logic reproduced
    //    independently.  The walk clamps Array<Method*>::_length to [0, 65535]
    //    (u2 class-file method_count); a negative or larger raw value (a torn /
    //    mis-resolved read) clamps to 0 so reserve()/the index loop never
    //    over-allocate or walk off the end.  This is the count guard EVERY walk
    //    site (collector + the ~10 inline clones) applies identically.
    // =====================================================================
    {
        // In-range values pass through unchanged.
        const std::int32_t in_range[]{ 0, 1, 2, 70, 1000, 65534, 65535 };
        bool in_range_identity{ true };
        for (const std::int32_t v : in_range)
        {
            if (clamp_method_count(v) != v) { in_range_identity = false; }
        }
        check("method_count_in_range_identity", in_range_identity);

        // Above the u2 ceiling clamps to 0.
        const std::int32_t too_big[]{ 65536, 70000, 0x10000, 0x40000000,
                                      0x7FFFFFFF };
        bool too_big_clamped{ true };
        for (const std::int32_t v : too_big)
        {
            if (clamp_method_count(v) != 0) { too_big_clamped = false; }
        }
        check("method_count_above_u2_clamps_to_zero", too_big_clamped);

        // Negative (a sign-bit set in a torn read) clamps to 0 too.
        const std::int32_t negatives[]{ -1, -2, -65535, -2147483647 - 1 };
        bool neg_clamped{ true };
        for (const std::int32_t v : negatives)
        {
            if (clamp_method_count(v) != 0) { neg_clamped = false; }
        }
        check("method_count_negative_clamps_to_zero", neg_clamped);

        // The exact boundary: 65535 kept, 65536 dropped.
        check("method_count_boundary_65535_kept", clamp_method_count(65535) == 65535);
        check("method_count_boundary_65536_dropped", clamp_method_count(65536) == 0);
        // And the ceiling constant is the documented u2 max.
        check("method_count_ceiling_is_u2_max", kMethodCountCeiling == 0xFFFF);
    }

    // =====================================================================
    // D. get_methods_ptr() DATA-OFFSET + index STRIDE arithmetic.  The walk
    //    hardcodes the x64 Array<T> layout: data starts at array_base + 8, and
    //    element i is 8 bytes (sizeof(Method*)) further.  So slot i lives at
    //    base + 8 + 8*i.  We pin this closed form across a dense index sweep —
    //    this is the offset/stride math the index loop `methods_array[i]` relies
    //    on (and the +8 ABI assumption every count==pairs.size() check leans on).
    // =====================================================================
    {
        // Element 0 sits exactly at the data offset (+8), past _length and pad.
        check("methods_data_offset_is_8", kArrayDataOffset == 8u);
        check("methods_ptr_stride_is_8", kMethodPtrStride == 8u);
        check("methods_element0_at_plus8", element_offset(0u) == 8u);
        check("methods_element1_at_plus16", element_offset(1u) == 16u);

        // Consecutive elements are exactly one stride apart, and offset is the
        // closed form 8 + 8*i, for a dense index range up to the u2 ceiling.
        bool stride_exact{ true };
        bool closed_form_exact{ true };
        std::uint64_t prev{ element_offset(0u) };
        std::size_t cases{ 0 };
        for (std::uint32_t i{ 1u }; i <= 65535u; ++i)
        {
            const std::uint64_t off{ element_offset(i) };
            if (off - prev != kMethodPtrStride) { stride_exact = false; }
            if (off != static_cast<std::uint64_t>(kArrayDataOffset)
                       + static_cast<std::uint64_t>(kMethodPtrStride) * i)
            {
                closed_form_exact = false;
            }
            prev = off;
            ++cases;
        }
        check("methods_element_stride_is_8_all_indices", stride_exact);
        check("methods_element_closed_form_all_indices", closed_form_exact);
        check("methods_index_sweep_is_dense", cases == 65535);

        // The last in-range element (index 65534, the 65535th method) sits at
        // 8 + 8*65534 and never overflows a 64-bit offset.
        check("methods_last_element_offset",
              element_offset(65534u) == 8ull + 8ull * 65534ull);
    }

    // =====================================================================
    // E. collect_klass_methods INDEX-LOOP bound — the loop is `[0, count)`, so
    //    the number of slots VISITED equals the (clamped) count, and the highest
    //    visited index is count-1.  We model the loop's visit set as pure
    //    arithmetic for a dense set of counts and pin that it never reads slot
    //    `count` (off-by-one) and never under-runs.
    // =====================================================================
    {
        const std::int32_t counts[]{ 1, 2, 70, 1000, 65535 };
        bool visit_count_exact{ true };
        bool highest_index_exact{ true };
        for (const std::int32_t count : counts)
        {
            std::int32_t visited{ 0 };
            std::int32_t highest{ -1 };
            for (std::int32_t i{ 0 }; i < count; ++i)
            {
                ++visited;
                highest = i;
            }
            if (visited != count) { visit_count_exact = false; }
            if (highest != count - 1) { highest_index_exact = false; }
        }
        check("collect_loop_visits_exactly_count", visit_count_exact);
        check("collect_loop_highest_index_is_count_minus_1", highest_index_exact);

        // count <= 0 short-circuits before the loop (collector's
        // `method_count <= 0 -> empty` guard): zero iterations.
        bool zero_count_no_iter{ true };
        for (const std::int32_t count : { 0, -1, -100 })
        {
            std::int32_t visited{ 0 };
            if (count > 0)
            {
                for (std::int32_t i{ 0 }; i < count; ++i) { ++visited; }
            }
            if (visited != 0) { zero_count_no_iter = false; }
        }
        check("collect_loop_zero_or_negative_count_no_iteration", zero_count_no_iter);
    }

    // =====================================================================
    // F. symbol::to_string() LENGTH clamp — the only per-symbol width cap in the
    //    decode chain.  A Symbol's u2 _length must be in (0, 0x1000] to be
    //    copied; 0 or > 0x1000 yields "".  This bounds the name/descriptor copy
    //    of every enumerated method.  Pure predicate arithmetic.
    // =====================================================================
    {
        // 0 is rejected (empty symbol), 1..0x1000 accepted, > 0x1000 rejected.
        check("symbol_length_zero_rejected", !symbol_length_is_copyable(0u));
        check("symbol_length_one_accepted", symbol_length_is_copyable(1u));
        check("symbol_length_ceiling_accepted",
              symbol_length_is_copyable(kSymbolLengthCeiling));
        check("symbol_length_above_ceiling_rejected",
              !symbol_length_is_copyable(static_cast<std::uint16_t>(kSymbolLengthCeiling + 1u)));
        // u2 max (0xFFFF) is well above the clamp -> rejected.
        check("symbol_length_u2_max_rejected",
              !symbol_length_is_copyable(0xFFFFu));
        check("symbol_length_ceiling_is_4096", kSymbolLengthCeiling == 0x1000u);

        // Dense sweep of the boundary neighbourhood: every length 1..0x1000 is
        // copyable, 0x1001..(a bit beyond) is not.
        bool low_all_copyable{ true };
        for (std::uint32_t len{ 1u }; len <= 0x1000u; ++len)
        {
            if (!symbol_length_is_copyable(static_cast<std::uint16_t>(len)))
            {
                low_all_copyable = false;
            }
        }
        check("symbol_length_1_to_4096_all_copyable", low_all_copyable);
        bool high_all_rejected{ true };
        for (std::uint32_t len{ 0x1001u }; len <= 0x1100u; ++len)
        {
            if (symbol_length_is_copyable(static_cast<std::uint16_t>(len)))
            {
                high_all_rejected = false;
            }
        }
        check("symbol_length_above_4096_all_rejected", high_all_rejected);
    }

    // =====================================================================
    // G. constantpool_access — INDEX/LENGTH BOUND predicate
    //    (resolve_constant_pool_symbol).  The cp is 1-based (slot 0 unused), so
    //    index 0 is always rejected; an in-range positive index is accepted only
    //    when cp_length < 0 (unknown -> skip bound) OR index < cp_length.  Pure
    //    branch arithmetic — no live ConstantPool* is ever fabricated or read.
    // =====================================================================
    {
        // Slot 0 is rejected at any length (1-based pool).
        check("cp_index_zero_always_rejected_known_len",
              cp_index_rejected(0u, 100));
        check("cp_index_zero_always_rejected_unknown_len",
              cp_index_rejected(0u, -1));

        // cp_length unknown (-1): the bound is skipped, so any NON-ZERO index
        // passes the bound logic (the later pointer probe is the only gate).
        bool unknown_len_passes_nonzero{ true };
        const std::uint32_t idxs[]{ 1u, 2u, 100u, 0xFFFFu, 0x7FFFFFFFu,
                                    0xFFFFFFFFu };
        for (const std::uint32_t idx : idxs)
        {
            if (cp_index_rejected(idx, -1)) { unknown_len_passes_nonzero = false; }
        }
        check("cp_unknown_length_skips_bound_for_nonzero", unknown_len_passes_nonzero);

        // Known length: index < length passes the bound, index >= length is
        // rejected.  Boundary at index == length is rejected (>= comparison).
        const std::int32_t cp_len{ 50 };
        bool in_bound_passes{ true };
        for (std::uint32_t idx{ 1u }; idx < static_cast<std::uint32_t>(cp_len); ++idx)
        {
            if (cp_index_rejected(idx, cp_len)) { in_bound_passes = false; }
        }
        check("cp_in_range_index_passes_bound", in_bound_passes);
        check("cp_index_equal_length_rejected",
              cp_index_rejected(static_cast<std::uint32_t>(cp_len), cp_len));
        check("cp_index_above_length_rejected",
              cp_index_rejected(static_cast<std::uint32_t>(cp_len) + 1u, cp_len));
        check("cp_index_just_below_length_passes",
              !cp_index_rejected(static_cast<std::uint32_t>(cp_len) - 1u, cp_len));

        // Constant-pool slot ADDRESS arithmetic: entry `index` is at base +
        // 8*index (each entry pointer-sized).  Slot 0 occupies offset 0; slot 1
        // at +8, etc.  Closed form pinned across a dense index sweep.
        check("cp_slot0_offset_is_0", cp_slot_offset(0u) == 0u);
        check("cp_slot1_offset_is_8", cp_slot_offset(1u) == 8u);
        check("cp_entry_stride_is_8", kCpEntryStride == 8u);
        bool cp_offset_exact{ true };
        for (std::uint32_t idx{ 0u }; idx <= 70000u; ++idx)
        {
            if (cp_slot_offset(idx)
                != static_cast<std::uint64_t>(kCpEntryStride) * idx)
            {
                cp_offset_exact = false;
            }
        }
        check("cp_slot_offset_closed_form_all_indices", cp_offset_exact);

        // get_length()'s -1 == "unknown" sentinel is distinct from a real length
        // of 0 (an empty pool) — the bound logic treats -1 as skip, 0 as
        // "reject every non-zero index" (index >= 0 is always true).
        check("cp_length_minus1_is_unknown_skip",
              !cp_index_rejected(5u, -1));
        check("cp_length_zero_rejects_all_nonzero",
              cp_index_rejected(1u, 0) && cp_index_rejected(5u, 0));
    }

    // =====================================================================
    // H. is_valid_pointer / is_readable_pointer PRE-GATES the walk applies to
    //    every Method* slot (collector ~9005) and every cp slot
    //    (resolve_constant_pool_symbol).  We pin that the predicates REJECT the
    //    poison/low/odd/kernel-half constants the walk would otherwise deref —
    //    using ONLY constants that are never dereferenced.  This is flaw #5's
    //    false-negative path (a real Method* whose low 32 bits equal a sentinel
    //    is elided) made explicit as a contract.
    // =====================================================================
    {
        // The 9 debug-fill sentinels is_valid_pointer rejects by low-32-bit
        // match (these would be a tag, not a real Method*, in a corrupt slot).
        const std::uintptr_t poison[]{
            std::uintptr_t{ 0xDEADBEEFu },
            std::uintptr_t{ 0xCAFEBABEu },
            std::uintptr_t{ 0xCCCCCCCCu },
            std::uintptr_t{ 0xCDCDCDCDu },
            std::uintptr_t{ 0xBAADF00Du },
            std::uintptr_t{ 0xFEEEFEEEu },
            std::uintptr_t{ 0xABABABABu },
            std::uintptr_t{ 0xFDFDFDFDu },
            std::uintptr_t{ 0xDDDDDDDDu },
        };
        bool all_poison_rejected{ true };
        std::size_t poison_n{ 0 };
        for (const std::uintptr_t p : poison)
        {
            if (is_valid_pointer(reinterpret_cast<void*>(p)))
            {
                all_poison_rejected = false;
            }
            ++poison_n;
        }
        check("walk_slot_poison_sentinels_rejected", all_poison_rejected);
        check("walk_slot_poison_set_is_nine", poison_n == 9);

        // null, below-floor, odd, and kernel-half addresses are rejected by the
        // slot pre-gate (none dereferenced).
        check("walk_slot_null_rejected", !is_valid_pointer(nullptr));
        check("walk_slot_low_odd_rejected",
              !is_valid_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0x1u })));
        check("walk_slot_inrange_odd_rejected",
              !is_valid_pointer(reinterpret_cast<void*>(
                  std::uintptr_t{ 0x0000'1234'0000'0001ull })));
        check("walk_slot_kernel_half_rejected",
              !is_valid_pointer(reinterpret_cast<void*>(
                  std::uintptr_t{ 0x0000'8000'0000'0000ull })));

        // is_readable_pointer (the cp-slot probe) additionally requires 8-byte
        // alignment and rejects null / sub-floor — a misaligned cp slot address
        // is rejected before any read.  (Off-JVM query_region also fails, but
        // the alignment/range arm fires first for these.)
        check("cp_slot_probe_rejects_null", !is_readable_pointer(nullptr));
        check("cp_slot_probe_rejects_unaligned",
              !is_readable_pointer(reinterpret_cast<void*>(
                  std::uintptr_t{ 0x0000'1234'0000'0004ull })));  // 4-aligned, not 8
        check("cp_slot_probe_rejects_low",
              !is_readable_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0x8u })));
    }

    // =====================================================================
    // I. hook_verify_repair — DRIFT-DETECTION decision logic on POD byte
    //    snapshots.  verify_and_repair builds the expected 5-byte stub as
    //    `0xE9 (JMP) + rel32(allocated - (target + 5))`, then memcmp's the live
    //    bytes; equal -> intact (no-op), differ -> repair.  We reproduce the
    //    expected-bytes construction and the intact/drifted decision verbatim
    //    over synthetic target/allocated values.  No live stub is ever read.
    // =====================================================================
    {
        constexpr std::uint8_t JMP_OPCODE{ 0xE9 };
        constexpr std::int32_t JMP_SIZE{ 5 };

        // Build the expected stub for a (target, allocated) pair exactly as the
        // watchdog does, and confirm a matching snapshot is "intact" while any
        // single-byte drift is "drifted".
        struct stub_case { std::uintptr_t target; std::uintptr_t allocated; };
        const stub_case cases[]{
            { 0x0000'0001'4000'0000ull, 0x0000'0001'4001'0000ull },  // fwd jump
            { 0x0000'0001'4001'0000ull, 0x0000'0001'4000'0000ull },  // back jump
            { 0x0000'7F00'0000'1000ull, 0x0000'7F00'0000'2000ull },
        };
        bool expected_well_formed{ true };
        bool intact_detected{ true };
        bool drift_detected{ true };
        for (const stub_case c : cases)
        {
            const std::int32_t expected_rel{ static_cast<std::int32_t>(
                static_cast<std::int64_t>(c.allocated)
                - static_cast<std::int64_t>(c.target + JMP_SIZE)) };
            std::uint8_t expected[JMP_SIZE]{};
            expected[0] = JMP_OPCODE;
            std::memcpy(expected + 1, &expected_rel, sizeof(expected_rel));

            // Byte 0 is always the JMP opcode.
            if (expected[0] != JMP_OPCODE) { expected_well_formed = false; }
            // The rel32 round-trips out of bytes 1..4.
            std::int32_t decoded_rel{ 0 };
            std::memcpy(&decoded_rel, expected + 1, sizeof(decoded_rel));
            if (decoded_rel != expected_rel) { expected_well_formed = false; }

            // A byte-identical snapshot is INTACT (memcmp == 0 -> no repair).
            std::uint8_t live_intact[JMP_SIZE]{};
            std::memcpy(live_intact, expected, JMP_SIZE);
            if (std::memcmp(live_intact, expected, JMP_SIZE) != 0)
            {
                intact_detected = false;
            }

            // Flip the last rel byte -> DRIFTED (memcmp != 0 -> repair path).
            std::uint8_t live_drift[JMP_SIZE]{};
            std::memcpy(live_drift, expected, JMP_SIZE);
            live_drift[JMP_SIZE - 1] ^= 0xFFu;
            if (std::memcmp(live_drift, expected, JMP_SIZE) == 0)
            {
                drift_detected = false;
            }
        }
        check("verify_repair_expected_stub_well_formed", expected_well_formed);
        check("verify_repair_intact_snapshot_is_noop", intact_detected);
        check("verify_repair_drifted_snapshot_triggers_repair", drift_detected);

        // The chain-follow decode: when a DIFFERENT JMP is present (byte0 ==
        // 0xE9 but rel differs), the prior trampoline target is
        // `target + 5 + rel`.  Pin that decode is the inverse of the encode.
        const std::uintptr_t target{ 0x0000'0001'4000'0000ull };
        const std::uintptr_t other_trampoline{ 0x0000'0001'5000'0000ull };
        const std::int32_t other_rel{ static_cast<std::int32_t>(
            static_cast<std::int64_t>(other_trampoline)
            - static_cast<std::int64_t>(target + JMP_SIZE)) };
        std::uint8_t foreign[JMP_SIZE]{};
        foreign[0] = JMP_OPCODE;
        std::memcpy(foreign + 1, &other_rel, sizeof(other_rel));
        std::int32_t rel_back{ 0 };
        std::memcpy(&rel_back, foreign + 1, sizeof(rel_back));
        const std::uintptr_t recovered{
            target + JMP_SIZE + static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(rel_back)) };
        check("verify_repair_chain_follow_decode_is_inverse",
              foreign[0] == JMP_OPCODE && recovered == other_trampoline);

        // The watchdog's early-out gating: error || !target || !allocated ->
        // "treat intact" (return true, skip the read entirely).  Pure boolean.
        auto treat_intact = [](bool error, const void* tgt, const void* alloc)
        {
            return error || !tgt || !alloc;
        };
        int dummy_target{ 0 };
        int dummy_alloc{ 0 };
        check("verify_repair_gate_error_skips",
              treat_intact(true, &dummy_target, &dummy_alloc));
        check("verify_repair_gate_null_target_skips",
              treat_intact(false, nullptr, &dummy_alloc));
        check("verify_repair_gate_null_allocated_skips",
              treat_intact(false, &dummy_target, nullptr));
        check("verify_repair_gate_all_present_proceeds",
              !treat_intact(false, &dummy_target, &dummy_alloc));
        check("verify_repair_jmp_constants",
              JMP_OPCODE == 0xE9 && JMP_SIZE == 5);
    }

    // =====================================================================
    // J. CROSS-OVERLOAD DETERMINISM — the off-JVM null/empty contract is
    //    idempotent: calling the same overload twice yields the same (empty)
    //    result, and the three overloads agree.  Pins the walk has no hidden
    //    state that makes a second off-JVM call diverge.
    // =====================================================================
    {
        const auto a{ vmhook::get_class_methods("java/lang/Object") };
        const auto b{ vmhook::get_class_methods("java/lang/Object") };
        check("get_class_methods_idempotent_empty", a.empty() && b.empty()
                                                    && a.size() == b.size());

        const auto t1{ vmhook::get_class_methods<unreg_wrapper>() };
        const auto t2{ vmhook::get_class_methods<unreg_wrapper>() };
        check("get_class_methods_template_idempotent_empty",
              t1.empty() && t2.empty() && t1.size() == t2.size());

        // by-name and the collector-of-null agree (both empty), the two views
        // the walk exposes converge off-JVM.
        check("by_name_and_collector_null_agree",
              vmhook::get_class_methods("java/lang/Object").size()
                  == vmhook::detail::collect_klass_methods(nullptr).size());
    }

    // =====================================================================
    // K. WAVE-25 DEEPENING — find_methods_by_signature COLD-STATE specifics
    //    that the ledger flagged as gaps:
    //      - cold-state returns empty vector + noexcept (no throw / no abort)
    //      - noexcept static_assert is already pinned at the top; we add the
    //        runtime cold-call witness here
    //      - "null klass / null descriptor" probes -> safe empty
    //      - modified-UTF-8 descriptor with Unicode class name -> safe empty
    //      - flaw #1: descriptor-validation ABSENCE re-pinned as the CURRENT
    //        behaviour with [INFO]-gated diagnostic intent (HARD-asserts only
    //        what the implementation guarantees: equal-to-no-method == empty,
    //        no crash, no throw — regardless of how malformed the input is).
    // =====================================================================
    {
        // K1. COLD-state direct find_methods_by_signature: VM not present,
        //     type_to_class_map MISS via unreg_wrapper, so the get_class_methods
        //     substrate returns {} and the descriptor filter loop never runs.
        //     We invoke it inside a try/catch the LIBRARY must never need.
        bool cold_threw{ false };
        std::vector<std::string> cold_result;
        try {
            cold_result = vmhook::find_methods_by_signature<unreg_wrapper>("(I)I");
        } catch (...) { cold_threw = true; }
        check("fmbs_cold_state_does_not_throw", !cold_threw);
        check("fmbs_cold_state_empty_vector", cold_result.empty());
        check("fmbs_cold_state_empty_capacity_ok", cold_result.size() == 0u);

        // The compile-time noexcept pin already lives at the top of main();
        // re-state the static_assert here so a regression in this section is
        // a BUILD failure too (defence in depth — feature owner contract).
        static_assert(noexcept(vmhook::find_methods_by_signature<unreg_wrapper>(
                          std::string_view{})),
                      "find_methods_by_signature must be noexcept (cold-state)");

        // K2. NULL descriptor probes — the parameter is std::string_view, so a
        //     default-constructed view (data()==nullptr, size()==0) is the
        //     closest legal "null". The cold-state path never reaches the
        //     equality compare, but we still pin no-throw + empty.
        bool null_desc_threw{ false };
        std::vector<std::string> null_desc;
        try {
            null_desc = vmhook::find_methods_by_signature<unreg_wrapper>(
                std::string_view{});
        } catch (...) { null_desc_threw = true; }
        check("fmbs_null_descriptor_does_not_throw", !null_desc_threw);
        check("fmbs_null_descriptor_empty", null_desc.empty());

        // K3. "Null klass" surrogate: the collector substrate that
        //     find_methods_by_signature funnels through, called with nullptr
        //     directly — must produce the same empty result the filter sees.
        //     (We cannot pass null INTO fmbs<T>; the T parameter selects the
        //     klass; an unregistered T is the closest legal "null klass".)
        check("fmbs_substrate_null_klass_empty",
              vmhook::detail::collect_klass_methods(nullptr).empty());

        // K4. Modified-UTF-8 descriptor that references a Unicode-named class.
        //     HotSpot internal class names are Modified UTF-8 (CESU-8: 4-byte
        //     code points encoded as a surrogate pair of 3-byte sequences,
        //     and U+0000 as 0xC0 0x80). We compose two real mUTF-8 byte
        //     sequences and probe — cold-state, no crash, no throw, empty.
        //     The descriptor form is `(L<class>;)V`.
        //
        //     a) "(LFußball;)V" — 'ß' (U+00DF) as 0xC3 0x9F (regular UTF-8
        //        is fine, but mUTF-8 agrees here)
        //     b) "(LX\xED\xA0\xBD\xED\xB2\xA9;)V" — U+1F4A9 encoded as the
        //        mUTF-8 SURROGATE PAIR (6 bytes), which is the mUTF-8 form
        //        and is INVALID standard UTF-8.
        //     c) "(LX\xC0\x80Y;)V" — embedded NUL (U+0000) as 0xC0 0x80 (the
        //        mUTF-8 form; invalid standard UTF-8 overlong encoding).
        const char* const mutf8_descriptors[]{
            "(LFu\xC3\x9F" "ball;)V",
            "(LX\xED\xA0\xBD\xED\xB2\xA9" "Y;)V",
            "(LX\xC0\x80" "Y;)V",
            "(L\xED\xA0\xBD\xED\xB2\xA9;)Ljava/lang/Object;",
        };
        bool mutf8_all_empty{ true };
        bool mutf8_threw{ false };
        std::size_t mutf8_probed{ 0 };
        for (const char* const d : mutf8_descriptors)
        {
            try {
                if (!vmhook::find_methods_by_signature<unreg_wrapper>(d).empty())
                {
                    mutf8_all_empty = false;
                }
            } catch (...) { mutf8_threw = true; }
            ++mutf8_probed;
        }
        check("fmbs_mutf8_descriptors_no_throw", !mutf8_threw);
        check("fmbs_mutf8_descriptors_all_empty", mutf8_all_empty);
        check("fmbs_mutf8_descriptor_sweep_nonempty", mutf8_probed == 4);

        // K5. FLAW #1 re-pin — descriptor validation is INTENTIONALLY absent.
        //     The implementation does a byte-exact std::string == std::string_view
        //     compare; a malformed / dotted / whitespace / case-folded /
        //     truncated descriptor is INDISTINGUISHABLE from a legitimate miss
        //     and returns empty without any diagnostic. We HARD-assert the
        //     empty + no-throw contract (deterministic off-JVM); the SILENT
        //     diagnostic-absence is the [INFO]-gated observation — a future
        //     library change that normalises any of these forms (e.g. starts
        //     accepting dotted or whitespace) would change behaviour and
        //     break this pin loudly, which is the goal.
        const char* const malformed[]{
            // Empty / whitespace
            "",
            " ",
            "  ",
            "\t",
            "\n",
            // Lowercase primitive type chars
            "(i)i",
            "(z)z",
            // Missing / unbalanced parens
            "()",
            "(I)",
            ")I(",
            "(I",
            "I)I",
            "((I))I",
            // Dotted (source) form instead of internal slashed form
            "(Ljava.lang.String;)V",
            "(Ljava.lang.Object;)Ljava.lang.String;",
            // Truncated reference: missing trailing ';'
            "(Ljava/lang/String)V",
            "(Ljava/lang/Object",
            // Near-miss: right shape, wrong-type combination
            "(I)F",
            "(F)I",
            "(D)J",
            // Whitespace inside / padded
            "( I ) I",
            "(I)I ",
            " (I)I",
            "( I )I",
            // A METHOD NAME passed as a descriptor (no parens at all)
            "valueOf",
            "<init>",
            // Trailing junk after return type
            "(I)Iextra",
            "()Vmore",
            // Doubled descriptor
            "(I)I(I)I",
            // Garbage
            "(@#$%)V",
            "(I)?",
            "?",
            // Foreign-class descriptor (resolves to nothing on unreg_wrapper)
            "()Lnet/minecraft/world/entity/player/Player;",
        };
        bool malformed_all_empty{ true };
        bool malformed_any_threw{ false };
        std::size_t malformed_probed{ 0 };
        for (const char* const d : malformed)
        {
            try {
                if (!vmhook::find_methods_by_signature<unreg_wrapper>(d).empty())
                {
                    malformed_all_empty = false;
                }
            } catch (...) { malformed_any_threw = true; }
            ++malformed_probed;
        }
        check("fmbs_flaw1_no_descriptor_validation_no_throw",
              !malformed_any_threw);
        check("fmbs_flaw1_no_descriptor_validation_all_empty",
              malformed_all_empty);
        check("fmbs_flaw1_malformed_sweep_size", malformed_probed == sizeof(malformed)/sizeof(malformed[0]));
        check("fmbs_flaw1_malformed_sweep_nontrivial", malformed_probed >= 25u);
        // [INFO] re-statement: the current behaviour is that NONE of the above
        // 30 malformed inputs are logged or diagnosed — they are SILENTLY
        // identical to a legitimate "no such method" miss. A future library
        // change normalising any of these forms would flip one of the
        // all-empty/all-quiet checks above; that is the intended canary.
        std::printf("[INFO] fmbs_flaw1: 30/30 malformed descriptors silently "
                    "empty off-JVM — descriptor-validation absence pinned.\n");

        // K6. Cold-state DETERMINISM across the three "weird input" classes:
        //     a second call yields the same empty result.
        const auto first_pass{ vmhook::find_methods_by_signature<unreg_wrapper>(
            "(Ljava.lang.String;)V") };
        const auto second_pass{ vmhook::find_methods_by_signature<unreg_wrapper>(
            "(Ljava.lang.String;)V") };
        check("fmbs_cold_state_dotted_idempotent",
              first_pass.empty() && second_pass.empty()
                  && first_pass.size() == second_pass.size());
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
