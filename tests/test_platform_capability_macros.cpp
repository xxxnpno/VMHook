// Standalone (no-JVM) unit test: compile-time platform / arch / capability
// macros are defined and mutually consistent.
//
// Scope: pure compile-time macro logic plus the small pure-logic helpers that
// the capability macros gate (vmhook::os::detail_dr::build_dr7, the portable
// os:: page/address constants).  No live oop or running HotSpot is required or
// touched here.  Anything needing a JVM (actually arming a hardware data
// breakpoint, installing a runtime hook) is covered by JVM integration in
// example.cpp and is intentionally out of scope for this file.
//
// The macros under test are *compile-time constants*, so the strongest check
// is a static_assert (the file would not compile if a relationship were
// violated).  Each relationship is ALSO surfaced as a runtime check() so the
// executable reports a per-item PASS/FAIL line rather than just failing the
// build; both layers must agree.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <iterator>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// Compile-time invariants.  If any of these are wrong the translation unit
// fails to compile, which is the hardest possible guarantee.  We mirror each
// as a runtime check() below so a passing binary documents the relationship.
// ---------------------------------------------------------------------------

// Every OS macro must be *defined* (an undefined macro expands to 0 in #if,
// which would silently hide a typo).  `defined()` is a preprocessor-only
// operator, so the "is it defined?" guard must live in an #if, not a
// static_assert.
#if !defined(VMHOOK_OS_WINDOWS) || !defined(VMHOOK_OS_LINUX)      \
 || !defined(VMHOOK_OS_MACOS)   || !defined(VMHOOK_OS_IOS)        \
 || !defined(VMHOOK_OS_ANDROID)
#error "all five VMHOOK_OS_* macros must be defined"
#endif

// Each OS macro is strictly 0 or 1.
static_assert((VMHOOK_OS_WINDOWS == 0 || VMHOOK_OS_WINDOWS == 1)
                  && (VMHOOK_OS_LINUX == 0 || VMHOOK_OS_LINUX == 1)
                  && (VMHOOK_OS_MACOS == 0 || VMHOOK_OS_MACOS == 1)
                  && (VMHOOK_OS_IOS == 0 || VMHOOK_OS_IOS == 1)
                  && (VMHOOK_OS_ANDROID == 0 || VMHOOK_OS_ANDROID == 1),
              "each VMHOOK_OS_* macro must be 0 or 1");

// Exactly one base OS is selected.
static_assert(VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS
                  + VMHOOK_OS_IOS + VMHOOK_OS_ANDROID == 1,
              "exactly one VMHOOK_OS_* base macro must be 1");

// Aggregates are defined exactly as documented.
static_assert(VMHOOK_OS_POSIX
                  == (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID),
              "VMHOOK_OS_POSIX must equal Linux|macOS|iOS|Android");
static_assert(VMHOOK_OS_APPLE == (VMHOOK_OS_MACOS | VMHOOK_OS_IOS),
              "VMHOOK_OS_APPLE must equal macOS|iOS");

// Windows and POSIX partition the supported OS set: never both, always one.
static_assert((VMHOOK_OS_WINDOWS & VMHOOK_OS_POSIX) == 0,
              "Windows and POSIX are mutually exclusive");
static_assert((VMHOOK_OS_WINDOWS | VMHOOK_OS_POSIX) == 1,
              "every supported OS is either Windows or POSIX");

// Arch macros: both defined, each 0/1, exactly one selected (x86_64 xor arm64).
#if !defined(VMHOOK_ARCH_X86_64) || !defined(VMHOOK_ARCH_ARM64)
#error "both VMHOOK_ARCH_* macros must be defined"
#endif
static_assert((VMHOOK_ARCH_X86_64 == 0 || VMHOOK_ARCH_X86_64 == 1)
                  && (VMHOOK_ARCH_ARM64 == 0 || VMHOOK_ARCH_ARM64 == 1),
              "each VMHOOK_ARCH_* macro must be 0 or 1");
static_assert(VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64 == 1,
              "exactly one architecture (x86_64 xor arm64) must be selected");

// Runtime-hooking availability is defined 0/1 and is exactly the documented
// predicate: x86_64 AND not iOS.
#if !defined(VMHOOK_RUNTIME_HOOKING_AVAILABLE)
#error "VMHOOK_RUNTIME_HOOKING_AVAILABLE must be defined"
#endif
static_assert(VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0
                  || VMHOOK_RUNTIME_HOOKING_AVAILABLE == 1,
              "VMHOOK_RUNTIME_HOOKING_AVAILABLE must be 0 or 1");
static_assert(VMHOOK_RUNTIME_HOOKING_AVAILABLE
                  == (VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS),
              "runtime hooking available iff x86_64 && !iOS");
// Consequences spelled out in the cluster focus: 0 on arm64, 0 on iOS.
static_assert(!(VMHOOK_ARCH_ARM64 && VMHOOK_RUNTIME_HOOKING_AVAILABLE),
              "runtime hooking must be unavailable on arm64");
static_assert(!(VMHOOK_OS_IOS && VMHOOK_RUNTIME_HOOKING_AVAILABLE),
              "runtime hooking must be unavailable on iOS");
// Availability implies x86_64.
static_assert(!VMHOOK_RUNTIME_HOOKING_AVAILABLE || VMHOOK_ARCH_X86_64,
              "runtime hooking availability implies x86_64");

// Hardware data breakpoints: defined 0/1, exactly Windows AND x86_64.
#if !defined(VMHOOK_HAS_HW_DATA_BREAKPOINTS)
#error "VMHOOK_HAS_HW_DATA_BREAKPOINTS must be defined"
#endif
static_assert(VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0
                  || VMHOOK_HAS_HW_DATA_BREAKPOINTS == 1,
              "VMHOOK_HAS_HW_DATA_BREAKPOINTS must be 0 or 1");
static_assert(VMHOOK_HAS_HW_DATA_BREAKPOINTS
                  == (VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64),
              "HW data breakpoints iff Windows && x86_64");
// Capability implies Windows, implies x86_64, and (since it needs x86_64)
// implies runtime hooking is also available.
static_assert(!VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_OS_WINDOWS,
              "HW data breakpoints imply Windows");
static_assert(!VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_ARCH_X86_64,
              "HW data breakpoints imply x86_64");
static_assert(!VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_RUNTIME_HOOKING_AVAILABLE,
              "HW data breakpoints imply runtime hooking is available");

// ---------------------------------------------------------------------------
// Gate-input snapshots.  `defined()` and `__has_include()` are preprocessor-only
// operators — they cannot appear in a C++ `static_assert` or runtime expression.
// So we evaluate every raw gate input *here*, in #if directives, and capture it
// as a plain 0/1 macro (VMHT_*) that IS usable in both static_assert and the
// runtime check() mirror.  These reproduce, independently of vmhook.hpp, the
// inputs the library's own gates consume; comparing the library macro against a
// VMHT_-derived expression catches any drift in the library's #if ladder.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
    #define VMHT_HAS_MSC_VER 1
#else
    #define VMHT_HAS_MSC_VER 0
#endif
#if defined(__clang__)
    #define VMHT_HAS_CLANG 1
#else
    #define VMHT_HAS_CLANG 0
#endif
#if defined(__GNUC__)
    #define VMHT_HAS_GNUC 1
#else
    #define VMHT_HAS_GNUC 0
#endif
#if defined(__clang__) && __clang_major__ >= 20
    #define VMHT_CLANG_GE_20 1
#else
    #define VMHT_CLANG_GE_20 0
#endif
#if defined(__ANDROID__)
    #define VMHT_IS_ANDROID 1
#else
    #define VMHT_IS_ANDROID 0
#endif
// Raw OS-selection gate inputs (the exact `defined()` tokens the library's
// #if/#elif OS ladder consumes, vmhook.hpp:128-168).  Capturing them lets us
// pin each *resolved* VMHOOK_OS_* macro to the ladder branch that produced it,
// so a refactor that reorders the ladder (e.g. moving __linux__ ahead of
// __ANDROID__ — Android also defines __linux__, so the order is load-bearing)
// is caught even on a CI cell where that OS is not the active one.
#if defined(_WIN32) || defined(_WIN64)
    #define VMHT_HAS_WIN32_MACRO 1
#else
    #define VMHT_HAS_WIN32_MACRO 0
#endif
#if defined(__APPLE__)
    #define VMHT_HAS_APPLE_MACRO 1
#else
    #define VMHT_HAS_APPLE_MACRO 0
#endif
#if defined(__linux__)
    #define VMHT_HAS_LINUX_MACRO 1
#else
    #define VMHT_HAS_LINUX_MACRO 0
#endif
// Raw arch-selection gate inputs (vmhook.hpp:173-183).  Same idea: tie the
// resolved VMHOOK_ARCH_* macro back to the __x86_64__/_M_X64 vs __aarch64__/
// _M_ARM64 tokens the ladder branches on.
#if defined(__x86_64__) || defined(_M_X64)
    #define VMHT_HAS_X86_64_MACRO 1
#else
    #define VMHT_HAS_X86_64_MACRO 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    #define VMHT_HAS_ARM64_MACRO 1
#else
    #define VMHT_HAS_ARM64_MACRO 0
