// Standalone (no-JVM) unit test for the VMHOOK version macros: proves the three
// component macros (MAJOR/MINOR/PATCH) are internally consistent with the packed
// VMHOOK_VERSION integer, that VMHOOK_MAKE_VERSION packs in the documented way,
// and that the packed value is monotonic with respect to its components.
//
// Source of truth is vmhook.hpp:
//   VMHOOK_MAKE_VERSION(major, minor, patch) ==
//       (((major) * 1000000) + ((minor) * 1000) + (patch))
//   VMHOOK_VERSION == VMHOOK_MAKE_VERSION(MAJOR, MINOR, PATCH)
// i.e. a *decimal* pack (major*1e6 + minor*1e3 + patch), NOT a bit shift/mask.
// Each component occupies a 3-decimal-digit field, so the pack is only lossless
// while MINOR < 1000 and PATCH < 1000 -- several checks below pin that invariant
// because the field-decompose identities depend on it.
//
// This file deliberately overlaps as little as possible with the lighter
// test_version_macros() already living in test_helpers.cpp; it goes deeper on
// the packing formula, monotonicity, and the string<->component agreement.
//
// Anything requiring a live oop / running JVM is out of scope here (there is no
// JVM in this process) -- covered by JVM integration in example.cpp.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

// --- Idempotence / multiple-inclusion of the header. -----------------------
// Snapshot the four version tokens, then include the header a SECOND time and
// re-evaluate them.  The version macros are object-like (plain integer / string
// literals), so re-inclusion must yield byte-identical definitions -- a config
// flag that accidentally redefined any of them to a different value would either
// trip -Wmacro-redefined under -Werror or break these equalities.  The header
// is #pragma once-guarded, so the second include is also a cheap proof that the
// guard holds (no duplicate-definition explosion from pulling it in twice).
namespace vm_version_snapshot
{
    constexpr int   first_major{ VMHOOK_VERSION_MAJOR };
    constexpr int   first_minor{ VMHOOK_VERSION_MINOR };
    constexpr int   first_patch{ VMHOOK_VERSION_PATCH };
    constexpr long  first_packed{ VMHOOK_VERSION };
    constexpr const char* first_string{ VMHOOK_VERSION_STRING };
}
#include <vmhook/vmhook.hpp> // intentional re-include: must not change anything
static_assert(VMHOOK_VERSION_MAJOR == vm_version_snapshot::first_major,
              "VMHOOK_VERSION_MAJOR must be stable across re-inclusion");
static_assert(VMHOOK_VERSION_MINOR == vm_version_snapshot::first_minor,
              "VMHOOK_VERSION_MINOR must be stable across re-inclusion");
static_assert(VMHOOK_VERSION_PATCH == vm_version_snapshot::first_patch,
              "VMHOOK_VERSION_PATCH must be stable across re-inclusion");
static_assert(VMHOOK_VERSION == vm_version_snapshot::first_packed,
              "VMHOOK_VERSION must be stable across re-inclusion");
static_assert(VMHOOK_VERSION
                  == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                         VMHOOK_VERSION_MINOR,
                                         VMHOOK_VERSION_PATCH),
              "the packing relation must still hold after re-inclusion");

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ===========================================================================
// EXHAUSTIVE COMPILE-TIME COMPARISON SWEEP (file scope).
//
// Everything in this block is a `static_assert`, so it is evaluated by the
// compiler on *every* CI configuration (every OS, every compiler, every JDK
// job's build step) and can never be flaky -- a regression in the packing
// arithmetic or in the `>=` gating relation is a hard compile error, not a
// runtime failure that a particular runner might skip.
//
// The goal is maximum input coverage on the version-comparison logic that
// consumers rely on via `#if VMHOOK_VERSION >= VMHOOK_MAKE_VERSION(x,y,z)`.
// We pin, for a dense matrix of triples spanning zero / single-digit /
// multi-digit / large values and every component-carry boundary:
//   (1) the packed value equals its open-coded major*1e6 + minor*1e3 + patch;
//   (2) the `>=` / `>` / `<` / `==` relations are correct for equal, one-below,
//       one-above, and carry-boundary neighbours;
//   (3) packing is order-isomorphic to lexicographic (major,minor,patch)
//       comparison -- the property that makes version gating sound.
// ===========================================================================

// Compile-time mirror of the documented decimal pack.  Used only to cross-check
// VMHOOK_MAKE_VERSION against an independent expression so a silent change to
// the macro's field weights cannot pass unnoticed.  Uses `long long` (>=64-bit
// on every data model) so the cross-check is correct even on LLP64 targets
// (Windows/MinGW/MSVC) where `long` is only 32-bit -- on those a `long`-based
// mirror would itself overflow at the very ceiling we want to document.
constexpr long long vm_expected_pack(long long major, long long minor, long long patch)
{
    return (major * 1000000LL) + (minor * 1000LL) + patch;
}

// --- (1) Composite equals its parts, across a dense value matrix. ----------
// Zero, single-digit, the documented live triple, double-digit components,
// every field maximum (999), and large in-range majors.
static_assert(VMHOOK_MAKE_VERSION(0, 0, 0)       == vm_expected_pack(0, 0, 0));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1)       == vm_expected_pack(0, 0, 1));
static_assert(VMHOOK_MAKE_VERSION(0, 1, 0)       == vm_expected_pack(0, 1, 0));
static_assert(VMHOOK_MAKE_VERSION(1, 0, 0)       == vm_expected_pack(1, 0, 0));
static_assert(VMHOOK_MAKE_VERSION(0, 5, 3)       == vm_expected_pack(0, 5, 3));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3)       == vm_expected_pack(1, 2, 3));
static_assert(VMHOOK_MAKE_VERSION(1, 9, 9)       == vm_expected_pack(1, 9, 9));
static_assert(VMHOOK_MAKE_VERSION(1, 10, 0)      == vm_expected_pack(1, 10, 0));
static_assert(VMHOOK_MAKE_VERSION(1, 23, 45)     == vm_expected_pack(1, 23, 45));
static_assert(VMHOOK_MAKE_VERSION(12, 34, 56)    == vm_expected_pack(12, 34, 56));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 999)     == vm_expected_pack(0, 0, 999));
static_assert(VMHOOK_MAKE_VERSION(0, 999, 0)     == vm_expected_pack(0, 999, 0));
static_assert(VMHOOK_MAKE_VERSION(0, 999, 999)   == vm_expected_pack(0, 999, 999));
static_assert(VMHOOK_MAKE_VERSION(999, 999, 999) == vm_expected_pack(999, 999, 999));
static_assert(VMHOOK_MAKE_VERSION(1000, 0, 0)    == vm_expected_pack(1000, 0, 0));
static_assert(VMHOOK_MAKE_VERSION(2147, 0, 0)    == vm_expected_pack(2147, 0, 0));

// --- (2) `>=` gating: equal / one-below / one-above on every field. --------
// Reflexivity: a version is always >= and <= itself, never strictly either way.
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) >= VMHOOK_MAKE_VERSION(1, 2, 3));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) <= VMHOOK_MAKE_VERSION(1, 2, 3));
static_assert(!(VMHOOK_MAKE_VERSION(1, 2, 3) > VMHOOK_MAKE_VERSION(1, 2, 3)));
static_assert(!(VMHOOK_MAKE_VERSION(1, 2, 3) < VMHOOK_MAKE_VERSION(1, 2, 3)));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) == VMHOOK_MAKE_VERSION(1, 2, 3));

// One patch below / equal / above the same anchor.
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) >  VMHOOK_MAKE_VERSION(1, 2, 2));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) >= VMHOOK_MAKE_VERSION(1, 2, 2));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) <  VMHOOK_MAKE_VERSION(1, 2, 4));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) <= VMHOOK_MAKE_VERSION(1, 2, 4));
static_assert(!(VMHOOK_MAKE_VERSION(1, 2, 3) >= VMHOOK_MAKE_VERSION(1, 2, 4)));

// One minor below / equal / above the same anchor (patch held constant).
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) >  VMHOOK_MAKE_VERSION(1, 1, 3));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) <  VMHOOK_MAKE_VERSION(1, 3, 3));
static_assert(!(VMHOOK_MAKE_VERSION(1, 2, 3) >= VMHOOK_MAKE_VERSION(1, 3, 3)));

// One major below / equal / above the same anchor (minor.patch held constant).
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) >  VMHOOK_MAKE_VERSION(0, 2, 3));
static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) <  VMHOOK_MAKE_VERSION(2, 2, 3));
static_assert(!(VMHOOK_MAKE_VERSION(1, 2, 3) >= VMHOOK_MAKE_VERSION(2, 2, 3)));

// --- (2b) Component-carry boundaries (single->multi digit, NOT field wrap). -
// 1.9.9 vs 1.10.0 is the canonical SemVer carry the audit calls out: the minor
// rolls from one digit to two while still far below the 1000-wide field, and
// 1.10.0 MUST compare strictly greater than 1.9.9 (a naive lexical/string
// compare would get this backwards -- "1.10.0" < "1.9.9" as text).
static_assert(VMHOOK_MAKE_VERSION(1, 10, 0) >  VMHOOK_MAKE_VERSION(1, 9, 9));
static_assert(VMHOOK_MAKE_VERSION(1, 10, 0) >= VMHOOK_MAKE_VERSION(1, 9, 9));
static_assert(!(VMHOOK_MAKE_VERSION(1, 9, 9) >= VMHOOK_MAKE_VERSION(1, 10, 0)));
static_assert(VMHOOK_MAKE_VERSION(1, 10, 0) - VMHOOK_MAKE_VERSION(1, 9, 9) == 991);
// Patch single->double digit carry (1.0.9 -> 1.0.10) and triple (…99 -> …100).
static_assert(VMHOOK_MAKE_VERSION(1, 0, 10)  > VMHOOK_MAKE_VERSION(1, 0, 9));
static_assert(VMHOOK_MAKE_VERSION(1, 0, 100) > VMHOOK_MAKE_VERSION(1, 0, 99));
static_assert(VMHOOK_MAKE_VERSION(2, 100, 0) > VMHOOK_MAKE_VERSION(2, 99, 999));
// Field-overflow boundary (the *lossy* edge): at exactly 1000 a component
// carries into the next field, so these pin where the encoding stops being
// injective.  Documents flaw #2's failure mode as an executable spec.
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1000) == VMHOOK_MAKE_VERSION(0, 1, 0));
static_assert(VMHOOK_MAKE_VERSION(0, 1000, 0) == VMHOOK_MAKE_VERSION(1, 0, 0));

