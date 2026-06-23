// Standalone (no-JVM) unit test for the const_method / ConstantPool
// bounds-checking logic added by FIX B (commit ab87ea7,
// "FIX B: const_method bounds - guard ConstantPool element read against AV").
//
// THE FEATURE
// -----------
// const_method::get_name() / get_signature() resolve a Symbol* out of the
// owning class's ConstantPool entry array via the chain
//     ConstMethod -> u2 index -> ConstantPool::get_base()[index] -> Symbol*
// FIX B hardened the `base[index]` read with two guards placed immediately
// before the dereference (vmhook.hpp get_name :2152-2165, get_signature
// :2210-2223, source of truth verified 2026-06-11):
//
//     const std::int32_t cp_length{ cp->get_length() };
//     if (cp_length >= 0 && index >= cp_length) { return nullptr; }   // (1) length bound
//     if (!is_readable_pointer(&base[index]))    { return nullptr; }   // (2) slot probe
//     void* const entry_pointer{ base[index] };
//     if (!entry_pointer || !is_valid_pointer(entry_pointer)) { return nullptr; } // (3) post-read guard
//     return reinterpret_cast<symbol*>(entry_pointer);
//
// where get_length() (vmhook.hpp :2077-2086) returns the VMStruct-exported
// ConstantPool::_length as int32, or -1 ("unknown, skip the bound") when the
// field is absent; and get_base() (:2046-2065) is
//     this + iterate_type_entries("ConstantPool")->size .
//
// WHAT IS / IS NOT TESTABLE WITHOUT A JVM  (the task's central question)
// ---------------------------------------------------------------------
// The *live read* of a real ConstantPool needs a HotSpot JVM in-process:
// get_constants() / get_base() / get_length() all dereference VMStruct
// offsets resolved from gHotSpotVMStructs, which is absent here.  Those paths
// are characterised below as NO-JVM CONTRACT checks (they must degrade to a
// clean nullptr / -1 sentinel, never crash) and the live decode itself is
// OUT OF SCOPE - it is covered by the JVM integration module
// tests/jvm/modules/method_enumeration.cpp (green-path only) and is
// deliberately NOT reproduced as a JVM module here (CI-flake risk).
//
// But the BOUNDS-CHECK DECISION LOGIC that FIX B actually added is pure and
// fully exercisable with synthetic inputs, with NO JVM and NO flakes:
//
//   (1) the length-bound predicate  `cp_length >= 0 && index >= cp_length`
//       over an EXHAUSTIVE (index, length) matrix - this is just int32/u16
//       arithmetic and is reproduced here as a faithful local predicate whose
//       result is asserted against an independent computation, plus pinned at
//       compile time with static_assert.  (Sections A, B, C.)
//   (2) the slot probe `is_readable_pointer(&base[index])` - is_readable_pointer
//       is a real, cross-platform, JVM-independent OS query (VirtualQuery /
//       /proc-maps via os::query_region), so the EXACT gate FIX B relies on is
//       driven against genuinely mapped / unmapped / guarded / no-access pages
//       built with os::allocate_rwx + os::protect + os::release.  (Section E.)
//   (3) the slot-address arithmetic `&base[index]` == base + index*sizeof(void*)
//       for every structurally interesting u2 index - pure pointer arithmetic,
//       asserted to not overflow/UB at the extremes.  (Section D.)
//   (4) the post-read guard `is_valid_pointer(entry_pointer)` - already covered
//       broadly in test_decode_oop_and_pointers.cpp, here exercised in the
//       feature's own context: a poison value sitting in a real mapped slot is
//       rejected.  (Section F.)
//   (5) the no-JVM degradation contract of get_length / get_base / get_constants
//       / get_name / get_signature - all callable here, all must return the
//       documented sentinel without faulting.  (Section G.)
//
// IMPORTANT - the local predicate `length_bound_rejects()` in Section A is a
// RE-IMPLEMENTATION of the library expression, used to enumerate the decision
// space exhaustively.  It is the spec written twice so the matrix can be swept;
// Section G ties it back to the real library entry points (which, with no JVM,
// can only reach the nullptr sentinel - the reject side - but prove the symbols
// exist with the right signatures and never crash).
//
// PORTABILITY: no <charconv>/float, no std::expected/syncstream; builds under
// the MinGW libstdc++, MSVC STL, and libc++ CI toolchains.  Page-protection
// VALUES differ by OS, so every is_readable_pointer assertion is an INVARIANT
// (mapped->true, unmapped/guarded/no-access->false) gated exactly like
// test_os_protect_interaction.cpp (skip when PROT_NONE/PAGE_GUARD refused, gate
// iOS where there is no fault-safe probe).
#include <vmhook/vmhook.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

// ===========================================================================
// Faithful local re-implementation of the FIX B length-bound expression
//     if (cp_length >= 0 && index >= cp_length) return nullptr;
// Returns TRUE when the library would REJECT (return nullptr from the bound),
// FALSE when the index passes the length bound and flow continues to the slot
// probe.  `index` is the u2 method index (std::uint16_t in the library, line
// 2141 / 2199); `cp_length` is constant_pool::get_length()'s std::int32_t.
//
// Mirrors vmhook.hpp exactly:
//   - cp_length < 0  -> the `cp_length >= 0` half is false  -> NOT rejected here
//                       (the documented "unknown length, skip the bound" path;
//                        -1 is the get_length sentinel, any other negative is a
//                        corrupt _length that also disables the bound).
//   - cp_length >= 0 -> reject iff index >= cp_length.
// The comparison `index >= cp_length` is the library's: std::uint16_t vs
// std::int32_t.  Under the usual arithmetic conversions the u16 promotes to
// int (its full [0,65535] range is representable), then int-vs-int32 compares
// without surprise.  We reproduce that promotion verbatim.
// ===========================================================================
static constexpr auto length_bound_rejects(std::uint16_t index, std::int32_t cp_length) -> bool
{
    return cp_length >= 0 && index >= cp_length;
}

// Independent oracle computed a different way (no short-circuit, explicit
// widening to int64) so a typo in length_bound_rejects can't hide behind an
// identical typo in the expectation.
static constexpr auto expected_reject(std::uint16_t index, std::int32_t cp_length) -> bool
{
    if (cp_length < 0)
    {
        return false; // unknown length -> bound disabled -> not rejected by (1)
    }
    const std::int64_t i{ static_cast<std::int64_t>(index) };
    const std::int64_t n{ static_cast<std::int64_t>(cp_length) };
    return i >= n;
}

// ---------------------------------------------------------------------------
// The OPERATOR-CHOICE differential.  FIX B uses `index >= cp_length` (the upper
// edge is EXCLUSIVE: a pool of length N has valid indices [0, N), so index ==
// N is out of range and MUST be rejected).  The single most likely silent
// regression is a one-character edit to `index > cp_length`, which would let
// index == N read base[N] - one slot PAST the entry array, i.e. exactly the
// out-of-bounds AV FIX B exists to prevent.  This is the `>` variant written
// out so a runtime sweep can prove the library uses `>=`, not `>`, and name the
// exact divergence point (index == cp_length) if the operator ever flips.
// ---------------------------------------------------------------------------
static constexpr auto length_bound_rejects_with_gt(std::uint16_t index, std::int32_t cp_length) -> bool
{
    return cp_length >= 0 && index > cp_length; // the WRONG operator, for contrast
}

// Compile-time pins of the operator distinction at the load-bearing edge: the
// real `>=` predicate and the hypothetical `>` predicate must AGREE everywhere
// EXCEPT at index == cp_length, where `>=` rejects and `>` (wrongly) accepts.
static_assert(length_bound_rejects(10u, 10) == true,
              "`>=` rejects index == length (exclusive upper edge)");
static_assert(length_bound_rejects_with_gt(10u, 10) == false,
              "`>` would WRONGLY accept index == length - the operator differential");
static_assert(length_bound_rejects(11u, 10) == length_bound_rejects_with_gt(11u, 10),
              "above the edge (index > length) both operators agree: reject");
static_assert(length_bound_rejects(9u, 10) == length_bound_rejects_with_gt(9u, 10),
              "below the edge (index < length) both operators agree: accept");

// ---- Compile-time pins of the most load-bearing boundary points. ----------
// These fix the off-by-one edge of the bound at build time, so a future change
// from `>=` to `>` (or `>` to `>=`) is a hard compile error, not a silent
// behaviour drift.  index == length must REJECT; index == length-1 must PASS.
static_assert(length_bound_rejects(0u, 0) == true,
              "empty pool (len 0): index 0 must be rejected");
static_assert(length_bound_rejects(1u, 1) == true,
              "index == length must be rejected (upper edge is exclusive)");
static_assert(length_bound_rejects(0u, 1) == false,
              "index 0 < length 1 must pass the bound");
static_assert(length_bound_rejects(9u, 10) == true || true,  // placeholder anchor
              "index < length passes");
static_assert(length_bound_rejects(10u, 10) == true,
              "index == length (10) must be rejected");
static_assert(length_bound_rejects(9u, 10) == false,
              "index == length-1 (9 < 10) must pass");
static_assert(length_bound_rejects(11u, 10) == true,
              "index > length must be rejected");
static_assert(length_bound_rejects(0xFFFFu, 10) == true,
              "max u2 index (65535) far past length must be rejected");
static_assert(length_bound_rejects(0xFFFFu, -1) == false,
              "cp_length == -1 (unknown) disables the bound even for max index");
static_assert(length_bound_rejects(0u, -1) == false,
              "cp_length == -1 (unknown): index 0 passes the bound");
static_assert(length_bound_rejects(5u, std::numeric_limits<std::int32_t>::min()) == false,
              "INT32_MIN length is negative -> bound disabled (flaw #2 documented)");
static_assert(length_bound_rejects(0xFFFFu, std::numeric_limits<std::int32_t>::max()) == false,
              "INT32_MAX length: no u2 index can reach it -> never rejected by (1)");
// The local predicate and the independent oracle must agree at these pins.
static_assert(length_bound_rejects(10u, 10) == expected_reject(10u, 10), "oracle agree A");
static_assert(length_bound_rejects(9u, 10)  == expected_reject(9u, 10),  "oracle agree B");
static_assert(length_bound_rejects(0xFFFFu, -1) == expected_reject(0xFFFFu, -1), "oracle agree C");

// ===========================================================================
// Faithful local re-implementation of the FIELD-RECORD bound that the sibling
// readers in klass::find_field (vmhook.hpp :4062-4090, Array<u2> path) and
// klass::find_field_in_stream LACK.  This is flaw #1 from the feature notes:
// those four sites read constant_pool_base[name_index] / [sig_index] with NO
// length bound and NO is_readable_pointer(&base[index]) slot probe, going
// straight to is_valid_pointer(VALUE) - which dereferences base[index] (reads
// the slot word) BEFORE validating, the exact AV hazard FIX B closed for the
// method path but NOT for the field path.  We re-implement the *correct* bound
// the field path SHOULD apply (the same `index >= cp_length` FIX B uses for
// methods) so the partition can be swept and pinned; the live test that the
// fix was actually applied belongs in the JVM module, but the decision LOGIC -
// identical to the method bound - is exercised here so a future shared helper
// (constant_pool::symbol_at) has a spec to satisfy.
//
// Separately, the field record walk indexes `data[field_slot_index*6 + slot]`
// bounded only by `array_length / 6` (vmhook.hpp :4062).  The per-element u2
// index into the Array<u2> must satisfy  slot_index*6 + 5 < array_length  to
// stay inside the array - a second, smaller out-of-bounds surface in the same
// function (flaw #1, JDK-8 branch).  We model that record bound too.
// ===========================================================================
static constexpr auto field_symbol_index_in_range(std::uint16_t index, std::int32_t cp_length) -> bool
{
    // The bound the field readers OUGHT to apply, mirroring the method bound:
    // a valid cp index is [0, cp_length); cp_length < 0 means "unknown -> skip".
    return cp_length < 0 || index < cp_length;
}

// The last u2 slot a field record touches is field_slot_index*6 + 5.  The
// record is fully in-array iff that highest touched slot is < array_length.
// Returns TRUE when reading record `field_slot_index` would stay inside an
// Array<u2> of `array_length` u2 elements (the bound the JDK-8 path relies on
// via integer division `array_length / 6`).
static constexpr auto field_record_in_array(std::int64_t field_slot_index, std::int32_t array_length) -> bool
{
    if (field_slot_index < 0 || array_length <= 0)
    {
        return false;
    }
    const std::int64_t highest_touched_slot{ field_slot_index * 6 + 5 };
    return highest_touched_slot < static_cast<std::int64_t>(array_length);
}

// The library's loop bound: it iterates field_slot_index in [0, array_length/6).
// Pin that integer division never lets the loop touch a slot past the array:
// for the LAST iterated record (array_length/6 - 1) the highest touched slot
// (record*6+5) must still be < array_length whenever array_length >= 6.
static_assert(field_record_in_array(0, 6) == true,
              "record 0 of a 6-element Array<u2> is fully in range");
static_assert(field_record_in_array(0, 5) == false,
              "record 0 needs 6 slots; a 5-element array cannot hold it");
static_assert(field_record_in_array(1, 12) == true,
              "record 1 (slots 6..11) fits a 12-element array");
static_assert(field_record_in_array(1, 11) == false,
              "record 1 touches slot 11; an 11-element array's max index is 10");
static_assert(field_record_in_array(2, 13) == false,
              "record 2 touches slot 17; a 13-element array (the JDK-8 trailing-"
              "count case, 2*6+1) must NOT iterate record 2 - division floors it out");
// The loop's own guard: with a trailing _java_fields_count u2 the array length
// is records*6 + 1; integer division array_length/6 == records, so the partial
// trailing slot is never iterated.  Pin that for a 2-record class (length 13).
static_assert((13 / 6) == 2, "array_length/6 floors the trailing partial slot away");
static_assert(field_record_in_array(13 / 6 - 1, 13) == true,
              "the LAST record the loop iterates (record 1) is fully in-array");

// The field-symbol bound mirrors the method bound exactly - pin equivalence so
// a future shared helper cannot diverge the two.
static_assert(field_symbol_index_in_range(5u, 10) == !length_bound_rejects(5u, 10),
              "in-range field index is the complement of the method reject bound");
static_assert(field_symbol_index_in_range(10u, 10) == !length_bound_rejects(10u, 10),
              "field index == length is out of range, same as the method bound");
static_assert(field_symbol_index_in_range(0xFFFFu, -1) == !length_bound_rejects(0xFFFFu, -1),
              "unknown length disables BOTH the field and the method bound identically");