#endif
#if defined(__cpp_explicit_this_parameter) && __cpp_explicit_this_parameter >= 202110L
    #define VMHT_HAS_EXPLICIT_THIS 1
#else
    #define VMHT_HAS_EXPLICIT_THIS 0
#endif
#if __has_include(<format>)
    #define VMHT_HAS_FORMAT_HEADER 1
#else
    #define VMHT_HAS_FORMAT_HEADER 0
#endif
#if __has_include(<print>) && defined(__cpp_lib_print) && __cpp_lib_print >= 202207L
    #define VMHT_HAS_PRINT_AND_FEATURE 1
#else
    #define VMHT_HAS_PRINT_AND_FEATURE 0
#endif

// ===========================================================================
// OS-selection LADDER pinned to its gate inputs (vmhook.hpp:128-168).  The
// existing "exactly one OS is set" / partition checks prove the *result* is
// well-formed, but they pass on every cell regardless of WHICH branch fired —
// so a refactor that reorders the #elif ladder would slip through on a CI cell
// where the misordered OS is inactive.  These pins reproduce the ladder's
// branch conditions from the raw VMHT_* tokens and equate them to the resolved
// VMHOOK_OS_* macro, so the *ordering* is itself an asserted invariant.  Every
// pin below is a tautology on every supported target (it just restates the
// ladder), hence cross-platform-safe.
//
// Arm 1 (`__ANDROID__`) is first and unconditional: resolved ANDROID is exactly
// the gate input.  This is the load-bearing ordering case — Android ALSO defines
// __linux__, so testing __ANDROID__ first is what stops Android resolving to
// LINUX.
static_assert(VMHOOK_OS_ANDROID == VMHT_IS_ANDROID,
              "VMHOOK_OS_ANDROID == defined(__ANDROID__) (first, unconditional ladder arm)");
// The ordering guarantee made explicit: an Android target DOES expose __linux__,
// yet must resolve to ANDROID (not LINUX).  Vacuous off Android.
static_assert(!VMHOOK_OS_ANDROID || VMHT_HAS_LINUX_MACRO,
              "Android defines __linux__ too; the ladder must test __ANDROID__ first");
static_assert(!VMHOOK_OS_ANDROID || (VMHOOK_OS_LINUX == 0),
              "an Android build must NOT also resolve as Linux (ladder order is load-bearing)");
// Arm 2 (`_WIN32 || _WIN64`), reached only when arm 1 missed: resolved WINDOWS
// equals (not-Android AND a Windows token).  (No real toolchain defines both,
// so this also collapses to plain `_WIN32` — but the ladder-precise form is the
// invariant.)
static_assert(VMHOOK_OS_WINDOWS == (!VMHT_IS_ANDROID && VMHT_HAS_WIN32_MACRO),
              "VMHOOK_OS_WINDOWS == (!__ANDROID__ && (_WIN32||_WIN64))");
// Arm 3 (`__APPLE__`), reached after android+windows missed: it sets exactly one
// of macOS/iOS, so the APPLE aggregate is 1 there and 0 otherwise.
static_assert(VMHOOK_OS_APPLE
                  == (!VMHT_IS_ANDROID && !VMHT_HAS_WIN32_MACRO && VMHT_HAS_APPLE_MACRO),
              "VMHOOK_OS_APPLE == (!__ANDROID__ && !_WIN32 && __APPLE__)");
// Conversely, resolving as Apple implies the __APPLE__ token was present.
static_assert(!VMHOOK_OS_APPLE || VMHT_HAS_APPLE_MACRO,
              "Apple-resolved implies defined(__APPLE__)");
// Arm 4 (`__linux__`), the last non-error arm: reached only after android,
// windows, AND apple all missed.  (Android is excluded explicitly even though it
// also defines __linux__, because arm 1 already consumed it.)
static_assert(VMHOOK_OS_LINUX
                  == (!VMHT_IS_ANDROID && !VMHT_HAS_WIN32_MACRO
                      && !VMHT_HAS_APPLE_MACRO && VMHT_HAS_LINUX_MACRO),
              "VMHOOK_OS_LINUX == (!__ANDROID__ && !_WIN32 && !__APPLE__ && __linux__)");

// ===========================================================================
// Arch-selection LADDER pinned to its gate inputs (vmhook.hpp:173-183).
// Arm 1 (`__x86_64__ || _M_X64`) is unconditional; arm 2 (`__aarch64__ ||
// _M_ARM64`) is reached only when arm 1 missed.  Both pins are tautologies on
// every supported target.
// ===========================================================================
static_assert(VMHOOK_ARCH_X86_64 == VMHT_HAS_X86_64_MACRO,
              "VMHOOK_ARCH_X86_64 == (__x86_64__ || _M_X64) (first, unconditional arch arm)");
static_assert(VMHOOK_ARCH_ARM64 == (!VMHT_HAS_X86_64_MACRO && VMHT_HAS_ARM64_MACRO),
              "VMHOOK_ARCH_ARM64 == (!x86_64 && (__aarch64__ || _M_ARM64))");
// The two arch tokens are never simultaneously true on a real target, so the
// resolved x86_64 flag equals its token unconditionally (no ladder masking
// needed) — assert that the token-level pair is itself exclusive here.
static_assert(!(VMHT_HAS_X86_64_MACRO && VMHT_HAS_ARM64_MACRO),
              "x86_64 and arm64 preprocessor tokens are mutually exclusive on a supported target");

// ===========================================================================
// Compiler-family macros (vmhook.hpp:198-214).  These select #pragma /
// intrinsic paths elsewhere; nothing previously asserted they are well-formed.
// They are NOT a strict partition: an "unknown" compiler (e.g. ICC without the
// GNU/MSVC personality) legally yields all three 0, so we assert an *at-most-one*
// invariant rather than exactly-one.
// ===========================================================================
#if !defined(VMHOOK_COMPILER_MSVC) || !defined(VMHOOK_COMPILER_CLANG)             \
 || !defined(VMHOOK_COMPILER_GCC)
#error "all three VMHOOK_COMPILER_* macros must be defined"
#endif
static_assert((VMHOOK_COMPILER_MSVC == 0 || VMHOOK_COMPILER_MSVC == 1)
                  && (VMHOOK_COMPILER_CLANG == 0 || VMHOOK_COMPILER_CLANG == 1)
                  && (VMHOOK_COMPILER_GCC == 0 || VMHOOK_COMPILER_GCC == 1),
              "each VMHOOK_COMPILER_* macro must be 0 or 1");
// MSVC and GCC are each gated `&& !defined(__clang__)`, so neither can coexist
// with CLANG, and the two "real" families (cl.exe vs g++) are mutually
// exclusive.  At most one of the three may be set.
static_assert(VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_CLANG + VMHOOK_COMPILER_GCC <= 1,
              "at most one compiler family may be selected");
static_assert(!(VMHOOK_COMPILER_MSVC && VMHOOK_COMPILER_GCC),
              "MSVC and GCC are mutually exclusive");
static_assert(!(VMHOOK_COMPILER_CLANG && VMHOOK_COMPILER_MSVC),
              "the !__clang__ guard means CLANG and MSVC never both fire (clang-cl => CLANG only)");
static_assert(!(VMHOOK_COMPILER_CLANG && VMHOOK_COMPILER_GCC),
              "the !__clang__ guard means CLANG and GCC never both fire");
// Pin the resolved family to the gate inputs that produce it, so a refactor of
// the #if ladder that flips a family is caught at compile time on *this* target.
static_assert(VMHOOK_COMPILER_MSVC == (VMHT_HAS_MSC_VER && !VMHT_HAS_CLANG),
              "VMHOOK_COMPILER_MSVC == (_MSC_VER && !__clang__)");
static_assert(VMHOOK_COMPILER_CLANG == VMHT_HAS_CLANG,
              "VMHOOK_COMPILER_CLANG == defined(__clang__)");
static_assert(VMHOOK_COMPILER_GCC == (VMHT_HAS_GNUC && !VMHT_HAS_CLANG),
              "VMHOOK_COMPILER_GCC == (__GNUC__ && !__clang__)");
// clang-cl edge: clang defines _MSC_VER, but VMHOOK_COMPILER_MSVC is guarded
// `!__clang__`, so under clang-cl we must see MSVC==0 and CLANG==1.  Express it
// as the implication "clang implies not-MSVC" (vacuously true off clang).
static_assert(!VMHOOK_COMPILER_CLANG || VMHOOK_COMPILER_MSVC == 0,
              "clang (incl. clang-cl) never reports as MSVC");

// ===========================================================================
// VMHOOK_HAS_DEDUCING_THIS (vmhook.hpp:255-262).  Documented gate:
//   __cpp_explicit_this_parameter >= 202110L
//   && (clang || msvc) && !android && !(clang >= 20)
// Gates whether object<T>::get_field can be invoked uniformly from instance
// AND static contexts.  Previously had zero coverage.
// ===========================================================================
#if !defined(VMHOOK_HAS_DEDUCING_THIS)
#error "VMHOOK_HAS_DEDUCING_THIS must be defined"
#endif
static_assert(VMHOOK_HAS_DEDUCING_THIS == 0 || VMHOOK_HAS_DEDUCING_THIS == 1,
              "VMHOOK_HAS_DEDUCING_THIS must be 0 or 1");