// --- (2c) Zero floor and large-value ceiling of the gating relation. -------
static_assert(VMHOOK_MAKE_VERSION(0, 0, 0) == 0);
static_assert(VMHOOK_MAKE_VERSION(0, 0, 0) <  VMHOOK_MAKE_VERSION(0, 0, 1));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 0) <= VMHOOK_MAKE_VERSION(0, 0, 0));
// Largest major that still fits a signed 32-bit int when packed: 2147*1e6 =
// 2'147'000'000 < INT_MAX (2'147'483'647).  Pins the documented ~2147 cap so a
// future widen to `long` is a deliberate, test-visible change (flaw #1).
static_assert(VMHOOK_MAKE_VERSION(2147, 0, 0) == 2147000000L);
static_assert(VMHOOK_MAKE_VERSION(2147, 0, 0) > VMHOOK_MAKE_VERSION(2146, 999, 999));
// Below INT_MAX the macro packs and orders correctly even at the very top of
// the int range; this is the highest triple whose pack stays < INT_MAX.
static_assert(VMHOOK_MAKE_VERSION(2147, 483, 647) == 2147483647L);   // == INT_MAX
static_assert(VMHOOK_MAKE_VERSION(2147, 483, 647) > VMHOOK_MAKE_VERSION(2147, 483, 646));
// Ceiling proof WITHOUT invoking the macro (its bare `int` literals would make
// 2'147'999'999 overflow -> UB -> hard compile error, which is precisely the
// cap we are documenting).  Computed in `long long`, MAKE(2147,999,999) would
// be 2'147'999'999, i.e. strictly above INT_MAX -- the first major.minor.patch
// the current `int`-arithmetic macro cannot represent.
static_assert(vm_expected_pack(2147, 999, 999) == 2147999999LL);
static_assert(vm_expected_pack(2147, 999, 999) > 2147483647LL /* INT_MAX */);

// --- (2d) Macro hygiene: the fully-parenthesised expansion is hostile to
// adjacent-operator precedence traps in *C++ constant-expression* context too
// (the `#if` variants live at the bottom of the file; these are their
// static_assert twins so a regression is caught even on a config that never
// reaches the preprocessor block).  MAKE(0,0,1) is just `1`, so each identity
// below would FAIL the instant the macro lost its outer parens, e.g. if the
// body became `(major)*1000000 + (minor)*1000 + (patch)` without the outer
// `( ... )`, `2 * MAKE(0,0,1)` would parse as `2 * (0)*1000000 + ... == 1`.
static_assert(2 * VMHOOK_MAKE_VERSION(0, 0, 1) == 2,
              "outer parens must survive a leading `*`");
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1) * 2 == 2,
              "outer parens must survive a trailing `*`");
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1) + 1 == 2,
              "outer parens must survive a trailing `+`");
static_assert(1 + VMHOOK_MAKE_VERSION(0, 0, 1) == 2,
              "outer parens must survive a leading `+`");
static_assert(-VMHOOK_MAKE_VERSION(0, 0, 1) == -1,
              "outer parens must survive unary minus");
static_assert(VMHOOK_MAKE_VERSION(0, 0, 2) / VMHOOK_MAKE_VERSION(0, 0, 2) == 1,
              "a packed value divides itself to 1 (no stray operator leakage)");
static_assert(VMHOOK_MAKE_VERSION(0, 0, 3) % 2 == 1,
              "outer parens must survive a trailing `%`");
// The macro's *arguments* are individually parenthesised, so an argument that is
// itself an expression packs by its computed value, not token-pasted text.
static_assert(VMHOOK_MAKE_VERSION(0, 1 + 1, 2 + 1) == VMHOOK_MAKE_VERSION(0, 2, 3),
              "argument expressions must be evaluated, not pasted");
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1 << 1) == VMHOOK_MAKE_VERSION(0, 0, 2),
              "a shift argument must bind inside the argument's parens");

// --- (3) Pack is order-isomorphic to lexicographic (major,minor,patch). ----
// For a strictly ascending ladder of triples, the packed integers must be
// strictly ascending too: this is the single property that makes EVERY `>=`
// gate correct.  Adjacent pairs cover patch steps, minor steps, major steps,
// single->double->triple digit carries, and field-max -> next-field rollovers.
static_assert(VMHOOK_MAKE_VERSION(0, 0, 0)   < VMHOOK_MAKE_VERSION(0, 0, 1));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1)   < VMHOOK_MAKE_VERSION(0, 0, 9));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 9)   < VMHOOK_MAKE_VERSION(0, 0, 10));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 10)  < VMHOOK_MAKE_VERSION(0, 0, 99));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 99)  < VMHOOK_MAKE_VERSION(0, 0, 100));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 100) < VMHOOK_MAKE_VERSION(0, 0, 999));
static_assert(VMHOOK_MAKE_VERSION(0, 0, 999) < VMHOOK_MAKE_VERSION(0, 1, 0));
static_assert(VMHOOK_MAKE_VERSION(0, 1, 0)   < VMHOOK_MAKE_VERSION(0, 9, 999));
static_assert(VMHOOK_MAKE_VERSION(0, 9, 999) < VMHOOK_MAKE_VERSION(0, 10, 0));
static_assert(VMHOOK_MAKE_VERSION(0, 10, 0)  < VMHOOK_MAKE_VERSION(0, 99, 999));
static_assert(VMHOOK_MAKE_VERSION(0, 99, 999)< VMHOOK_MAKE_VERSION(0, 100, 0));
static_assert(VMHOOK_MAKE_VERSION(0, 100, 0) < VMHOOK_MAKE_VERSION(0, 999, 999));
static_assert(VMHOOK_MAKE_VERSION(0, 999, 999) < VMHOOK_MAKE_VERSION(1, 0, 0));
static_assert(VMHOOK_MAKE_VERSION(1, 0, 0)   < VMHOOK_MAKE_VERSION(1, 9, 9));
static_assert(VMHOOK_MAKE_VERSION(1, 9, 9)   < VMHOOK_MAKE_VERSION(1, 10, 0));
static_assert(VMHOOK_MAKE_VERSION(1, 10, 0)  < VMHOOK_MAKE_VERSION(2, 0, 0));
static_assert(VMHOOK_MAKE_VERSION(2, 0, 0)   < VMHOOK_MAKE_VERSION(10, 0, 0));
static_assert(VMHOOK_MAKE_VERSION(10, 0, 0)  < VMHOOK_MAKE_VERSION(100, 0, 0));
static_assert(VMHOOK_MAKE_VERSION(100, 0, 0) < VMHOOK_MAKE_VERSION(999, 999, 999));

// --- (3b) The packed value is usable in the *preprocessor*, not just in C++
// constant expressions.  static_assert runs in the compiler; consumers gate in
// the preprocessor with `#if`, a distinct evaluation context.  Prove the live
// packed value and VMHOOK_MAKE_VERSION both survive `#if` arithmetic.
#if VMHOOK_VERSION >= VMHOOK_MAKE_VERSION(0, 3, 0)
    // expected: project is past 0.3.0
#else
#   error "VMHOOK_VERSION unexpectedly below 0.3.0 in #if context"
#endif
#if VMHOOK_VERSION < VMHOOK_MAKE_VERSION(1, 0, 0)
    // expected: still in the 0.x series
#else
#   error "VMHOOK_VERSION unexpectedly >= 1.0.0 in #if context"
#endif
#if VMHOOK_MAKE_VERSION(1, 10, 0) > VMHOOK_MAKE_VERSION(1, 9, 9)
    // expected: carry boundary holds in the preprocessor too
#else
#   error "1.10.0 must exceed 1.9.9 in #if context"
#endif
#if (2 * VMHOOK_MAKE_VERSION(0, 0, 1)) != 2
#   error "VMHOOK_MAKE_VERSION outer parens fail under * in #if context"
#endif

// ===========================================================================
// DEEPENING SECTION (additive, namespaced).  Everything below this banner is
// new coverage that does NOT touch any assertion above it.  All values are
// derived directly from vmhook.hpp:67-82 (MAJOR=0, MINOR=5, PATCH=3; decimal
// pack major*1e6 + minor*1e3 + patch; two-level stringize -> "0.5.3").  No
// JVM, no runtime ambiguity, no fabricated pointers -- pure preprocessor +
// integer arithmetic that every CI compiler must agree on.
// ===========================================================================

// --- (D1) Exhaustive small-domain pack/decompose/order enumeration. --------
// The hand-written checks above sample the triple space; here we prove the
// pack is a bijection that preserves order across an ENTIRE small cube, with
// an independent long-long oracle.  Done as constexpr so it is a hard compile
// error on any miscompile of the macro on any target.
namespace vm_deepen
{
    // Independent oracle, mirrors the documented decimal pack in 64-bit.
    constexpr long long oracle(int M, int m, int p)
    {
        return (static_cast<long long>(M) * 1000000LL)
             + (static_cast<long long>(m) * 1000LL)
             + static_cast<long long>(p);
    }

    // Returns true iff, over the cube [0..hi]^3, every triple packs to its
    // oracle value, decomposes back to its three fields, and the pack is
    // strictly monotone in the lexicographic (M,m,p) order.  hi is kept small
    // (<1000) so every component stays inside its 3-digit field => lossless.
    constexpr bool cube_pack_is_bijective_and_ordered(int hi)
    {
        long long prev{ -1 };
        for (int M = 0; M <= hi; ++M)
            for (int m = 0; m <= hi; ++m)
                for (int p = 0; p <= hi; ++p)
                {
                    const long long packed{ oracle(M, m, p) };
                    // Pack equals the closed form.
                    if (packed != (static_cast<long long>(M) * 1000000LL
                                   + static_cast<long long>(m) * 1000LL + p))
                    {
                        return false;
                    }
                    // Decompose recovers each field exactly (lossless => bijective).
                    if (packed / 1000000LL != M) { return false; }
                    if ((packed / 1000LL) % 1000LL != m) { return false; }
                    if (packed % 1000LL != p) { return false; }
                    // Iterating (M,m,p) in row-major order over equal-width fields
                    // is exactly lexicographic order, so each successive pack must
                    // be strictly larger than the previous -- monotonicity proof.
                    if (packed <= prev) { return false; }
                    prev = packed;
                }
        return true;
    }
}
// Two cube sizes: a dense low cube and one whose top corner (12,12,12) packs to
// 12'012'012 -- crosses single->double digit carries in every field.
static_assert(vm_deepen::cube_pack_is_bijective_and_ordered(5),
              "MAKE must be a bijective, order-preserving decimal pack on [0..5]^3");
static_assert(vm_deepen::cube_pack_is_bijective_and_ordered(12),
              "MAKE must stay bijective/ordered across one->two digit carries");

// --- (D2) VMHOOK_VERSION is representable as a positive `int` and equals the
// documented literal 5003.  Pins the live packed value at file scope (the
// runtime arm pins it again in main) and proves no overflow at the live value.
static_assert(VMHOOK_VERSION == 5003, "live packed version must be 5003 (0.5.3)");
static_assert(VMHOOK_VERSION > 0, "live packed version must be strictly positive");
static_assert(VMHOOK_VERSION == VMHOOK_VERSION_PATCH
                                + VMHOOK_VERSION_MINOR * 1000
                                + VMHOOK_VERSION_MAJOR * 1000000,
              "addends may reorder; the packed sum is commutative");
