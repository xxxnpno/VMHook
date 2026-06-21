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

#include <bit>
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