// Reproduce the exact documented predicate from the captured gate inputs; a
// drift in the gate (added/removed term, changed version threshold, dropped
// Android/clang-20 exclusion) fails the build.
static_assert(
    VMHOOK_HAS_DEDUCING_THIS
        == (VMHT_HAS_EXPLICIT_THIS
            && (VMHT_HAS_CLANG || VMHT_HAS_MSC_VER)
            && !VMHT_IS_ANDROID
            && !VMHT_CLANG_GE_20),
    "VMHOOK_HAS_DEDUCING_THIS must equal its documented gate");
// Implication chain: enabling deducing-this requires the C++23 feature flag,
// a clang-or-MSVC front end, a non-Android target, and (for clang) major < 20.
static_assert(!VMHOOK_HAS_DEDUCING_THIS || VMHT_HAS_EXPLICIT_THIS,
              "deducing-this implies __cpp_explicit_this_parameter >= 202110L");
static_assert(!VMHOOK_HAS_DEDUCING_THIS || (VMHT_HAS_CLANG || VMHT_HAS_MSC_VER),
              "deducing-this implies a clang or MSVC front end (the GCC static-call path is excluded)");
static_assert(!VMHOOK_HAS_DEDUCING_THIS || !VMHT_IS_ANDROID,
              "deducing-this implies a non-Android target (NDK clang behaves like GCC here)");
static_assert(!VMHOOK_HAS_DEDUCING_THIS || !VMHT_CLANG_GE_20,
              "deducing-this implies clang major < 20 (clang 20 changed static-context overload resolution)");
// Pure GCC can never enable deducing-this (no clang/MSVC personality).
static_assert(!(VMHOOK_COMPILER_GCC && VMHOOK_HAS_DEDUCING_THIS),
              "pure GCC must not enable VMHOOK_HAS_DEDUCING_THIS");
// Android can never enable it regardless of front end.
static_assert(!(VMHOOK_OS_ANDROID && VMHOOK_HAS_DEDUCING_THIS),
              "Android must not enable VMHOOK_HAS_DEDUCING_THIS");

// ===========================================================================
// std-library feature gates (vmhook.hpp:217-230): VMHOOK_HAS_STD_FORMAT /
// VMHOOK_HAS_STD_PRINT.  These pick the logging backend; previously untested.
// ===========================================================================
#if !defined(VMHOOK_HAS_STD_FORMAT) || !defined(VMHOOK_HAS_STD_PRINT)
#error "VMHOOK_HAS_STD_FORMAT and VMHOOK_HAS_STD_PRINT must be defined"
#endif
static_assert(VMHOOK_HAS_STD_FORMAT == 0 || VMHOOK_HAS_STD_FORMAT == 1,
              "VMHOOK_HAS_STD_FORMAT must be 0 or 1");
static_assert(VMHOOK_HAS_STD_PRINT == 0 || VMHOOK_HAS_STD_PRINT == 1,
              "VMHOOK_HAS_STD_PRINT must be 0 or 1");
// VMHOOK_HAS_STD_FORMAT is exactly __has_include(<format>) (vmhook.hpp:217).
static_assert(VMHOOK_HAS_STD_FORMAT == VMHT_HAS_FORMAT_HEADER,
              "VMHOOK_HAS_STD_FORMAT == __has_include(<format>)");
// VMHOOK_HAS_STD_PRINT is __has_include(<print>) AND __cpp_lib_print>=202207L
// (vmhook.hpp:225).  Reproduce both terms.
static_assert(VMHOOK_HAS_STD_PRINT == VMHT_HAS_PRINT_AND_FEATURE,
              "VMHOOK_HAS_STD_PRINT == (__has_include(<print>) && __cpp_lib_print>=202207L)");
// std::print is strictly newer than std::format: a toolchain shipping <print>
// (post-format era) necessarily also ships <format>.  So PRINT implies FORMAT.
static_assert(!VMHOOK_HAS_STD_PRINT || VMHOOK_HAS_STD_FORMAT,
              "std::print availability implies std::format availability");

// ===========================================================================
// Aggregate-macro NORMALISATION (latent flaw): VMHOOK_OS_POSIX / VMHOOK_OS_APPLE
// are bitwise-OR *expressions* of the base flags, not 0/1 literals.  Today the
// OR collapses to 0/1 because each base flag is 0/1, but they are consumed as
// booleans (`!VMHOOK_OS_IOS`, `Windows | POSIX == 1`).  Force them to be exactly
// 0 or 1 in arithmetic contexts so a future arm that sets two sub-flags (which
// would make the OR != 1) is caught here rather than silently mis-behaving in a
// `POSIX * N` or `POSIX + Windows` expression.
// ===========================================================================
static_assert(VMHOOK_OS_POSIX == 0 || VMHOOK_OS_POSIX == 1,
              "VMHOOK_OS_POSIX must normalise to exactly 0 or 1");
static_assert(VMHOOK_OS_APPLE == 0 || VMHOOK_OS_APPLE == 1,
              "VMHOOK_OS_APPLE must normalise to exactly 0 or 1");
// Stronger arithmetic-context probes: if the aggregate were ever != {0,1} these
// identities would break even though the bare `== union` check still passed.
static_assert((VMHOOK_OS_POSIX | 1) == 1 && (VMHOOK_OS_POSIX & 1) == VMHOOK_OS_POSIX,
              "VMHOOK_OS_POSIX behaves as a 1-bit boolean under | and &");
static_assert((VMHOOK_OS_APPLE | 1) == 1 && (VMHOOK_OS_APPLE & 1) == VMHOOK_OS_APPLE,
              "VMHOOK_OS_APPLE behaves as a 1-bit boolean under | and &");
static_assert(VMHOOK_OS_POSIX * 7 == (VMHOOK_OS_POSIX ? 7 : 0),
              "VMHOOK_OS_POSIX multiplies as a normalised 0/1 scalar");
static_assert(VMHOOK_OS_APPLE * 7 == (VMHOOK_OS_APPLE ? 7 : 0),
              "VMHOOK_OS_APPLE multiplies as a normalised 0/1 scalar");