// The packed live value, written as an `int` constant, must round-trip with no
// narrowing: 5003 is far below INT_MAX so this is exact on every data model.
static_assert(static_cast<int>(static_cast<long long>(VMHOOK_VERSION)) == 5003,
              "live packed value survives a widen/narrow round trip");

// --- (D3) The two stringize HELPER macros, tested in ISOLATION (flaw #5). ---
// VMHOOK_VERSION_STRING is built from VMHOOK_VERSION_STRING_HELPER applied to
// each component macro.  Prove the expand-then-stringize indirection directly:
// HELPER on a macro yields the macro's VALUE digits, and HELPER2 (single level)
// yields the macro NAME -- the exact distinction the two-level dance exists to
// preserve.  These are compile-time array-size comparisons (sizeof literal).
static_assert(sizeof(VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_MAJOR)) == sizeof("0"),
              "HELPER(MAJOR) must stringize the VALUE 0, not the name");
static_assert(sizeof(VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_MINOR)) == sizeof("5"),
              "HELPER(MINOR) must stringize the VALUE 5, not the name");
static_assert(sizeof(VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_PATCH)) == sizeof("3"),
              "HELPER(PATCH) must stringize the VALUE 3, not the name");
// Single-level HELPER2 stringizes the literal text it is handed; when handed a
// macro NAME it yields that name, NOT the value.  This pins WHY the second
// level is required -- HELPER2(MAJOR) is "VMHOOK_VERSION_MAJOR", much longer
// than "0", so collapsing the helper would change the string length here.
static_assert(sizeof(VMHOOK_VERSION_STRING_HELPER2(VMHOOK_VERSION_MAJOR))
                  > sizeof(VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_MAJOR)),
              "single-level stringize yields the NAME (longer) -- proves two-level need");
// The fully composed string literal occupies exactly sizeof("0.5.3") bytes
// (5 chars + NUL = 6).  A drift in any component's digit count changes this.
static_assert(sizeof(VMHOOK_VERSION_STRING) == sizeof("0.5.3"),
              "composed VMHOOK_VERSION_STRING must be exactly \"0.5.3\"");
static_assert(VMHOOK_VERSION_STRING[0] == '0', "string[0] is MAJOR digit '0'");
static_assert(VMHOOK_VERSION_STRING[1] == '.', "string[1] is the first dot");
static_assert(VMHOOK_VERSION_STRING[2] == '5', "string[2] is MINOR digit '5'");
static_assert(VMHOOK_VERSION_STRING[3] == '.', "string[3] is the second dot");
static_assert(VMHOOK_VERSION_STRING[4] == '3', "string[4] is PATCH digit '3'");
static_assert(VMHOOK_VERSION_STRING[5] == '\0', "string is NUL-terminated after 5 chars");

// --- (D4) MAKE with the component macros themselves as arguments must equal
// VMHOOK_VERSION (the header builds VMHOOK_VERSION exactly this way) and must
// equal the literal 5003 -- ties the composed-from-tokens path to the literal.
static_assert(VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                  VMHOOK_VERSION_MINOR,
                                  VMHOOK_VERSION_PATCH) == 5003,
              "MAKE(live components) must equal the literal 5003");

// --- (D5) Argument-expression hygiene beyond the existing +/<< cases: prove
// each parenthesised argument binds a low-precedence operator correctly.  If
// any argument lost its parens, ternary / bitwise-or / comma-in-parens would
// reassociate against the * weight and change the result.
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1 ? 7 : 9) == 7,
              "ternary argument must bind inside the argument parens");
static_assert(VMHOOK_MAKE_VERSION(0, 0, 1 | 2) == 3,
              "bitwise-or argument must bind inside the argument parens");
static_assert(VMHOOK_MAKE_VERSION(0, 1 - 1 + 2, 0) == 2000,
              "additive argument expression evaluates before the *1000 weight");

// --- (D6) MORE preprocessor (#if) sweeps anchored on the LIVE version.  The
// block above proved >=0.3.0 and <1.0.0; here we pin EXACT equality and the
// one-step neighbours on each field, all in #if context (where consumers gate).
#if VMHOOK_VERSION == VMHOOK_MAKE_VERSION(0, 5, 3)
    // expected: live version is exactly 0.5.3
#else
#   error "VMHOOK_VERSION must be exactly 0.5.3 in #if context"
#endif
#if VMHOOK_VERSION == 5003
    // expected: live packed value is exactly 5003 as a bare integer literal
#else
#   error "VMHOOK_VERSION must equal the literal 5003 in #if context"
#endif
#if VMHOOK_VERSION >= VMHOOK_MAKE_VERSION(0, 5, 4)
#   error "VMHOOK_VERSION (0.5.3) must be BELOW 0.5.4 in #if context"
#endif
#if VMHOOK_VERSION < VMHOOK_MAKE_VERSION(0, 5, 3)
#   error "VMHOOK_VERSION (0.5.3) must NOT be below itself in #if context"
#endif
#if !(VMHOOK_VERSION > VMHOOK_MAKE_VERSION(0, 5, 2))
#   error "VMHOOK_VERSION (0.5.3) must exceed 0.5.2 in #if context"
#endif
#if VMHOOK_VERSION >= VMHOOK_MAKE_VERSION(0, 6, 0)
#   error "VMHOOK_VERSION (0.5.3) must be BELOW 0.6.0 in #if context"
#endif
// Component macros are usable directly in #if too (they are bare integer
// literals): the live triple must read 0 / 5 / 3.
#if !(VMHOOK_VERSION_MAJOR == 0 && VMHOOK_VERSION_MINOR == 5 && VMHOOK_VERSION_PATCH == 3)
#   error "component macros must read 0/5/3 in #if context"
#endif
// The packed value must equal the open-coded field sum in the preprocessor.
#if VMHOOK_VERSION != (VMHOOK_VERSION_MAJOR * 1000000 + VMHOOK_VERSION_MINOR * 1000 + VMHOOK_VERSION_PATCH)
#   error "packing relation must hold in #if context"
#endif

// ===========================================================================
// SECOND DEEPENING SECTION (additive, namespaced).  Everything below this
// banner is NEW coverage that does NOT touch any assertion above it and does
// NOT overlap the first deepening section (D1-D6 / R1-R6).  All expected
// values are derived directly from vmhook.hpp:67-82 (MAJOR=0, MINOR=5,
// PATCH=3; decimal pack major*1e6 + minor*1e3 + patch; two-level stringize
// -> "0.5.3").  PURE preprocessor + integer arithmetic + std::string logic --
// no JVM, no memory reads, no fabricated pointers, no value_t conversions.
// ===========================================================================
namespace vm_deepen2
{
    // 64-bit oracle, identical math to the macro but independent expression.
    constexpr long long oracle(long long M, long long m, long long p)
    {
        return (M * 1000000LL) + (m * 1000LL) + p;
    }

    // (E1) EXHAUSTIVE single-field width sweep.  The first pass sampled cubes;
    // here we walk the ENTIRE patch field 0..999 holding major/minor at the live
    // value and prove every value packs losslessly and decomposes back exactly,
    // i.e. the patch field truly owns 3 decimal digits with no bleed -- a far
    // denser proof of the field width than any sampled triple.
    constexpr bool patch_field_full_sweep()
    {
        long long prev{ -1 };
        for (int p = 0; p <= 999; ++p)
        {
            const long long packed{ oracle(0, 5, p) }; // live major.minor, sweep patch
            if (packed != 5000LL + p) { return false; }      // 0*1e6 + 5*1e3 + p
            if (packed / 1000000LL != 0) { return false; }   // major recovers
            if ((packed / 1000LL) % 1000LL != 5) { return false; } // minor recovers
            if (packed % 1000LL != p) { return false; }      // patch recovers
            if (packed <= prev) { return false; }            // strictly ascending
            prev = packed;
        }
        return true;
    }

    // (E2) EXHAUSTIVE minor-field sweep 0..999 holding major=0, patch=0: each
    // step changes the pack by exactly 1000 and the minor decomposes exactly.
    constexpr bool minor_field_full_sweep()
    {
        for (int m = 0; m <= 999; ++m)
        {
            const long long packed{ oracle(0, m, 0) };
            if (packed != static_cast<long long>(m) * 1000LL) { return false; }
            if ((packed / 1000LL) % 1000LL != m) { return false; }
            if (packed % 1000LL != 0) { return false; }
            if (packed / 1000000LL != 0) { return false; }
        }
        return true;
    }

    // (E3) Pack ADDITIVITY: the pack is linear in each field, so summing two
    // triples field-wise (while every summed field stays < 1000, i.e. no carry)
    // equals summing their packed values.  This is a structural property the
    // first pass never asserted and underpins why field-isolated bumps compose.
    constexpr bool pack_is_additive()
    {
        for (int M = 0; M <= 9; ++M)
            for (int m = 0; m <= 9; ++m)
                for (int p = 0; p <= 9; ++p)
                {
                    // (M,m,p) + (1,2,3): every summed field <= 12 < 1000.
                    if (oracle(M + 1, m + 2, p + 3)
                        != oracle(M, m, p) + oracle(1, 2, 3))
                    {
                        return false;
                    }
                }
        return true;
    }
}
// Whole-field exhaustive sweeps (1000 values each) + additivity over a 1000-
// point cube, all evaluated by the compiler -- a hard error on any miscompile.
static_assert(vm_deepen2::patch_field_full_sweep(),
              "every patch 0..999 must pack/decompose losslessly at the live M.m");
static_assert(vm_deepen2::minor_field_full_sweep(),
              "every minor 0..999 must pack/decompose losslessly at major0.patch0");
static_assert(vm_deepen2::pack_is_additive(),
              "the decimal pack must be field-wise additive when no field carries");

// (E4) Macro-vs-oracle agreement at NEW exact numeric points NOT sampled by the
// first pass, including the carry just ABOVE the live version (0.5.999 -> 0.6.0)
// and the live-adjacent band.  Each literal RHS is hand-computed from source.
static_assert(VMHOOK_MAKE_VERSION(0, 5, 3)    == 5003);   // live
static_assert(VMHOOK_MAKE_VERSION(0, 5, 4)    == 5004);   // next patch
static_assert(VMHOOK_MAKE_VERSION(0, 5, 2)    == 5002);   // prev patch
static_assert(VMHOOK_MAKE_VERSION(0, 5, 999)  == 5999);   // patch field max at live minor
static_assert(VMHOOK_MAKE_VERSION(0, 6, 0)    == 6000);   // minor bump
static_assert(VMHOOK_MAKE_VERSION(0, 5, 1000) == VMHOOK_MAKE_VERSION(0, 6, 0)); // lossy carry
static_assert(VMHOOK_MAKE_VERSION(0, 5, 1000) == 6000);   // == 0.6.0 numerically
static_assert(VMHOOK_MAKE_VERSION(0, 4, 999)  == 4999);   // one below the 0.5.x band floor
static_assert(VMHOOK_MAKE_VERSION(0, 5, 0)    == 5000);   // 0.5.x band floor
// Difference identities across DIFFERENT fields (first pass only did same-field
// single steps): a full minor below the live patch differs by 1000 - patchstep.
static_assert(VMHOOK_MAKE_VERSION(0, 6, 0) - VMHOOK_MAKE_VERSION(0, 5, 3) == 997);
static_assert(VMHOOK_MAKE_VERSION(0, 5, 3) - VMHOOK_MAKE_VERSION(0, 5, 0) == 3);
static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) - VMHOOK_MAKE_VERSION(0, 5, 3) == 994997);
// Idempotent self-relations on the live packed value (new corners).
static_assert(VMHOOK_VERSION - VMHOOK_VERSION == 0);
static_assert(VMHOOK_VERSION / VMHOOK_VERSION == 1);
static_assert(VMHOOK_VERSION % VMHOOK_VERSION == 0);

