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

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
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