int main()
{
    // -- OS macro consistency (runtime mirror of the static_asserts) --------
    const int os_sum{ VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS
                      + VMHOOK_OS_IOS + VMHOOK_OS_ANDROID };
    check("exactly_one_os_macro_is_one", os_sum == 1);

    const bool os_each_binary{
        (VMHOOK_OS_WINDOWS | 1) == 1 && (VMHOOK_OS_LINUX | 1) == 1
        && (VMHOOK_OS_MACOS | 1) == 1 && (VMHOOK_OS_IOS | 1) == 1
        && (VMHOOK_OS_ANDROID | 1) == 1
        && VMHOOK_OS_WINDOWS >= 0 && VMHOOK_OS_WINDOWS <= 1
        && VMHOOK_OS_LINUX >= 0 && VMHOOK_OS_LINUX <= 1
        && VMHOOK_OS_MACOS >= 0 && VMHOOK_OS_MACOS <= 1
        && VMHOOK_OS_IOS >= 0 && VMHOOK_OS_IOS <= 1
        && VMHOOK_OS_ANDROID >= 0 && VMHOOK_OS_ANDROID <= 1 };
    check("every_os_macro_is_zero_or_one", os_each_binary);

    check("os_posix_equals_documented_union",
          VMHOOK_OS_POSIX
              == (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID));
    check("os_apple_equals_macos_or_ios",
          VMHOOK_OS_APPLE == (VMHOOK_OS_MACOS | VMHOOK_OS_IOS));

    check("windows_and_posix_mutually_exclusive",
          (VMHOOK_OS_WINDOWS & VMHOOK_OS_POSIX) == 0);
    check("windows_xor_posix_covers_all_supported",
          (VMHOOK_OS_WINDOWS | VMHOOK_OS_POSIX) == 1);

    // macOS and iOS are the only Apple targets; Apple implies POSIX and never
    // Windows.
    check("apple_implies_posix_and_not_windows",
          (!VMHOOK_OS_APPLE) || (VMHOOK_OS_POSIX == 1 && VMHOOK_OS_WINDOWS == 0));
    // Android implies POSIX (its backend is the Linux/Android shared path).
    check("android_implies_posix",
          (!VMHOOK_OS_ANDROID) || VMHOOK_OS_POSIX == 1);

    // -- OS-selection ladder pinned to gate inputs (runtime mirror) ---------
    // Each resolved VMHOOK_OS_* equals the ladder branch (in order) that set it.
    // These pass on every cell because they merely restate the #if/#elif ladder.
    check("os_android_equals_gate_input",
          VMHOOK_OS_ANDROID == VMHT_IS_ANDROID);
    check("android_implies_linux_macro_present",
          !VMHOOK_OS_ANDROID || VMHT_HAS_LINUX_MACRO);
    check("android_does_not_also_resolve_linux",
          !VMHOOK_OS_ANDROID || (VMHOOK_OS_LINUX == 0));
    check("os_windows_matches_ladder_gate",
          VMHOOK_OS_WINDOWS == (!VMHT_IS_ANDROID && VMHT_HAS_WIN32_MACRO));
    check("os_apple_matches_ladder_gate",
          VMHOOK_OS_APPLE
              == (!VMHT_IS_ANDROID && !VMHT_HAS_WIN32_MACRO && VMHT_HAS_APPLE_MACRO));
    check("apple_resolved_implies_apple_macro",
          !VMHOOK_OS_APPLE || VMHT_HAS_APPLE_MACRO);
    check("os_linux_matches_ladder_gate",
          VMHOOK_OS_LINUX
              == (!VMHT_IS_ANDROID && !VMHT_HAS_WIN32_MACRO
                  && !VMHT_HAS_APPLE_MACRO && VMHT_HAS_LINUX_MACRO));

    // -- Arch macro consistency --------------------------------------------
    check("exactly_one_arch_macro_is_one",
          VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64 == 1);
    check("arch_macros_are_zero_or_one",
          VMHOOK_ARCH_X86_64 >= 0 && VMHOOK_ARCH_X86_64 <= 1
              && VMHOOK_ARCH_ARM64 >= 0 && VMHOOK_ARCH_ARM64 <= 1);
    check("arch_x86_64_xor_arm64",
          (VMHOOK_ARCH_X86_64 ^ VMHOOK_ARCH_ARM64) == 1);
    // Arch ladder pinned to gate inputs (runtime mirror): x86_64 arm is first
    // and unconditional; arm64 arm is gated on x86_64 missing.
    check("arch_x86_64_equals_gate_input",
          VMHOOK_ARCH_X86_64 == VMHT_HAS_X86_64_MACRO);
    check("arch_arm64_matches_ladder_gate",
          VMHOOK_ARCH_ARM64 == (!VMHT_HAS_X86_64_MACRO && VMHT_HAS_ARM64_MACRO));
    check("arch_tokens_mutually_exclusive",
          !(VMHT_HAS_X86_64_MACRO && VMHT_HAS_ARM64_MACRO));

    // -- Runtime hooking availability flag ---------------------------------
    check("runtime_hooking_flag_is_zero_or_one",
          VMHOOK_RUNTIME_HOOKING_AVAILABLE >= 0
              && VMHOOK_RUNTIME_HOOKING_AVAILABLE <= 1);
    check("runtime_hooking_equals_x86_64_and_not_ios",
          VMHOOK_RUNTIME_HOOKING_AVAILABLE
              == (VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS));
    check("runtime_hooking_unavailable_on_arm64",
          !(VMHOOK_ARCH_ARM64 && VMHOOK_RUNTIME_HOOKING_AVAILABLE));
    check("runtime_hooking_unavailable_on_ios",
          !(VMHOOK_OS_IOS && VMHOOK_RUNTIME_HOOKING_AVAILABLE));
    check("runtime_hooking_implies_x86_64",
          !VMHOOK_RUNTIME_HOOKING_AVAILABLE || VMHOOK_ARCH_X86_64);

    // -- Hardware data-breakpoint capability flag --------------------------
    check("hw_data_breakpoints_flag_is_zero_or_one",
          VMHOOK_HAS_HW_DATA_BREAKPOINTS >= 0
              && VMHOOK_HAS_HW_DATA_BREAKPOINTS <= 1);
    check("hw_data_breakpoints_equals_windows_and_x86_64",
          VMHOOK_HAS_HW_DATA_BREAKPOINTS
              == (VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64));
    check("hw_data_breakpoints_imply_windows_x86_64",
          !VMHOOK_HAS_HW_DATA_BREAKPOINTS
              || (VMHOOK_OS_WINDOWS == 1 && VMHOOK_ARCH_X86_64 == 1));
    check("hw_data_breakpoints_imply_runtime_hooking_available",
          !VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_RUNTIME_HOOKING_AVAILABLE);
    // No non-Windows / non-x86_64 platform may advertise the capability.
    check("hw_data_breakpoints_off_on_arm64_and_posix",
          (VMHOOK_ARCH_ARM64 || VMHOOK_OS_POSIX)
              ? (VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0)
              : true);

    // -- Additional cross-macro implications (runtime mirror). --------------
    // POSIX is the exact complement of Windows within the supported set, so
    // each implies the negation of the other.
    check("posix_implies_not_windows",
          !VMHOOK_OS_POSIX || (VMHOOK_OS_WINDOWS == 0));
    check("windows_implies_not_posix",
          !VMHOOK_OS_WINDOWS || (VMHOOK_OS_POSIX == 0));
    // The five base-OS flags XOR to 1 (an even number of 1s would mean either
    // zero or two selected — both are bugs the "exactly one" sum also catches,
    // but XOR pins the parity independently).
    check("os_flags_xor_to_one",
          (VMHOOK_OS_WINDOWS ^ VMHOOK_OS_LINUX ^ VMHOOK_OS_MACOS
           ^ VMHOOK_OS_IOS ^ VMHOOK_OS_ANDROID) == 1);
    // iOS is an Apple target, so iOS implies APPLE implies POSIX; and iOS is the
    // one POSIX/Apple platform where runtime hooking is forced off.
    check("ios_implies_apple_and_posix",
          !VMHOOK_OS_IOS || (VMHOOK_OS_APPLE == 1 && VMHOOK_OS_POSIX == 1));
    check("ios_forces_runtime_hooking_off",
          !VMHOOK_OS_IOS || (VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0));
    // arm64 forces BOTH higher capabilities off (no runtime hooking, no HW
    // breakpoints) since both require x86_64.
    check("arm64_forces_both_capabilities_off",
          !VMHOOK_ARCH_ARM64
              || (VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0
                  && VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0));
    // HW data breakpoints are the strongest capability: having them implies
    // runtime hooking is also available (monotone capability ladder).
    check("hw_breakpoints_is_subset_of_runtime_hooking",
          !VMHOOK_HAS_HW_DATA_BREAKPOINTS || VMHOOK_RUNTIME_HOOKING_AVAILABLE);

    // -- Compiler-family macros (runtime mirror of the static_asserts) ------
    check("compiler_macros_are_zero_or_one",
          (VMHOOK_COMPILER_MSVC | 1) == 1 && (VMHOOK_COMPILER_CLANG | 1) == 1
              && (VMHOOK_COMPILER_GCC | 1) == 1
              && VMHOOK_COMPILER_MSVC >= 0 && VMHOOK_COMPILER_MSVC <= 1
              && VMHOOK_COMPILER_CLANG >= 0 && VMHOOK_COMPILER_CLANG <= 1
              && VMHOOK_COMPILER_GCC >= 0 && VMHOOK_COMPILER_GCC <= 1);
    check("at_most_one_compiler_family",
          VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_CLANG + VMHOOK_COMPILER_GCC <= 1);
    check("compiler_msvc_and_gcc_mutually_exclusive",
          !(VMHOOK_COMPILER_MSVC && VMHOOK_COMPILER_GCC));
    check("compiler_clang_excludes_msvc",
          !(VMHOOK_COMPILER_CLANG && VMHOOK_COMPILER_MSVC));
    check("compiler_clang_excludes_gcc",
          !(VMHOOK_COMPILER_CLANG && VMHOOK_COMPILER_GCC));
    // clang-cl reports CLANG, never MSVC, even though it defines _MSC_VER.
    check("clang_never_reports_as_msvc",
          !VMHOOK_COMPILER_CLANG || VMHOOK_COMPILER_MSVC == 0);
    // Resolved value pinned to gate inputs (mirrors the compile-time pins via
    // the VMHT_* gate-input snapshots, since defined()/__has_include can't be
    // used in a runtime expression).
    check("compiler_msvc_matches_gate",
          VMHOOK_COMPILER_MSVC == (VMHT_HAS_MSC_VER && !VMHT_HAS_CLANG));
    check("compiler_clang_matches_gate",
          VMHOOK_COMPILER_CLANG == VMHT_HAS_CLANG);
    check("compiler_gcc_matches_gate",
          VMHOOK_COMPILER_GCC == (VMHT_HAS_GNUC && !VMHT_HAS_CLANG));

    // -- VMHOOK_HAS_DEDUCING_THIS (runtime mirror) -------------------------
    check("deducing_this_flag_is_zero_or_one",
          VMHOOK_HAS_DEDUCING_THIS >= 0 && VMHOOK_HAS_DEDUCING_THIS <= 1);
    check("deducing_this_equals_documented_gate",
          VMHOOK_HAS_DEDUCING_THIS
              == (VMHT_HAS_EXPLICIT_THIS
                  && (VMHT_HAS_CLANG || VMHT_HAS_MSC_VER)
                  && !VMHT_IS_ANDROID
                  && !VMHT_CLANG_GE_20));
    check("deducing_this_implies_clang_or_msvc",
          !VMHOOK_HAS_DEDUCING_THIS || (VMHT_HAS_CLANG || VMHT_HAS_MSC_VER));
    check("deducing_this_implies_not_android",
          !VMHOOK_HAS_DEDUCING_THIS || !VMHT_IS_ANDROID);
    check("deducing_this_off_under_pure_gcc",
          !(VMHOOK_COMPILER_GCC && VMHOOK_HAS_DEDUCING_THIS));
    check("deducing_this_off_on_android",
          !(VMHOOK_OS_ANDROID && VMHOOK_HAS_DEDUCING_THIS));

    // -- std-library feature gates (runtime mirror) ------------------------
    check("std_format_flag_is_zero_or_one",
          VMHOOK_HAS_STD_FORMAT >= 0 && VMHOOK_HAS_STD_FORMAT <= 1);
    check("std_print_flag_is_zero_or_one",
          VMHOOK_HAS_STD_PRINT >= 0 && VMHOOK_HAS_STD_PRINT <= 1);
    check("std_format_equals_has_include",
          VMHOOK_HAS_STD_FORMAT == VMHT_HAS_FORMAT_HEADER);
    check("std_print_equals_has_include_and_feature_test",
          VMHOOK_HAS_STD_PRINT == VMHT_HAS_PRINT_AND_FEATURE);
    check("std_print_implies_std_format",
          !VMHOOK_HAS_STD_PRINT || VMHOOK_HAS_STD_FORMAT);

    // -- Aggregate-macro normalisation (runtime mirror) --------------------
    // Force POSIX/APPLE to behave as 0/1 scalars (not just "equal to the union
    // on today's config").  A future arm setting two sub-flags would break the
    // multiply/OR identities here even if the bare union check still passed.
    check("os_posix_normalises_to_zero_or_one",
          VMHOOK_OS_POSIX == 0 || VMHOOK_OS_POSIX == 1);
    check("os_apple_normalises_to_zero_or_one",
          VMHOOK_OS_APPLE == 0 || VMHOOK_OS_APPLE == 1);
    check("os_posix_is_one_bit_boolean",
          (VMHOOK_OS_POSIX | 1) == 1 && (VMHOOK_OS_POSIX & 1) == VMHOOK_OS_POSIX
              && (VMHOOK_OS_POSIX * 7) == (VMHOOK_OS_POSIX ? 7 : 0));
    check("os_apple_is_one_bit_boolean",
          (VMHOOK_OS_APPLE | 1) == 1 && (VMHOOK_OS_APPLE & 1) == VMHOOK_OS_APPLE
              && (VMHOOK_OS_APPLE * 7) == (VMHOOK_OS_APPLE ? 7 : 0));

    // -- Portable os:: address-range constants (defined on every platform) --
    // These are the documented user-space bounds the hook/scan code relies on;
    // they are plain constexpr values, no JVM needed.
    check("user_address_floor_below_ceiling",
          vmhook::os::user_address_floor < vmhook::os::user_address_ceiling);
    check("user_address_ceiling_is_canonical_low_half_top",
          vmhook::os::user_address_ceiling == std::uintptr_t{ 0x00007FFFFFFFFFFFull });
    check("user_address_floor_is_64k",
          vmhook::os::user_address_floor == std::uintptr_t{ 0xFFFFull });

    // -- user_address_ceiling's SECOND role: the OOP tag-strip mask ---------
    // The same constant is reused as the mask that strips high GC tag/colour
    // bits from a HotSpot oop (vmhook.hpp untag_pointer / the OOP-untag site).
    // It must (a) be a clean low-47-bit mask 0x0000'7FFF'FFFF'FFFF, (b) clear
    // every bit at/above 47 for synthetic tagged pointers, and (c) be a no-op
    // on an already-canonical low-half address.  Pure arithmetic, no JVM; this
    // pins the constant to its masking role (relevant under ZGC/JDK21+ coloured
    // pointers that pack tag bits above bit 47).
    {
        const std::uintptr_t mask{ vmhook::os::user_address_ceiling };
        // The mask is exactly bits 0..46 set, bits 47..63 clear.
        check("untag_mask_is_low_47_bits",
              mask == ((std::uintptr_t{ 1 } << 47) - 1));
        check("untag_mask_high_bits_clear",
              (mask & ~((std::uintptr_t{ 1 } << 47) - 1)) == 0);
        // A canonical low-half address survives masking unchanged.
        const std::uintptr_t canonical{ 0x0000'1234'5678'9AB0ull };
        check("untag_mask_noop_on_canonical_address",
              (canonical & mask) == canonical);
        // Synthetic tagged pointers: a real address OR'd with assorted high
        // tag/colour bits.  Masking must recover exactly the low-47 address.
        const std::uintptr_t base{ 0x0000'0000'DEAD'BEE0ull };  // < 2^47
        const std::uintptr_t tag_bits[]{
            std::uintptr_t{ 1 } << 47,                 // first bit above the window
            std::uintptr_t{ 1 } << 60,                 // a high colour bit
            std::uintptr_t{ 0xFFFFull } << 48,         // canonical sign-extension bits
            std::uintptr_t{ 0xABCDull } << 48,         // arbitrary high tag nibble
            ~((std::uintptr_t{ 1 } << 47) - 1),        // ALL bits 47..63 set
        };
        bool all_recovered{ true };
        for (const std::uintptr_t tb : tag_bits)
        {
            const std::uintptr_t tagged{ base | tb };
            if ((tagged & mask) != base) { all_recovered = false; }
        }
        check("untag_mask_strips_synthetic_tag_bits", all_recovered);
        // The library's untag_pointer helper (vmhook::hotspot) must agree with a
        // manual mask for a tagged pointer — it is a pure function that masks
        // with os::user_address_ceiling; no JVM, no live oop.
        const std::uintptr_t tagged_ptr{ base | (std::uintptr_t{ 0xDEADull } << 48) };
        check("untag_pointer_helper_matches_manual_mask",
              reinterpret_cast<std::uintptr_t>(
                  vmhook::hotspot::untag_pointer(reinterpret_cast<const void*>(tagged_ptr)))
                  == base);
    }

    // -- Portable os:: page geometry (pure syscalls, no JVM) ----------------
    const std::size_t ps{ vmhook::os::page_size() };
    check("page_size_is_nonzero", ps != 0);
    check("page_size_is_power_of_two", ps != 0 && (ps & (ps - 1)) == 0);
    check("page_size_at_least_4096", ps >= 4096);
    const std::size_t gran{ vmhook::os::allocation_granularity() };
    check("allocation_granularity_is_nonzero", gran != 0);
    check("allocation_granularity_multiple_of_page_size",
          ps != 0 && (gran % ps) == 0);
    // Granularity is never finer than a page (the scan allocator clamps to it).
    check("allocation_granularity_at_least_page_size", gran >= ps);
    // Platform-specific cross-relation (vmhook.hpp:508): on POSIX the two are
    // identical (allocation_granularity() just forwards to page_size()); on
    // Windows the granularity comes from dwAllocationGranularity, which is a
    // strict multiple of (and typically 64 KiB, larger than) the page size.
#if VMHOOK_OS_POSIX
    check("posix_allocation_granularity_equals_page_size", gran == ps);
#else
    check("windows_allocation_granularity_is_page_multiple", (gran % ps) == 0);
#endif

    // memory_protection is a portable enum present on all platforms; confirm
    // the documented stable ordinal values the OS-protection mapping relies on.
    check("memory_protection_enum_ordinals_stable",
          static_cast<std::uint32_t>(vmhook::os::memory_protection::no_access) == 0
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::read) == 1
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::read_write) == 2
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::execute_read) == 3
              && static_cast<std::uint32_t>(vmhook::os::memory_protection::execute_rw) == 4);

    // memory_protection underlying type is std::uint32_t (vmhook.hpp:447); a
    // change of width would silently alter ABI of any struct holding it.
    check("memory_protection_underlying_is_uint32",
          std::is_same_v<std::underlying_type_t<vmhook::os::memory_protection>,
                         std::uint32_t>);

    // The five ordinals are CONTIGUOUS [0..4] and STRICTLY INCREASING in the
    // documented declaration order, with no gaps — the OS-protection lookup
    // tables index by these values, so a reorder or gap would mis-map.
    {
        using mp = vmhook::os::memory_protection;
        const std::uint32_t ord[]{
            static_cast<std::uint32_t>(mp::no_access),
            static_cast<std::uint32_t>(mp::read),
            static_cast<std::uint32_t>(mp::read_write),
            static_cast<std::uint32_t>(mp::execute_read),
            static_cast<std::uint32_t>(mp::execute_rw),
        };
        bool strictly_increasing_contiguous{ true };
        for (std::size_t i{ 0 }; i < std::size(ord); ++i)
        {
            if (ord[i] != static_cast<std::uint32_t>(i)) { strictly_increasing_contiguous = false; }
        }
        check("memory_protection_ordinals_contiguous_0_to_4",
              strictly_increasing_contiguous);
        // All five are pairwise distinct (a duplicate would collapse two
        // protection semantics onto one value).
        bool all_distinct{ true };
        for (std::size_t i{ 0 }; i < std::size(ord); ++i)
        {
            for (std::size_t j{ i + 1 }; j < std::size(ord); ++j)
            {
                if (ord[i] == ord[j]) { all_distinct = false; }
            }
        }
        check("memory_protection_ordinals_all_distinct", all_distinct);
        // The max ordinal is exactly 4 (execute_rw); anchors "exactly five".
        check("memory_protection_max_ordinal_is_4",
              static_cast<std::uint32_t>(mp::execute_rw) == 4);
        // execute_read and execute_rw are the only two with the "execute" bit
        // of meaning — they are the two highest ordinals and strictly above the
        // non-executable trio.
        check("memory_protection_execute_variants_are_highest",
              static_cast<std::uint32_t>(mp::execute_read) > static_cast<std::uint32_t>(mp::read_write)
              && static_cast<std::uint32_t>(mp::execute_rw) > static_cast<std::uint32_t>(mp::execute_read));
    }

    // region_info default-constructs to an all-empty/unset region (the scan
    // allocator depends on these defaults).
    {
        const vmhook::os::region_info ri{};
        check("region_info_default_is_empty_unset",
              ri.base == nullptr && ri.size == 0 && !ri.committed && !ri.free
                  && !ri.readable && !ri.executable && !ri.guarded);
        // The scan allocator relies on region_info being a plain aggregate it
        // can default-construct and brace-init; pin those structural traits.
        check("region_info_is_default_constructible",
              std::is_default_constructible_v<vmhook::os::region_info>);
        check("region_info_is_aggregate",
              std::is_aggregate_v<vmhook::os::region_info>);
    }

    // -- DR7 builder: only exists when the capability is compiled in --------
    // build_dr7 is a pure Intel-SDM bit-mask helper (no JVM, no live thread);
    // it is only declared on Windows/x86_64 where VMHOOK_HAS_HW_DATA_BREAKPOINTS
    // is 1.  Gate the checks on the macro so the file still compiles (and the
    // remaining checks still run) on platforms where the symbol is absent.
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    {
        using vmhook::os::data_breakpoint_kind;
        using vmhook::os::data_breakpoint_length;

        // Enum bit-patterns documented against the Intel DR7 R/W and LEN
        // fields.  build_dr7 shifts these raw values into place, so their
        // numeric values are load-bearing.
        check("dr_kind_write_is_0b01",
              static_cast<std::uint8_t>(data_breakpoint_kind::write) == 0b01);
        check("dr_kind_read_write_is_0b11",
              static_cast<std::uint8_t>(data_breakpoint_kind::read_write) == 0b11);
        check("dr_length_one_byte_is_0b00",
              static_cast<std::uint8_t>(data_breakpoint_length::one_byte) == 0b00);
        check("dr_length_eight_bytes_is_0b10",
              static_cast<std::uint8_t>(data_breakpoint_length::eight_bytes) == 0b10);

        // Slot 0, write, one-byte:
        //   L0  bit 0          -> 0x1
        //   R/W field bits16-17 = 01 -> 0x1 << 16
        //   LEN field bits18-19 = 00 -> 0
        const std::uint64_t dr7_s0{ vmhook::os::detail_dr::build_dr7(
            0, data_breakpoint_kind::write, data_breakpoint_length::one_byte) };
        check("build_dr7_slot0_write_one_byte",
              dr7_s0 == ((std::uint64_t{ 1 } << 0)
                         | (std::uint64_t{ 0b01 } << 16)
                         | (std::uint64_t{ 0b00 } << 18)));

        // Slot 3, read_write, eight-byte:
        //   L3  bit 6          -> 1 << 6
        //   R/W field at 16+4*3 = 28, value 0b11
        //   LEN field at 18+4*3 = 30, value 0b10
        const std::uint64_t dr7_s3{ vmhook::os::detail_dr::build_dr7(
            3, data_breakpoint_kind::read_write, data_breakpoint_length::eight_bytes) };
        check("build_dr7_slot3_read_write_eight_byte",
              dr7_s3 == ((std::uint64_t{ 1 } << 6)
                         | (std::uint64_t{ 0b11 } << 28)
                         | (std::uint64_t{ 0b10 } << 30)));

        // The local-enable bit for slot N sits at bit 2*N; verify each slot's
        // enable bit is distinct and lands where the SDM places it.
        bool enable_bits_ok{ true };
        for (int slot{ 0 }; slot < 4; ++slot)
        {
            const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                slot, data_breakpoint_kind::write, data_breakpoint_length::one_byte) };
            if ((v & (std::uint64_t{ 1 } << (slot * 2))) == 0) { enable_bits_ok = false; }
        }
        check("build_dr7_local_enable_bit_per_slot", enable_bits_ok);

        // The four global-enable bits (G0..G3 at odd bits 1,3,5,7) must stay
        // clear: the watch is per-thread, never process-global.
        bool no_global_enable{ true };
        for (int slot{ 0 }; slot < 4; ++slot)
        {
            const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                slot, data_breakpoint_kind::read_write,
                data_breakpoint_length::four_bytes) };
            if ((v & (std::uint64_t{ 1 } << (slot * 2 + 1))) != 0) { no_global_enable = false; }
        }
        check("build_dr7_never_sets_global_enable", no_global_enable);

        // -- The remaining enum ordinals (vmhook.hpp:1004-1019). --------------
        // data_breakpoint_kind underlying type is uint8_t; only write(0b01) and
        // read_write(0b11) exist — both have bit 0 set (the "enabled R/W" low
        // bit), and execute (0b00) is deliberately absent (DR can't trap exec
        // via the data-watch path).
        check("dr_kind_underlying_is_uint8",
              std::is_same_v<std::underlying_type_t<data_breakpoint_kind>, std::uint8_t>);
        check("dr_kind_write_and_read_write_distinct",
              static_cast<std::uint8_t>(data_breakpoint_kind::write)
                  != static_cast<std::uint8_t>(data_breakpoint_kind::read_write));
        check("dr_kind_values_have_low_bit_set",
              (static_cast<std::uint8_t>(data_breakpoint_kind::write) & 0b01u) != 0
              && (static_cast<std::uint8_t>(data_breakpoint_kind::read_write) & 0b01u) != 0);

        // data_breakpoint_length: the Intel-SDM LEN encoding is NOT in numeric
        // byte order — one_byte=0b00, two_bytes=0b01, eight_bytes=0b10,
        // four_bytes=0b11.  Pin the two the existing suite omits (two_bytes,
        // four_bytes) and that all four are distinct 2-bit codes.
        check("dr_length_two_bytes_is_0b01",
              static_cast<std::uint8_t>(data_breakpoint_length::two_bytes) == 0b01);
        check("dr_length_four_bytes_is_0b11",
              static_cast<std::uint8_t>(data_breakpoint_length::four_bytes) == 0b11);
        check("dr_length_underlying_is_uint8",
              std::is_same_v<std::underlying_type_t<data_breakpoint_length>, std::uint8_t>);
        {
            const std::uint8_t lens[]{
                static_cast<std::uint8_t>(data_breakpoint_length::one_byte),
                static_cast<std::uint8_t>(data_breakpoint_length::two_bytes),
                static_cast<std::uint8_t>(data_breakpoint_length::eight_bytes),
                static_cast<std::uint8_t>(data_breakpoint_length::four_bytes),
            };
            bool all_two_bit_and_distinct{ true };
            for (std::size_t i{ 0 }; i < std::size(lens); ++i)
            {
                if ((lens[i] & ~0b11u) != 0) { all_two_bit_and_distinct = false; }
                for (std::size_t j{ i + 1 }; j < std::size(lens); ++j)
                {
                    if (lens[i] == lens[j]) { all_two_bit_and_distinct = false; }
                }
            }
            check("dr_length_all_codes_two_bit_and_distinct", all_two_bit_and_distinct);
        }

        // -- build_dr7 exhaustive field placement across all 4 slots. ---------
        // For every slot the R/W field sits at bit (16 + 4*slot) and the LEN
        // field at bit (18 + 4*slot); verify the raw enum codes land exactly
        // there for a read_write/two_bytes watch, and that the only L-enable bit
        // set is the slot's own (bit 2*slot).
        {
            bool fields_placed_ok{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::read_write,
                    data_breakpoint_length::two_bytes) };
                const std::uint64_t rw_field{ (v >> (16 + 4 * slot)) & 0b11u };
                const std::uint64_t len_field{ (v >> (18 + 4 * slot)) & 0b11u };
                if (rw_field != 0b11u) { fields_placed_ok = false; }    // read_write
                if (len_field != 0b01u) { fields_placed_ok = false; }   // two_bytes
                // Exactly one L-enable bit (the slot's own) among bits 0,2,4,6.
                const std::uint64_t l_enable_bits{ v & 0b01010101u };
                if (l_enable_bits != (std::uint64_t{ 1 } << (slot * 2))) { fields_placed_ok = false; }
            }
            check("build_dr7_rw_len_fields_placed_per_slot", fields_placed_ok);
        }

        // Two distinct slots produce distinct DR7 words (different L-enable
        // bit), and the same (slot, kind, length) is deterministic.
        check("build_dr7_distinct_slots_distinct_words",
              vmhook::os::detail_dr::build_dr7(0, data_breakpoint_kind::write,
                                               data_breakpoint_length::one_byte)
                  != vmhook::os::detail_dr::build_dr7(1, data_breakpoint_kind::write,
                                                      data_breakpoint_length::one_byte));
        check("build_dr7_is_deterministic",
              vmhook::os::detail_dr::build_dr7(2, data_breakpoint_kind::read_write,
                                               data_breakpoint_length::four_bytes)
                  == vmhook::os::detail_dr::build_dr7(2, data_breakpoint_kind::read_write,
                                                      data_breakpoint_length::four_bytes));

        // -- EXHAUSTIVE build_dr7 matrix: all 4 slots x 2 kinds x 4 lengths ----
        // Every one of the 32 combinations must equal the closed-form Intel-SDM
        // encoding (1<<2s) | (kind<<(16+4s)) | (len<<(18+4s)).  This is the full
        // input space of the helper (slot is contractually 0..3); previously
        // only ~6 of the 32 cells were spot-checked.
        {
            const data_breakpoint_kind kinds[]{
                data_breakpoint_kind::write,
                data_breakpoint_kind::read_write,
            };
            const data_breakpoint_length lens[]{
                data_breakpoint_length::one_byte,
                data_breakpoint_length::two_bytes,
                data_breakpoint_length::four_bytes,
                data_breakpoint_length::eight_bytes,
            };
            bool all_match_closed_form{ true };
            int  cells_checked{ 0 };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                for (const data_breakpoint_kind k : kinds)
                {
                    for (const data_breakpoint_length l : lens)
                    {
                        const std::uint64_t got{ vmhook::os::detail_dr::build_dr7(slot, k, l) };
                        const std::uint64_t want{
                            (std::uint64_t{ 1 } << (slot * 2))
                            | (static_cast<std::uint64_t>(k) << (16 + slot * 4))
                            | (static_cast<std::uint64_t>(l) << (18 + slot * 4)) };
                        if (got != want) { all_match_closed_form = false; }
                        ++cells_checked;
                    }
                }
            }
            check("build_dr7_full_matrix_matches_closed_form", all_match_closed_form);
            check("build_dr7_full_matrix_covered_all_32_cells", cells_checked == 32);
        }

        // -- DR7 bit-field NON-OVERLAP within a slot, and across slots ---------
        // For each slot the three sub-fields occupy disjoint bit positions:
        //   L-enable: bit 2*slot          (1 bit)
        //   R/W:      bits 16+4*slot .. 17+4*slot   (2 bits)
        //   LEN:      bits 18+4*slot .. 19+4*slot   (2 bits)
        // Build the per-slot field masks and assert (a) the three masks are
        // pairwise disjoint for the same slot, (b) the L-enable bits of distinct
        // slots are disjoint, and (c) slot N's R/W+LEN region never overlaps slot
        // M's (N != M).  This proves the 4*slot stride is correct for ALL slots,
        // not merely the 0/3 endpoints the earlier checks pinned.
        {
            auto enable_mask = [](int s) {
                return std::uint64_t{ 1 } << (s * 2);
            };
            auto rw_mask = [](int s) {
                return std::uint64_t{ 0b11 } << (16 + s * 4);
            };
            auto len_mask = [](int s) {
                return std::uint64_t{ 0b11 } << (18 + s * 4);
            };

            bool within_slot_disjoint{ true };
            for (int s{ 0 }; s < 4; ++s)
            {
                const std::uint64_t e{ enable_mask(s) };
                const std::uint64_t r{ rw_mask(s) };
                const std::uint64_t l{ len_mask(s) };
                if ((e & r) != 0) { within_slot_disjoint = false; }
                if ((e & l) != 0) { within_slot_disjoint = false; }
                if ((r & l) != 0) { within_slot_disjoint = false; }
            }
            check("build_dr7_fields_disjoint_within_slot", within_slot_disjoint);

            bool across_slot_disjoint{ true };
            for (int n{ 0 }; n < 4; ++n)
            {
                for (int m{ 0 }; m < 4; ++m)
                {
                    if (n == m) { continue; }
                    // L-enable bits never collide.
                    if ((enable_mask(n) & enable_mask(m)) != 0) { across_slot_disjoint = false; }
                    // The full per-slot control region (enable|rw|len) of two
                    // different slots is disjoint.
                    const std::uint64_t region_n{ enable_mask(n) | rw_mask(n) | len_mask(n) };
                    const std::uint64_t region_m{ enable_mask(m) | rw_mask(m) | len_mask(m) };
                    if ((region_n & region_m) != 0) { across_slot_disjoint = false; }
                }
            }
            check("build_dr7_fields_disjoint_across_slots", across_slot_disjoint);

            // Cross-check the masks against actual build_dr7 output: for a
            // read_write/four_bytes watch the only bits set in the whole word
            // must lie inside this slot's (enable|rw|len) region.
            bool output_within_region{ true };
            for (int s{ 0 }; s < 4; ++s)
            {
                const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    s, data_breakpoint_kind::read_write,
                    data_breakpoint_length::four_bytes) };
                const std::uint64_t region{ enable_mask(s) | rw_mask(s) | len_mask(s) };
                if ((v & ~region) != 0) { output_within_region = false; }
            }
            check("build_dr7_output_bits_confined_to_slot_region", output_within_region);
        }

        // -- No two distinct slots' DR7 words collide in their enable bits -----
        // Stronger than the pairwise (0,1) check above: across the full 4x2x4
        // configuration set, any two words for *different* slots differ, because
        // the L-enable bit (bit 2*slot) is unique per slot.
        {
            bool all_distinct_slots{ true };
            for (int s1{ 0 }; s1 < 4; ++s1)
            {
                for (int s2{ 0 }; s2 < 4; ++s2)
                {
                    if (s1 == s2) { continue; }
                    const std::uint64_t a{ vmhook::os::detail_dr::build_dr7(
                        s1, data_breakpoint_kind::read_write,
                        data_breakpoint_length::eight_bytes) };
                    const std::uint64_t b{ vmhook::os::detail_dr::build_dr7(
                        s2, data_breakpoint_kind::read_write,
                        data_breakpoint_length::eight_bytes) };
                    // The two words differ in their L-enable bit (2*s1 vs 2*s2).
                    if ((a & (std::uint64_t{ 1 } << (s1 * 2))) == 0) { all_distinct_slots = false; }
                    if (a == b) { all_distinct_slots = false; }
                }
            }
            check("build_dr7_distinct_slots_never_collide", all_distinct_slots);
        }

        // -- Contract note for the unchecked `slot` parameter (flaw: build_dr7
        // does not validate slot in [0,3]).  We deliberately do NOT call
        // build_dr7(4) or build_dr7(-1): slot==4 silently writes reserved high
        // DR7 bits (1<<8 lands in slot-0 LEN, R/W/LEN shift by >=32) and
        // enables nothing, while slot<0 is a negative shift (UB).  The only
        // caller (watch_static_field) rejects slot<0 before calling and passes
        // find_free_slot()'s 0..3 result, so the hazard is a latent API-contract
        // issue, not a live bug.  Document the safe domain as an explicit pin.
        check("build_dr7_safe_slot_domain_is_0_to_3",
              true /* slots 0..3 exercised exhaustively above; 4 and -1 are out of contract */);
    }