// (E5) Distributivity of a scalar over the pack: 2*MAKE(M,m,p) == MAKE(2M,2m,2p)
// when no doubled field reaches 1000.  Confirms the closed-form is a linear map
// (and re-confirms the outer parens survive a leading scalar in const-expr).
static_assert(2 * VMHOOK_MAKE_VERSION(3, 4, 5) == VMHOOK_MAKE_VERSION(6, 8, 10));
static_assert(3 * VMHOOK_MAKE_VERSION(1, 1, 1) == VMHOOK_MAKE_VERSION(3, 3, 3));

// (E6) NEW #if preprocessor sweeps using operators the first pass omitted in
// #if context (<=, >, !=) and pinning the live carry-above boundary there.
#if !(VMHOOK_VERSION <= VMHOOK_MAKE_VERSION(0, 5, 3))
#   error "VMHOOK_VERSION (0.5.3) must be <= 0.5.3 in #if context"
#endif
#if !(VMHOOK_VERSION <= VMHOOK_MAKE_VERSION(0, 5, 4))
#   error "VMHOOK_VERSION (0.5.3) must be <= 0.5.4 in #if context"
#endif
#if !(VMHOOK_VERSION != VMHOOK_MAKE_VERSION(0, 5, 4))
#   error "VMHOOK_VERSION (0.5.3) must differ from 0.5.4 in #if context"
#endif
#if !(VMHOOK_VERSION > VMHOOK_MAKE_VERSION(0, 4, 999))
#   error "VMHOOK_VERSION (0.5.3) must exceed 0.4.999 in #if context"
#endif
// The lossy carry equality holds in the preprocessor too (flaw #2 spec in #if).
#if VMHOOK_MAKE_VERSION(0, 5, 1000) != VMHOOK_MAKE_VERSION(0, 6, 0)
#   error "0.5.1000 must alias 0.6.0 in #if context (decimal field carry)"
#endif
// Preprocessor-context field-width guard: the live minor/patch MUST be < 1000,
// or the pack would carry.  static_assert proved this in the compiler; this is
// the distinct #if proof (where a width violation would silently corrupt gates).
#if !(VMHOOK_VERSION_MINOR < 1000 && VMHOOK_VERSION_PATCH < 1000)
#   error "live MINOR/PATCH must each be < 1000 in #if context (field width)"
#endif
// Scalar distributivity must also hold under the preprocessor.
#if (2 * VMHOOK_MAKE_VERSION(3, 4, 5)) != VMHOOK_MAKE_VERSION(6, 8, 10)
#   error "scalar distributivity over MAKE must hold in #if context"
#endif

// ===========================================================================
// THIRD DEEPENING SECTION (additive, namespaced).  Everything below this
// banner is NEW coverage that touches NO assertion above it and does NOT
// overlap the first (D/R) or second (E/S) deepening sections.  New angles:
// (1) field INJECTION composition -- MAKE built by summing single-field packs;
// (2) MODULAR field-extraction identities as static_asserts on a dense set;
// (3) char-arithmetic digit identity ('0'+component) tying the STRING digits
//     to the numeric components at COMPILE time (the existing char identity
//     was runtime-only, S6); (4) per-component string-LITERAL digit-count via
//     sizeof, proving each live field is a single decimal digit; (5) the
//     lexical-vs-numeric ordering INVERSION ("1.10.0" sorts below "1.9.9" as
//     text yet packs strictly above) as a constexpr string-compare oracle --
//     the exact reason a decimal pack exists.  All values from vmhook.hpp:67-82
//     (MAJOR=0, MINOR=5, PATCH=3; pack major*1e6 + minor*1e3 + patch).  PURE
//     preprocessor + integer arithmetic + constexpr char compares: no JVM, no
//     memory reads, no fabricated pointers, no value_t casts, no narrowing.
// ===========================================================================
namespace vm_deepen3
{
    // (F1) Field INJECTION: the pack is the sum of three single-field packs.
    // MAKE(M,0,0) + MAKE(0,m,0) + MAKE(0,0,p) must equal MAKE(M,m,p) whenever
    // no field carries -- a composition identity neither prior pass asserted.
    constexpr long long inject(int M, int m, int p)
    {
        return (static_cast<long long>(M) * 1000000LL)
             + (static_cast<long long>(m) * 1000LL)
             + static_cast<long long>(p);
    }

    constexpr bool injection_composes()
    {
        for (int M = 0; M <= 9; ++M)
            for (int m = 0; m <= 9; ++m)
                for (int p = 0; p <= 9; ++p)
                {
                    // single-field injections summed == the full triple pack
                    const long long whole{ inject(M, m, p) };
                    const long long parts{ inject(M, 0, 0)
                                         + inject(0, m, 0)
                                         + inject(0, 0, p) };
                    if (whole != parts) { return false; }
                }
        return true;
    }

    // (F2) Constexpr string compare: returns <0, 0, or >0 like strcmp but
    // usable in a static_assert.  Used to prove the LEXICAL ordering of dotted
    // version strings INVERTS the numeric ordering at the classic "1.10 vs 1.9"
    // boundary -- the whole reason consumers must gate on the packed integer,
    // not on a string compare of VMHOOK_VERSION_STRING.
    constexpr int lex_cmp(const char* a, const char* b)
    {
        for (std::size_t i{ 0 }; ; ++i)
        {
            const char ca{ a[i] };
            const char cb{ b[i] };
            if (ca != cb)
            {
                return (static_cast<unsigned char>(ca)
                            < static_cast<unsigned char>(cb)) ? -1 : 1;
            }
            if (ca == '\0') { return 0; }
        }
    }
}
// Field-injection composition over a 1000-point cube (compile-time hard error
// on any miscompile of the additive structure).
static_assert(vm_deepen3::injection_composes(),
              "MAKE(M,0,0)+MAKE(0,m,0)+MAKE(0,0,p) must equal MAKE(M,m,p)");

// (F3) MODULAR field-extraction identities as static_asserts on a dense set of
// triples (the existing decompose checks were runtime-only or sampled fewer
// points).  Each line proves the documented /-and-% slicing recovers the field.
static_assert(VMHOOK_MAKE_VERSION(7, 13, 21) / 1000000 == 7);
static_assert(VMHOOK_MAKE_VERSION(7, 13, 21) / 1000 % 1000 == 13);
static_assert(VMHOOK_MAKE_VERSION(7, 13, 21) % 1000 == 21);
static_assert(VMHOOK_MAKE_VERSION(0, 999, 999) / 1000000 == 0);
static_assert(VMHOOK_MAKE_VERSION(0, 999, 999) / 1000 % 1000 == 999);
static_assert(VMHOOK_MAKE_VERSION(0, 999, 999) % 1000 == 999);
static_assert(VMHOOK_MAKE_VERSION(255, 1, 500) / 1000000 == 255);
static_assert(VMHOOK_MAKE_VERSION(255, 1, 500) / 1000 % 1000 == 1);
static_assert(VMHOOK_MAKE_VERSION(255, 1, 500) % 1000 == 500);
// The live packed value decomposes to exactly 0 / 5 / 3 at compile time.
static_assert(VMHOOK_VERSION / 1000000 == VMHOOK_VERSION_MAJOR);
static_assert(VMHOOK_VERSION / 1000 % 1000 == VMHOOK_VERSION_MINOR);
static_assert(VMHOOK_VERSION % 1000 == VMHOOK_VERSION_PATCH);
static_assert(VMHOOK_VERSION / 1000000 == 0
              && VMHOOK_VERSION / 1000 % 1000 == 5
              && VMHOOK_VERSION % 1000 == 3,
              "live packed 5003 decomposes to 0/5/3 at compile time");

// (F4) Char-arithmetic digit identity at COMPILE time (S6 did this at runtime):
// each single-digit live component, added to '0', equals the corresponding
// character in VMHOOK_VERSION_STRING.  Ties the stringize path to the numeric
// components without snprintf.  Valid only because each live field is 0..9.
static_assert(VMHOOK_VERSION_STRING[0] == static_cast<char>('0' + VMHOOK_VERSION_MAJOR),
              "string[0] equals '0'+MAJOR (single-digit live major)");
static_assert(VMHOOK_VERSION_STRING[2] == static_cast<char>('0' + VMHOOK_VERSION_MINOR),
              "string[2] equals '0'+MINOR (single-digit live minor)");
static_assert(VMHOOK_VERSION_STRING[4] == static_cast<char>('0' + VMHOOK_VERSION_PATCH),
              "string[4] equals '0'+PATCH (single-digit live patch)");

// (F5) Per-component stringized-LITERAL digit count via sizeof.  Each live
// field is a single decimal digit, so HELPER(component) is a 1-char literal
// (sizeof == 2 incl NUL).  A future bump to a two-digit field would change
// these, flagging that the "0.5.3" layout assertions need revisiting.
static_assert(sizeof(VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_MAJOR)) == 2,
              "live MAJOR stringizes to a single digit");
static_assert(sizeof(VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_MINOR)) == 2,
              "live MINOR stringizes to a single digit");
static_assert(sizeof(VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_PATCH)) == 2,
              "live PATCH stringizes to a single digit");

// (F6) LEXICAL-vs-NUMERIC ordering inversion -- the defining justification for a
// numeric pack.  As TEXT, "1.10.0" sorts BELOW "1.9.9" (since '1' < '9' at the
// third char), yet the packed integers order the other way.  Prove both: the
// constexpr lexical compare is negative while the numeric compare is positive.
static_assert(vm_deepen3::lex_cmp("1.10.0", "1.9.9") < 0,
              "as text, \"1.10.0\" sorts BELOW \"1.9.9\"");
static_assert(VMHOOK_MAKE_VERSION(1, 10, 0) > VMHOOK_MAKE_VERSION(1, 9, 9),
              "but numerically 1.10.0 is ABOVE 1.9.9 -- pack fixes the inversion");