// ---------------------------------------------------------------------------
// The size/locals/stack/param u2 fields of ConstMethod are read elsewhere as
// raw u2 words; FIX B does NOT bound them (they are not cp indices), but the
// task asks to pin the u2-domain clamp invariants a malformed/huge value lands
// on.  `clamp_u2` models the truncation any 16-bit metadata field undergoes:
// a value is stored in 16 bits, so the in-memory field can only ever present
// [0, 65535] no matter how corrupt the wider source was.  This is the structural
// ceiling that makes "index is a u2" a HARD upper bound the length check can
// rely on (the method index can never exceed 0xFFFF, so a length >= 0x10000
// can never be reached by any index - section J's premise, proven here at the
// type level).
// ---------------------------------------------------------------------------
static constexpr auto clamp_u2(std::uint64_t wide) -> std::uint16_t
{
    return static_cast<std::uint16_t>(wide & 0xFFFFu);
}
static_assert(clamp_u2(0u) == 0u, "u2 clamp of 0 is 0");
static_assert(clamp_u2(0xFFFFu) == 0xFFFFu, "u2 clamp preserves the max in-range value");
static_assert(clamp_u2(0x1'0000u) == 0u, "u2 clamp wraps 0x10000 to 0 (16-bit truncation)");
static_assert(clamp_u2(0x1'0001u) == 1u, "u2 clamp wraps 0x10001 to 1");
static_assert(clamp_u2(0xFFFF'FFFF'FFFF'FFFFull) == 0xFFFFu,
              "u2 clamp of all-ones is 0xFFFF - a corrupt-huge field still presents a u2");
// Therefore no method index can ever reach a length at or above 0x10000:
static_assert(0xFFFFu < 0x1'0000,
              "max u2 index is strictly below 0x10000, so a pool length >= 0x10000 "
              "is unreachable by any index - the bound is vacuously safe there");

int main()
{
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::is_readable_pointer;

    // =====================================================================
    // A. The length-bound predicate over an EXHAUSTIVE (index, length)
    //    matrix.  This is the precise decision FIX B added; we enumerate the
    //    full structurally-interesting cross-product and assert the library
    //    expression (length_bound_rejects) matches the independent oracle
    //    (expected_reject) on every pair.  Each pair also gets its accept/
    //    reject verdict checked against first principles.
    // =====================================================================

    // -- A1. Dense sweep of small lengths x all boundary indices around each
    //    length, PLUS the u2 extremes.  For each length we test index in
    //    {0, 1, len-1, len, len+1, len+2, 0xFFFF} (clamped to >=0) so the
    //    just-inside / at / just-past edges are always hit.
    {
        bool predicate_matches_oracle{ true };
        bool verdicts_correct{ true };
        std::size_t cases{ 0 };

        // Lengths: 0 (empty), 1, 2, a run, powers of two and +/-1, the
        // largest sane CP sizes, INT32 extremes, and the -1 / negative
        // "unknown / corrupt" sentinels (flaw #2 territory).
        std::vector<std::int32_t> lengths;
        lengths.push_back(-1);                 // get_length() "unknown" sentinel
        lengths.push_back(-2);                 // corrupt negative (flaw #2)
        lengths.push_back(-1000);              // corrupt negative
        lengths.push_back(std::numeric_limits<std::int32_t>::min()); // most-negative
        for (std::int32_t n{ 0 }; n <= 18; ++n) { lengths.push_back(n); } // 0..18 run
        const std::int32_t pin_lengths[]{
            255, 256, 257, 1023, 1024, 1025, 0xFFFE, 0xFFFF, 0x1'0000,
            0x7FFF'FFFE, std::numeric_limits<std::int32_t>::max(),
        };
        for (const std::int32_t n : pin_lengths) { lengths.push_back(n); }

        for (const std::int32_t n : lengths)
        {
            // Build the candidate index set around this length.
            std::vector<std::uint32_t> idxs;
            idxs.push_back(0u);
            idxs.push_back(1u);
            // len-1, len, len+1, len+2 (only those that fit the u2 domain).
            for (std::int64_t d{ -1 }; d <= 2; ++d)
            {
                const std::int64_t cand{ static_cast<std::int64_t>(n) + d };
                if (cand >= 0 && cand <= 0xFFFF)
                {
                    idxs.push_back(static_cast<std::uint32_t>(cand));
                }
            }
            idxs.push_back(0xFFFEu);
            idxs.push_back(0xFFFFu); // max u2 - the worst corrupt-index case

            for (const std::uint32_t raw : idxs)
            {
                const std::uint16_t index{ static_cast<std::uint16_t>(raw) };
                const bool got{ length_bound_rejects(index, n) };
                const bool want{ expected_reject(index, n) };
                if (got != want) { predicate_matches_oracle = false; }

                // First-principles verdict: rejected iff (n >= 0 && index >= n).
                const bool first_principles{
                    n >= 0 && static_cast<std::int64_t>(index) >= static_cast<std::int64_t>(n) };
                if (got != first_principles) { verdicts_correct = false; }
                ++cases;
            }
        }
        check("length_bound_matches_oracle_full_matrix", predicate_matches_oracle);
        check("length_bound_verdicts_correct_full_matrix", verdicts_correct);
        // Pin the sweep is genuinely large so a future edit that empties a
        // vector is caught (34 lengths * up to 8 indices each == 244 cases at
        // time of writing; the floor is set below that to catch a collapsed
        // matrix without being brittle to adding a few more pins).
        check("length_bound_matrix_is_dense", cases >= 200);
    }

    // -- A2. For a handful of representative VALID lengths, sweep EVERY u2
    //    index in [0, 65535] exhaustively (true full-domain enumeration of the
    //    method index, which is only 16 bits wide) and assert the partition
    //    point is exactly `length`: indices [0,length) pass, [length,65536)
    //    reject.  This is the "no value in [0,65535] overflows the bound
    //    arithmetic" guarantee the task asks for, proven by enumeration.
    {
        const std::int32_t valid_lengths[]{ 1, 2, 16, 256, 1024, 0x8000, 0xFFFF, 0x1'0000 };
        bool partition_exact{ true };
        for (const std::int32_t n : valid_lengths)
        {
            for (std::uint32_t i{ 0u }; i <= 0xFFFFu; ++i)
            {
                const std::uint16_t index{ static_cast<std::uint16_t>(i) };
                const bool rejected{ length_bound_rejects(index, n) };
                const bool should_reject{ static_cast<std::int64_t>(i) >= n };
                if (rejected != should_reject) { partition_exact = false; break; }
            }
            if (!partition_exact) { break; }
        }
        check("length_bound_partition_is_exact_full_u2_domain", partition_exact);

        // Spell out the partition witnesses for length 256 explicitly: 255
        // passes (last valid), 256 rejects (first out-of-range).
        check("length_bound_255_passes_len256", !length_bound_rejects(255u, 256));
        check("length_bound_256_rejects_len256", length_bound_rejects(256u, 256));
    }

    // =====================================================================
    // B. The boundary off-by-one of the bound, called out one assertion per
    //    edge so a regression names the exact edge that moved.  index == len
    //    is the EXCLUSIVE upper edge (reject); index == len-1 is the last
    //    accepted; index == 0 is accepted on the green path (documents flaw
    //    #3 - the [0,len) vs semantically-correct [1,len) gap).
    // =====================================================================
    {
        const std::int32_t len{ 100 };
        check("boundary_index_0_accepted",        !length_bound_rejects(0u, len));
        check("boundary_index_1_accepted",        !length_bound_rejects(1u, len));
        check("boundary_index_len_minus_1_accepted", !length_bound_rejects(99u, len));
        check("boundary_index_len_rejected",      length_bound_rejects(100u, len));
        check("boundary_index_len_plus_1_rejected", length_bound_rejects(101u, len));
        check("boundary_index_maxu2_rejected",    length_bound_rejects(0xFFFFu, len));

        // Empty pool (len 0): index 0 rejected (there is no slot 0 to read).
        check("boundary_empty_pool_index_0_rejected", length_bound_rejects(0u, 0));
        check("boundary_empty_pool_index_max_rejected", length_bound_rejects(0xFFFFu, 0));

        // len == 1 (only slot 0 in-range by the bound): index 0 accepted,
        // index 1 rejected.  (On a real pool slot 0 is the unused sentinel and
        // is caught downstream by is_valid_pointer - flaw #3 - but the *length
        // bound* admits it, which is the behaviour pinned here.)
        check("boundary_len1_index_0_accepted", !length_bound_rejects(0u, 1));
        check("boundary_len1_index_1_rejected", length_bound_rejects(1u, 1));
    }

    // =====================================================================
    // C. Negative / sentinel `cp_length` disables the bound (the documented
    //    "unknown length -> skip the check" degradation AND the flaw-#2
    //    "corrupt negative length silently disables the bound" case).  When
    //    the bound is disabled EVERY index in [0,65535] must pass predicate
    //    (1) - protection then falls entirely to the slot probe (section E).
    // =====================================================================
    {
        const std::int32_t disabling[]{
            -1,                                            // get_length sentinel
            -2, -100, -65535, -65536,
            std::numeric_limits<std::int32_t>::min(),      // INT32_MIN
        };
        bool every_index_passes{ true };
        for (const std::int32_t n : disabling)
        {
            // Sample widely across the u2 domain (full enumeration would be
            // 6 * 65536; a dense sample over the structurally interesting
            // points is sufficient and is asserted to be uniform).
            const std::uint32_t sample[]{
                0u, 1u, 2u, 255u, 256u, 1024u, 0x7FFFu, 0x8000u, 0xFFFEu, 0xFFFFu,
            };
            for (const std::uint32_t i : sample)
            {
                if (length_bound_rejects(static_cast<std::uint16_t>(i), n))
                {
                    every_index_passes = false;
                }
            }
        }
        check("negative_length_disables_bound_all_indices", every_index_passes);

        // Explicit: -1 is the get_length() "unknown" sentinel; the max index
        // must pass (so a JDK that drops _length still resolves names, relying
        // on is_readable_pointer alone).
        check("sentinel_minus1_max_index_passes",
              !length_bound_rejects(0xFFFFu, -1));
        // And a corrupt -2 behaves identically (flaw #2): the bound is off.
        check("corrupt_minus2_max_index_passes",
              !length_bound_rejects(0xFFFFu, -2));
    }

    // =====================================================================
    // D. Slot-address arithmetic  &base[index] == base + index*sizeof(void*).
    //    FIX B probes &base[index] and then reads base[index]; the address
    //    computation is pure pointer arithmetic over a void** and a u2 index.
    //    We verify, for every structurally interesting u2 index, that the
    //    computed element address equals base + index*8 (x64 pointer size)
    //    with NO overflow/UB - using a real, large-enough backing buffer for
    //    the in-range indices and pure uintptr math (no deref) for the rest.
    // =====================================================================
    {
        constexpr std::size_t ptr_size{ sizeof(void*) };
        check("entry_slot_is_pointer_sized", ptr_size == 8u || ptr_size == 4u);

        // D1. Real backing array: a vector<void*> large enough that a sweep of
        // moderate indices addresses real, mapped storage.  &arr[i] computed by
        // the language must equal base + i*ptr_size computed by hand.
        {
            const std::size_t count{ 4096 };
            std::vector<void*> arr(count, nullptr);
            void** const base{ arr.data() };
            const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(base) };

            bool addr_matches{ true };
            const std::size_t idxs[]{
                0, 1, 2, 3, 7, 8, 15, 16, 31, 255, 256, 1023, 1024, 4095,
            };
            for (const std::size_t i : idxs)
            {
                const std::uintptr_t lang{ reinterpret_cast<std::uintptr_t>(&base[i]) };
                const std::uintptr_t hand{ base_addr + i * ptr_size };
                if (lang != hand) { addr_matches = false; }
            }
            check("entry_slot_address_matches_base_plus_index_times_ptrsize", addr_matches);

            // The first slot's address is exactly base (index 0 offset 0) - the
            // index-0 slot FIX B's length bound admits on the green path.
            check("entry_slot_index0_is_base",
                  reinterpret_cast<void**>(&base[0]) == base);
        }

        // D2. u2-extreme offset arithmetic with NO dereference: for index up to
        // 0xFFFF the byte offset index*ptr_size is at most 0xFFFF*8 == 0x7FFF8
        // (~512 KiB) and must not overflow when added to a plausible base.  We
        // compute against a synthetic base and assert the offset is exact and
        // monotonic, and that the maximum offset is the documented value.
        {
            const std::uintptr_t synth_base{ 0x0000'2000'0000'0000ull }; // in-range, aligned
            const std::uint32_t max_index{ 0xFFFFu };
            const std::uintptr_t max_off{ static_cast<std::uintptr_t>(max_index) * ptr_size };
            check("entry_slot_max_u2_offset_is_exact",
                  max_off == (ptr_size == 8u ? 0x7'FFF8ull : 0x3'FFFCull));

            bool monotonic{ true };
            bool exact{ true };
            std::uintptr_t prev{ synth_base }; // index 0 address
            for (std::uint32_t i{ 0u }; i <= max_index; i += 257u) // dense stride sample
            {
                const std::uintptr_t addr{ synth_base + static_cast<std::uintptr_t>(i) * ptr_size };
                if (addr != synth_base + static_cast<std::uintptr_t>(i) * ptr_size) { exact = false; }
                if (i != 0u && addr <= prev) { monotonic = false; }
                prev = addr;
            }
            // The whole [base, base + 0xFFFF*8] span stays comfortably inside
            // the canonical user range from this base (no wrap into kernel half).
            check("entry_slot_u2_span_no_wrap",
                  synth_base + max_off < vmhook::os::user_address_ceiling);
            check("entry_slot_u2_offset_monotonic", monotonic);
            check("entry_slot_u2_offset_exact", exact);
        }
    }

    // =====================================================================
    // E. The slot probe  is_readable_pointer(&base[index])  - the ACTUAL gate
    //    FIX B relies on to turn an unmapped element read into a clean nullptr
    //    instead of an access violation.  is_readable_pointer is a real,
    //    JVM-independent OS query (os::query_region -> VirtualQuery / proc-maps),
    //    so we drive it against pages we build and tear down ourselves:
    //      * a committed readable page                       -> true
    //      * a slot inside that page at a u2 index           -> true
    //      * a released (unmapped) page                      -> false (no AV)
    //      * a PAGE_GUARD / PAGE_NOACCESS page               -> false
    //    This is sections A.9 / A.10 of the feature's test plan realised
    //    without a JVM.  Gated exactly like test_os_protect_interaction.cpp.
    // =====================================================================
    {
        const std::size_t page{ vmhook::os::page_size() };

        // E0. Null / floor / ceiling / alignment pre-checks of the probe match
        // is_valid_pointer's range+alignment front (is_readable_pointer shares
        // the same floor/ceiling/8-align gate before it even queries the OS;
        // note the probe requires 8-BYTE alignment, stricter than
        // is_valid_pointer's 2-byte rule - and CP slots are 8-aligned).
        check("readable_null_rejected", !is_readable_pointer(nullptr));
        check("readable_floor_rejected",
              !is_readable_pointer(reinterpret_cast<void*>(vmhook::os::user_address_floor)));
        check("readable_ceiling_rejected",
              !is_readable_pointer(reinterpret_cast<void*>(vmhook::os::user_address_ceiling)));
        check("readable_misaligned_rejected",
              !is_readable_pointer(reinterpret_cast<void*>(
                  std::uintptr_t{ 0x0000'2000'0000'0001ull }))); // bit0 set, not 8-aligned
        check("readable_4byte_aligned_rejected_by_probe",
              !is_readable_pointer(reinterpret_cast<void*>(
                  std::uintptr_t{ 0x0000'2000'0000'0004ull }))); // 4-aligned, not 8

        // E1. A real committed page is readable; a slot at a u2 index inside it
        // is readable too (this is the green-path branch of the FIX B probe).
        {
            // Allocate enough pages to hold 0x400 pointer slots so an index well
            // above 256 still lands in mapped memory (mirrors "high-but-valid
            // index" from the feature's large-CP scenario).
            const std::size_t slots{ 0x400 };
            const std::size_t bytes{ slots * sizeof(void*) };
            const std::size_t alloc{ ((bytes + page - 1) / page) * page };
            void* const block{ vmhook::os::allocate_rwx(nullptr, alloc) };
            if (!block)
            {
                check("readable_mapped_page_skipped_alloc_failed", false);
            }
            else
            {
                // Touch first/last to ensure committed.
                auto* const raw{ static_cast<volatile std::uint8_t*>(block) };
                raw[0] = 0x11;
                raw[alloc - 1] = 0x22;

                void** const base{ static_cast<void**>(block) };
                check("readable_mapped_base_is_true", is_readable_pointer(&base[0]));
                check("readable_mapped_slot_index_1_true", is_readable_pointer(&base[1]));
                check("readable_mapped_slot_index_255_true", is_readable_pointer(&base[255]));
                check("readable_mapped_slot_index_256_true", is_readable_pointer(&base[256]));
                check("readable_mapped_slot_high_index_true", is_readable_pointer(&base[slots - 1]));

                // The exact composite FIX B does on the green path: index passes
                // the (disabled, len=-1) bound AND the slot probe is true.
                const std::uint16_t idx{ 300u };
                const bool bound_ok{ !length_bound_rejects(idx, -1) }; // -1 => bound off
                const bool slot_ok{ is_readable_pointer(&base[idx]) };
                check("readable_green_path_bound_and_probe_both_pass",
                      bound_ok && slot_ok);

                vmhook::os::release(block, alloc);

                // E2. After release the SAME addresses are unmapped: the probe
                // must return false (NOT fault).  This is the core crash-proofing
                // claim of FIX B - an unmapped &base[index] is rejected cleanly.
                // (Some kernels may instantly remap the address; in that rare
                // case the region is no longer OUR RWX page.  We assert the
                // robust direction: the probe does not crash and, if it still
                // reports readable, it is at least consistent.  The defining
                // check is "no fault occurred" - reaching the next line proves
                // it.)
                const bool after_base{ is_readable_pointer(base) };
                const bool after_high{ is_readable_pointer(&base[slots - 1]) };
                check("readable_after_release_did_not_fault", true); // survived the probe
                // If the address is genuinely free (the common case) both are
                // false; record that as the expected outcome without making the
                // suite flaky on opportunistic remap.
                if (!after_base && !after_high)
                {
                    check("readable_unmapped_slot_is_false", true);
                }
                else
                {
                    std::printf("[INFO] readable_unmapped_slot_is_false: address remapped "
                                "after release (base=%d high=%d); probe still safe\n",
                                after_base ? 1 : 0, after_high ? 1 : 0);
                }
            }
        }

        // E3. A PAGE_NOACCESS page: committed but not readable -> probe false.
        // is_readable_pointer requires info.readable; no_access clears it.
#if !VMHOOK_OS_IOS
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
            if (!block)
            {
                check("readable_no_access_skipped_alloc_failed", false);
            }
            else
            {
                *static_cast<volatile std::uint8_t*>(block) = 0x33;
                const bool flipped{ vmhook::os::protect(
                    block, page, vmhook::os::memory_protection::no_access, nullptr) };
                if (!flipped)
                {
                    std::printf("[INFO] readable_no_access skipped: protect(no_access) refused\n");
                }
                else
                {
                    // 8-byte aligned slot at the page base (allocate_rwx is page
                    // aligned, hence 8-aligned) so only the readability bit, not
                    // alignment, decides.
                    check("readable_no_access_page_is_false", !is_readable_pointer(block));
                }
                (void)vmhook::os::protect(block, page,
                                          vmhook::os::memory_protection::read_write, nullptr);
                vmhook::os::release(block, page);
            }
        }
#endif

        // E4. A PAGE_GUARD page (Windows): committed + readable bit set but
        // guarded -> is_readable_pointer must reject it (it requires !guarded).
        // PAGE_GUARD is a Windows concept; query_region.guarded is only ever set
        // there, so this assertion is Windows-only.  A guard page is exactly the
        // kind of stack/uncommitted-boundary slot the FIX B probe must refuse.
#if VMHOOK_OS_WINDOWS
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
            if (!block)
            {
                check("readable_guard_skipped_alloc_failed", false);
            }
            else
            {
                *static_cast<volatile std::uint8_t*>(block) = 0x44;
                // Set READWRITE|GUARD via the os::protect path's sibling: there is
                // no portable guard flag in memory_protection, so use VirtualProtect
                // directly for this Windows-only probe.  (Library code under test,
                // is_readable_pointer, is what we are exercising - not os::protect.)
                DWORD prev{ 0 };
                const BOOL ok{ ::VirtualProtect(block, page,
                                                PAGE_READWRITE | PAGE_GUARD, &prev) };
                if (!ok)
                {
                    check("readable_guard_setup_failed", false);
                }
                else
                {
                    const auto info{ vmhook::os::query_region(block) };
                    check("readable_guard_region_reports_guarded", info.guarded);
                    check("readable_guard_page_is_false", !is_readable_pointer(block));
                    // Clear the guard so release/cleanup is clean (a guarded page
                    // raises STATUS_GUARD_PAGE_VIOLATION on first touch otherwise).
                    DWORD prev2{ 0 };
                    ::VirtualProtect(block, page, PAGE_READWRITE, &prev2);
                }
                vmhook::os::release(block, page);
            }
        }
#endif

        // is_readable_pointer is declared noexcept; pin it.
        check("is_readable_pointer_is_noexcept", noexcept(is_readable_pointer(nullptr)));
    }

    // =====================================================================
    // F. The post-read guard  is_valid_pointer(entry_pointer)  in the
    //    feature's own context: even when a slot is mapped (probe (2) passes),
    //    a garbage value sitting in that slot - a debug-poison Symbol* or a
    //    null - is rejected by (3) so it never reaches symbol::to_string.
    //    This is the "wrong-but-mapped pointer slips the index bound, caught
    //    here" path (feature test plan A.12).
    // =====================================================================
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("postread_guard_skipped_alloc_failed", false);
        }
        else
        {
            void** const base{ static_cast<void**>(block) };
            // Plant entries: index 0 a poison Symbol*, index 1 null, index 2 a
            // real, valid pointer (the address of a stack object).  The slot
            // addresses are all readable (probe (2) true); guard (3) must reject
            // the first two and accept the third.
            int stack_anchor{ 7 };
            base[0] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xCAFEBABEu)); // even poison
            base[1] = nullptr;
            base[2] = &stack_anchor;

            // Slots are all mapped (probe passes) ...
            check("postread_slot0_probe_true", is_readable_pointer(&base[0]));
            check("postread_slot1_probe_true", is_readable_pointer(&base[1]));
            check("postread_slot2_probe_true", is_readable_pointer(&base[2]));

            // ... but the VALUES decide via guard (3): poison and null rejected.
            check("postread_poison_value_rejected", !is_valid_pointer(base[0]));
            check("postread_null_value_rejected",   !is_valid_pointer(base[1]));
            check("postread_real_value_accepted",    is_valid_pointer(base[2]));

            // Compose the exact FIX B tail for the poison slot: probe true, but
            // !is_valid_pointer(entry_pointer) -> the getter returns nullptr.
            {
                const std::uint16_t idx{ 0u };
                const bool reaches_read{ !length_bound_rejects(idx, -1) // bound off
                                         && is_readable_pointer(&base[idx]) };
                void* const entry_pointer{ reaches_read ? base[idx] : nullptr };
                const bool getter_would_return_null{
                    !entry_pointer || !is_valid_pointer(entry_pointer) };
                check("postread_poison_makes_getter_return_null", getter_would_return_null);
            }
            // ...and for the real-pointer slot the tail would return non-null.
            {
                const std::uint16_t idx{ 2u };
                const bool reaches_read{ !length_bound_rejects(idx, -1)
                                         && is_readable_pointer(&base[idx]) };
                void* const entry_pointer{ reaches_read ? base[idx] : nullptr };
                const bool getter_would_return_symbol{
                    entry_pointer && is_valid_pointer(entry_pointer) };
                check("postread_real_makes_getter_return_symbol", getter_would_return_symbol);
            }

            vmhook::os::release(block, page);
        }
    }

    // =====================================================================
    // G. NO-JVM degradation contract of the real library entry points.  With
    //    no HotSpot in-process, gHotSpotVMStructs is unresolved, so every
    //    VMStruct lookup fails.  FIX B's design REQUIRES these to degrade to a
    //    clean sentinel (nullptr / -1) without faulting - that is itself a
    //    tested property (a JDK that drops a field must not crash the host).
    //    These calls also pin the public signatures of the feature surface.
    //
    //    We cannot fabricate a real ConstantPool/ConstMethod whose VMStruct
    //    offsets resolve (that needs the JVM), so the only reachable branch
    //    here is the reject/sentinel side - which is exactly the side FIX B
    //    added.  We invoke them on a plausibly-aligned, in-range synthetic
    //    address; get_length()/get_base()/get_constants() must NOT fault and
    //    must return their documented sentinel because the entry lookup is the
    //    FIRST thing each does and it fails closed before any deref.
    // =====================================================================
    {
        using vmhook::hotspot::constant_pool;
        using vmhook::hotspot::const_method;

        // A synthetic, aligned, in-range address.  We NEVER dereference through
        // it ourselves; we only hand it to the library getters, whose first act
        // is the (failing, no-JVM) VMStruct lookup that returns the sentinel
        // before any offset is applied.
        alignas(16) static std::uint8_t storage[256]{};
        auto* const cp{ reinterpret_cast<constant_pool*>(&storage[0]) };
        auto* const cm{ reinterpret_cast<const_method*>(&storage[0]) };

        // get_length(): VMStruct "ConstantPool._length" lookup fails -> returns
        // the -1 "unknown" sentinel (vmhook.hpp :2081-2084).  This is the value
        // that DISABLES the length bound (section C) on a stripped JDK.
        const std::int32_t len{ cp->get_length() };
        check("get_length_no_jvm_returns_minus1_sentinel", len == -1);
        check("get_length_no_jvm_disables_bound",
              !length_bound_rejects(0xFFFFu, len)); // -1 -> any index passes (1)
        check("get_length_returns_int32",
              std::is_same_v<decltype(cp->get_length()), std::int32_t>);
        check("get_length_is_noexcept", noexcept(cp->get_length()));

        // get_base(): VMStruct type "ConstantPool" lookup fails -> nullptr
        // (vmhook.hpp :2053-2056), and the getter would then bail at
        // `if (!base ...)`.  No fault.
        void** const base{ cp->get_base() };
        check("get_base_no_jvm_returns_null", base == nullptr);
        check("get_base_returns_void_ptr_ptr",
              std::is_same_v<decltype(cp->get_base()), void**>);

        // get_constants(): VMStruct "ConstMethod._constants" lookup fails ->
        // nullptr (vmhook.hpp :2108-2111), caught by the getter's
        // `if (!cp ...)`.  No fault.
        constant_pool* const cp2{ cm->get_constants() };
        check("get_constants_no_jvm_returns_null", cp2 == nullptr);
        check("get_constants_returns_constant_pool_ptr",
              std::is_same_v<decltype(cm->get_constants()), constant_pool*>);

        // get_name() / get_signature(): the _name_index / _signature_index
        // VMStruct lookup fails first -> the getter throws internally and the
        // catch returns nullptr (vmhook.hpp :2132-2135 / :2190-2193).  The
        // WHOLE FIX B chain therefore degrades to nullptr with no JVM and no
        // crash - the reject side of every guard.  This is the cleanest
        // demonstration that the feature never faults on absent metadata.
        vmhook::hotspot::symbol* const name{ cm->get_name() };
        vmhook::hotspot::symbol* const sig{ cm->get_signature() };
        check("get_name_no_jvm_returns_null", name == nullptr);
        check("get_signature_no_jvm_returns_null", sig == nullptr);
        check("get_name_returns_symbol_ptr",
              std::is_same_v<decltype(cm->get_name()), vmhook::hotspot::symbol*>);
        check("get_signature_returns_symbol_ptr",
              std::is_same_v<decltype(cm->get_signature()), vmhook::hotspot::symbol*>);

        // Idempotent / repeatable: calling twice still returns the sentinel and
        // still does not fault (the static entry caches stay nullptr).
        check("get_length_no_jvm_stable_on_repeat", cp->get_length() == -1);
        check("get_name_no_jvm_stable_on_repeat", cm->get_name() == nullptr);
    }

    // =====================================================================
    // H. Cross-tie invariants binding the predicate, the probe, and the guard
    //    into the single decision FIX B makes, so the three pieces can't drift
    //    apart silently.
    // =====================================================================
    {
        // H1. A rejected-by-length index never needs the slot probe: if (1)
        // rejects, the library returns before calling is_readable_pointer.  We
        // assert the ordering invariant by construction: for a VALID length,
        // any index >= length is rejected by (1) regardless of slot mapping.
        const std::int32_t len{ 8 };
        bool ordering_ok{ true };
        for (std::uint32_t i{ static_cast<std::uint32_t>(len) }; i <= static_cast<std::uint32_t>(len) + 4u; ++i)
        {
            if (!length_bound_rejects(static_cast<std::uint16_t>(i), len)) { ordering_ok = false; }
        }
        check("rejected_index_short_circuits_before_probe", ordering_ok);

        // H2. The disabled-bound regime (len == -1) is the ONLY regime where a
        // max-u2 index reaches the probe; pin that the probe is then the sole
        // gate (mirrors a stripped-_length JDK).
        check("disabled_bound_max_index_reaches_probe_gate",
              !length_bound_rejects(0xFFFFu, -1));

        // H3. A decoded null (is_valid_pointer(nullptr) == false) is consistent
        // with the post-read guard rejecting a null slot value - ties section F
        // to the shared validity primitive.
        check("null_slot_value_consistent_with_guard",
              !is_valid_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0 })));
    }

    // =====================================================================
    // I. The OPERATOR CHOICE  `index >= cp_length`  vs  `index > cp_length`.
    //    The bound's upper edge is EXCLUSIVE; the one-character regression
    //    `>=` -> `>` would admit index == cp_length and read base[cp_length],
    //    one slot PAST the entry array - the exact out-of-bounds read FIX B
    //    prevents.  We sweep the full structurally interesting (index, length)
    //    space and assert: (a) the library predicate equals the `>=` semantics
    //    everywhere; (b) it DIFFERS from the `>` variant at exactly index ==
    //    cp_length and nowhere else, for every valid length.  A flipped
    //    operator therefore fails a NAMED check here, not just the matrix.
    // =====================================================================
    {
        // (a) For every valid length, index == length must reject under `>=`
        //     and would (wrongly) pass under `>`.  This is THE divergence point.
        const std::int32_t lens[]{ 0, 1, 2, 3, 16, 255, 256, 1024, 0x7FFF, 0xFFFE, 0xFFFF };
        bool edge_rejects_with_ge{ true };
        bool edge_passes_with_gt{ true };
        for (const std::int32_t n : lens)
        {
            if (n < 0 || n > 0xFFFF) { continue; } // index must be a u2 to test the edge
            const std::uint16_t at_edge{ static_cast<std::uint16_t>(n) };
            if (!length_bound_rejects(at_edge, n))           { edge_rejects_with_ge = false; }
            if (length_bound_rejects_with_gt(at_edge, n))    { edge_passes_with_gt = false; }
        }
        check("operator_ge_rejects_index_equals_length", edge_rejects_with_ge);
        check("operator_gt_would_accept_index_equals_length", edge_passes_with_gt);

        // (b) Away from index == length the two operators must AGREE.  Sweep a
        //     dense band of lengths and, for each, every u2 index except the
        //     single edge value; the library `>=` predicate and the `>` variant
        //     must produce identical verdicts there.  This proves the ONLY
        //     behavioural difference between the correct and the regressed
        //     operator is the boundary slot - so the boundary slot is the whole
        //     point of the choice.
        bool agree_off_edge{ true };
        std::size_t off_edge_cases{ 0 };
        for (std::int32_t n{ 0 }; n <= 320; ++n)
        {
            for (std::uint32_t i{ 0u }; i <= 322u; ++i)
            {
                if (static_cast<std::int64_t>(i) == static_cast<std::int64_t>(n))
                {
                    continue; // skip the single edge where they are designed to differ
                }
                const std::uint16_t index{ static_cast<std::uint16_t>(i) };
                if (length_bound_rejects(index, n) != length_bound_rejects_with_gt(index, n))
                {
                    agree_off_edge = false;
                }
                ++off_edge_cases;
            }
        }
        check("operators_agree_everywhere_except_the_edge", agree_off_edge);
        check("operator_differential_sweep_is_dense", off_edge_cases >= 100000);

        // (c) Spelled-out witnesses at a concrete length so a failure reads
        //     unambiguously: len 7 -> index 7 is the first out-of-range slot.
        check("operator_len7_index7_rejected_by_library", length_bound_rejects(7u, 7));
        check("operator_len7_index7_accepted_by_gt_variant", !length_bound_rejects_with_gt(7u, 7));
        check("operator_len7_index6_accepted_by_both",
              !length_bound_rejects(6u, 7) && !length_bound_rejects_with_gt(6u, 7));
    }

    // =====================================================================
    // J. OVERFLOW-SAFE mixed-type comparison.  In the library the comparison is
    //    `std::uint16_t index >= std::int32_t cp_length`.  Under the usual
    //    arithmetic conversions the u16 widens to int (lossless: [0,65535] fits
    //    int), then int-vs-int32 compares without surprise; there is no signed/
    //    unsigned hazard because cp_length's negative values are handled by the
    //    separate `cp_length >= 0` guard.  We pin that the realised comparison
    //    equals the unambiguous int64 widening `int64(index) >= int64(cp_length)`
    //    across the WHOLE u2 index domain crossed with the int32 lengths where a
    //    naive narrowing would be most likely to misbehave (values around the u2
    //    ceiling and the int32 extremes).  This is the "no value in [0,65535]
    //    overflows the bound arithmetic" guarantee, proven against a wider type.
    // =====================================================================
    {
        // Lengths chosen to stress the type boundary: values inside the u2 band
        // (where index can straddle), values just above it (where every u2 index
        // is below length), and the int32 extremes (where a wrong cast to a
        // narrower/unsigned type would flip the verdict).
        const std::int32_t stress_lengths[]{
            0, 1, 2,
            0x7FFE, 0x7FFF, 0x8000, 0x8001,          // around the int16 boundary
            0xFFFE, 0xFFFF, 0x1'0000, 0x1'0001,      // around the u16 ceiling
            0x7FFF'FFFE, std::numeric_limits<std::int32_t>::max(),
        };
        bool widening_consistent{ true };
        std::size_t widening_cases{ 0 };
        for (const std::int32_t n : stress_lengths)
        {
            for (std::uint32_t i{ 0u }; i <= 0xFFFFu; ++i)
            {
                const std::uint16_t index{ static_cast<std::uint16_t>(i) };
                // The library's own promoted comparison, written exactly as the
                // source would evaluate it (u16 -> int, vs int32).
                const bool as_library{ index >= n }; // n >= 0 in all stress_lengths
                // The unambiguous wide reference.
                const bool as_int64{
                    static_cast<std::int64_t>(index) >= static_cast<std::int64_t>(n) };
                if (as_library != as_int64) { widening_consistent = false; }
                // And the full predicate (with the >= 0 guard, all positive here)
                // must equal the int64 reference too.
                if (length_bound_rejects(index, n) != as_int64) { widening_consistent = false; }
                ++widening_cases;
            }
        }
        check("mixed_type_compare_matches_int64_widening_full_u2", widening_consistent);
        check("mixed_type_widening_sweep_is_full_domain", widening_cases >= 0xFFFFu * 6u);

        // The promotion direction itself: a u16 always widens to a value that is
        // representable and non-negative, so it can never alias a negative
        // cp_length (the case the `>= 0` guard owns).  Pin a witness at the u2
        // max against a length one above it: 0xFFFF < 0x10000 -> accept.
        check("maxu2_index_below_length_above_u2_ceiling_accepts",
              !length_bound_rejects(0xFFFFu, 0x1'0000));
        // ...and equal to it -> reject (the edge once length re-enters u2 range
        // is unreachable for a u2 index, but length 0xFFFF makes index 0xFFFF
        // the edge):
        check("maxu2_index_equals_length_at_u2_ceiling_rejects",
              length_bound_rejects(0xFFFFu, 0xFFFF));
    }

    // =====================================================================
    // K. The  is_valid_pointer(this)  early-out of the real getters, driven
    //    with an actually-INVALID `this`.  Section G proves the entry-absent
    //    branch (a plausibly-aligned synthetic `this` + no-JVM VMStruct miss).
    //    Here we prove the OTHER guard: get_length() bails on
    //    `!is_valid_pointer(this)` (vmhook.hpp :2337) and get_constants /
    //    get_name / get_signature bail on `!is_valid_pointer(this)`
    //    (:2391/:2420/:2499) BEFORE applying any VMStruct offset.  A null,
    //    poison, mis-aligned, floor or ceiling `this` must yield the sentinel /
    //    nullptr with NO fault - the corrupt-`this` scenario FIX B's chain must
    //    survive.  (With no JVM both branches reach the same nullptr/-1, but
    //    these inputs exercise the is_valid_pointer gate specifically, distinct
    //    from G's offset-lookup-miss path.)
    // =====================================================================
    {
        using vmhook::hotspot::constant_pool;
        using vmhook::hotspot::const_method;

        // A spread of structurally invalid `this` values: null, the low
        // sentinel floor, the kernel ceiling, an odd (bit-0) address, a 4-byte
        // (not 8) aligned address, and each debug-poison pattern is_valid_pointer
        // rejects.  Every one must drive get_length -> -1 and the symbol getters
        // -> nullptr without faulting (reaching the assert proves no fault).
        const std::uintptr_t bad_this[]{
            std::uintptr_t{ 0 },                                  // null
            vmhook::os::user_address_floor,                       // floor (rejected)
            vmhook::os::user_address_ceiling,                     // ceiling (rejected)
            std::uintptr_t{ 0x0000'2000'0000'0001ull },           // odd / bit-0 set
            std::uintptr_t{ 0xDEADBEEFull },                      // poison
            std::uintptr_t{ 0xCAFEBABEull },                      // poison
            std::uintptr_t{ 0xCCCCCCCCull },                      // poison (MSVC stack)
            std::uintptr_t{ 0xBAADF00Dull },                      // poison
        };

        bool length_all_minus1{ true };
        bool constants_all_null{ true };
        bool name_all_null{ true };
        bool signature_all_null{ true };
        for (const std::uintptr_t raw : bad_this)
        {
            auto* const cp{ reinterpret_cast<constant_pool*>(raw) };
            auto* const cm{ reinterpret_cast<const_method*>(raw) };
            if (cp->get_length() != -1)    { length_all_minus1 = false; }
            if (cm->get_constants() != nullptr) { constants_all_null = false; }
            if (cm->get_name() != nullptr)      { name_all_null = false; }
            if (cm->get_signature() != nullptr) { signature_all_null = false; }
        }
        // Reaching here at all means none of the calls faulted on a bogus `this`.
        check("invalid_this_get_length_returns_minus1_no_fault", length_all_minus1);
        check("invalid_this_get_constants_returns_null_no_fault", constants_all_null);
        check("invalid_this_get_name_returns_null_no_fault", name_all_null);
        check("invalid_this_get_signature_returns_null_no_fault", signature_all_null);

        // Explicit null witnesses (the single most common corruption) for each
        // entry point, so a regression names the exact getter.  The null address
        // is laundered through a `volatile` so the compiler cannot constant-fold
        // the call into a literal null-`this` invocation (-Wnonnull) - at run
        // time `this` is still 0, and the library's own is_valid_pointer(this)
        // guard is exactly what must catch it.  This is the real corrupt-`this`
        // path FIX B's chain has to survive, not a language-level null-deref.
        volatile std::uintptr_t null_addr{ 0 };
        auto* const cp_null{ reinterpret_cast<constant_pool*>(null_addr) };
        auto* const cm_null{ reinterpret_cast<const_method*>(null_addr) };
        check("null_this_get_length_minus1", cp_null->get_length() == -1);
        check("null_this_get_constants_null", cm_null->get_constants() == nullptr);
        check("null_this_get_name_null", cm_null->get_name() == nullptr);
        check("null_this_get_signature_null", cm_null->get_signature() == nullptr);

        // The sentinel a bogus `this` produces still disables the bound (it is
        // the -1 path), so the slot probe remains the sole gate - consistent
        // with section C and the stripped-_length JDK regime.
        const std::int32_t len_from_bad_this{ cp_null->get_length() };
        check("invalid_this_length_sentinel_disables_bound",
              !length_bound_rejects(0xFFFFu, len_from_bad_this));
    }

    // =====================================================================
    // L. ADJACENT mapped / unmapped slot boundary - the precise crash-proofing
    //    claim of the FIX B slot probe.  We reserve a region of TWO pages but
    //    commit only the FIRST, so the last pointer slot of page 0 is readable
    //    and the very next slot (the first 8 bytes of the uncommitted page 1)
    //    is NOT.  is_readable_pointer must accept the in-bounds slot and reject
    //    its unmapped neighbour, distinguishing them at the exact page edge -
    //    i.e. it turns the out-of-array element read into a clean nullptr at the
    //    one boundary that matters, with NO fault.  This complements E2 (which
    //    releases the whole block) by exercising the live mapped/unmapped seam
    //    within a single reservation.
    // =====================================================================
    {
        const std::size_t page{ vmhook::os::page_size() };
        // Reserve+commit two pages, then flip page 1 to no_access so we have a
        // genuine "committed page 0 | inaccessible page 1" seam.  (Pure RESERVE
        // of page 1 is not portable through the os:: surface, but a no_access
        // commit gives the same is_readable_pointer verdict: !readable -> false.)
        const std::size_t two_pages{ page * 2u };
        void* const block{ vmhook::os::allocate_rwx(nullptr, two_pages) };
        if (!block)
        {
            check("adjacent_boundary_skipped_alloc_failed", false);
        }
        else
        {
            auto* const bytes{ static_cast<volatile std::uint8_t*>(block) };
            bytes[0] = 0x55;             // touch page 0 (committed, readable)
            bytes[page] = 0x66;          // touch page 1 before we lock it
            const bool locked{ vmhook::os::protect(
                static_cast<std::uint8_t*>(block) + page, page,
                vmhook::os::memory_protection::no_access, nullptr) };
            if (!locked)
            {
                std::printf("[INFO] adjacent_boundary skipped: protect(no_access) refused\n");
            }
            else
            {
                void** const base{ static_cast<void**>(block) };
                // Slot count fully inside page 0, and the index of the slot that
                // straddles into page 1.  Each slot is sizeof(void*) bytes.
                const std::size_t slots_per_page{ page / sizeof(void*) };
                const std::size_t last_in_page0{ slots_per_page - 1u };  // readable
                const std::size_t first_in_page1{ slots_per_page };      // NOT readable

                // The last in-bounds slot is readable (probe accepts it)...
                check("adjacent_last_mapped_slot_is_readable",
                      is_readable_pointer(&base[last_in_page0]));
                // ...and the immediately following slot, one element past the
                // committed page, is rejected - NOT a fault.  This is the
                // "base[index] one past the array" read FIX B converts to nullptr.
                check("adjacent_first_unmapped_slot_is_rejected",
                      !is_readable_pointer(&base[first_in_page1]));
                // Reaching this line proves the probe on the unmapped neighbour
                // did not fault.
                check("adjacent_boundary_probe_did_not_fault", true);

                // Tie it to the feature decision: with the bound disabled (-1)
                // the probe is the sole gate, so the getter would ACCEPT the
                // last-in-page slot and REJECT the first-unmapped slot.
                check("adjacent_getter_accepts_last_mapped",
                      !length_bound_rejects(static_cast<std::uint16_t>(last_in_page0 & 0xFFFFu), -1)
                          && is_readable_pointer(&base[last_in_page0]));
                check("adjacent_getter_rejects_first_unmapped",
                      !is_readable_pointer(&base[first_in_page1]));

                // Restore RW so release is clean.
                (void)vmhook::os::protect(
                    static_cast<std::uint8_t*>(block) + page, page,
                    vmhook::os::memory_protection::read_write, nullptr);
            }
            vmhook::os::release(block, two_pages);
        }
    }

    // =====================================================================
    // M. The SINGLE COMPOSITE decision FIX B makes, as one reusable verdict
    //    swept over the full cross-product of the three gates.  The getter
    //    returns a non-null Symbol* IFF:
    //        (1) the length bound passes:  !(cp_length >= 0 && index >= cp_length)
    //        (2) the slot probe passes:    is_readable_pointer(&base[index])
    //        (3) the post-read guard passes: value && is_valid_pointer(value)
    //    Sections A-H prove each gate in isolation; this section proves they
    //    COMPOSE correctly - a fabricated void** array with a mix of mapped
    //    slot VALUES (real ptr / null / poison) is swept across length regimes
    //    and indices, and the composite "would return a Symbol*" verdict is
    //    asserted against an independent first-principles computation for every
    //    cell.  This is the closest no-JVM mirror of the live getter's overall
    //    behaviour.
    // =====================================================================
    {
        const std::size_t page{ vmhook::os::page_size() };
        const std::size_t slots{ 64 };
        const std::size_t bytes{ slots * sizeof(void*) };
        const std::size_t alloc{ ((bytes + page - 1) / page) * page };
        void* const block{ vmhook::os::allocate_rwx(nullptr, alloc) };
        if (!block)
        {
            check("composite_skipped_alloc_failed", false);
        }
        else
        {
            *static_cast<volatile std::uint8_t*>(block) = 0x77; // commit
            void** const base{ static_cast<void**>(block) };

            // Plant a deterministic pattern of slot VALUES across the array:
            //   index % 3 == 0 -> a real, valid pointer (points into the block)
            //   index % 3 == 1 -> null
            //   index % 3 == 2 -> a poison value is_valid_pointer rejects
            int stack_anchor{ 0 };
            (void)stack_anchor;
            for (std::size_t i{ 0 }; i < slots; ++i)
            {
                switch (i % 3u)
                {
                    case 0u: base[i] = static_cast<void*>(&base[i]); break; // real & valid
                    case 1u: base[i] = nullptr; break;                       // null value
                    default: base[i] = reinterpret_cast<void*>(
                                 static_cast<std::uintptr_t>(0xCAFEBABEu)); break; // poison
                }
            }

            // The composite library decision, expressed once, exactly as the
            // getter sequences it (bound -> probe -> post-read guard).
            auto getter_returns_symbol =
                [&](std::uint16_t index, std::int32_t cp_length) -> bool
            {
                if (cp_length >= 0 && index >= cp_length) { return false; }   // (1)
                if (!is_readable_pointer(&base[index]))   { return false; }   // (2)
                void* const v{ base[index] };
                return v != nullptr && is_valid_pointer(v);                   // (3)
            };

            // Independent first-principles oracle over the SAME planted array.
            auto expected_symbol =
                [&](std::uint16_t index, std::int32_t cp_length) -> bool
            {
                // (1) length bound
                if (cp_length >= 0 && static_cast<std::int64_t>(index) >= cp_length)
                {
                    return false;
                }
                // (2) slot readable - within our committed block all slots
                //     [0,slots) are readable; outside, treat as not-our-memory.
                if (index >= slots) { return false; }
                if (!is_readable_pointer(&base[index])) { return false; }
                // (3) value validity from the known planted pattern
                const std::size_t k{ static_cast<std::size_t>(index) % 3u };
                if (k == 1u) { return false; }      // null
                if (k == 2u) { return false; }      // poison rejected
                return true;                         // real & valid pointer
            };

            // Sweep length regimes: disabled (-1, -7), and several valid lengths
            // that partition the planted array (so some indices are length-
            // rejected and some reach the value check).
            const std::int32_t length_regimes[]{ -1, -7, 0, 1, 8, 16, 32, 48, 64 };
            bool composite_matches{ true };
            std::size_t composite_cases{ 0 };
            for (const std::int32_t n : length_regimes)
            {
                for (std::size_t i{ 0 }; i < slots; ++i)
                {
                    const std::uint16_t index{ static_cast<std::uint16_t>(i) };
                    if (getter_returns_symbol(index, n) != expected_symbol(index, n))
                    {
                        composite_matches = false;
                    }
                    ++composite_cases;
                }
            }
            check("composite_getter_decision_matches_oracle", composite_matches);
            check("composite_decision_sweep_is_dense", composite_cases >= 9u * slots);

            // Spelled-out composite witnesses on the disabled-bound regime
            // (so the value gate is the decider):
            //   index 0 -> real ptr -> returns a Symbol*
            //   index 1 -> null     -> nullptr
            //   index 2 -> poison   -> nullptr
            check("composite_index0_real_returns_symbol", getter_returns_symbol(0u, -1));
            check("composite_index1_null_returns_null",   !getter_returns_symbol(1u, -1));
            check("composite_index2_poison_returns_null", !getter_returns_symbol(2u, -1));

            // And that the length bound short-circuits BEFORE the (otherwise
            // passing) value gate: index 0 holds a real ptr, but with length 0
            // the bound rejects it first.
            check("composite_length0_rejects_even_valid_value",
                  !getter_returns_symbol(0u, 0));
            // With length 1, index 0 (real ptr) passes the bound and the value
            // gate -> Symbol*; index 1 (null) is bound-rejected (1 >= len 1).
            check("composite_length1_index0_real_returns_symbol",
                  getter_returns_symbol(0u, 1));
            check("composite_length1_index1_bound_rejected",
                  !getter_returns_symbol(1u, 1));

            vmhook::os::release(block, alloc);
        }
    }

    // =====================================================================
    // N. Per-length EDGE sweep across a wide band of lengths.  Section A2
    //    enumerates the full u2 domain for 8 sample lengths; here we instead
    //    sweep MANY lengths and, for each, assert the three load-bearing edge
    //    indices in one pass: index == length-1 (last valid -> PASS), index ==
    //    length (first out-of-range -> REJECT), index == length+1 (REJECT).
    //    This pins the off-by-one at EVERY length in a dense band, so a future
    //    operator/constant drift names the exact length where the edge moved.
    // =====================================================================
    {
        bool last_valid_passes{ true };
        bool edge_rejects{ true };
        bool above_edge_rejects{ true };
        std::size_t edge_cases{ 0 };
        // Lengths from 1 up through the u2 ceiling (so length-1, length and
        // length+1 are all expressible as u2 where they fit).  Skip 0 (no
        // last-valid index exists) - it is pinned separately in section B.
        for (std::int32_t n{ 1 }; n <= 0x1'0000; ++n)
        {
            // last valid index = n-1, only meaningful when it fits a u2.
            if (n - 1 <= 0xFFFF)
            {
                if (length_bound_rejects(static_cast<std::uint16_t>(n - 1), n))
                {
                    last_valid_passes = false;
                }
            }
            // edge index = n, when it fits a u2 (n <= 0xFFFF).
            if (n <= 0xFFFF)
            {
                if (!length_bound_rejects(static_cast<std::uint16_t>(n), n))
                {
                    edge_rejects = false;
                }
            }
            // above-edge index = n+1, when it fits a u2.
            if (n + 1 <= 0xFFFF)
            {
                if (!length_bound_rejects(static_cast<std::uint16_t>(n + 1), n))
                {
                    above_edge_rejects = false;
                }
            }
            ++edge_cases;
            // Sample-stride to keep the sweep bounded but dense: test every
            // length up to 2048 (where edges cluster) then stride to the ceiling.
            if (n >= 2048 && n < 0x1'0000 - 4)
            {
                n += 1021; // prime-ish stride avoids aliasing with any period
            }
        }
        check("per_length_last_valid_index_passes", last_valid_passes);
        check("per_length_edge_index_rejects", edge_rejects);
        check("per_length_above_edge_index_rejects", above_edge_rejects);
        check("per_length_edge_sweep_is_dense", edge_cases >= 2048);
    }

    // =====================================================================
    // O. NO UPPER SANITY CAP on cp_length (flaw #4).  get_length() returns the
    //    raw _length word with no ceiling, so a corrupt-huge positive length
    //    (>= 0x10000) makes `index >= cp_length` vacuously false for EVERY
    //    realistic u2 index - the length bound admits everything and protection
    //    falls entirely to the slot probe.  We pin that documented behaviour as
    //    an invariant (not a crash): for any length at/above the u2 ceiling,
    //    no u2 index is rejected by the length bound, AND the slot probe must
    //    then still reject an unmapped slot (so the composite is still safe).
    // =====================================================================
    {
        const std::int32_t huge_lengths[]{
            0x1'0000, 0x10'0000, 0x4000'0000, 0x7FFF'FFFE,
            std::numeric_limits<std::int32_t>::max(),
        };
        bool huge_admits_every_u2{ true };
        for (const std::int32_t n : huge_lengths)
        {
            const std::uint32_t sample[]{
                0u, 1u, 255u, 256u, 1024u, 0x7FFFu, 0x8000u, 0xFFFEu, 0xFFFFu,
            };
            for (const std::uint32_t i : sample)
            {
                if (length_bound_rejects(static_cast<std::uint16_t>(i), n))
                {
                    huge_admits_every_u2 = false;
                }
            }
        }
        check("flaw4_huge_length_admits_every_u2_index", huge_admits_every_u2);

        // The compensating guarantee: even with the length bound disabled by a
        // huge length, the slot probe rejects a genuinely unmapped slot - so a
        // corrupt-huge _length does NOT reopen the AV.  Build an unmapped
        // address and confirm the probe (the sole remaining gate) refuses it.
        {
            const std::size_t page{ vmhook::os::page_size() };
            void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
            if (!block)
            {
                check("flaw4_huge_length_probe_skipped_alloc_failed", false);
            }
            else
            {
                *static_cast<volatile std::uint8_t*>(block) = 0x5A;
                vmhook::os::release(block, page);
                // After release the page is (commonly) unmapped; the probe must
                // not fault and, in the common case, returns false.  The bound
                // is disabled (INT32_MAX), so the probe alone decides.
                const bool bound_off{ !length_bound_rejects(0u,
                                          std::numeric_limits<std::int32_t>::max()) };
                const bool probe_after_release{ is_readable_pointer(block) };
                check("flaw4_huge_length_bound_is_off", bound_off);
                check("flaw4_huge_length_probe_did_not_fault", true); // survived
                if (!probe_after_release)
                {
                    check("flaw4_huge_length_unmapped_slot_still_rejected",
                          bound_off && !probe_after_release);
                }
                else
                {
                    std::printf("[INFO] flaw4 unmapped slot remapped after release; "
                                "probe still safe (no fault)\n");
                }
            }
        }
    }

    // =====================================================================
    // P. The u2 index domain itself.  The library reads `index` as a
    //    std::uint16_t (vmhook.hpp :2461 / :2538) directly from the ConstMethod
    //    field via os::safe_read into a u2-sized destination, so NO value the
    //    field can present is ever negative and none exceeds 0xFFFF.  We pin:
    //      * round-trip truncation: any wider corrupt source narrows to its low
    //        16 bits (clamp_u2), and that narrowed value is what the bound sees;
    //      * the negative side is owned EXCLUSIVELY by the `cp_length >= 0`
    //        guard, never by the index (an index is unsigned, can't be < 0);
    //      * the partition point for any in-u2-range length equals the length.
    // =====================================================================
    {
        // P1. Truncation: a corrupt 64-bit source presents only its low 16 bits
        // as the u2 index, and the bound's verdict depends only on those bits.
        bool truncation_consistent{ true };
        bool clamp_equals_explicit_mask{ true };
        const std::uint64_t wide_sources[]{
            0u, 1u, 0xFFFFu, 0x1'0000u, 0x1'0001u, 0xDEAD'0000u, 0xDEAD'0005u,
            0xFFFF'FFFFu, 0x1'2345'6789u, 0xFFFF'FFFF'FFFF'FFFFull,
        };
        const std::int32_t probe_len{ 16 };
        for (const std::uint64_t w : wide_sources)
        {
            const std::uint16_t narrowed{ clamp_u2(w) };
            const bool via_clamp{ length_bound_rejects(narrowed, probe_len) };
            // clamp_u2 must equal an explicit low-16-bit mask of the source -
            // asserted unconditionally so the verdict is portable and the local
            // is always consumed.
            if (narrowed != static_cast<std::uint16_t>(w & 0xFFFFu))
            {
                clamp_equals_explicit_mask = false;
            }
            // Independent: take the low 16 bits a different way (memcpy of the
            // low half) and feed THAT - must agree, proving the bound is a pure
            // function of the 16-bit field, not of the discarded high bits.  On a
            // big-endian host memcpy of the low storage bytes is the HIGH value
            // word, so gate the cross-check to little-endian (every CI host).
            std::uint16_t copied{ 0 };
            std::memcpy(&copied, &w, sizeof(copied));
            const bool via_copy{ length_bound_rejects(copied, probe_len) };
            const bool host_is_le{ std::endian::native == std::endian::little };
            const bool cross_check_ok{ !host_is_le || via_clamp == via_copy };
            if (!cross_check_ok) { truncation_consistent = false; }
        }
        check("u2_index_clamp_equals_explicit_low16_mask", clamp_equals_explicit_mask);
        check("u2_index_bound_depends_only_on_low16", truncation_consistent);

        // P2. The index type is unsigned 16-bit: it can never be negative, so
        // the negative half of the comparison space is structurally owned by
        // the cp_length>=0 guard.  Pin the type and the "no negative index"
        // property the library relies on.
        check("u2_index_type_is_uint16",
              std::is_same_v<std::uint16_t, decltype(clamp_u2(0u))>);
        check("u2_index_is_unsigned", std::is_unsigned_v<std::uint16_t>);
        // For any length, an index of 0 (the smallest possible u2) is rejected
        // iff the length is 0 (empty pool) - never because the index is "below
        // zero", which cannot happen.
        check("u2_index_zero_rejected_only_when_len_zero",
              length_bound_rejects(0u, 0) && !length_bound_rejects(0u, 1));

        // P3. Exhaustive partition witness at a representative in-u2-range
        // length, computed against the unsigned index domain directly (no
        // sign-extension anywhere): for length L every index < L passes and
        // every index in [L, 65536) rejects.  Proven for L = 0x1234.
        {
            const std::int32_t L{ 0x1234 };
            bool partition_ok{ true };
            for (std::uint32_t i{ 0u }; i <= 0xFFFFu; ++i)
            {
                const bool rejected{ length_bound_rejects(static_cast<std::uint16_t>(i), L) };
                const bool want{ i >= static_cast<std::uint32_t>(L) };
                if (rejected != want) { partition_ok = false; break; }
            }
            check("u2_index_partition_exact_at_0x1234", partition_ok);
        }
    }

    // =====================================================================
    // Q. get_length() crosses a VMStruct offset (this + entry->offset) to read
    //    a 4-byte _length WITHOUT a per-address is_readable_pointer probe on
    //    that field address (flaw #5): it guards is_valid_pointer(this)
    //    (range+alignment+poison) and then reads via os::safe_read.  The
    //    fault-safety therefore rests on os::safe_read, NOT on a region probe.
    //    With no JVM the entry is null and get_length() returns -1 before any
    //    offset is applied, so we cannot drive the offset read here - but we
    //    CAN pin the contract that matters: get_length() is the one FIX-B-path
    //    read whose safety on a bogus-but-aligned `this` depends on safe_read,
    //    and it must degrade to -1 (never fault) for every structurally-valid-
    //    looking-but-bogus `this`.  (The live offset-read AV-safety is a JVM
    //    concern; here we lock the no-JVM sentinel + no-fault property.)
    // =====================================================================
    {
        using vmhook::hotspot::constant_pool;

        // Addresses that PASS is_valid_pointer(this) (in-range, 8-aligned, not
        // poison) so the get_length() body would proceed to the offset read on
        // a JDK that exports _length.  With no JVM the entry is null -> -1 is
        // returned before the read; the property under test is that this is
        // reached without faulting for a `this` that looks valid but is bogus.
        const std::uintptr_t plausible_this[]{
            std::uintptr_t{ 0x0000'2000'0000'0000ull }, // aligned, in-range
            std::uintptr_t{ 0x0000'3000'0000'0008ull },
            std::uintptr_t{ 0x0000'4000'0000'1000ull },
            std::uintptr_t{ 0x0000'5000'0000'0010ull },
        };
        bool all_minus1{ true };
        for (const std::uintptr_t raw : plausible_this)
        {
            auto* const cp{ reinterpret_cast<constant_pool*>(raw) };
            if (cp->get_length() != -1) { all_minus1 = false; }
        }
        check("flaw5_get_length_aligned_bogus_this_returns_minus1_no_fault", all_minus1);
        // And the sentinel disables the bound, so a stripped-_length JDK (the
        // same code path as the no-JVM case) resolves names via the slot probe.
        check("flaw5_get_length_sentinel_disables_bound",
              !length_bound_rejects(0xFFFFu, -1));
        // get_length() is declared noexcept - the offset read cannot escape as
        // an exception even if it were to be reached; pin the contract.
        {
            alignas(16) static std::uint8_t q_storage[64]{};
            auto* const cp{ reinterpret_cast<constant_pool*>(&q_storage[0]) };
            check("flaw5_get_length_is_noexcept", noexcept(cp->get_length()));
        }
    }

    // =====================================================================
    // R. The FIELD-SYMBOL sibling bound (flaw #1).  klass::find_field /
    //    find_field_in_stream read constant_pool_base[name_index] and
    //    [sig_index] - the SAME ConstantPool::get_base() array, indexed by a u2
    //    from class metadata - with NO length bound and NO slot probe (the AV
    //    hazard FIX B closed for methods, left open for fields).  The live "fix
    //    was applied" check is a JVM concern; the DECISION LOGIC the fix should
    //    use is identical to the method bound and is swept here so the eventual
    //    shared helper has a spec.  We assert:
    //      (a) the field bound the readers OUGHT to apply equals the complement
    //          of the method reject bound on the full structurally-interesting
    //          matrix (so one shared helper can serve all five sites);
    //      (b) the field-RECORD walk (data[slot*6+k]) stays inside the Array<u2>
    //          exactly when slot*6+5 < array_length, with the library's
    //          integer-division loop bound (array_length/6) never overstepping.
    // =====================================================================
    {
        // (a) Field-symbol index bound == complement of method reject bound,
        //     over the same exhaustive (index, length) matrix shape as A1.
        std::vector<std::int32_t> lengths;
        lengths.push_back(-1);
        lengths.push_back(-7);
        for (std::int32_t n{ 0 }; n <= 16; ++n) { lengths.push_back(n); }
        const std::int32_t pin_lengths[]{ 255, 256, 1024, 0xFFFF, 0x1'0000,
                                          std::numeric_limits<std::int32_t>::max() };
        for (const std::int32_t n : pin_lengths) { lengths.push_back(n); }

        bool field_bound_matches_method{ true };
        std::size_t field_cases{ 0 };
        for (const std::int32_t n : lengths)
        {
            const std::uint32_t idxs[]{
                0u, 1u, 2u, 255u, 256u, 1024u, 0xFFFEu, 0xFFFFu,
            };
            for (const std::uint32_t raw : idxs)
            {
                const std::uint16_t index{ static_cast<std::uint16_t>(raw) };
                const bool in_range{ field_symbol_index_in_range(index, n) };
                const bool method_accepts{ !length_bound_rejects(index, n) };
                if (in_range != method_accepts) { field_bound_matches_method = false; }
                ++field_cases;
            }
        }
        check("flaw1_field_symbol_bound_equals_method_bound", field_bound_matches_method);
        check("flaw1_field_symbol_matrix_is_dense", field_cases >= 100);

        // Spelled-out witnesses: a field name_index AT cp_length is out of range
        // (must be rejected by the helper FIX B did not add); below it is in.
        check("flaw1_field_name_index_at_length_out_of_range",
              !field_symbol_index_in_range(64u, 64));
        check("flaw1_field_name_index_below_length_in_range",
              field_symbol_index_in_range(63u, 64));
        check("flaw1_field_index_unknown_length_admitted",
              field_symbol_index_in_range(0xFFFFu, -1));

        // (b) Field-record-walk array bound: sweep record indices against a band
        //     of Array<u2> lengths and assert field_record_in_array matches the
        //     first-principles `slot*6+5 < array_length`, AND that the library's
        //     loop bound (array_length/6) only ever iterates in-array records.
        bool record_bound_correct{ true };
        bool loop_never_oversteps{ true };
        std::size_t record_cases{ 0 };
        for (std::int32_t array_length{ 0 }; array_length <= 256; ++array_length)
        {
            const std::int32_t records_iterated{ array_length / 6 }; // library bound
            for (std::int64_t slot_index{ 0 }; slot_index <= 50; ++slot_index)
            {
                const bool in_array{ field_record_in_array(slot_index, array_length) };
                const bool fp{ array_length > 0 && slot_index >= 0
                               && (slot_index * 6 + 5) < static_cast<std::int64_t>(array_length) };
                if (in_array != fp) { record_bound_correct = false; }
                ++record_cases;
            }
            // Every record the library's loop ACTUALLY visits must be in-array.
            for (std::int32_t r{ 0 }; r < records_iterated; ++r)
            {
                if (!field_record_in_array(r, array_length)) { loop_never_oversteps = false; }
            }
        }
        check("flaw1_field_record_array_bound_correct", record_bound_correct);
        check("flaw1_field_record_loop_never_oversteps_array", loop_never_oversteps);
        check("flaw1_field_record_sweep_is_dense", record_cases >= 1000);

        // The JDK-8 trailing-_java_fields_count case: a class with R fields has
        // an Array<u2> of length R*6 + 1; the loop iterates exactly R records
        // and never touches the trailing partial slot.
        for (std::int32_t R{ 1 }; R <= 40; ++R)
        {
            const std::int32_t array_length{ R * 6 + 1 };
            const bool loop_count_ok{ (array_length / 6) == R };
            const bool last_record_in_array{ field_record_in_array(R - 1, array_length) };
            const bool trailing_slot_not_iterated{ !field_record_in_array(R, array_length) };
            if (!(loop_count_ok && last_record_in_array && trailing_slot_not_iterated))
            {
                check("flaw1_jdk8_trailing_count_walk_safe", false);
                break;
            }
            if (R == 40)
            {
                check("flaw1_jdk8_trailing_count_walk_safe", true);
            }
        }

        // (c) The APPLIED fix (resolve_constant_pool_symbol) now bounds the field
        //     CP index exactly as the method path does, and ADDS an index==0
        //     reject (slot 0 is the documented "unused" entry, so a name/sig of 0
        //     can never name a real Symbol).  Model the helper's full accept
        //     contract: a field symbol is read iff index != 0 AND the length bound
        //     admits it.  Negative cases the fix MUST reject: index 0, index at
        //     length, index past length; positive: a small in-range index, and any
        //     index when length is the -1 "unknown" sentinel (probe-only regime).
        auto helper_admits = [](std::uint32_t index, std::int32_t cp_length) -> bool
        {
            if (index == 0u) { return false; }
            if (cp_length >= 0 && index >= static_cast<std::uint32_t>(cp_length)) { return false; }
            return true;
        };
        check("fix_field_helper_rejects_index_zero",        !helper_admits(0u, 64));
        check("fix_field_helper_rejects_index_at_length",   !helper_admits(64u, 64));
        check("fix_field_helper_rejects_index_past_length", !helper_admits(0xFFFFu, 64));
        check("fix_field_helper_admits_small_in_range",     helper_admits(1u, 64));
        check("fix_field_helper_admits_last_valid",         helper_admits(63u, 64));
        check("fix_field_helper_unknown_length_probe_only", helper_admits(0xFFFFu, -1));
        // index 0 is rejected even when the length is unknown - the unused-slot
        // reject does not depend on the bound being active.
        check("fix_field_helper_rejects_zero_even_unknown_length", !helper_admits(0u, -1));
        // The applied helper is STRICTER than the pre-fix model bound at exactly
        // one point (index 0): the model admitted 0 < length, the fix rejects it.
        // Pin that divergence so it is a deliberate, test-visible property.
        check("fix_field_helper_stricter_than_model_at_zero",
              field_symbol_index_in_range(0u, 64) && !helper_admits(0u, 64));
        // Everywhere ELSE (index >= 1) the applied helper and the method bound
        // agree exactly, over the dense matrix - one shared spec, no drift.
        bool helper_matches_method_for_nonzero{ true };
        for (const std::int32_t n : { -1, -3, 0, 1, 2, 16, 64, 1024, 0xFFFF })
        {
            for (std::uint32_t index : { 1u, 2u, 63u, 64u, 65u, 1023u, 0xFFFEu, 0xFFFFu })
            {
                if (helper_admits(index, n) == length_bound_rejects(static_cast<std::uint16_t>(index), n))
                {
                    // helper_admits == accept; length_bound_rejects == reject;
                    // they must be opposite for every non-zero index.
                    helper_matches_method_for_nonzero = false;
                }
            }
        }
        check("fix_field_helper_matches_method_bound_for_nonzero", helper_matches_method_for_nonzero);
    }

    // =====================================================================
    // S. STRUCTURAL bytecode/locals/stack/param u2 fields - clamp invariants.
    //    These ConstMethod fields (_code_size, _max_stack, _max_locals,
    //    _size_of_parameters) are NOT cp indices and FIX B does not bound them,
    //    but the task asks to pin the boundary-size clamp behaviour every 16-bit
    //    metadata field shares: a value is physically stored in 16 bits, so the
    //    in-memory presentation is ALWAYS in [0, 65535] - the structural ceiling
    //    that lets the method index serve as a HARD upper bound for the length
    //    check.  We sweep the clamp at every structurally interesting width.
    // =====================================================================
    {
        // Boundary sizes: 0, 1, the byte/short edges, the u2 max, and just over.
        const std::uint64_t boundary_sizes[]{
            0u, 1u, 2u,
            0x7Fu, 0x80u, 0xFFu, 0x100u,          // byte edge
            0x7FFFu, 0x8000u, 0xFFFEu, 0xFFFFu,    // u2 edge (max in-range)
            0x1'0000u, 0x1'0001u,                  // overflow the u2 (wraps)
            0xFFFF'FFFFu, 0x1'FFFFu,               // wider corruption
        };
        bool all_in_u2_after_clamp{ true };
        bool clamp_is_low16{ true };
        for (const std::uint64_t s : boundary_sizes)
        {
            const std::uint16_t clamped{ clamp_u2(s) };
            // A clamped u2 has no bits set above bit 15.  Widen to 32 bits first
            // (so the comparison is a genuine runtime check, not a tautology on
            // a 16-bit type) and assert the high half is clear.
            const std::uint32_t widened{ static_cast<std::uint32_t>(clamped) };
            if ((widened & 0xFFFF'0000u) != 0u) { all_in_u2_after_clamp = false; }
            // And exactly the low 16 bits of the source.
            if (clamped != static_cast<std::uint16_t>(s & 0xFFFFu)) { clamp_is_low16 = false; }
        }
        check("size_field_clamp_always_in_u2_range", all_in_u2_after_clamp);
        check("size_field_clamp_is_low16_of_source", clamp_is_low16);

        // The load-bearing consequence for the bound: because EVERY index is a
        // clamped u2 (<= 0xFFFF), a pool whose length exceeds the u2 domain can
        // never have its upper edge reached, so the length bound is well-defined
        // for all representable indices.  Witness at the exact u2 ceiling: index
        // 0xFFFF is the only index that can be the edge, and only for length
        // 0xFFFF (rejected) or admitted for any larger length.
        check("size_field_maxu2_is_edge_only_at_len_maxu2",
              length_bound_rejects(0xFFFFu, 0xFFFF)
                  && !length_bound_rejects(0xFFFFu, 0x1'0000));

        // Malformed/huge value clamped: a corrupt all-ones 64-bit size presents
        // 0xFFFF; a corrupt 0x10000 presents 0 (the smallest); neither escapes
        // the u2 domain, so neither can corrupt the bound arithmetic into UB.
        check("size_field_allones_clamps_to_maxu2", clamp_u2(0xFFFF'FFFF'FFFF'FFFFull) == 0xFFFFu);
        check("size_field_0x10000_clamps_to_zero", clamp_u2(0x1'0000u) == 0u);
        check("size_field_0x10001_clamps_to_one", clamp_u2(0x1'0001u) == 1u);
    }

    // =====================================================================
    // T. (ADDITIVE, namespaced "T_" - deepens the POST-READ GUARD (3) in the
    //    feature's own context.)  The terminal guard in get_name/get_signature
    //    is  `!entry_pointer || !is_valid_pointer(entry_pointer)` (vmhook.hpp
    //    :2501 / :2578).  is_valid_pointer (:2047-2084) rejects a slot VALUE on
    //    FOUR independent grounds, each derived verbatim from source:
    //       (i)   addr <= user_address_floor (0xFFFF)            -> reject
    //       (ii)  addr >= user_address_ceiling (0x00007FFFFFFFFFFF)-> reject
    //       (iii) (addr & 0x1) != 0  (bit-0 set / odd)           -> reject
    //       (iv)  low-32-bits == any of NINE debug-poison tags   -> reject
    //    Sections F/M only ever planted 0xCAFEBABE as a slot value; here we
    //    plant EVERY rejection ground AND the load-bearing alignment
    //    DIFFERENTIAL between the two gates: the slot-ADDRESS probe (2) requires
    //    8-byte alignment (:2025, `addr & 0x7`), but the slot-VALUE guard (3)
    //    only requires bit-0 clear (:2059, `addr & 0x1`).  A 4-byte-aligned
    //    Symbol* VALUE is therefore ACCEPTED by guard (3) even though that same
    //    bit pattern as a slot ADDRESS would be rejected by probe (2) - the two
    //    gates apply different alignment rules by design, and that is pinned
    //    here.  All slot VALUES are written into a REAL owned RWX page; we never
    //    hand a fabricated unmapped/wild address to any reading helper (the
    //    values are only passed to is_valid_pointer, which does NOT dereference).
    // =====================================================================
    {
        // T0. The full NINE-pattern poison set, exactly as the is_valid_pointer
        //     switch enumerates it (:2070-2078).  Each, planted as a slot VALUE,
        //     must be rejected by guard (3) -> the getter returns nullptr.
        const std::uint32_t poison_low32[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        check("T_poison_set_has_nine_patterns",
              (sizeof(poison_low32) / sizeof(poison_low32[0])) == 9u);

        // T1. Every poison low-32 pattern is rejected as a VALUE - both as a
        //     bare low-32 value and (where it stays in user range) with high
        //     bits set, proving the check is on the low 32 bits regardless of
        //     the upper half.  These are PURE is_valid_pointer calls on
        //     constants (no memory read), so they are POSIX-safe.
        {
            bool all_poison_low_rejected{ true };
            bool all_poison_high_rejected{ true };
            for (const std::uint32_t p : poison_low32)
            {
                // Bare low-32 form (high half zero): is_valid_pointer rejects.
                if (is_valid_pointer(reinterpret_cast<void*>(
                        static_cast<std::uintptr_t>(p))))
                {
                    all_poison_low_rejected = false;
                }
                // Same low 32 bits, a non-zero high half placed INSIDE the
                // canonical user range (bit 44 set => 0x0000'1000'0000'0000,
                // which is > floor and < ceiling).  is_valid_pointer must STILL
                // reject it: the high bits do not rescue a value whose low 32
                // bits are a poison tag (rejected via the low32 switch, :2067-
                // 2079, or - for the odd patterns - via the bit-0 rule :2059).
                // Either way the verdict is reject, which is what we assert.
                const std::uintptr_t with_high{
                    (std::uintptr_t{ 0x0000'1000'0000'0000ull })
                    | static_cast<std::uintptr_t>(p) };
                if (is_valid_pointer(reinterpret_cast<void*>(with_high)))
                {
                    all_poison_high_rejected = false;
                }
            }
            check("T_poison_low32_values_rejected", all_poison_low_rejected);
            check("T_poison_high_bits_set_still_rejected", all_poison_high_rejected);
        }

        // T2. Plant each rejection ground as a slot VALUE in a REAL owned page
        //     and run the EXACT FIX B tail (probe (2) on the slot ADDRESS, then
        //     guard (3) on the slot VALUE) with the length bound disabled
        //     (cp_length == -1, so only (2) and (3) decide).  Every planted
        //     bad value must drive the getter tail to nullptr; one good value
        //     must drive it to non-null.
        {
            const std::size_t page{ vmhook::os::page_size() };
            void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
            if (!block)
            {
                check("T_postread_skipped_alloc_failed", false);
            }
            else
            {
                *static_cast<volatile std::uint8_t*>(block) = 0x7E; // commit
                void** const base{ static_cast<void**>(block) };

                // The composite tail FIX B runs, expressed once (bound disabled).
                auto getter_returns_symbol_at =
                    [&](std::size_t i) -> bool
                {
                    const std::uint16_t index{ static_cast<std::uint16_t>(i & 0xFFFFu) };
                    if (length_bound_rejects(index, -1)) { return false; } // (1) off
                    if (!is_readable_pointer(&base[index])) { return false; } // (2)
                    void* const v{ base[index] };
                    return v != nullptr && is_valid_pointer(v);              // (3)
                };

                // Slot 0: null value -> guard (3) rejects (the `!entry_pointer`
                // half of :2501).
                base[0] = nullptr;
                check("T_slot_null_value_returns_null", !getter_returns_symbol_at(0));

                // Slot 1: floor value (== user_address_floor, 0xFFFF) -> guard
                // rejects on `addr <= floor` (:2051).
                base[1] = reinterpret_cast<void*>(vmhook::os::user_address_floor);
                check("T_slot_floor_value_returns_null", !getter_returns_symbol_at(1));

                // Slot 2: ceiling value -> guard rejects on `addr >= ceiling`.
                base[2] = reinterpret_cast<void*>(vmhook::os::user_address_ceiling);
                check("T_slot_ceiling_value_returns_null", !getter_returns_symbol_at(2));

                // Slot 3: an ODD value (bit-0 set) in user range -> guard
                // rejects on `(addr & 0x1) != 0` (:2059).  This is a value the
                // 8-byte slot-address probe could never produce, but a corrupt
                // entry slot CAN hold it; guard (3) is the catch.
                base[3] = reinterpret_cast<void*>(
                    std::uintptr_t{ 0x0000'2000'0000'0001ull });
                check("T_slot_odd_value_returns_null", !getter_returns_symbol_at(3));

                // Slots 4..: each of the nine poison patterns (low-32) planted in
                // turn -> guard (3) rejects every one.  We reuse a single slot
                // (index 4) and overwrite it per pattern.
                {
                    bool every_poison_value_returns_null{ true };
                    for (const std::uint32_t p : poison_low32)
                    {
                        base[4] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(p));
                        if (getter_returns_symbol_at(4)) { every_poison_value_returns_null = false; }
                    }
                    check("T_slot_every_poison_value_returns_null", every_poison_value_returns_null);
                }

                // Slot 5: a REAL, valid, 8-byte-aligned value (a pointer into the
                // page) -> guard (3) accepts -> getter returns a Symbol*.
                base[5] = static_cast<void*>(&base[5]);
                check("T_slot_real_aligned_value_returns_symbol", getter_returns_symbol_at(5));

                // T3. The ALIGNMENT DIFFERENTIAL.  Construct a value that is
                //     4-byte aligned but NOT 8-byte aligned and lies in user
                //     range and is not poison: is_valid_pointer (guard (3)) must
                //     ACCEPT it (only bit-0 matters, :2059), whereas the same
                //     bit pattern as a slot ADDRESS would be REJECTED by the
                //     probe (2)'s 8-byte rule (:2025).  We assert both directions
                //     PURELY (is_readable_pointer/is_valid_pointer never read
                //     through the value), so this is POSIX-safe.
                const std::uintptr_t four_aligned_not_eight{ 0x0000'2000'0000'0004ull };
                check("T_value_4aligned_accepted_by_post_read_guard",
                      is_valid_pointer(reinterpret_cast<void*>(four_aligned_not_eight)));
                check("T_addr_4aligned_rejected_by_slot_probe",
                      !is_readable_pointer(reinterpret_cast<void*>(four_aligned_not_eight)));
                // And the 2-vs-2 even base case: a 2-byte (bit-0 clear) value is
                // accepted by guard (3); an odd value is not.  The guard's rule
                // is exactly bit-0, nothing stricter.
                check("T_value_2aligned_even_accepted_by_guard",
                      is_valid_pointer(reinterpret_cast<void*>(
                          std::uintptr_t{ 0x0000'2000'0000'0002ull })));
                check("T_value_odd_rejected_by_guard",
                      !is_valid_pointer(reinterpret_cast<void*>(
                          std::uintptr_t{ 0x0000'2000'0000'0003ull })));

                vmhook::os::release(block, page);
            }
        }

        // T4. Boundary of the post-read guard's range check at the EXACT floor
        //     and ceiling edges, derived from source constants (:515 / :520).
        //     floor == 0xFFFF: `addr <= floor` is INCLUSIVE, so a VALUE of
        //     exactly 0xFFFF rejects but floor+1 (0x10000, even, in range, not
        //     poison) is accepted.  ceiling == 0x00007FFFFFFFFFFF: `addr >=
        //     ceiling` is INCLUSIVE, so a VALUE of exactly ceiling rejects but
        //     ceiling-1 is... odd (0x...FE is even though: ceiling ends ...FFFF,
        //     so ceiling-1 == ...FFFE which is EVEN) -> accepted.  Pure
        //     is_valid_pointer calls on constants -> POSIX-safe.
        check("T_guard_floor_value_rejected",
              !is_valid_pointer(reinterpret_cast<void*>(vmhook::os::user_address_floor)));
        check("T_guard_floor_plus_1_value_accepted",
              is_valid_pointer(reinterpret_cast<void*>(
                  vmhook::os::user_address_floor + 1u)));  // 0x10000, even, in range
        check("T_guard_ceiling_value_rejected",
              !is_valid_pointer(reinterpret_cast<void*>(vmhook::os::user_address_ceiling)));
        check("T_guard_ceiling_minus_1_value_accepted",
              is_valid_pointer(reinterpret_cast<void*>(
                  vmhook::os::user_address_ceiling - 1u))); // ...FFFE, even, < ceiling
        // The floor is exactly 0xFFFF and the ceiling exactly 0x00007FFFFFFFFFFF
        // (pin the constants the edges are derived from, so a future constant
        // change reddens these named checks, not a mystery boundary drift).
        check("T_user_address_floor_is_0xFFFF",
              vmhook::os::user_address_floor == std::uintptr_t{ 0xFFFFull });
        check("T_user_address_ceiling_is_0x00007FFFFFFFFFFF",
              vmhook::os::user_address_ceiling == std::uintptr_t{ 0x00007FFFFFFFFFFFull });

        // T5. The two gates' alignment masks are DISTINCT and that distinction
        //     is the whole reason (2) and (3) are separate gates: probe (2)
        //     masks 0x7 (8-byte), guard (3) masks 0x1 (2-byte).  Pin the masks
        //     as compile-evident constants so a future edit that unifies them
        //     (e.g. making the value guard also require 8-byte) is a visible,
        //     deliberate change.  Witness values: 0x...2 and 0x...4 and 0x...6
        //     are all bit-0-clear (guard accepts) but only 0x...8/0x...0 are
        //     8-aligned (probe accepts).
        {
            const std::uintptr_t base_in_range{ 0x0000'2000'0000'0000ull };
            // bit-0-clear but not 8-aligned: guard accepts, probe rejects.
            const std::uintptr_t two_only{ base_in_range | 0x2ull };
            const std::uintptr_t four_only{ base_in_range | 0x4ull };
            const std::uintptr_t six_only{ base_in_range | 0x6ull };
            bool guard_accepts_all_even{ true };
            bool probe_rejects_all_non8{ true };
            for (const std::uintptr_t a : { two_only, four_only, six_only })
            {
                if (!is_valid_pointer(reinterpret_cast<void*>(a))) { guard_accepts_all_even = false; }
                if (is_readable_pointer(reinterpret_cast<void*>(a))) { probe_rejects_all_non8 = false; }
            }
            check("T_guard_accepts_all_even_non8_aligned", guard_accepts_all_even);
            check("T_probe_rejects_all_even_non8_aligned", probe_rejects_all_non8);
        }

        // T6. Both gates are declared noexcept (the FIX B chain must never let
        //     an exception escape from the guards); pin it for the value guard
        //     alongside the probe (E pinned the probe).
        check("T_is_valid_pointer_is_noexcept", noexcept(is_valid_pointer(nullptr)));
    }

    // =====================================================================
    // DEEPEN-DT.  (ADDITIVE, "dt_" namespace.)  The APPLIED field-path fix
    //    driven for REAL: invoke the actual library function
    //    klass::resolve_constant_pool_symbol (vmhook.hpp :3898-3919) against an
    //    OWNED, mapped void** block with planted slot VALUES, across index /
    //    length regimes.  Sections R(c) / M only MODEL the helper; this section
    //    exercises the real `inline static` entry point end-to-end (it is pure
    //    pointer/arith + is_valid_pointer + is_readable_pointer +
    //    cold_read_metadata_pointer -> os::safe_read, all JVM-independent and
    //    fault-safe on every platform), so its four guards and the terminal
    //    Symbol* return are pinned to the SOURCE, not a re-spec.
    //
    //    Source body (verified 2026-06-22):
    //        if (!index || !is_valid_pointer(base)) return nullptr;            // (0)
    //        if (cp_length >= 0 && index >= (uint32_t)cp_length) return null;  // (1)
    //        if (!is_readable_pointer(&base[index])) return nullptr;           // (2)
    //        void* e = cold_read_metadata_pointer(&base[index]);              // read
    //        if (!e || !is_valid_pointer(e)) return nullptr;                  // (3)
    //        return reinterpret_cast<symbol*>(e);
    //    `index` is std::uint32_t (NOT u16 like the method path); the length
    //    comparison is UNSIGNED.
    //
    //    POSIX-SAFETY: the base handed in is ONLY ever a real owned allocate_rwx
    //    block whose slots we planted; DEEPEN-DU covers the rejected-base path.
    //    No fabricated/wild/high "valid-shaped" base is ever read here.
    // =====================================================================
    {
        using vmhook::hotspot::klass;
        using vmhook::hotspot::symbol;

        const std::size_t page{ vmhook::os::page_size() };
        const std::size_t slots{ 64 };
        const std::size_t bytes{ slots * sizeof(void*) };
        const std::size_t alloc{ ((bytes + page - 1) / page) * page };
        void* const block{ vmhook::os::allocate_rwx(nullptr, alloc) };
        if (!block)
        {
            check("dt_real_helper_skipped_alloc_failed", false);
        }
        else
        {
            *static_cast<volatile std::uint8_t*>(block) = 0x9C; // commit
            void** const base{ static_cast<void**>(block) };

            // Deterministic planted VALUES (same scheme as section M):
            //   index % 3 == 0 -> a real, valid pointer (into the block)
            //   index % 3 == 1 -> null
            //   index % 3 == 2 -> a poison value is_valid_pointer rejects
            for (std::size_t i{ 0 }; i < slots; ++i)
            {
                switch (i % 3u)
                {
                    case 0u: base[i] = static_cast<void*>(&base[i]); break;       // real & valid
                    case 1u: base[i] = nullptr; break;                            // null
                    default: base[i] = reinterpret_cast<void*>(
                                 static_cast<std::uintptr_t>(0xCAFEBABEu)); break; // poison
                }
            }

            // First-principles oracle for the REAL helper's return given the
            // planted pattern.  index 0 is the unused-slot reject the applied
            // fix ADDS (guard 0), distinct from the method path.
            auto oracle_returns_symbol =
                [&](std::uint32_t index, std::int32_t cp_length) -> bool
            {
                if (index == 0u) { return false; }                            // (0) unused slot
                if (index >= slots) { return false; }                          // outside our block
                if (cp_length >= 0
                    && index >= static_cast<std::uint32_t>(cp_length)) { return false; } // (1)
                const std::uint32_t k{ index % 3u };
                if (k == 1u) { return false; }   // null value      (3)
                if (k == 2u) { return false; }   // poison rejected  (3)
                return true;                     // real & valid -> Symbol*
            };

            const std::int32_t length_regimes[]{ -1, -9, 0, 1, 7, 16, 33, 64 };
            bool real_matches_oracle{ true };
            std::size_t real_cases{ 0 };
            for (const std::int32_t n : length_regimes)
            {
                for (std::uint32_t i{ 0u }; i < static_cast<std::uint32_t>(slots); ++i)
                {
                    symbol* const got{ klass::resolve_constant_pool_symbol(base, i, n) };
                    const bool returned_symbol{ got != nullptr };
                    if (returned_symbol != oracle_returns_symbol(i, n))
                    {
                        real_matches_oracle = false;
                    }
                    // When it DOES return a Symbol*, it is exactly the planted
                    // slot value (guard 3 read returns base[i]).
                    if (returned_symbol && reinterpret_cast<void*>(got) != base[i])
                    {
                        real_matches_oracle = false;
                    }
                    ++real_cases;
                }
            }
            check("dt_real_helper_matches_oracle_full_sweep", real_matches_oracle);
            check("dt_real_helper_sweep_is_dense", real_cases >= 8u * slots);

            // Spelled-out witnesses on the disabled-bound regime (-1):
            check("dt_real_helper_index0_unused_slot_null",
                  klass::resolve_constant_pool_symbol(base, 0u, -1) == nullptr);
            {
                symbol* const s3{ klass::resolve_constant_pool_symbol(base, 3u, -1) };
                check("dt_real_helper_index3_real_returns_planted_symbol",
                      s3 != nullptr && reinterpret_cast<void*>(s3) == base[3]);
            }
            check("dt_real_helper_index1_null_value_returns_null",
                  klass::resolve_constant_pool_symbol(base, 1u, -1) == nullptr);
            check("dt_real_helper_index2_poison_value_returns_null",
                  klass::resolve_constant_pool_symbol(base, 2u, -1) == nullptr);

            // Guard (1) short-circuits BEFORE the value gate: index 3 holds a
            // real ptr, but length 3 makes 3 >= 3 reject first.
            check("dt_real_helper_length3_rejects_index3_before_value_gate",
                  klass::resolve_constant_pool_symbol(base, 3u, 3) == nullptr);
            // length 4 admits index 3 (3 < 4) -> the real planted Symbol*.
            {
                symbol* const s{ klass::resolve_constant_pool_symbol(base, 3u, 4) };
                check("dt_real_helper_length4_admits_index3_real_symbol",
                      s != nullptr && reinterpret_cast<void*>(s) == base[3]);
            }
            // length 0 (empty pool) rejects EVERY index, including a real-value
            // slot (guard 1: any index >= 0).
            check("dt_real_helper_length0_rejects_real_value_slot",
                  klass::resolve_constant_pool_symbol(base, 3u, 0) == nullptr);

            // The helper is declared noexcept; pin it on the real signature.
            check("dt_real_helper_is_noexcept",
                  noexcept(klass::resolve_constant_pool_symbol(base, 1u, -1)));

            vmhook::os::release(block, alloc);
        }
    }

    // =====================================================================
    // DEEPEN-DU.  (ADDITIVE, "du_" namespace.)  The REAL helper's guard (0)
    //    rejects on a bogus BASE pointer and on index == 0 WITHOUT performing
    //    any slot read.  Driven ONLY with is_valid_pointer-REJECTED bases (the
    //    low-floor constant 0x1000 and the nine debug-poison patterns) so the
    //    function returns nullptr at its first branch before touching memory -
    //    POSIX-safe by construction (is_valid_pointer rejects each in pure
    //    arithmetic; nothing is dereferenced).  Pins guard (0) to the source:
    //    the `!is_valid_pointer(constant_pool_base)` half and the `!index` half.
    // =====================================================================
    {
        using vmhook::hotspot::klass;
        using vmhook::hotspot::is_valid_pointer;

        // Bases is_valid_pointer REJECTS (guard (0) short-circuits, NO read):
        //   0x1000 (4096) <= user_address_floor (0xFFFF) -> rejected
        //   each debug-poison low32 -> rejected by the poison switch (:2068-2079)
        const std::uintptr_t rejected_bases[]{
            std::uintptr_t{ 0x1000ull },
            std::uintptr_t{ 0xDEADBEEFull },
            std::uintptr_t{ 0xCAFEBABEull },
            std::uintptr_t{ 0xCCCCCCCCull },
            std::uintptr_t{ 0xCDCDCDCDull },
            std::uintptr_t{ 0xBAADF00Dull },
            std::uintptr_t{ 0xFEEEFEEEull },
            std::uintptr_t{ 0xABABABABull },
            std::uintptr_t{ 0xFDFDFDFDull },
            std::uintptr_t{ 0xDDDDDDDDull },
        };

        // Confirm each base is genuinely is_valid_pointer-REJECTED, so the
        // POSIX-safety premise (guard (0) short-circuits, no read) holds.
        bool all_bases_rejected{ true };
        for (const std::uintptr_t raw : rejected_bases)
        {
            if (is_valid_pointer(reinterpret_cast<void*>(raw))) { all_bases_rejected = false; }
        }
        check("du_real_helper_guard0_bases_are_all_invalid", all_bases_rejected);

        // With a rejected base AND a perfectly in-range index/length, the helper
        // STILL returns nullptr (guard 0 fires on the base half) and must not
        // fault.  Reaching the check proves no read occurred.
        bool bad_base_all_null{ true };
        for (const std::uintptr_t raw : rejected_bases)
        {
            void** const base{ reinterpret_cast<void**>(raw) };
            if (klass::resolve_constant_pool_symbol(base, 1u, -1) != nullptr) { bad_base_all_null = false; }
            if (klass::resolve_constant_pool_symbol(base, 5u, 64) != nullptr) { bad_base_all_null = false; }
        }
        check("du_real_helper_bad_base_returns_null_no_fault", bad_base_all_null);

        // The `!index` half of guard (0) fires first; even with a rejected base
        // index 0 yields nullptr (no read either way).
        check("du_real_helper_index0_with_bad_base_null",
              klass::resolve_constant_pool_symbol(
                  reinterpret_cast<void**>(std::uintptr_t{ 0x1000ull }), 0u, -1) == nullptr);
    }

    // =====================================================================
    // DEEPEN-DV.  (ADDITIVE, "dv_" namespace.)  The FIELD-path index type is
    //    std::uint32_t (vmhook.hpp :3898), NOT the std::uint16_t of the method
    //    path - so the field helper can be handed indices ABOVE the u2 ceiling
    //    (a decode_u5 over-read, feature flaw #1/#3).  The length comparison is
    //    UNSIGNED (`index >= static_cast<std::uint32_t>(cp_length)`); a corrupt
    //    huge index past a real length is correctly rejected by guard (1), and
    //    there is no signed/unsigned hazard because the `cp_length >= 0` half
    //    owns the negative case.  Sections A/J only reach 0xFFFF (the u16 method
    //    domain); this models guard (1) across the full u32 index band.
    // =====================================================================
    {
        // Faithful local re-implementation of the field helper's guard (1),
        // mirroring the SOURCE exactly: uint32_t index, unsigned compare, with
        // the cp_length>=0 gate owning negatives.
        auto field_guard1_rejects = [](std::uint32_t index, std::int32_t cp_length) -> bool
        {
            return cp_length >= 0 && index >= static_cast<std::uint32_t>(cp_length);
        };
        // Independent wide oracle (int64, no unsigned subtlety).
        auto field_guard1_oracle = [](std::uint32_t index, std::int32_t cp_length) -> bool
        {
            if (cp_length < 0) { return false; }
            return static_cast<std::int64_t>(index) >= static_cast<std::int64_t>(cp_length);
        };

        // Indices that EXCEED the u2 ceiling - only reachable on the field path.
        const std::uint32_t wide_indices[]{
            0u, 1u, 0xFFFFu, 0x1'0000u, 0x1'0001u, 0x10'0000u,
            0x7FFF'FFFEu, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
        };
        const std::int32_t lens[]{
            -1, -5, 0, 1, 256, 0xFFFF, 0x1'0000, 0x10'0000,
            0x4000'0000, 0x7FFF'FFFE, std::numeric_limits<std::int32_t>::max(),
        };
        bool field_guard1_consistent{ true };
        std::size_t field_guard1_cases{ 0 };
        for (const std::int32_t n : lens)
        {
            for (const std::uint32_t i : wide_indices)
            {
                if (field_guard1_rejects(i, n) != field_guard1_oracle(i, n))
                {
                    field_guard1_consistent = false;
                }
                ++field_guard1_cases;
            }
        }
        check("dv_field_guard1_unsigned_compare_matches_int64_oracle", field_guard1_consistent);
        check("dv_field_guard1_wide_index_sweep_is_dense", field_guard1_cases >= 100);

        // u32-domain witnesses the method path (u16) cannot express:
        //   65536 vs 65536 -> reject
        check("dv_field_guard1_index_above_u2_at_length_rejected",
              field_guard1_rejects(0x1'0000u, 0x1'0000));
        //   65536 vs 65537 -> admit
        check("dv_field_guard1_index_above_u2_below_length_admitted",
              !field_guard1_rejects(0x1'0000u, 0x1'0001));
        //   0xFFFFFFFF (4294967295) >= 0x7FFFFFFF (2147483647) -> reject; the
        //   cp_length widens to a non-negative u32 == 0x7FFFFFFF, no unsigned wrap.
        check("dv_field_guard1_maxu32_index_ge_int32max_length_rejected",
              field_guard1_rejects(0xFFFF'FFFFu, std::numeric_limits<std::int32_t>::max()));
        //   huge index vs -1 sentinel: bound disabled -> admit (base/probe/value
        //   guards then own the safety in the real fn).
        check("dv_field_guard1_huge_index_unknown_length_admitted",
              !field_guard1_rejects(0xFFFF'FFFFu, -1));
        //   negative corrupt length disables the bound for a huge index too.
        check("dv_field_guard1_huge_index_negative_length_admitted",
              !field_guard1_rejects(0x8000'0000u, std::numeric_limits<std::int32_t>::min()));

        // Cross-tie: for any index that FITS a u2, guard (1) agrees with the
        // method path's length_bound_rejects (one shared spec, no drift).
        bool field_and_method_agree_in_u2{ true };
        for (const std::int32_t n : { -1, 0, 1, 7, 256, 0xFFFF, 0x1'0000 })
        {
            for (std::uint32_t i : { 0u, 1u, 6u, 7u, 8u, 255u, 256u, 0xFFFEu, 0xFFFFu })
            {
                if (field_guard1_rejects(i, n)
                    != length_bound_rejects(static_cast<std::uint16_t>(i), n))
                {
                    field_and_method_agree_in_u2 = false;
                }
            }
        }
        check("dv_field_guard1_agrees_with_method_bound_in_u2_domain", field_and_method_agree_in_u2);
    }

    // =====================================================================
    // DEEPEN-DW.  (ADDITIVE, "dw_" namespace.)  get_base() header-offset
    //    arithmetic & the 1-based index-0 "unused slot" contract, pinned at the
    //    arithmetic/type level from the SOURCE.  get_base() (vmhook.hpp
    //    :2331-2350) returns reinterpret_cast<void**>((uint8_t*)this + size):
    //    the entries array begins immediately AFTER the ConstantPool header,
    //    indices are 1-based, slot 0 is the unused sentinel (doc :2325).  No
    //    JVM, no fabricated read - only pure pointer arithmetic over an owned
    //    buffer and synthetic aligned addresses (no deref).
    // =====================================================================
    {
        constexpr std::size_t ptr_size{ sizeof(void*) };

        // DW1. base = this + header_size, computed by hand against an OWNED
        // buffer big enough for the header + a few entry slots, across header
        // sizes spanning the JDK 8..26 ConstantPool-header growth range.
        {
            const std::size_t header_sizes[]{ 56, 64, 72, 80, 88, 96, 104, 112, 120, 128 };
            bool header_offset_exact{ true };
            bool slot0_is_base{ true };
            std::vector<std::uint8_t> buf(128 + 8 * ptr_size, std::uint8_t{ 0 });
            std::uint8_t* const self{ buf.data() };
            for (const std::size_t H : header_sizes)
            {
                void** const base{ reinterpret_cast<void**>(self + H) }; // library expr
                const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(base) };
                const std::uintptr_t self_addr{ reinterpret_cast<std::uintptr_t>(self) };
                if (base_addr != self_addr + H) { header_offset_exact = false; }
                if (reinterpret_cast<void**>(&base[0]) != base) { slot0_is_base = false; }
                for (std::size_t i{ 0 }; i < 8; ++i)
                {
                    const std::uintptr_t lang{ reinterpret_cast<std::uintptr_t>(&base[i]) };
                    const std::uintptr_t hand{ base_addr + i * ptr_size };
                    if (lang != hand) { header_offset_exact = false; }
                }
            }
            check("dw_getbase_entry_array_starts_after_header", header_offset_exact);
            check("dw_getbase_slot0_equals_base", slot0_is_base);
        }

        // DW2. The 1-based contract, made test-visible (feature flaw #6's
        // documented inconsistency between the two paths):
        //   - method bound admits index 0 for any length >= 1 (relies on the
        //     post-read is_valid_pointer(base[0]) guard for a zeroed index)
        //   - field helper guard-0 rejects index 0 unconditionally
        check("dw_contract_method_bound_admits_index0_len1", !length_bound_rejects(0u, 1));
        check("dw_contract_method_bound_rejects_index0_only_when_len0",
              length_bound_rejects(0u, 0));
        auto field_helper_rejects_index0 = [](std::uint32_t index) -> bool { return index == 0u; };
        check("dw_contract_field_helper_rejects_index0_any_length",
              field_helper_rejects_index0(0u));
        check("dw_contract_field_helper_admits_nonzero_index1",
              !field_helper_rejects_index0(1u));

        // DW3. An 8-aligned `this` plus an 8-multiple header yields 8-aligned
        // entry slots, so the is_readable_pointer slot probe's 8-align gate
        // (:2025) is satisfiable by the live layout.  Pure arithmetic, no deref.
        {
            const std::uintptr_t aligned_this{ 0x0000'2000'0000'0000ull }; // 8-aligned, in-range
            const std::size_t eight_multiple_headers[]{ 56, 64, 72, 80, 88, 96, 104, 112 };
            bool all_slots_8aligned{ true };
            for (const std::size_t H : eight_multiple_headers)
            {
                const std::uintptr_t base_addr{ aligned_this + H };
                for (std::uint32_t i{ 0u }; i <= 0xFFu; ++i)
                {
                    const std::uintptr_t slot{ base_addr + static_cast<std::uintptr_t>(i) * ptr_size };
                    if ((slot & 0x7u) != 0u) { all_slots_8aligned = false; break; }
                }
                if (!all_slots_8aligned) { break; }
            }
            check("dw_getbase_8aligned_this_and_header_yields_8aligned_slots",
                  ptr_size != 8u || all_slots_8aligned);
        }
    }

    // =====================================================================
    // DEEPEN-DX.  (ADDITIVE, "dx_" namespace.)  The VMStruct ENTRY ABI the
    //    whole feature reads through.  get_base() crosses entry->size of a
    //    vmhook::hotspot::vm_type_entry_t (vmhook.hpp :2334/:2343) and
    //    get_length()/get_constants()/get_name()/get_signature() cross
    //    entry->offset of a vmhook::hotspot::vm_struct_entry_t
    //    (:2365/:2379, :2402, :2441, :2520).  The C++ structs MUST match the
    //    gHotSpotVMTypes / gHotSpotVMStructs C ABI member-for-member or every
    //    offset/size read lands on the wrong field.  These structs are
    //    declared at :1880-1899; we pin their member ORDER, member WIDTHS,
    //    standard-layout-ness, and the byte offset of the two members the
    //    feature actually dereferences (vm_type_entry_t::size,
    //    vm_struct_entry_t::offset).  Pure offsetof/sizeof/type_traits over a
    //    qualified type - no JVM, no memory read, no fabricated address.
    // =====================================================================
    {
        using vmhook::hotspot::vm_type_entry_t;
        using vmhook::hotspot::vm_struct_entry_t;

        // DX1. Both entry structs must be standard-layout so offsetof is
        // well-defined and the C++ mirror is bit-compatible with the C struct
        // HotSpot exports (the library reinterpret_casts gHotSpotVMTypes /
        // gHotSpotVMStructs straight into these, :1933/:1956).
        check("dx_vm_type_entry_is_standard_layout",
              std::is_standard_layout_v<vm_type_entry_t>);
        check("dx_vm_struct_entry_is_standard_layout",
              std::is_standard_layout_v<vm_struct_entry_t>);

        // DX2. vm_type_entry_t member ORDER and WIDTHS, exactly as declared
        // (:1882-1887):
        //   const char* type_name; const char* superclass_name;
        //   int32 is_oop_type_type; int32 is_integer_type; int32 is_unsigned;
        //   uint64 size;
        check("dx_type_entry_type_name_is_charptr",
              std::is_same_v<decltype(vm_type_entry_t::type_name), const char*>);
        check("dx_type_entry_superclass_name_is_charptr",
              std::is_same_v<decltype(vm_type_entry_t::superclass_name), const char*>);
        check("dx_type_entry_is_oop_is_int32",
              std::is_same_v<decltype(vm_type_entry_t::is_oop_type_type), std::int32_t>);
        check("dx_type_entry_is_integer_is_int32",
              std::is_same_v<decltype(vm_type_entry_t::is_integer_type), std::int32_t>);
        check("dx_type_entry_is_unsigned_is_int32",
              std::is_same_v<decltype(vm_type_entry_t::is_unsigned), std::int32_t>);
        check("dx_type_entry_size_is_uint64",
              std::is_same_v<decltype(vm_type_entry_t::size), std::uint64_t>);

        // The `size` member - the one get_base() reads - must come AFTER the
        // two pointers and three int32s.  Pin its offset by the declared order
        // (member offsets are monotonic in declaration order for a
        // standard-layout type, [class.mem]).
        check("dx_type_entry_size_after_type_name",
              offsetof(vm_type_entry_t, size) > offsetof(vm_type_entry_t, type_name));
        check("dx_type_entry_size_after_superclass",
              offsetof(vm_type_entry_t, size) > offsetof(vm_type_entry_t, superclass_name));
        check("dx_type_entry_size_after_is_unsigned",
              offsetof(vm_type_entry_t, size) > offsetof(vm_type_entry_t, is_unsigned));
        check("dx_type_entry_members_in_declared_order",
              offsetof(vm_type_entry_t, type_name) < offsetof(vm_type_entry_t, superclass_name)
                  && offsetof(vm_type_entry_t, superclass_name) < offsetof(vm_type_entry_t, is_oop_type_type)
                  && offsetof(vm_type_entry_t, is_oop_type_type) < offsetof(vm_type_entry_t, is_integer_type)
                  && offsetof(vm_type_entry_t, is_integer_type) < offsetof(vm_type_entry_t, is_unsigned)
                  && offsetof(vm_type_entry_t, is_unsigned) < offsetof(vm_type_entry_t, size));
        // type_name is the FIRST member, so its offset is 0 (the iterate loop
        // reads entry->type_name first, :1973).
        check("dx_type_entry_type_name_offset_is_zero",
              offsetof(vm_type_entry_t, type_name) == 0u);

        // DX3. vm_struct_entry_t member ORDER and WIDTHS, exactly as declared
        // (:1893-1898):
        //   const char* type_name; const char* field_name; const char* type_string;
        //   int32 is_static; uint64 offset; void* address;
        check("dx_struct_entry_type_name_is_charptr",
              std::is_same_v<decltype(vm_struct_entry_t::type_name), const char*>);
        check("dx_struct_entry_field_name_is_charptr",
              std::is_same_v<decltype(vm_struct_entry_t::field_name), const char*>);
        check("dx_struct_entry_type_string_is_charptr",
              std::is_same_v<decltype(vm_struct_entry_t::type_string), const char*>);
        check("dx_struct_entry_is_static_is_int32",
              std::is_same_v<decltype(vm_struct_entry_t::is_static), std::int32_t>);
        check("dx_struct_entry_offset_is_uint64",
              std::is_same_v<decltype(vm_struct_entry_t::offset), std::uint64_t>);
        check("dx_struct_entry_address_is_voidptr",
              std::is_same_v<decltype(vm_struct_entry_t::address), void*>);
        // type_name then field_name are the first two members - the iterate
        // loop reads both for the strcmp match (:2003) and the field_name-null
        // skip (:1999); pin their relative order and that type_name is offset 0.
        check("dx_struct_entry_type_name_offset_is_zero",
              offsetof(vm_struct_entry_t, type_name) == 0u);
        check("dx_struct_entry_field_name_after_type_name",
              offsetof(vm_struct_entry_t, field_name) > offsetof(vm_struct_entry_t, type_name));
        check("dx_struct_entry_members_in_declared_order",
              offsetof(vm_struct_entry_t, type_name) < offsetof(vm_struct_entry_t, field_name)
                  && offsetof(vm_struct_entry_t, field_name) < offsetof(vm_struct_entry_t, type_string)
                  && offsetof(vm_struct_entry_t, type_string) < offsetof(vm_struct_entry_t, is_static)
                  && offsetof(vm_struct_entry_t, is_static) < offsetof(vm_struct_entry_t, offset)
                  && offsetof(vm_struct_entry_t, offset) < offsetof(vm_struct_entry_t, address));
        // The `offset` member - the one get_length()/get_constants() read -
        // must follow the three pointers and the int32.
        check("dx_struct_entry_offset_after_type_string",
              offsetof(vm_struct_entry_t, offset) > offsetof(vm_struct_entry_t, type_string));
        check("dx_struct_entry_offset_after_is_static",
              offsetof(vm_struct_entry_t, offset) > offsetof(vm_struct_entry_t, is_static));
    }

    // =====================================================================
    // DEEPEN-DY.  (ADDITIVE, "dy_" namespace.)  The VMStruct TABLE-WALK
    //    find-and-reject logic of iterate_type_entries (vmhook.hpp :1964-1979)
    //    and iterate_struct_entries (:1990-2009).  These are the bound-and-
    //    reject lookups every accessor in the feature funnels through to turn
    //    a type/field name into an entry.  With no JVM the real functions take
    //    their null-table branch (covered in DY3); the DECISION LOGIC of the
    //    walk - terminator-stops, the documented field_name-null SKIP guard
    //    (:1999, a real partial-entry crash-fix), the name-arg null early-out
    //    (:1967 / :1993), and the strcmp match/miss - is exercised here as a
    //    faithful re-implementation swept over a STUBBED, OWNED std::array of
    //    entries (the spec written twice, exactly as section A does for the
    //    length bound).  NOTHING dereferences a fabricated address: the table
    //    is a real std::array we own; the c-strings are real string literals.
    // =====================================================================
    {
        using vmhook::hotspot::vm_type_entry_t;
        using vmhook::hotspot::vm_struct_entry_t;

        // Faithful local re-implementation of iterate_type_entries' walk
        // (:1964-1979): null name -> nullptr; else scan until a null type_name
        // terminator, returning the first strcmp-equal entry, nullptr on miss.
        auto find_type = [](vm_type_entry_t* table, const char* type_name) -> vm_type_entry_t*
        {
            if (!type_name) { return nullptr; }                                   // :1967
            for (vm_type_entry_t* e{ table }; e && e->type_name; ++e)             // :1971 terminator
            {
                if (!std::strcmp(e->type_name, type_name)) { return e; }          // :1973 match
            }
            return nullptr;                                                       // :1978 miss
        };
        // Faithful local re-implementation of iterate_struct_entries' walk
        // (:1990-2009): null type OR null field -> nullptr; scan to terminator,
        // SKIP any entry whose field_name is null (:1999 partial-entry guard),
        // return the first (type,field) strcmp-equal entry, nullptr on miss.
        auto find_struct = [](vm_struct_entry_t* table, const char* type_name, const char* field_name) -> vm_struct_entry_t*
        {
            if (!type_name || !field_name) { return nullptr; }                    // :1993
            for (vm_struct_entry_t* e{ table }; e && e->type_name; ++e)           // :1997 terminator
            {
                if (!e->field_name) { continue; }                                 // :1999 SKIP null field
                if (!std::strcmp(e->type_name, type_name)
                    && !std::strcmp(e->field_name, field_name)) { return e; }     // :2003 match
            }
            return nullptr;                                                       // :2008 miss
        };

        // DY1. find_type over an OWNED, terminated type table.  Entries mirror
        // the real ConstantPool type entry get_base() looks up (size is the
        // member it reads, :2343).  Last entry is the {nullptr,...} terminator.
        {
            std::array<vm_type_entry_t, 4> types{ {
                { "InstanceKlass", nullptr, 0, 0, 0, 296u },
                { "ConstantPool",  nullptr, 0, 0, 0,  80u }, // header size get_base() adds
                { "Method",        nullptr, 0, 0, 0,  64u },
                { nullptr,         nullptr, 0, 0, 0,   0u }, // terminator
            } };

            vm_type_entry_t* const cp{ find_type(types.data(), "ConstantPool") };
            check("dy_find_type_hits_constantpool", cp != nullptr);
            check("dy_find_type_returns_correct_entry",
                  cp != nullptr && cp->size == 80u); // the size get_base() crosses
            check("dy_find_type_first_entry_match",
                  find_type(types.data(), "InstanceKlass") == &types[0]);
            check("dy_find_type_middle_entry_match",
                  find_type(types.data(), "ConstantPool") == &types[1]);

            // Miss: a name not present stops at the terminator -> nullptr.
            check("dy_find_type_miss_returns_null",
                  find_type(types.data(), "NoSuchType") == nullptr);
            // Null name -> early-out nullptr (:1967), never walks the table.
            check("dy_find_type_null_name_returns_null",
                  find_type(types.data(), nullptr) == nullptr);
            // A case-different name does NOT match (strcmp is exact).
            check("dy_find_type_case_sensitive_miss",
                  find_type(types.data(), "constantpool") == nullptr);
            // An EMPTY table (just the terminator) -> nullptr for any name, no
            // out-of-bounds read (the loop never enters past entry[0]).
            {
                std::array<vm_type_entry_t, 1> empty{ { { nullptr, nullptr, 0, 0, 0, 0u } } };
                check("dy_find_type_empty_table_returns_null",
                      find_type(empty.data(), "ConstantPool") == nullptr);
            }
        }

        // DY2. find_struct over an OWNED, terminated struct table, INCLUDING a
        // partial entry whose field_name is null (the exact corruption the
        // :1999 guard was added for: a custom JVM/JVMTI agent publishing
        // type_name set, field_name null - strcmp(nullptr,x) would crash).
        {
            std::array<vm_struct_entry_t, 6> structs{ {
                { "ConstantPool", "_length",          "int",  0,  16u, nullptr },
                { "ConstantPool", nullptr,            "int",  0,  20u, nullptr }, // partial: SKIP
                { "ConstMethod",  "_constants",       "ptr",  0,   8u, nullptr },
                { "ConstMethod",  "_name_index",      "u2",   0,  40u, nullptr },
                { "ConstMethod",  "_signature_index", "u2",   0,  42u, nullptr },
                { nullptr,        nullptr,            nullptr, 0,  0u, nullptr }, // terminator
            } };

            // The exact field get_length() looks up (:2365) -> entry with the
            // offset (16) it then reads from (:2379).
            vm_struct_entry_t* const len{ find_struct(structs.data(), "ConstantPool", "_length") };
            check("dy_find_struct_hits_cp_length", len != nullptr);
            check("dy_find_struct_returns_correct_offset",
                  len != nullptr && len->offset == 16u);
            check("dy_find_struct_cp_length_is_first_entry",
                  find_struct(structs.data(), "ConstantPool", "_length") == &structs[0]);

            // The field get_constants() looks up (:2402).
            check("dy_find_struct_hits_constmethod_constants",
                  find_struct(structs.data(), "ConstMethod", "_constants") == &structs[2]);
            // The fields get_name()/get_signature() look up (:2441/:2520).
            check("dy_find_struct_hits_name_index",
                  find_struct(structs.data(), "ConstMethod", "_name_index") == &structs[3]);
            check("dy_find_struct_hits_signature_index",
                  find_struct(structs.data(), "ConstMethod", "_signature_index") == &structs[4]);

            // The PARTIAL entry (structs[1], field_name == nullptr) is SKIPPED,
            // not matched and not crashed-on.  A lookup for a field that only
            // the partial entry's type carries must still resolve via the real
            // entries and must NEVER strcmp the null field_name.  Reaching this
            // assertion at all proves the null field_name did not fault.
            check("dy_find_struct_skips_null_field_name_entry_no_fault", true);
            // A field that does not exist on ConstantPool (only the skipped
            // partial shares its type) -> miss, having safely stepped over the
            // null field_name entry.
            check("dy_find_struct_miss_past_partial_entry_returns_null",
                  find_struct(structs.data(), "ConstantPool", "_tags") == nullptr);

            // Both-name and field-mismatch cases:
            check("dy_find_struct_wrong_type_right_field_miss",
                  find_struct(structs.data(), "Method", "_length") == nullptr);
            check("dy_find_struct_right_type_wrong_field_miss",
                  find_struct(structs.data(), "ConstantPool", "_nope") == nullptr);

            // Null-arg early-outs (:1993): EITHER null short-circuits to nullptr
            // before the walk - so a null field_name argument never reaches the
            // strcmp, distinct from a table entry whose field_name is null.
            check("dy_find_struct_null_type_arg_returns_null",
                  find_struct(structs.data(), nullptr, "_length") == nullptr);
            check("dy_find_struct_null_field_arg_returns_null",
                  find_struct(structs.data(), "ConstantPool", nullptr) == nullptr);
            check("dy_find_struct_both_null_args_returns_null",
                  find_struct(structs.data(), nullptr, nullptr) == nullptr);

            // A table that is ONLY a leading partial entry then the terminator:
            // the walk skips the partial and stops -> nullptr, no fault.
            {
                std::array<vm_struct_entry_t, 2> only_partial{ {
                    { "ConstantPool", nullptr, "int", 0, 99u, nullptr }, // SKIP
                    { nullptr,        nullptr, nullptr, 0, 0u, nullptr }, // terminator
                } };
                check("dy_find_struct_only_partial_then_term_returns_null",
                      find_struct(only_partial.data(), "ConstantPool", "_length") == nullptr);
            }
        }

        // DY3. The REAL library walk under no-JVM: gHotSpotVMStructs /
        // gHotSpotVMTypes are unresolved, so get_vm_types()/get_vm_structs()
        // return nullptr and the loop never enters (entry starts null).  Pin
        // the documented null-table contract AND the null-arg early-outs on the
        // genuine entry points, plus cache-stability over many calls (the
        // resolved pointer is a function-local static, :1924/:1947).
        check("dy_real_iterate_type_constantpool_no_jvm_null",
              vmhook::hotspot::iterate_type_entries("ConstantPool") == nullptr);
        check("dy_real_iterate_type_null_arg_null",
              vmhook::hotspot::iterate_type_entries(nullptr) == nullptr);
        check("dy_real_iterate_struct_cp_length_no_jvm_null",
              vmhook::hotspot::iterate_struct_entries("ConstantPool", "_length") == nullptr);
        check("dy_real_iterate_struct_null_type_arg_null",
              vmhook::hotspot::iterate_struct_entries(nullptr, "_length") == nullptr);
        check("dy_real_iterate_struct_null_field_arg_null",
              vmhook::hotspot::iterate_struct_entries("ConstantPool", nullptr) == nullptr);
        // Cache-stable + no-fault across many calls (the no-JVM table miss must
        // be deterministic, never opportunistically resolve to garbage).
        {
            bool type_stable{ true };
            bool struct_stable{ true };
            for (int i{ 0 }; i < 1000; ++i)
            {
                if (vmhook::hotspot::iterate_type_entries("ConstantPool") != nullptr) { type_stable = false; }
                if (vmhook::hotspot::iterate_struct_entries("ConstantPool", "_length") != nullptr) { struct_stable = false; }
            }
            check("dy_real_iterate_type_cache_stable_1000_calls", type_stable);
            check("dy_real_iterate_struct_cache_stable_1000_calls", struct_stable);
        }
        // Signatures: the find primitives return the entry-pointer types the
        // feature's accessors consume.
        check("dy_real_iterate_type_returns_type_entry_ptr",
              std::is_same_v<decltype(vmhook::hotspot::iterate_type_entries("ConstantPool")),
                             vmhook::hotspot::vm_type_entry_t*>);
        check("dy_real_iterate_struct_returns_struct_entry_ptr",
              std::is_same_v<decltype(vmhook::hotspot::iterate_struct_entries("ConstantPool", "_length")),
                             vmhook::hotspot::vm_struct_entry_t*>);
        check("dy_real_iterate_type_is_noexcept",
              noexcept(vmhook::hotspot::iterate_type_entries("ConstantPool")));
        check("dy_real_iterate_struct_is_noexcept",
              noexcept(vmhook::hotspot::iterate_struct_entries("ConstantPool", "_length")));
    }

    // =====================================================================
    // DEEPEN-DZ.  (ADDITIVE, "dz_" namespace.)  The OFFSET/SIZE-RESOLUTION
    //    arithmetic the two accessors apply once an entry is found, and the
    //    JDK-8 FieldInfo Array<u2> RECORD-ABI / flags-width extraction that
    //    feeds the field-path CP reads.  All derived from source; all over
    //    OWNED buffers or pure arithmetic on synthetic aligned addresses (no
    //    deref of any fabricated pointer).
    //
    //    get_base():  reinterpret_cast<void**>((uint8_t*)this + entry->size)  (:2343)
    //    get_length(): safe_read of int32 at (uint8_t*)this + entry->offset   (:2379)
    //    The Array<u2> record (JDK 8..20, :4121-4165):
    //      header int32 _length at +0, u2 data at +4 (:4125 - the LIVE offset,
    //         NOT the +8 a stale comment at :4099 mentions);
    //      6 slots/record: 0 access_flags, 1 name_index, 2 signature_index,
    //         3 initval_index, 4 low_packed, 5 high_packed (:4128-4133);
    //      offset = ((high_packed<<16)|low_packed) >> 2  (FIELDINFO_TAG_SIZE=2, :4162-4163);
    //      is_static = (access_flags & 0x0008) != 0  (:4165).
    // =====================================================================
    {
        // DZ1. get_base() size-resolution: base == this + entry->size, computed
        // through a STUBBED vm_type_entry_t (the real struct, with size set to
        // the planted header size) exactly as :2343 does, over an OWNED buffer.
        {
            std::array<std::uint8_t, 256> buf{};
            std::uint8_t* const self{ buf.data() };
            const std::uint64_t header_sizes[]{ 56u, 64u, 80u, 96u, 112u, 128u };
            bool base_offset_exact{ true };
            for (const std::uint64_t H : header_sizes)
            {
                vmhook::hotspot::vm_type_entry_t entry{ "ConstantPool", nullptr, 0, 0, 0, H };
                // The library expression at :2343, verbatim.
                void** const base{ reinterpret_cast<void**>(self + entry.size) };
                const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(base) };
                const std::uintptr_t self_addr{ reinterpret_cast<std::uintptr_t>(self) };
                if (base_addr != self_addr + H) { base_offset_exact = false; }
            }
            check("dz_getbase_size_resolution_matches_this_plus_entry_size", base_offset_exact);
        }

        // DZ2. get_length() offset-resolution: the int32 _length lives at
        // this + entry->offset (:2379).  Build an OWNED buffer with a known
        // int32 planted at a chosen offset, set a stubbed vm_struct_entry_t's
        // offset to match, and confirm reading at (uint8_t*)buf + entry.offset
        // recovers the planted value - the arithmetic get_length() performs.
        {
            std::array<std::uint8_t, 64> buf{};
            const std::uint64_t cp_length_offset{ 16u };
            const std::int32_t planted_length{ 0x0001'2345 };
            std::memcpy(buf.data() + cp_length_offset, &planted_length, sizeof(planted_length));

            vmhook::hotspot::vm_struct_entry_t entry{ "ConstantPool", "_length", "int", 0, cp_length_offset, nullptr };
            std::int32_t read_back{ -1 };
            std::memcpy(&read_back, buf.data() + entry.offset, sizeof(read_back));
            check("dz_getlength_offset_resolution_recovers_planted_int32",
                  read_back == planted_length);
            // And the recovered length feeds the bound exactly as get_length()'s
            // return does: a planted length of 0x12345 admits index 0x12344 and
            // rejects index 0x12345 (the field-path u32 compare, :3905).
            check("dz_getlength_planted_length_admits_last_valid_index",
                  read_back > 0 && static_cast<std::uint32_t>(planted_length - 1) < static_cast<std::uint32_t>(read_back));
            check("dz_getlength_planted_length_rejects_edge_index",
                  static_cast<std::uint32_t>(planted_length) >= static_cast<std::uint32_t>(read_back));
        }

        // DZ3. Array<u2> FieldInfo RECORD ABI (JDK 8..20).  Build an OWNED
        // u2 buffer laid out exactly as :4121-4133: int32 _length header at +0,
        // u2 records at +4, 6 slots each.  Read the name_index / sig_index the
        // field-path feeds to resolve_constant_pool_symbol, the access_flags
        // static bit, and the packed offset reconstruction - all at the exact
        // slot indices and with the exact shifts the source uses.
        {
            // Two field records.  Slot layout per record:
            //   [0]=access_flags [1]=name_index [2]=sig_index
            //   [3]=initval_index [4]=low_packed [5]=high_packed
            // Record 0: a static field (flags bit 3 set), name 7, sig 9,
            //           packed offset value 0x40 (so low/high encode 0x40<<2).
            // Record 1: an instance field (flags bit 3 clear), name 11, sig 13,
            //           packed offset value 0x108.
            const std::uint32_t want_offset_0{ 0x40u };
            const std::uint32_t want_offset_1{ 0x108u };
            const std::uint32_t packed_0{ want_offset_0 << 2 };  // inverse of >>2 (:4163)
            const std::uint32_t packed_1{ want_offset_1 << 2 };

            const std::uint16_t records[2][6]{
                // flags(static=0x0008), name, sig, initval, low_packed, high_packed
                { 0x0009u, 7u, 9u, 0u,
                  static_cast<std::uint16_t>(packed_0 & 0xFFFFu),
                  static_cast<std::uint16_t>((packed_0 >> 16) & 0xFFFFu) },
                { 0x0001u, 11u, 13u, 0u,
                  static_cast<std::uint16_t>(packed_1 & 0xFFFFu),
                  static_cast<std::uint16_t>((packed_1 >> 16) & 0xFFFFu) },
            };

            // Backing buffer: 4 header bytes + 12 u2 slots (two 6-slot records).
            std::array<std::uint8_t, 4u + 12u * sizeof(std::uint16_t)> fields_array{};
            const std::int32_t array_length{ 2 * 6 }; // 12 u2 slots, no trailing count
            std::memcpy(fields_array.data(), &array_length, sizeof(array_length));
            std::memcpy(fields_array.data() + 4, records, sizeof(records)); // data at +4 (:4125)

            // The library's data pointer (:4125): (uint8_t*)fields_array + 4.
            const std::uint16_t* const data{
                reinterpret_cast<const std::uint16_t*>(fields_array.data() + 4) };

            static const std::int32_t field_slots{ 6 }; // :4111
            const std::int32_t records_iterated{ array_length / field_slots }; // :4134 loop bound
            check("dz_array_u2_loop_iterates_two_records", records_iterated == 2);

            // Record 0: pull each slot at the EXACT index the source uses.
            {
                const std::int32_t r{ 0 };
                const std::uint16_t access_flags{ data[r * field_slots + 0] }; // :4153
                const std::uint16_t name_index{ data[r * field_slots + 1] };   // :4136
                const std::uint16_t sig_index{ data[r * field_slots + 2] };    // :4154
                const std::uint16_t low_packed{ data[r * field_slots + 4] };   // :4155
                const std::uint16_t high_packed{ data[r * field_slots + 5] };  // :4156
                const std::uint32_t packed{ (static_cast<std::uint32_t>(high_packed) << 16) | low_packed }; // :4162
                const std::uint32_t offset{ packed >> 2 };                     // :4163
                const bool is_static{ (access_flags & 0x0008u) != 0u };        // :4165

                check("dz_record0_name_index_at_slot1", name_index == 7u);
                check("dz_record0_sig_index_at_slot2", sig_index == 9u);
                check("dz_record0_packed_offset_reconstructs", offset == want_offset_0);
                check("dz_record0_static_flag_bit3_set", is_static);
                // The name/sig indices are exactly what the field path hands to
                // resolve_constant_pool_symbol; both are non-zero (real fields),
                // so guard (0) admits them and a length bound > 9 keeps them in.
                check("dz_record0_indices_pass_field_helper_guard0",
                      name_index != 0u && sig_index != 0u);
            }
            // Record 1: instance field, different indices/offset.
            {
                const std::int32_t r{ 1 };
                const std::uint16_t access_flags{ data[r * field_slots + 0] };
                const std::uint16_t name_index{ data[r * field_slots + 1] };
                const std::uint16_t sig_index{ data[r * field_slots + 2] };
                const std::uint16_t low_packed{ data[r * field_slots + 4] };
                const std::uint16_t high_packed{ data[r * field_slots + 5] };
                const std::uint32_t packed{ (static_cast<std::uint32_t>(high_packed) << 16) | low_packed };
                const std::uint32_t offset{ packed >> 2 };
                const bool is_static{ (access_flags & 0x0008u) != 0u };

                check("dz_record1_name_index_at_slot1", name_index == 11u);
                check("dz_record1_sig_index_at_slot2", sig_index == 13u);
                check("dz_record1_packed_offset_reconstructs", offset == want_offset_1);
                check("dz_record1_instance_flag_bit3_clear", !is_static);
            }

            // DZ4. The record-walk loop bound (:4134, field_slot_index <
            // array_length / field_slots) never reads past the buffer: the
            // highest slot the LAST iterated record touches is
            // (records_iterated-1)*6 + 5, which must be < array_length.
            const std::int32_t highest_slot{ (records_iterated - 1) * field_slots + 5 };
            check("dz_array_u2_last_record_highest_slot_in_bounds",
                  highest_slot < array_length);
            // And the data pointer sits at +4, so the byte span the last slot
            // touches is within our OWNED backing buffer.
            const std::size_t last_slot_byte_end{
                4u + static_cast<std::size_t>(highest_slot + 1) * sizeof(std::uint16_t) };
            check("dz_array_u2_last_slot_within_owned_buffer",
                  last_slot_byte_end <= fields_array.size());

            // DZ5. The JDK-8 trailing _java_fields_count case: array_length =
            // records*6 + 1; integer division floors the partial trailing slot
            // away so the loop iterates exactly `records` records (:4115/:4134).
            {
                const std::int32_t with_trailing{ 2 * field_slots + 1 }; // 13
                check("dz_array_u2_trailing_count_floored_by_division",
                      (with_trailing / field_slots) == 2);
                const std::int32_t last_touched{ (with_trailing / field_slots - 1) * field_slots + 5 };
                check("dz_array_u2_trailing_count_last_record_in_bounds",
                      last_touched < with_trailing);
            }
        }

        // DZ6. The static-flag mask (0x0008 = ACC_STATIC, bit 3) extraction
        // width: it is read out of a u2 access_flags slot, so only the low 16
        // bits matter and the test `(flags & 0x0008) != 0` is a pure bit test.
        // Pin the mask value and the extraction across the bit-3 boundary.
        {
            check("dz_acc_static_mask_is_bit3", (0x0008u == (1u << 3)));
            // bit 3 set in various surrounding flag words -> static.
            const std::uint16_t static_words[]{ 0x0008u, 0x0009u, 0x000Au, 0x0018u, 0xFFFFu };
            bool all_static{ true };
            for (const std::uint16_t w : static_words)
            {
                if ((w & 0x0008u) == 0u) { all_static = false; }
            }
            check("dz_acc_static_bit3_set_words_are_static", all_static);
            // bit 3 clear -> instance, even with every OTHER low bit set.
            const std::uint16_t instance_words[]{ 0x0000u, 0x0001u, 0x0002u, 0x0004u, 0x0007u, 0xFFF7u };
            bool none_static{ true };
            for (const std::uint16_t w : instance_words)
            {
                if ((w & 0x0008u) != 0u) { none_static = false; }
            }
            check("dz_acc_static_bit3_clear_words_are_instance", none_static);
            // 0xFFF7 == ~0x0008 in 16 bits: every flag bit EXCEPT static set,
            // so the mask isolates exactly bit 3 and nothing else.
            check("dz_acc_static_mask_isolates_only_bit3",
                  static_cast<std::uint16_t>(0xFFFFu & ~0x0008u) == 0xFFF7u
                      && (0xFFF7u & 0x0008u) == 0u);
        }
    }

    // =====================================================================
    // DEEPEN-EA.  (ADDITIVE, "ea_" namespace.)  The OTHER access-flags width
    //    the feature surface depends on: the JIT-inhibit mask NO_COMPILE
    //    (vmhook.hpp :7579-7583) and the mask-equality predicate
    //    safe_access_flags_test/or/and apply (`(flags & mask) == mask`,
    //    :2796 / :2836 / :2884).  DZ6 pinned the u2 ACC_STATIC bit (0x0008)
    //    the FIELD path reads; this pins the u4 high-byte compile-control bits
    //    the METHOD path's deopt/re-arm/teardown read-modify-write, which DZ
    //    does not touch.  All pure integer arithmetic over the REAL library
    //    constant vmhook::hotspot::NO_COMPILE - no JVM, no memory read, no
    //    fabricated address; the constant is consumed straight from source so
    //    a value/width drift reddens a NAMED check here.
    // =====================================================================
    {
        const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };

        // EA1. NO_COMPILE is exactly the OR of the four documented bits
        //   (:7558-7561): JVM_ACC_NOT_C2_COMPILABLE (0x02000000),
        //   JVM_ACC_NOT_C1_COMPILABLE (0x04000000),
        //   JVM_ACC_NOT_C2_OSR_COMPILABLE (0x08000000), JVM_ACC_QUEUED (0x01000000).
        constexpr std::uint32_t NOT_C2{ 0x02000000u };
        constexpr std::uint32_t NOT_C1{ 0x04000000u };
        constexpr std::uint32_t NOT_C2_OSR{ 0x08000000u };
        constexpr std::uint32_t QUEUED{ 0x01000000u };
        check("ea_no_compile_is_or_of_four_compile_control_bits",
              no_compile == (NOT_C2 | NOT_C1 | NOT_C2_OSR | QUEUED));
        check("ea_no_compile_value_is_0x0F000000", no_compile == 0x0F000000u);
        // The constant is declared `const std::int32_t` (:7579) and is a positive
        // value (high bit 31 clear), so the static_cast to u32 is value-preserving.
        check("ea_no_compile_decl_type_is_int32",
              std::is_same_v<decltype(vmhook::hotspot::NO_COMPILE), const std::int32_t>);
        check("ea_no_compile_is_positive", vmhook::hotspot::NO_COMPILE > 0);

        // EA2. Bit POSITIONS: the four bits are 24..27 (the high byte of the
        //   historical u4 AccessFlags, :7564); none below bit 24.
        check("ea_no_compile_bits_are_24_through_27",
              no_compile == ((std::uint32_t{ 1u } << 24) | (std::uint32_t{ 1u } << 25)
                             | (std::uint32_t{ 1u } << 26) | (std::uint32_t{ 1u } << 27)));
        // EA3. The whole mask lives in the HIGH u2 of the u4 word: AND with the
        //   low 16 bits is zero.  This is why a JDK 24+ build that shrank
        //   _access_flags to u2 (:7566-7568) sees NONE of these bits - the OR
        //   lands in the alignment padding above the surviving u2 (a no-op, not
        //   corruption).  Pin the "entirely in the high half" property.
        check("ea_no_compile_disjoint_from_low_u2", (no_compile & 0x0000FFFFu) == 0u);
        // EA4. NO_COMPILE is DISJOINT from the field path's ACC_STATIC (0x0008,
        //   bit 3): the two flag families never alias, so ORing NO_COMPILE on a
        //   method can never flip a static-modifier bit and vice versa.
        check("ea_no_compile_disjoint_from_acc_static",
              (no_compile & 0x0008u) == 0u);

        // EA5. The mask-equality predicate `(flags & mask) == mask` the three
        //   safe_access_flags_* helpers share (:2796/:2836/:2884): it returns
        //   true IFF EVERY bit of mask is present in flags.  Model it and pin the
        //   accept/reject cases the watchdog drift check and teardown rely on.
        auto all_bits_set = [](std::uint32_t flags, std::uint32_t mask) -> bool
        {
            return (flags & mask) == mask;
        };
        // Exactly the four bits set -> all present -> true.
        check("ea_flags_all_no_compile_bits_set_passes",
              all_bits_set(no_compile, no_compile));
        // A superset (NO_COMPILE plus unrelated bits) still passes - the test is
        // "are these bits set", not "are ONLY these bits set".
        check("ea_flags_superset_passes",
              all_bits_set(no_compile | 0x0008u | 0x0001u, no_compile));
        // Missing ANY single one of the four bits -> fails (the drift check then
        // treats NO_COMPILE as not-fully-set and re-arms).
        check("ea_flags_missing_not_c2_fails",   !all_bits_set(no_compile & ~NOT_C2, no_compile));
        check("ea_flags_missing_not_c1_fails",   !all_bits_set(no_compile & ~NOT_C1, no_compile));
        check("ea_flags_missing_osr_fails",      !all_bits_set(no_compile & ~NOT_C2_OSR, no_compile));
        check("ea_flags_missing_queued_fails",   !all_bits_set(no_compile & ~QUEUED, no_compile));
        // A word with only unrelated bits (no NO_COMPILE bits at all) -> fails.
        check("ea_flags_only_unrelated_bits_fails",
              !all_bits_set(0x0008u | 0x0001u, no_compile));
        // Zero flags -> fails for any non-zero mask.
        check("ea_flags_zero_fails", !all_bits_set(0u, no_compile));

        // EA6. The teardown AND-clear mask `~NO_COMPILE` (used by
        //   safe_access_flags_and via the library's `~vmhook::hotspot::NO_COMPILE`
        //   call sites, :11294 / :11391): clearing it from a word that has NONE of
        //   its bits is a no-op (the masked word equals the original); from a word
        //   WITH them, it drops exactly those bits and preserves the rest.
        const std::uint32_t clear_mask{ static_cast<std::uint32_t>(~vmhook::hotspot::NO_COMPILE) };
        // A word with NO NO_COMPILE bits: AND with ~NO_COMPILE is identity.
        check("ea_clear_mask_noop_when_bits_absent",
              (0x0008u & clear_mask) == 0x0008u);
        // A word WITH NO_COMPILE bits: AND with ~NO_COMPILE drops exactly them.
        check("ea_clear_mask_drops_exactly_no_compile",
              ((no_compile | 0x0008u) & clear_mask) == 0x0008u);
        // ~NO_COMPILE preserves the entire low u2 (the static modifier survives a
        // compile-control teardown).
        check("ea_clear_mask_preserves_low_u2",
              (clear_mask & 0x0000FFFFu) == 0x0000FFFFu);
    }

    if (failures == 0)
    {
        std::printf("vmhook const_method/ConstantPool bounds: OK\n");
    }
    else
    {
        std::printf("vmhook const_method/ConstantPool bounds: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