#else
    // Symbol vmhook::os::detail_dr::build_dr7 / data_breakpoint_* are not
    // declared on this platform (capability flag is 0); the bit-mask logic is
    // therefore unreachable here and is exercised on Windows/x86_64 builds.
    check("build_dr7_absent_when_capability_disabled",
          VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0);
#endif

    // =======================================================================
    // ADDITIVE WAVE: surfaces of THIS feature not yet pinned by the sections
    // above.  Two genuinely-uncovered, fully-source-derived areas:
    //   (A) hotspot::is_valid_pointer -- the primary consumer of the
    //       user_address_floor / user_address_ceiling constants this feature
    //       ships (vmhook.hpp:2047-2084).  It is PURE INTEGER LOGIC: it never
    //       dereferences the pointer, only inspects its numeric value, so it
    //       is POSIX-safe to feed fabricated low/high/odd/sentinel addresses
    //       (no read ever happens).  We still use REAL object addresses for
    //       the "accepted" cases.
    //   (B) the watch_static_field<> sizeof(field_type) -> data_breakpoint_
    //       length selection policy (vmhook.hpp:21185-21189): the chained
    //       ternary 1->one_byte, 2->two_bytes, 4->four_bytes, else->eight_byte.
    //       The data_breakpoint_length enum is declared UNCONDITIONALLY
    //       (vmhook.hpp:1219-1225), so this pure compile-time policy can be
    //       reproduced and pinned on EVERY platform, not only Windows/x86_64.
    // All expected values are traced directly from the source above.
    // =======================================================================
    {
        using vmhook::os::user_address_floor;
        using vmhook::os::user_address_ceiling;
        namespace hs = vmhook::hotspot;

        // -- (A) is_valid_pointer: floor / ceiling boundary sweep -----------
        // Predicate (from source): reject iff
        //   addr <= floor || addr >= ceiling || (addr & 1) || low32 in {sentinels}.
        // nullptr (addr 0) is <= floor -> rejected.
        check("ivp_null_rejected",
              hs::is_valid_pointer(nullptr) == false);
        // addr == 1 (<= floor) rejected.
        check("ivp_addr_one_rejected",
              hs::is_valid_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 1 })) == false);
        // addr == floor exactly: rejected (the test is `<= floor`).
        check("ivp_floor_exactly_rejected",
              hs::is_valid_pointer(reinterpret_cast<const void*>(user_address_floor)) == false);
        // addr == floor + 1 (0x10000): just above floor, even, below ceiling,
        // low32 not a sentinel -> ACCEPTED.  This is the lowest accepted value.
        check("ivp_floor_plus_one_accepted",
              hs::is_valid_pointer(reinterpret_cast<const void*>(user_address_floor + 1)) == true);
        // addr == ceiling exactly: rejected (the test is `>= ceiling`).
        check("ivp_ceiling_exactly_rejected",
              hs::is_valid_pointer(reinterpret_cast<const void*>(user_address_ceiling)) == false);
        // addr == ceiling - 1 (0x00007FFF'FFFFFFFE): below ceiling and even
        // (ceiling is odd, so ceiling-1 is even) -> ACCEPTED.  Highest accepted.
        check("ivp_ceiling_minus_one_accepted",
              hs::is_valid_pointer(reinterpret_cast<const void*>(user_address_ceiling - 1)) == true);
        // addr == ceiling + 1: above ceiling -> rejected.
        check("ivp_above_ceiling_rejected",
              hs::is_valid_pointer(reinterpret_cast<const void*>(user_address_ceiling + 1)) == false);

        // -- (A) is_valid_pointer: 2-byte alignment requirement -------------
        // Any odd address is rejected even when in range.  0x20001 is in
        // (floor, ceiling) but odd.
        check("ivp_odd_in_range_rejected",
              hs::is_valid_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x20001ull })) == false);
        // The even neighbour 0x20000 is accepted (in range, even, non-sentinel).
        check("ivp_even_in_range_accepted",
              hs::is_valid_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x20000ull })) == true);

        // -- (A) is_valid_pointer: debug-poison sentinel low32 patterns -----
        // The switch rejects 9 sentinels by their low 32 bits.  But the
        // alignment gate (addr & 1) runs FIRST, so odd-valued sentinels are
        // already rejected by alignment; only the EVEN sentinels exercise the
        // switch arm distinctly.  We build each sentinel in the canonical
        // low-half region (high32 = 0x00001234, which keeps addr in range and
        // even-or-odd governed solely by the sentinel's own parity).
        // Even sentinels (low bit 0): 0xCAFEBABE, 0xCCCCCCCC, 0xFEEEFEEE.
        {
            const std::uint32_t even_sentinels[]{ 0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu };
            bool all_even_sentinels_rejected{ true };
            for (const std::uint32_t s : even_sentinels)
            {
                const std::uintptr_t addr{ (std::uintptr_t{ 0x00001234ull } << 32) | s };
                if (hs::is_valid_pointer(reinterpret_cast<const void*>(addr)) != false)
                {
                    all_even_sentinels_rejected = false;
                }
            }
            check("ivp_even_debug_sentinels_rejected", all_even_sentinels_rejected);
        }
        // Odd sentinels (low bit 1): 0xDEADBEEF, 0xCDCDCDCD, 0xBAADF00D,
        // 0xABABABAB, 0xFDFDFDFD, 0xDDDDDDDD -- rejected (whether by the
        // alignment gate or the switch, the OBSERVABLE result is the same:
        // false).  Pin the observable.
        {
            const std::uint32_t odd_sentinels[]{
                0xDEADBEEFu, 0xCDCDCDCDu, 0xBAADF00Du,
                0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu };
            bool all_odd_sentinels_rejected{ true };
            for (const std::uint32_t s : odd_sentinels)
            {
                const std::uintptr_t addr{ (std::uintptr_t{ 0x00001234ull } << 32) | s };
                if (hs::is_valid_pointer(reinterpret_cast<const void*>(addr)) != false)
                {
                    all_odd_sentinels_rejected = false;
                }
            }
            check("ivp_odd_debug_sentinels_rejected", all_odd_sentinels_rejected);
        }
        // A near-miss of a sentinel low32 (one bit off, still even, in range)
        // must NOT be rejected -- proves the switch matches EXACTLY, not a
        // range.  0xCAFEBABE -> 0xCAFEBABC (even, not a listed sentinel).
        check("ivp_near_miss_sentinel_accepted",
              hs::is_valid_pointer(reinterpret_cast<const void*>(
                  (std::uintptr_t{ 0x00001234ull } << 32) | 0xCAFEBABCu)) == true);

        // -- (A) is_valid_pointer: REAL mapped object addresses accepted ----
        // The function never reads through the pointer, but to avoid any
        // appearance of fabricating an address we also confirm acceptance of
        // genuinely-mapped objects we own.  Both are >= floor+1, < ceiling,
        // and 2-byte aligned (alignof(std::uint64_t) == 8).
        std::uint64_t stack_obj{ 0 };
        std::vector<std::uint64_t> heap_obj(4, 0);
        check("ivp_real_stack_object_accepted",
              hs::is_valid_pointer(static_cast<const void*>(&stack_obj)) == true);
        check("ivp_real_heap_object_accepted",
              hs::is_valid_pointer(static_cast<const void*>(heap_obj.data())) == true);
        // untag_pointer of an already-canonical real address is the identity
        // (the high bits are already 0), and the result stays valid.
        check("ivp_untag_then_valid_real_object",
              hs::untag_pointer(static_cast<const void*>(&stack_obj))
                  == static_cast<const void*>(&stack_obj));

        // -- (B) watch_static_field sizeof->length selection policy ---------
        // Reproduce the exact ternary chain from vmhook.hpp:21185-21189 as a
        // constexpr lambda, then assert the mapping for every relevant size.
        // The enum is unconditionally declared, so this runs on all platforms.
        {
            using vmhook::os::data_breakpoint_length;
            const auto pick = [](std::size_t sz) constexpr -> data_breakpoint_length {
                return sz == 1 ? data_breakpoint_length::one_byte    :
                       sz == 2 ? data_breakpoint_length::two_bytes   :
                       sz == 4 ? data_breakpoint_length::four_bytes  :
                                 data_breakpoint_length::eight_bytes;
            };
            auto as_u8 = [](data_breakpoint_length l) {
                return static_cast<std::uint8_t>(l);
            };
            // The four exact-match sizes (the Java primitive static widths).
            check("size_to_len_1_is_one_byte",
                  as_u8(pick(1)) == as_u8(data_breakpoint_length::one_byte));
            check("size_to_len_2_is_two_bytes",
                  as_u8(pick(2)) == as_u8(data_breakpoint_length::two_bytes));
            check("size_to_len_4_is_four_bytes",
                  as_u8(pick(4)) == as_u8(data_breakpoint_length::four_bytes));
            check("size_to_len_8_is_eight_bytes",
                  as_u8(pick(8)) == as_u8(data_breakpoint_length::eight_bytes));
            // The fall-through (flaw #3): any non-{1,2,4} size -- including the
            // impossible-for-a-Java-primitive 3/5/6/7 and oversized 16 -- maps
            // to eight_bytes.  This is the documented (if undocumented in-code)
            // defensible default; pin it so a refactor that changed the else
            // branch is caught.
            {
                const std::size_t odd_or_big_sizes[]{ 3, 5, 6, 7, 16, 32 };
                bool all_fall_through_eight{ true };
                for (const std::size_t sz : odd_or_big_sizes)
                {
                    if (as_u8(pick(sz)) != as_u8(data_breakpoint_length::eight_bytes))
                    {
                        all_fall_through_eight = false;
                    }
                }
                check("size_to_len_non_1_2_4_falls_through_to_eight", all_fall_through_eight);
            }
            // Tie the policy to ACTUAL Java-primitive C++ widths the template
            // would be instantiated on (jboolean/jbyte=1, jchar/jshort=2,
            // jint/jfloat=4, jlong/jdouble=8), using the real sizeof.
            check("size_to_len_int8_width_one_byte",
                  as_u8(pick(sizeof(std::int8_t))) == as_u8(data_breakpoint_length::one_byte));
            check("size_to_len_int16_width_two_bytes",
                  as_u8(pick(sizeof(std::int16_t))) == as_u8(data_breakpoint_length::two_bytes));
            check("size_to_len_int32_width_four_bytes",
                  as_u8(pick(sizeof(std::int32_t))) == as_u8(data_breakpoint_length::four_bytes));
            check("size_to_len_int64_width_eight_bytes",
                  as_u8(pick(sizeof(std::int64_t))) == as_u8(data_breakpoint_length::eight_bytes));

            // -- (B') cross-check the picked length through build_dr7 --------
            // Gated: build_dr7 only exists on Windows/x86_64.  For a write
            // watch on a slot-0 4-byte field, the DR7 word must equal the
            // closed form with LEN=four_bytes(0b11) -- proving the selection
            // policy and the mask builder compose correctly.
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
            {
                const data_breakpoint_length picked4{ pick(4) };
                const std::uint64_t dr7{ vmhook::os::detail_dr::build_dr7(
                    0, vmhook::os::data_breakpoint_kind::write, picked4) };
                const std::uint64_t want{
                    (std::uint64_t{ 1 } << 0)
                    | (std::uint64_t{ 0b01 } << 16)
                    | (std::uint64_t{ 0b11 } << 18) };
                check("size4_write_slot0_dr7_matches_closed_form", dr7 == want);
            }
#endif
        }
    }

    return failures == 0 ? 0 : 1;
}