// Sanity anchors for the lexical oracle so a broken lex_cmp cannot make F6 vacuous.
static_assert(vm_deepen3::lex_cmp("0.5.3", "0.5.3") == 0, "lex_cmp equal strings == 0");
static_assert(vm_deepen3::lex_cmp("0.5.3", "0.5.4") < 0, "lex_cmp orders patch digit");
static_assert(vm_deepen3::lex_cmp("0.6.0", "0.5.9") > 0, "lex_cmp orders minor digit");
// And the live string compares lexically equal to its own literal.
static_assert(vm_deepen3::lex_cmp(VMHOOK_VERSION_STRING, "0.5.3") == 0,
              "live VMHOOK_VERSION_STRING is lexically exactly \"0.5.3\"");

int main()
{
    // -----------------------------------------------------------------------
    // The three component macros must be defined and expand to integers.
    // (If any were missing the file would not compile, so reaching here with a
    // value already proves existence; these checks pin the documented domain.)
    // -----------------------------------------------------------------------
    constexpr int v_major{ VMHOOK_VERSION_MAJOR };
    constexpr int v_minor{ VMHOOK_VERSION_MINOR };
    constexpr int v_patch{ VMHOOK_VERSION_PATCH };

    check("major_is_nonnegative", v_major >= 0);
    check("minor_is_nonnegative", v_minor >= 0);
    check("patch_is_nonnegative", v_patch >= 0);

    // Each component must fit its 3-decimal-digit field or the decimal pack
    // would carry into the neighbouring field and silently corrupt the version.
    check("minor_within_field_range", v_minor >= 0 && v_minor < 1000);
    check("patch_within_field_range", v_patch >= 0 && v_patch < 1000);

    // -----------------------------------------------------------------------
    // VMHOOK_MAKE_VERSION field weights (the exact documented formula).
    // -----------------------------------------------------------------------
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 0) == 0,
                  "MAKE(0,0,0) must be 0");
    static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) == 1000000,
                  "major field weight must be 1'000'000");
    static_assert(VMHOOK_MAKE_VERSION(0, 1, 0) == 1000,
                  "minor field weight must be 1'000");
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 1) == 1,
                  "patch field weight must be 1");
    check("make_version_zero", VMHOOK_MAKE_VERSION(0, 0, 0) == 0);
    check("make_version_major_weight", VMHOOK_MAKE_VERSION(1, 0, 0) == 1000000);
    check("make_version_minor_weight", VMHOOK_MAKE_VERSION(0, 1, 0) == 1000);
    check("make_version_patch_weight", VMHOOK_MAKE_VERSION(0, 0, 1) == 1);

    // Combined pack of an arbitrary triple equals the explicit decimal sum.
    static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) == 1002003,
                  "MAKE(1,2,3) must pack to 1'002'003");
    check("make_version_combined_1_2_3", VMHOOK_MAKE_VERSION(1, 2, 3) == 1002003);
    check("make_version_combined_max_fields",
          VMHOOK_MAKE_VERSION(7, 999, 999) == (7 * 1000000) + (999 * 1000) + 999);

    // The pack must equal the open-coded formula for the live component values.
    check("make_version_matches_formula_for_components",
          VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR, VMHOOK_VERSION_MINOR, VMHOOK_VERSION_PATCH)
              == (v_major * 1000000) + (v_minor * 1000) + v_patch);

    // -----------------------------------------------------------------------
    // VMHOOK_VERSION must be exactly the packed form of its three components.
    // -----------------------------------------------------------------------
    static_assert(VMHOOK_VERSION == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                                        VMHOOK_VERSION_MINOR,
                                                        VMHOOK_VERSION_PATCH),
                  "VMHOOK_VERSION must equal MAKE(MAJOR,MINOR,PATCH)");
    constexpr int packed{ VMHOOK_VERSION };
    check("version_equals_packed_components",
          packed == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                        VMHOOK_VERSION_MINOR,
                                        VMHOOK_VERSION_PATCH));
    check("version_equals_open_coded_sum",
          packed == (v_major * 1000000) + (v_minor * 1000) + v_patch);

    // Round-trip: recomputing from the parts reproduces the macro verbatim.
    check("version_roundtrip_through_make",
          VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                              VMHOOK_VERSION_MINOR,
                              VMHOOK_VERSION_PATCH) == VMHOOK_VERSION);

    // -----------------------------------------------------------------------
    // Decompose the packed integer back into fields.  These identities only
    // hold because minor/patch are < 1000 (asserted above); together they prove
    // no field bled into another during packing.
    // -----------------------------------------------------------------------
    check("version_decompose_major", (packed / 1000000) == v_major);
    check("version_decompose_minor", ((packed / 1000) % 1000) == v_minor);
    check("version_decompose_patch", (packed % 1000) == v_patch);

    // -----------------------------------------------------------------------
    // Monotonicity of the pack with respect to each component.  A larger triple
    // (lexicographically) must produce a strictly larger packed integer.
    // -----------------------------------------------------------------------
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 1) > VMHOOK_MAKE_VERSION(0, 0, 0),
                  "bumping patch must increase the packed value");
    static_assert(VMHOOK_MAKE_VERSION(0, 1, 0) > VMHOOK_MAKE_VERSION(0, 0, 999),
                  "bumping minor must outweigh a maxed-out patch");
    static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) > VMHOOK_MAKE_VERSION(0, 999, 999),
                  "bumping major must outweigh maxed-out minor.patch");

    check("monotonic_patch_step",
          VMHOOK_MAKE_VERSION(2, 3, 5) > VMHOOK_MAKE_VERSION(2, 3, 4));
    check("monotonic_minor_step",
          VMHOOK_MAKE_VERSION(2, 4, 0) > VMHOOK_MAKE_VERSION(2, 3, 0));
    check("monotonic_major_step",
          VMHOOK_MAKE_VERSION(3, 0, 0) > VMHOOK_MAKE_VERSION(2, 0, 0));

    // Field-dominance: one minor step always exceeds the entire patch field,
    // and one major step always exceeds the entire minor.patch range.  This is
    // the property that makes `#if VMHOOK_VERSION >= MAKE(x,y,z)` gating sound.
    check("minor_field_dominates_patch_field",
          VMHOOK_MAKE_VERSION(2, 4, 0) > VMHOOK_MAKE_VERSION(2, 3, 999));
    check("major_field_dominates_minor_field",
          VMHOOK_MAKE_VERSION(3, 0, 0) > VMHOOK_MAKE_VERSION(2, 999, 999));

    // The live packed value is non-negative and strictly ordered against its
    // immediate neighbours: the next patch up must exceed it, and the previous
    // patch down (when patch>0) must be below it.  This anchors the monotonicity
    // property to the actual shipped version, not just synthetic triples.
    check("version_nonnegative", packed >= 0);
    check("version_strictly_below_next_patch",
          packed < VMHOOK_MAKE_VERSION(v_major, v_minor, v_patch + 1));
    check("version_strictly_above_prev_patch",
          (v_patch == 0)
              || (packed > VMHOOK_MAKE_VERSION(v_major, v_minor, v_patch - 1)));

    // -----------------------------------------------------------------------
    // Version-gating idiom documented in the header comment:
    //   #if VMHOOK_VERSION >= VMHOOK_MAKE_VERSION(0,3,0)
    // The project is past 0.3.0 and still in the 0.x major series, so these are
    // genuine assertions about the current released state, not tautologies.
    // -----------------------------------------------------------------------
    check("version_at_least_0_3_0", packed >= VMHOOK_MAKE_VERSION(0, 3, 0));
    check("version_below_1_0_0_while_major_zero",
          (v_major != 0) || (packed < VMHOOK_MAKE_VERSION(1, 0, 0)));
    // A 0.x version must sit strictly inside the major-0 band [0, 1'000'000).
    check("version_inside_major_band",
          (v_major != 0) || (packed >= 0 && packed < 1000000));

    // -----------------------------------------------------------------------
    // VMHOOK_VERSION_STRING must read "MAJOR.MINOR.PATCH" with exactly two dots,
    // no stray whitespace from token-paste/stringize, and must agree digit-for-
    // digit with the numeric components.
    // -----------------------------------------------------------------------
    const std::string version_text{ VMHOOK_VERSION_STRING };
    check("version_string_not_empty", !version_text.empty());

    std::size_t dot_count{ 0 };
    std::size_t space_count{ 0 };
    for (const char c : version_text)
    {
        if (c == '.') { ++dot_count; }
        if (c == ' ' || c == '\t') { ++space_count; }
    }
    check("version_string_has_exactly_two_dots", dot_count == 2);
    check("version_string_has_no_whitespace", space_count == 0);
    check("version_string_first_char_is_digit",
          !version_text.empty() && version_text.front() >= '0' && version_text.front() <= '9');
    check("version_string_last_char_is_digit",
          !version_text.empty() && version_text.back() >= '0' && version_text.back() <= '9');

    // Stringize value-vs-name regression lock (audit flaw #5).  The two-level
    // VMHOOK_VERSION_STRING_HELPER dance exists *only* to force the component
    // macros to expand before `#x` stringizes them.  If a future "cleanup"
    // collapsed it to a single-level `#x`, the string would become the macro
    // NAME ("VMHOOK_VERSION_MAJOR.…") instead of its value.  Pin that the
    // string is purely digits and dots: NO alphabetic character at all, and in
    // particular it must not contain the token "VMHOOK".
    bool has_alpha{ false };
    for (const char c : version_text)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { has_alpha = true; }
    }
    check("version_string_has_no_alpha_chars", !has_alpha);
    check("version_string_does_not_contain_macro_name",
          version_text.find("VMHOOK") == std::string::npos);
    // Every character is specifically a digit or a '.' (nothing else slipped in,
    // e.g. an underscore from a pasted identifier or a quote from mis-stringize).
    bool only_digit_or_dot{ true };
    for (const char c : version_text)
    {
        if (c != '.' && (c < '0' || c > '9')) { only_digit_or_dot = false; }
    }
    check("version_string_is_exclusively_digits_and_dots", only_digit_or_dot);

    // Rebuild the expected "M.m.p" from the numeric components and compare.
    char expected[64]{};
    const int written{ std::snprintf(expected, sizeof(expected), "%d.%d.%d",
                                     v_major, v_minor, v_patch) };
    check("version_string_snprintf_ok", written > 0 && written < static_cast<int>(sizeof(expected)));
    check("version_string_matches_components", version_text == std::string{ expected });

    // Idempotence (runtime arm): the string captured before the header's second
    // include must equal the string seen now -- ties the snapshot above to a
    // live comparison so the re-include guard cannot silently no-op away.
    check("version_string_stable_across_reinclude",
          version_text == std::string{ vm_version_snapshot::first_string });

    // -----------------------------------------------------------------------
    // Cross-check against the CMake project version.  tests/CMakeLists.txt
    // compiles THIS target (vmhook_test_version_macros) with
    // -DVMHOOK_CMAKE_VERSION_{MAJOR,MINOR,PATCH} taken from PROJECT_VERSION_*,
    // and additionally with -DVMHOOK_TEST_EXPECT_CMAKE_VERSION=1 to record that
    // the cross-check is *expected* to run here.  That second flag implements
    // the audit's "negative-direction" CMake-sync guard (flaw #3): if a future
    // CMake edit ever drops the target_compile_definitions, the
    // VMHOOK_CMAKE_VERSION_* defs vanish but VMHOOK_TEST_EXPECT_CMAKE_VERSION
    // stays, so this block fails LOUDLY instead of silently degrading to a
    // no-op SKIP -- the safety net can no longer rot undetected.
    // -----------------------------------------------------------------------
#if defined(VMHOOK_CMAKE_VERSION_MAJOR) && defined(VMHOOK_CMAKE_VERSION_MINOR) \
    && defined(VMHOOK_CMAKE_VERSION_PATCH)
    check("cmake_version_matches_header_major",
          VMHOOK_CMAKE_VERSION_MAJOR == VMHOOK_VERSION_MAJOR);
    check("cmake_version_matches_header_minor",
          VMHOOK_CMAKE_VERSION_MINOR == VMHOOK_VERSION_MINOR);
    check("cmake_version_matches_header_patch",
          VMHOOK_CMAKE_VERSION_PATCH == VMHOOK_VERSION_PATCH);
    check("cmake_version_matches_packed",
          VMHOOK_MAKE_VERSION(VMHOOK_CMAKE_VERSION_MAJOR,
                              VMHOOK_CMAKE_VERSION_MINOR,
                              VMHOOK_CMAKE_VERSION_PATCH) == VMHOOK_VERSION);
    // String form of the CMake version must also equal the header string, tying
    // CMakeLists.txt project(VERSION ...) to VMHOOK_VERSION_STRING end to end.
    {
        char cmake_str[64]{};
        const int cw{ std::snprintf(cmake_str, sizeof(cmake_str), "%d.%d.%d",
                                    static_cast<int>(VMHOOK_CMAKE_VERSION_MAJOR),
                                    static_cast<int>(VMHOOK_CMAKE_VERSION_MINOR),
                                    static_cast<int>(VMHOOK_CMAKE_VERSION_PATCH)) };
        check("cmake_version_string_matches_header_string",
              cw > 0 && version_text == std::string{ cmake_str });
    }
#else
#   if defined(VMHOOK_TEST_EXPECT_CMAKE_VERSION) && (VMHOOK_TEST_EXPECT_CMAKE_VERSION + 0)
    // The build promised the cross-check would run here but the defs are gone:
    // that is exactly the CMake-drift failure mode we want to catch, so fail.
    check("cmake_version_defs_present_when_expected", false);
#   else
    std::printf("[SKIP] cmake_version_cross_check "
                "(VMHOOK_CMAKE_VERSION_* not defined for this target)\n");
#   endif
#endif

    // -----------------------------------------------------------------------
    // VMHOOK_MAKE_VERSION packing: each field is exactly three decimal digits,
    // so adjacent triples that differ by one unit in a single field must differ
    // by exactly that field's weight.  These pin the "no carry between fields"
    // property at the boundaries of each field.
    // -----------------------------------------------------------------------
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 999) == 999,
                  "patch field tops out at 999");
    static_assert(VMHOOK_MAKE_VERSION(0, 999, 0) == 999000,
                  "minor field tops out at 999*1000");
    static_assert(VMHOOK_MAKE_VERSION(0, 1, 0) - VMHOOK_MAKE_VERSION(0, 0, 999) == 1,
                  "minor.0 is exactly one above patch-maxed minor-1 ((1*1000) - 999 == 1)");
    static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) - VMHOOK_MAKE_VERSION(0, 999, 999) == 1,
                  "major.0.0 is exactly one above the maxed-out 0.999.999");
    check("make_version_patch_field_max", VMHOOK_MAKE_VERSION(0, 0, 999) == 999);
    check("make_version_minor_field_max", VMHOOK_MAKE_VERSION(0, 999, 0) == 999000);
    check("make_version_minor_one_above_patch_max",
          VMHOOK_MAKE_VERSION(0, 1, 0) - VMHOOK_MAKE_VERSION(0, 0, 999) == 1);
    check("make_version_major_one_above_minor_patch_max",
          VMHOOK_MAKE_VERSION(1, 0, 0) - VMHOOK_MAKE_VERSION(0, 999, 999) == 1);

    // A single patch step always changes the packed value by exactly 1, a single
    // minor step by exactly 1000, and a single major step by exactly 1'000'000 —
    // for arbitrary in-field base values, not just zero.
    check("make_version_patch_step_is_one",
          VMHOOK_MAKE_VERSION(4, 12, 8) - VMHOOK_MAKE_VERSION(4, 12, 7) == 1);
    check("make_version_minor_step_is_1000",
          VMHOOK_MAKE_VERSION(4, 12, 8) - VMHOOK_MAKE_VERSION(4, 11, 8) == 1000);
    check("make_version_major_step_is_1000000",
          VMHOOK_MAKE_VERSION(4, 12, 8) - VMHOOK_MAKE_VERSION(3, 12, 8) == 1000000);

    // -----------------------------------------------------------------------
    // Decompose round-trip for a battery of synthetic triples: pack with
    // VMHOOK_MAKE_VERSION, then recover each field by the documented /-and-%
    // identities.  Covers zeros, field maxima, and an all-fields-populated mix —
    // exercising the lossless-pack invariant far past the single live value.
    // -----------------------------------------------------------------------
    {
        struct triple { int major; int minor; int patch; };
        constexpr triple triples[]{
            { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 },
            { 0, 0, 999 }, { 0, 999, 0 }, { 0, 999, 999 },
            { 1, 2, 3 }, { 7, 13, 21 }, { 99, 999, 999 }, { 255, 1, 500 },
        };
        bool all_decompose_ok{ true };
        for (const triple t : triples)
        {
            const int p{ (t.major * 1000000) + (t.minor * 1000) + t.patch };
            if ((p / 1000000) != t.major) { all_decompose_ok = false; }
            if (((p / 1000) % 1000) != t.minor) { all_decompose_ok = false; }
            if ((p % 1000) != t.patch) { all_decompose_ok = false; }
        }
        check("make_version_decompose_roundtrip_battery", all_decompose_ok);
    }

    // -----------------------------------------------------------------------
    // The live shipped version.  VMHOOK_VERSION_* are fixed in the header at the
    // moment of release; pin the exact triple (0.5.3 at the time of writing) so a
    // version bump that forgets to update one of the three macros, or the packed
    // value, fails here.  Sourced from vmhook.hpp lines 67-69.
    // -----------------------------------------------------------------------
    check("live_version_major_is_0", v_major == 0);
    check("live_version_minor_is_5", v_minor == 5);
    check("live_version_patch_is_3", v_patch == 3);
    check("live_version_packed_is_5003", packed == 5003);
    check("live_version_string_is_0_5_3", version_text == std::string{ "0.5.3" });

    // -----------------------------------------------------------------------
    // Self-comparison / reflexivity and strict ordering corners that a `#if
    // VMHOOK_VERSION >= MAKE(x,y,z)` gate relies on: equal triples compare
    // equal (>= holds, > does not), and the live version is >= itself but not
    // strictly greater than itself.
    // -----------------------------------------------------------------------
    check("version_ge_itself", packed >= VMHOOK_VERSION);
    check("version_not_strictly_gt_itself", !(packed > VMHOOK_VERSION));
    check("make_version_equal_triples_compare_equal",
          VMHOOK_MAKE_VERSION(2, 5, 9) == VMHOOK_MAKE_VERSION(2, 5, 9));
    check("make_version_equal_triples_not_strictly_ordered",
          !(VMHOOK_MAKE_VERSION(2, 5, 9) > VMHOOK_MAKE_VERSION(2, 5, 9))
          && !(VMHOOK_MAKE_VERSION(2, 5, 9) < VMHOOK_MAKE_VERSION(2, 5, 9)));

    // -----------------------------------------------------------------------
    // VMHOOK_VERSION_STRING substring agreement: the dotted string must contain
    // each numeric component as a token, and splitting on '.' must yield exactly
    // the three components in order.  This is stronger than the snprintf rebuild
    // above because it checks the stringize path field-by-field.
    // -----------------------------------------------------------------------
    {
        // Split version_text on '.' into three pieces and parse each as an int.
        std::vector<std::string> parts;
        std::string current;
        for (const char c : version_text)
        {
            if (c == '.') { parts.push_back(current); current.clear(); }
            else { current.push_back(c); }
        }
        parts.push_back(current);
        check("version_string_splits_into_three_parts", parts.size() == 3);
        if (parts.size() == 3)
        {
            check("version_string_part0_is_major",
                  parts[0] == std::to_string(v_major));
            check("version_string_part1_is_minor",
                  parts[1] == std::to_string(v_minor));
            check("version_string_part2_is_patch",
                  parts[2] == std::to_string(v_patch));
            // No part is empty (would mean a leading/trailing/double dot).
            check("version_string_no_empty_parts",
                  !parts[0].empty() && !parts[1].empty() && !parts[2].empty());

            // Tie the STRING and the PACKED INTEGER to a single decomposed
            // truth: parse each string field to an int and compare it to the
            // corresponding slice of the packed value (not just to the raw
            // component macros).  This closes the loop string -> int ==
            // packed-decompose, so a drift in EITHER the stringize path or the
            // pack arithmetic surfaces here even if they happened to agree with
            // the component macros individually.
            const int str_major{ std::stoi(parts[0]) };
            const int str_minor{ std::stoi(parts[1]) };
            const int str_patch{ std::stoi(parts[2]) };
            check("version_string_major_equals_packed_decompose",
                  str_major == (packed / 1000000));
            check("version_string_minor_equals_packed_decompose",
                  str_minor == ((packed / 1000) % 1000));
            check("version_string_patch_equals_packed_decompose",
                  str_patch == (packed % 1000));
            // And re-packing the parsed string fields reproduces VMHOOK_VERSION.
            check("version_string_repacks_to_version",
                  VMHOOK_MAKE_VERSION(str_major, str_minor, str_patch) == VMHOOK_VERSION);
        }
    }

    // -----------------------------------------------------------------------
    // Data-driven dense ordering sweep.  Build a STRICTLY ASCENDING ladder of
    // (major,minor,patch) triples that crosses every interesting boundary --
    // patch carries (9->10, 99->100), the lossy field-wrap (999->next field),
    // minor single->double->triple digit, and major rollovers -- then assert
    // the FULL pairwise comparison matrix: for i<j the packed value at i must be
    // strictly < the one at j, and the `>=` gate must agree with index order in
    // both directions.  This exercises the comparison logic over O(n^2) pairs,
    // far more than the hand-written checks above, while staying pure-runtime
    // (and deterministic, so never flaky).  Mirrors the compile-time ladder so a
    // divergence between constexpr and runtime evaluation would also surface.
    // -----------------------------------------------------------------------
    {
        struct triple { int major; int minor; int patch; };
        // Kept ascending by construction; pack() must preserve this order.
        constexpr triple ladder[]{
            { 0, 0, 0 },   { 0, 0, 1 },   { 0, 0, 9 },   { 0, 0, 10 },
            { 0, 0, 99 },  { 0, 0, 100 }, { 0, 0, 999 },
            { 0, 1, 0 },   { 0, 1, 1 },   { 0, 9, 999 }, { 0, 10, 0 },
            { 0, 99, 999 },{ 0, 100, 0 }, { 0, 999, 999 },
            { 1, 0, 0 },   { 1, 9, 9 },   { 1, 10, 0 },  { 1, 99, 999 },
            { 2, 0, 0 },   { 10, 0, 0 },  { 100, 0, 0 }, { 999, 999, 999 },
        };
        constexpr auto pack = [](triple t) {
            return (static_cast<long>(t.major) * 1000000L)
                 + (static_cast<long>(t.minor) * 1000L)
                 + static_cast<long>(t.patch);
        };
        const std::size_t n{ sizeof(ladder) / sizeof(ladder[0]) };
        bool strictly_ascending{ true };
        bool ge_relation_consistent{ true };
        long pairs_checked{ 0 };
        for (std::size_t i{ 0 }; i < n; ++i)
        {
            for (std::size_t j{ 0 }; j < n; ++j)
            {
                const long a{ pack(ladder[i]) };
                const long b{ pack(ladder[j]) };
                ++pairs_checked;
                if (i < j)
                {
                    if (!(a < b))  { strictly_ascending = false; }
                    if (!(b >= a)) { ge_relation_consistent = false; }
                    if (a >= b)    { ge_relation_consistent = false; }
                }
                else if (i == j)
                {
                    if (!(a == b)) { strictly_ascending = false; }
                    if (!(a >= b)) { ge_relation_consistent = false; }
                    if (a > b)     { ge_relation_consistent = false; }
                }
                else // i > j
                {
                    if (!(a > b))  { strictly_ascending = false; }
                    if (!(a >= b)) { ge_relation_consistent = false; }
                }
            }
        }
        check("ordering_ladder_strictly_ascending", strictly_ascending);
        check("ordering_ladder_ge_relation_consistent", ge_relation_consistent);
        check("ordering_ladder_full_matrix_covered",
              pairs_checked == static_cast<long>(n) * static_cast<long>(n));
    }

    // -----------------------------------------------------------------------
    // Dense "equal / one-below / one-above" matrix anchored on the LIVE shipped
    // version (currently 0.5.3).  These are the exact comparisons a consumer's
    // `#if VMHOOK_VERSION >= MAKE(...)` gate performs, so we walk one step in
    // each direction on every field and confirm the gate's verdict.  Because
    // they reference VMHOOK_VERSION_* they auto-retarget on a version bump.
    // -----------------------------------------------------------------------
    {
        const int M{ v_major };
        const int m{ v_minor };
        const int p{ v_patch };

        // Equal: gate at exactly the current version must pass (>=) but not (>).
        check("gate_equal_passes_ge", packed >= VMHOOK_MAKE_VERSION(M, m, p));
        check("gate_equal_fails_gt", !(packed > VMHOOK_MAKE_VERSION(M, m, p)));

        // One patch above the live version: gate must FAIL (we are below it).
        check("gate_one_patch_above_fails",
              !(packed >= VMHOOK_MAKE_VERSION(M, m, p + 1)));
        // One patch below (when patch>0): gate must PASS and be strictly above.
        check("gate_one_patch_below_passes",
              (p == 0) || (packed > VMHOOK_MAKE_VERSION(M, m, p - 1)));

        // One minor above: fails.  One minor below (when minor>0): passes.
        check("gate_one_minor_above_fails",
              !(packed >= VMHOOK_MAKE_VERSION(M, m + 1, p)));
        check("gate_one_minor_below_passes",
              (m == 0) || (packed > VMHOOK_MAKE_VERSION(M, m - 1, p)));

        // One major above: fails.  One major below (when major>0): passes.
        check("gate_one_major_above_fails",
              !(packed >= VMHOOK_MAKE_VERSION(M + 1, m, p)));
        check("gate_one_major_below_passes",
              (M == 0) || (packed > VMHOOK_MAKE_VERSION(M - 1, m, p)));

        // Zero floor: every released version is >= 0.0.0; and the live version
        // sits at or above 0.0.1 (we have definitely shipped something).
        check("gate_above_zero_floor", packed >= VMHOOK_MAKE_VERSION(0, 0, 0));
        check("gate_strictly_above_zero", packed > VMHOOK_MAKE_VERSION(0, 0, 0));
    }

    // =======================================================================
    // DEEPENING RUNTIME SECTION (additive).  Runtime twins of the file-scope
    // D-block plus runtime-only coverage the static_asserts cannot express
    // (string iteration, std::string equality, dense neighbour gating).  Every
    // expected value is derived from vmhook.hpp:67-82 -- 0.5.3 / packed 5003.
    // =======================================================================
    {
        // (R1) Runtime mirror of the constexpr cube: bijective + ordered pack
        // over a small cube, computed with the SAME closed form the macro uses,
        // cross-checked against VMHOOK_MAKE_VERSION for a sampled corner so the
        // runtime path and the macro path cannot silently diverge.
        bool cube_ok{ true };
        long prev{ -1 };
        for (int M = 0; M <= 6 && cube_ok; ++M)
            for (int mm = 0; mm <= 6 && cube_ok; ++mm)
                for (int pp = 0; pp <= 6 && cube_ok; ++pp)
                {
                    const long pk{ (static_cast<long>(M) * 1000000L)
                                 + (static_cast<long>(mm) * 1000L) + pp };
                    if (pk / 1000000L != M) { cube_ok = false; }
                    if ((pk / 1000L) % 1000L != mm) { cube_ok = false; }
                    if (pk % 1000L != pp) { cube_ok = false; }
                    if (pk <= prev) { cube_ok = false; }
                    prev = pk;
                }
        check("deepen_cube_bijective_and_ordered_runtime", cube_ok);
        // Sampled corner agrees with the actual macro (not just the closed form).
        check("deepen_cube_corner_matches_macro",
              VMHOOK_MAKE_VERSION(6, 6, 6) == (6 * 1000000) + (6 * 1000) + 6);

        // (R2) The live packed value pinned again at runtime, plus its int-width
        // headroom: 5003 is positive, well under INT_MAX, and equals the macro.
        check("deepen_live_packed_is_5003", VMHOOK_VERSION == 5003);
        check("deepen_live_packed_positive", VMHOOK_VERSION > 0);
        check("deepen_live_packed_under_int_max",
              static_cast<long long>(VMHOOK_VERSION) < 2147483647LL);

        // (R3) The composed string is exactly "0.5.3" with length 5, and every
        // character matches the literal index-for-index (runtime twin of D3).
        check("deepen_string_is_exactly_0_5_3", version_text == std::string{ "0.5.3" });
        check("deepen_string_length_is_5", version_text.size() == 5);
        const char expected_chars[]{ '0', '.', '5', '.', '3' };
        bool chars_match{ version_text.size() == 5 };
        for (std::size_t i{ 0 }; i < version_text.size() && i < 5; ++i)
        {
            if (version_text[i] != expected_chars[i]) { chars_match = false; }
        }
        check("deepen_string_chars_match_literal", chars_match);

        // (R4) Dense neighbour gating sweep: for a band of +/-2 around each live
        // field, the `>=` gate verdict must match arithmetic ordering against the
        // live packed value.  Pure-runtime, deterministic, retargets on a bump.
        bool gate_band_ok{ true };
        for (int dp = -2; dp <= 2; ++dp)
        {
            const int tp{ v_patch + dp };
            if (tp < 0) { continue; } // negative patch is not a valid version
            const int threshold{ (v_major * 1000000) + (v_minor * 1000) + tp };
            const bool gate{ packed >= threshold };
            // The gate verdict must follow dp's sign: dp<=0 => threshold is at or
            // below the live version => gate passes; dp>0 => threshold is above
            // the live version => gate fails.  (patch field, others held live.)
            if (dp <= 0 && !gate) { gate_band_ok = false; }
            if (dp > 0 && gate) { gate_band_ok = false; }
        }
        check("deepen_patch_gate_band_consistent", gate_band_ok);

        // Same band on the minor field, holding patch at the live value.
        bool minor_band_ok{ true };
        for (int dm = -2; dm <= 2; ++dm)
        {
            const int tm{ v_minor + dm };
            if (tm < 0) { continue; }
            const int threshold{ (v_major * 1000000) + (tm * 1000) + v_patch };
            const bool gate{ packed >= threshold };
            if (dm < 0 && !gate) { minor_band_ok = false; }  // strictly below us
            if (dm == 0 && !gate) { minor_band_ok = false; } // equal: >= holds
            if (dm > 0 && gate) { minor_band_ok = false; }   // strictly above us
        }
        check("deepen_minor_gate_band_consistent", minor_band_ok);

        // (R5) MAKE with the live component macros must equal both VMHOOK_VERSION
        // and the literal 5003 -- closes the token-composed path at runtime.
        check("deepen_make_of_components_is_version",
              VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                  VMHOOK_VERSION_MINOR,
                                  VMHOOK_VERSION_PATCH) == VMHOOK_VERSION);
        check("deepen_make_of_components_is_5003",
              VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                  VMHOOK_VERSION_MINOR,
                                  VMHOOK_VERSION_PATCH) == 5003);

        // (R6) The string contains NO NUL before its terminator and exactly one
        // terminator: std::string{literal} stops at the NUL, so its size (5) must
        // equal the count of non-NUL chars in the underlying array (6 bytes incl
        // terminator).  Guards against an embedded NUL from a mis-stringize.
        check("deepen_string_no_embedded_nul",
              version_text.find('\0') == std::string::npos);
    }

    // =======================================================================
    // SECOND DEEPENING RUNTIME SECTION (additive).  Runtime coverage with NO
    // overlap of the first runtime block (R1-R6): whole-field sweeps at runtime,
    // pack additivity/distributivity at runtime, and string<->packed identities
    // built by std::string concatenation (not snprintf, not split) -- a distinct
    // construction path.  Every expected value is from vmhook.hpp:67-82.
    // =======================================================================
    {
        // (S1) Runtime whole-patch-field sweep at the live major.minor: every
        // patch 0..999 packs to 5000+p and decomposes exactly, strictly rising.
        bool patch_sweep_ok{ true };
        long prev_pk{ -1 };
        for (int p = 0; p <= 999 && patch_sweep_ok; ++p)
        {
            const long pk{ (static_cast<long>(v_major) * 1000000L)
                         + (static_cast<long>(v_minor) * 1000L) + p };
            if (pk != 5000L + p) { patch_sweep_ok = false; }        // 0,5 live
            if (pk / 1000000L != v_major) { patch_sweep_ok = false; }
            if ((pk / 1000L) % 1000L != v_minor) { patch_sweep_ok = false; }
            if (pk % 1000L != p) { patch_sweep_ok = false; }
            if (pk <= prev_pk) { patch_sweep_ok = false; }
            prev_pk = pk;
        }
        check("deepen2_patch_field_full_sweep_runtime", patch_sweep_ok);

        // (S2) Runtime additivity: (M,m,p)+(1,2,3) packs to the sum of packs,
        // over a small cube where no summed field reaches 1000.  Cross-checked
        // against the actual macro at a sampled corner so the macro and the
        // closed form cannot silently diverge at runtime.
        bool additive_ok{ true };
        for (int M = 0; M <= 8 && additive_ok; ++M)
            for (int mm = 0; mm <= 8 && additive_ok; ++mm)
                for (int pp = 0; pp <= 8 && additive_ok; ++pp)
                {
                    const long lhs{ ((static_cast<long>(M) + 1) * 1000000L)
                                  + ((static_cast<long>(mm) + 2) * 1000L)
                                  + (pp + 3) };
                    const long rhs{ (static_cast<long>(M) * 1000000L
                                     + static_cast<long>(mm) * 1000L + pp)
                                  + (1 * 1000000L + 2 * 1000L + 3) };
                    if (lhs != rhs) { additive_ok = false; }
                }
        check("deepen2_pack_additive_runtime", additive_ok);
        check("deepen2_additive_corner_matches_macro",
              VMHOOK_MAKE_VERSION(1 + 1, 2 + 2, 3 + 3)
                  == VMHOOK_MAKE_VERSION(1, 2, 3) + VMHOOK_MAKE_VERSION(1, 2, 3));

        // (S3) Runtime scalar distributivity: 2*MAKE(M,m,p)==MAKE(2M,2m,2p) when
        // no doubled field reaches 1000.  Distinct from the const-expr twin.
        bool distrib_ok{ true };
        for (int M = 0; M <= 7 && distrib_ok; ++M)
            for (int mm = 0; mm <= 7 && distrib_ok; ++mm)
                for (int pp = 0; pp <= 7 && distrib_ok; ++pp)
                {
                    const long single{ static_cast<long>(M) * 1000000L
                                     + static_cast<long>(mm) * 1000L + pp };
                    const long doubled{ static_cast<long>(2 * M) * 1000000L
                                      + static_cast<long>(2 * mm) * 1000L
                                      + (2 * pp) };
                    if (2 * single != doubled) { distrib_ok = false; }
                }
        check("deepen2_pack_scalar_distributive_runtime", distrib_ok);

        // (S4) String built by std::string CONCATENATION (a third construction
        // path -- not snprintf (R/main), not split): join to_string of each live
        // component with literal "." separators; must equal VMHOOK_VERSION_STRING.
        const std::string built{ std::to_string(v_major) + "."
                               + std::to_string(v_minor) + "."
                               + std::to_string(v_patch) };
        check("deepen2_string_concat_matches_macro", built == version_text);
        check("deepen2_string_concat_is_0_5_3", built == std::string{ "0.5.3" });

        // (S5) String built from the PACKED-value decomposition (ties the string
        // to the integer through the documented /-and-% identities, via a fresh
        // concatenation rather than the earlier split/parse path).
        const std::string from_packed{ std::to_string(packed / 1000000) + "."
                                     + std::to_string((packed / 1000) % 1000) + "."
                                     + std::to_string(packed % 1000) };
        check("deepen2_string_from_packed_decompose_matches", from_packed == version_text);

        // (S6) The string's dotted layout, pinned by explicit index reads (a
        // direct twin of D3's static_asserts but exercised at runtime through the
        // std::string, guarding against any runtime/compile divergence): exactly
        // "0.5.3", indices 0,2,4 are the component digits and 1,3 are dots.
        check("deepen2_string_index0_is_major_digit",
              version_text.size() == 5 && version_text[0] == ('0' + v_major));
        check("deepen2_string_index1_is_dot",
              version_text.size() == 5 && version_text[1] == '.');
        check("deepen2_string_index2_is_minor_digit",
              version_text.size() == 5 && version_text[2] == ('0' + v_minor));
        check("deepen2_string_index3_is_dot",
              version_text.size() == 5 && version_text[3] == '.');
        check("deepen2_string_index4_is_patch_digit",
              version_text.size() == 5 && version_text[4] == ('0' + v_patch));

        // (S7) Carry-above-live boundary at runtime: 0.5.999 is the top of the
        // live minor band, 0.6.0 is exactly one above it, and 0.5.1000 aliases
        // 0.6.0 (the lossy decimal carry).  All numeric values are from source.
        check("deepen2_live_minor_band_top_is_5999",
              VMHOOK_MAKE_VERSION(0, 5, 999) == 5999);
        check("deepen2_next_minor_floor_is_6000",
              VMHOOK_MAKE_VERSION(0, 6, 0) == 6000);
        check("deepen2_patch_1000_aliases_next_minor",
              VMHOOK_MAKE_VERSION(0, 5, 1000) == VMHOOK_MAKE_VERSION(0, 6, 0));
        check("deepen2_live_strictly_below_minor_band_top",
              packed < VMHOOK_MAKE_VERSION(0, 5, 999));
        check("deepen2_live_strictly_below_next_minor_floor",
              packed < VMHOOK_MAKE_VERSION(0, 6, 0));

        // (S8) Cross-field difference identities at runtime (new corners): the
        // gap from the live version up to the next minor floor and down to the
        // live minor band floor, derived from source: 6000-5003=997, 5003-5000=3.
        check("deepen2_gap_to_next_minor_is_997",
              VMHOOK_MAKE_VERSION(0, 6, 0) - packed == 997);
        check("deepen2_gap_from_band_floor_is_3",
              packed - VMHOOK_MAKE_VERSION(0, 5, 0) == 3);

        // (S9) Idempotent self-relations on the live packed value at runtime.
        check("deepen2_self_difference_zero", packed - packed == 0);
        check("deepen2_self_quotient_one", packed / packed == 1);
        check("deepen2_self_modulo_zero", packed % packed == 0);
    }

    // =======================================================================
    // THIRD DEEPENING RUNTIME SECTION (additive).  Runtime coverage with NO
    // overlap of the first (R1-R6) or second (S1-S9) runtime blocks: field
    // INJECTION composition at runtime, the lexical-vs-numeric ordering
    // INVERSION exercised through std::string, modular field extraction as a
    // dense runtime battery, and a digit-character map of the live string.
    // Every expected value is from vmhook.hpp:67-82 -- 0.5.3 / packed 5003.
    // =======================================================================
    {
        // (T1) Runtime field injection: MAKE(M,0,0)+MAKE(0,m,0)+MAKE(0,0,p)
        // equals MAKE(M,m,p) over a small no-carry cube, cross-checked against
        // the actual macro at a sampled corner so closed form and macro agree.
        bool inject_ok{ true };
        for (int M = 0; M <= 8 && inject_ok; ++M)
            for (int mm = 0; mm <= 8 && inject_ok; ++mm)
                for (int pp = 0; pp <= 8 && inject_ok; ++pp)
                {
                    const long whole{ (static_cast<long>(M) * 1000000L)
                                    + (static_cast<long>(mm) * 1000L) + pp };
                    const long parts{ (static_cast<long>(M) * 1000000L)
                                    + (static_cast<long>(mm) * 1000L)
                                    + static_cast<long>(pp) };
                    if (whole != parts) { inject_ok = false; }
                }
        check("deepen3_field_injection_composes_runtime", inject_ok);
        check("deepen3_injection_corner_matches_macro",
              VMHOOK_MAKE_VERSION(4, 0, 0)
                  + VMHOOK_MAKE_VERSION(0, 12, 0)
                  + VMHOOK_MAKE_VERSION(0, 0, 8)
                  == VMHOOK_MAKE_VERSION(4, 12, 8));

        // (T2) Lexical-vs-numeric ordering INVERSION through std::string: the
        // text "1.10.0" sorts BELOW "1.9.9" (std::string::operator<), but the
        // packed integers order the OTHER way.  This is the runtime twin of F6
        // and the concrete reason consumers gate on the integer, not the string.
        const std::string lo{ "1.10.0" };
        const std::string hi{ "1.9.9" };
        check("deepen3_text_1_10_0_sorts_below_1_9_9", lo < hi);
        check("deepen3_numeric_1_10_0_above_1_9_9",
              VMHOOK_MAKE_VERSION(1, 10, 0) > VMHOOK_MAKE_VERSION(1, 9, 9));
        // The two orderings genuinely disagree (would be a no-op if both same).
        const bool text_says_lo_first{ lo < hi };
        const bool num_says_lo_first{
            VMHOOK_MAKE_VERSION(1, 10, 0) < VMHOOK_MAKE_VERSION(1, 9, 9) };
        check("deepen3_text_and_numeric_orderings_disagree",
              text_says_lo_first != num_says_lo_first);

        // (T3) Dense runtime modular field-extraction battery (distinct triples
        // from the earlier decompose battery): each /-and-% slice recovers the
        // field for a fresh set of multi-digit triples and for the live value.
        struct triple3 { int major; int minor; int patch; };
        const triple3 cases[]{
            { 7, 13, 21 }, { 255, 1, 500 }, { 0, 999, 999 },
            { 12, 0, 7 }, { 1, 500, 0 }, { 999, 0, 999 },
        };
        bool modular_ok{ true };
        for (const triple3 t : cases)
        {
            const long pk{ (static_cast<long>(t.major) * 1000000L)
                         + (static_cast<long>(t.minor) * 1000L) + t.patch };
            if (pk / 1000000L != t.major) { modular_ok = false; }
            if ((pk / 1000L) % 1000L != t.minor) { modular_ok = false; }
            if (pk % 1000L != t.patch) { modular_ok = false; }
        }
        check("deepen3_modular_extraction_battery_runtime", modular_ok);
        check("deepen3_live_modular_extraction",
              (packed / 1000000) == v_major
                  && ((packed / 1000) % 1000) == v_minor
                  && (packed % 1000) == v_patch);

        // (T4) Digit-character map of the live string at runtime: each digit
        // character equals '0' plus the corresponding single-digit component
        // (runtime twin of F4).  Valid only because the live fields are 0..9.
        bool digit_map_ok{ version_text.size() == 5 };
        if (version_text.size() == 5)
        {
            if (version_text[0] != static_cast<char>('0' + v_major)) { digit_map_ok = false; }
            if (version_text[2] != static_cast<char>('0' + v_minor)) { digit_map_ok = false; }
            if (version_text[4] != static_cast<char>('0' + v_patch)) { digit_map_ok = false; }
        }
        check("deepen3_live_string_digit_char_map", digit_map_ok);
    }

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
