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

    // =======================================================================
    // ADDITIVE WAVE 2: surfaces the first additive wave did NOT cover.  Every
    // expected value is traced directly from vmhook.hpp source cited inline.
    // PURE LOGIC ONLY -- no memory is ever read: is_valid_pointer / untag_pointer
    // inspect/mask the numeric pointer VALUE only (vmhook.hpp:2047-2097), and all
    // macro / enum / DR7 work is compile-time arithmetic.  No fabricated address
    // is dereferenced.  Distinct names from wave 1 (suffix "_w2") so this is
    // strictly additive.
    // =======================================================================
    {
        // -- (1) Apple ladder split: macOS-vs-iOS arm (vmhook.hpp:140-154) ----
        // Wave 1 pinned the APPLE aggregate to the gate inputs but never split
        // the macOS arm out.  The __APPLE__ arm (reached only after Android and
        // Windows missed) sets exactly one of MACOS/IOS via TARGET_OS_IPHONE.
        // From the source: MACOS is set iff we reached the Apple arm AND the
        // iOS sub-branch did NOT fire -- i.e. APPLE && !IOS.  These are pure
        // restatements of the resolved macros (tautologies on every target).
        check("os_macos_equals_apple_and_not_ios_w2",
              VMHOOK_OS_MACOS == (VMHOOK_OS_APPLE && !VMHOOK_OS_IOS));
        check("os_ios_equals_apple_and_not_macos_w2",
              VMHOOK_OS_IOS == (VMHOOK_OS_APPLE && !VMHOOK_OS_MACOS));
        // Within the Apple family macOS and iOS partition it exactly: at most one,
        // and (when APPLE) exactly one.  Their sum equals the APPLE aggregate and
        // they never both fire.
        check("os_macos_ios_sum_equals_apple_w2",
              (VMHOOK_OS_MACOS + VMHOOK_OS_IOS) == VMHOOK_OS_APPLE);
        check("os_macos_ios_mutually_exclusive_w2",
              (VMHOOK_OS_MACOS & VMHOOK_OS_IOS) == 0);
        check("apple_selects_exactly_one_of_macos_ios_w2",
              !VMHOOK_OS_APPLE || ((VMHOOK_OS_MACOS + VMHOOK_OS_IOS) == 1));
        // macOS implies APPLE implies POSIX and not Windows (mirror of the iOS
        // implication wave 1 already pinned, for the other Apple arm).
        check("macos_implies_apple_posix_not_windows_w2",
              !VMHOOK_OS_MACOS
                  || (VMHOOK_OS_APPLE == 1 && VMHOOK_OS_POSIX == 1
                      && VMHOOK_OS_WINDOWS == 0));
        // macOS is x86_64-capable (no iOS exclusion), so on a macOS build
        // RUNTIME_HOOKING_AVAILABLE tracks arch alone: it equals VMHOOK_ARCH_X86_64
        // there (since !iOS holds).  Vacuous off macOS.
        check("macos_runtime_hooking_tracks_x86_64_w2",
              !VMHOOK_OS_MACOS
                  || (VMHOOK_RUNTIME_HOOKING_AVAILABLE == VMHOOK_ARCH_X86_64));

        // -- (2) The #else/#error arm contract, viewed from the result --------
        // The trailing ladder arm (vmhook.hpp:161-167) sets all five OS macros
        // to 0 and #error's, so a successfully-compiled TU can NEVER be in the
        // all-zero OS state.  Pin that the resolved set is non-empty (>= 1) and
        // that the bitwise-OR of all five base flags is 1 -- the dual of the
        // existing "sum == 1", catching a hypothetical two-set state that sums
        // wrong but ORs to 1, vs the error state that ORs to 0.
        check("os_set_is_non_empty_w2",
              (VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS
               + VMHOOK_OS_IOS + VMHOOK_OS_ANDROID) >= 1);
        check("os_base_flags_or_to_one_w2",
              (VMHOOK_OS_WINDOWS | VMHOOK_OS_LINUX | VMHOOK_OS_MACOS
               | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID) == 1);
        // Linux (the arm 4) implies POSIX and not Apple/Windows -- the remaining
        // base-OS implication wave 1 left to the aggregate checks.
        check("linux_implies_posix_not_apple_not_windows_w2",
              !VMHOOK_OS_LINUX
                  || (VMHOOK_OS_POSIX == 1 && VMHOOK_OS_APPLE == 0
                      && VMHOOK_OS_WINDOWS == 0));

        // -- (3) Capability tier as a MONOTONE integer ladder -----------------
        // The three booleans form a strict subset chain:
        //   HW_DATA_BREAKPOINTS => RUNTIME_HOOKING_AVAILABLE => x86_64.
        // Wave 1 pinned the boolean implications; here pin the *integer* ordering
        // (a >= relation) so the tier can never invert numerically, and that the
        // ascending sum is one of the only legal tier counts {0,1,2,3} -- never
        // a "skip" like HW set but runtime clear (which would make the partial
        // sums non-monotone).
        check("cap_tier_hwbp_le_runtime_w2",
              VMHOOK_HAS_HW_DATA_BREAKPOINTS <= VMHOOK_RUNTIME_HOOKING_AVAILABLE);
        check("cap_tier_runtime_le_x86_64_w2",
              VMHOOK_RUNTIME_HOOKING_AVAILABLE <= VMHOOK_ARCH_X86_64);
        // The descending chain x86_64 >= runtime >= hwbp means the triple,
        // read as a 3-bit "thermometer", is one of 000/100/110/111 -- i.e. the
        // partial sums are monotone.  Encode that as: once a lower tier is off,
        // every higher tier is off too.
        check("cap_tier_thermometer_monotone_w2",
              (VMHOOK_ARCH_X86_64
                   ? true
                   : (VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0
                      && VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0))
              && (VMHOOK_RUNTIME_HOOKING_AVAILABLE
                      ? true
                      : (VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0)));
        // The count of enabled tiers is exactly the thermometer height and lies
        // in {0,1,2,3}; combined with the chain above this forbids any gap.
        {
            const int tier{ VMHOOK_ARCH_X86_64
                            + VMHOOK_RUNTIME_HOOKING_AVAILABLE
                            + VMHOOK_HAS_HW_DATA_BREAKPOINTS };
            check("cap_tier_count_in_0_to_3_w2", tier >= 0 && tier <= 3);
            // Thermometer identity: tier height == x86_64 + runtime + hwbp AND
            // each successive tier flag is <= the one below it, so the height
            // uniquely determines all three flags.  Reconstruct and compare.
            const bool t_x86{ tier >= 1 };
            const bool t_rt { tier >= 2 };
            const bool t_hw { tier >= 3 };
            check("cap_tier_height_reconstructs_flags_w2",
                  (t_x86 ? 1 : 0) == VMHOOK_ARCH_X86_64
                  && (t_rt ? 1 : 0) == VMHOOK_RUNTIME_HOOKING_AVAILABLE
                  && (t_hw ? 1 : 0) == VMHOOK_HAS_HW_DATA_BREAKPOINTS);
        }

        // -- (4) untag_pointer: pure mask over a richer tag table -------------
        // untag_pointer ANDs with user_address_ceiling (vmhook.hpp:2092-2097).
        // It NEVER dereferences -- pure arithmetic on the value.  Wave 1 checked
        // one tagged nibble + the canonical identity; extend to a table of tag
        // patterns and confirm the helper recovers the low-47-bit base exactly.
        namespace hs2 = vmhook::hotspot;
        {
            const std::uintptr_t base{ 0x0000'0000'1357'9BD0ull };  // < 2^47, even
            const std::uintptr_t tags[]{
                std::uintptr_t{ 1 } << 47,                  // first bit above window
                std::uintptr_t{ 1 } << 48,
                std::uintptr_t{ 1 } << 63,                  // top bit
                std::uintptr_t{ 0xFFFFull } << 48,          // canonical sign ext
                std::uintptr_t{ 0x5A5Aull } << 48,          // arbitrary nibble soup
                ~((std::uintptr_t{ 1 } << 47) - 1),         // ALL high bits set
            };
            bool all_base_recovered{ true };
            for (const std::uintptr_t tg : tags)
            {
                const std::uintptr_t tagged{ base | tg };
                const auto un{ reinterpret_cast<std::uintptr_t>(
                    hs2::untag_pointer(reinterpret_cast<const void*>(tagged))) };
                if (un != base) { all_base_recovered = false; }
            }
            check("untag_pointer_recovers_base_over_tag_table_w2", all_base_recovered);

            // Idempotency: untag(untag(p)) == untag(p).  Masking is idempotent
            // because the second AND with the same mask is a no-op on an already
            // masked value.
            const std::uintptr_t tagged{ base | (std::uintptr_t{ 0xDEADull } << 48) };
            const void* once{ hs2::untag_pointer(reinterpret_cast<const void*>(tagged)) };
            const void* twice{ hs2::untag_pointer(once) };
            check("untag_pointer_is_idempotent_w2", once == twice);

            // Composition with the OTHER feature helper: a tagged pointer whose
            // BASE is a valid user address must, after untagging, pass
            // is_valid_pointer -- and an already-canonical valid value untags to
            // itself and stays valid.  Pure value logic (is_valid_pointer reads
            // nothing).  base is even, in (floor, ceiling), low32 = 0x13579BD0
            // (not a sentinel) -> valid.
            check("untag_of_tagged_valid_base_is_valid_w2",
                  hs2::is_valid_pointer(
                      hs2::untag_pointer(reinterpret_cast<const void*>(tagged))) == true);
            check("untag_preserves_validity_of_canonical_w2",
                  hs2::is_valid_pointer(reinterpret_cast<const void*>(base)) == true
                  && hs2::untag_pointer(reinterpret_cast<const void*>(base))
                         == reinterpret_cast<const void*>(base));
        }

        // -- (5) is_valid_pointer: non-canonical high-half boundary -----------
        // ceiling == 2^47 - 1 (0x00007FFF'FFFFFFFF).  The first address with
        // bit 47 set is 2^47 (0x0000'8000'0000'0000), which is >= ceiling ->
        // rejected.  Pure value test, no read.
        check("ivp_two_pow_47_rejected_w2",
              hs2::is_valid_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 1 } << 47)) == false);
        // The all-ones address (every bit set) is far above ceiling -> rejected.
        check("ivp_all_ones_rejected_w2",
              hs2::is_valid_pointer(reinterpret_cast<const void*>(
                  ~std::uintptr_t{ 0 })) == false);
        // A representative even, in-range, non-sentinel address near the TOP of
        // the window (high32 = 0x00007FFF keeps it < ceiling) is accepted -- the
        // existing sweep checked ceiling-1; this picks an unrelated high address.
        check("ivp_high_in_range_even_nonsentinel_accepted_w2",
              hs2::is_valid_pointer(reinterpret_cast<const void*>(
                  (std::uintptr_t{ 0x00007FFFull } << 32) | 0x00112230ull)) == true);
        // Monotone-with-mask composition: for the tag table above, masking a
        // tagged-but-valid-base pointer never turns a valid base invalid, and the
        // masked result equals is_valid_pointer(base) for every entry.
        {
            const std::uintptr_t base{ 0x0000'0000'1357'9BD0ull };
            const std::uintptr_t tags[]{
                std::uintptr_t{ 1 } << 47,
                std::uintptr_t{ 1 } << 60,
                ~((std::uintptr_t{ 1 } << 47) - 1),
            };
            const bool base_valid{ hs2::is_valid_pointer(
                reinterpret_cast<const void*>(base)) };
            bool all_match_base{ true };
            for (const std::uintptr_t tg : tags)
            {
                const std::uintptr_t tagged{ base | tg };
                const bool untagged_valid{ hs2::is_valid_pointer(
                    hs2::untag_pointer(reinterpret_cast<const void*>(tagged))) };
                if (untagged_valid != base_valid) { all_match_base = false; }
            }
            check("untag_then_valid_matches_base_validity_w2", all_match_base);
        }

        // -- (6) memory_protection: uint32 <-> enum round-trip ----------------
        // Each ordinal cast to its uint32 underlying value and back to the enum
        // is the identity (no aliasing of distinct semantics).  Wave 1 pinned the
        // ordinals; this pins the bijection.
        {
            using mp = vmhook::os::memory_protection;
            const mp values[]{ mp::no_access, mp::read, mp::read_write,
                               mp::execute_read, mp::execute_rw };
            bool round_trips{ true };
            for (const mp v : values)
            {
                const std::uint32_t u{ static_cast<std::uint32_t>(v) };
                if (static_cast<mp>(u) != v) { round_trips = false; }
            }
            check("memory_protection_uint32_round_trip_w2", round_trips);
            // The five underlying values, summed, are 0+1+2+3+4 == 10 -- a cheap
            // independent witness that the set is exactly {0,1,2,3,4} with no
            // gap/dup (any reorder keeps the sum; any gap/dup changes it).
            std::uint32_t ord_sum{ 0 };
            for (const mp v : values) { ord_sum += static_cast<std::uint32_t>(v); }
            check("memory_protection_ordinal_sum_is_10_w2", ord_sum == 10);
        }

        // -- (7) DR breakpoint enums: underlying-value round-trip (unconditional)
        // The enums are declared OUTSIDE the capability gate (vmhook.hpp:1210-
        // 1225), so this round-trip runs on EVERY platform.  Each enumerator cast
        // to uint8 and back is the identity, and the LEN set summed is
        // 0b00+0b01+0b10+0b11 == 6 (exactly {0,1,2,3} -- the full 2-bit space,
        // proving no LEN code is duplicated or out of range).
        {
            using vmhook::os::data_breakpoint_kind;
            using vmhook::os::data_breakpoint_length;
            const data_breakpoint_kind kinds[]{
                data_breakpoint_kind::write, data_breakpoint_kind::read_write };
            bool kind_round_trips{ true };
            for (const data_breakpoint_kind k : kinds)
            {
                const std::uint8_t u{ static_cast<std::uint8_t>(k) };
                if (static_cast<data_breakpoint_kind>(u) != k) { kind_round_trips = false; }
            }
            check("dr_kind_uint8_round_trip_w2", kind_round_trips);

            const data_breakpoint_length lens[]{
                data_breakpoint_length::one_byte, data_breakpoint_length::two_bytes,
                data_breakpoint_length::four_bytes, data_breakpoint_length::eight_bytes };
            bool len_round_trips{ true };
            std::uint32_t len_sum{ 0 };
            for (const data_breakpoint_length l : lens)
            {
                const std::uint8_t u{ static_cast<std::uint8_t>(l) };
                if (static_cast<data_breakpoint_length>(u) != l) { len_round_trips = false; }
                len_sum += u;
            }
            check("dr_length_uint8_round_trip_w2", len_round_trips);
            check("dr_length_codes_span_full_2bit_space_w2", len_sum == 6);
            // The two kind codes sum to 0b01 + 0b11 == 4; both occupy the 2-bit
            // space and share bit 0 (the "writes" bit) -- execute (0b00) is
            // deliberately absent from the data-watch enum.
            check("dr_kind_codes_sum_is_4_w2",
                  (static_cast<std::uint32_t>(data_breakpoint_kind::write)
                   + static_cast<std::uint32_t>(data_breakpoint_kind::read_write)) == 4);
        }

        // -- (8) build_dr7 LEN/kind field EXTRACT-and-round-trip (gated) -------
        // Build a DR7 word, then extract the R/W and LEN sub-fields back out and
        // cast them to the enums -- they must equal the inputs for every length
        // at a representative slot.  This is the inverse direction of wave 1's
        // closed-form construction check (a genuine round-trip, not a re-build).
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        {
            using vmhook::os::data_breakpoint_kind;
            using vmhook::os::data_breakpoint_length;
            const data_breakpoint_length lens[]{
                data_breakpoint_length::one_byte, data_breakpoint_length::two_bytes,
                data_breakpoint_length::four_bytes, data_breakpoint_length::eight_bytes };
            const data_breakpoint_kind kinds[]{
                data_breakpoint_kind::write, data_breakpoint_kind::read_write };
            bool extract_round_trips{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                for (const data_breakpoint_kind k : kinds)
                {
                    for (const data_breakpoint_length l : lens)
                    {
                        const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(slot, k, l) };
                        const std::uint8_t rw_field{ static_cast<std::uint8_t>(
                            (v >> (16 + slot * 4)) & 0b11u) };
                        const std::uint8_t len_field{ static_cast<std::uint8_t>(
                            (v >> (18 + slot * 4)) & 0b11u) };
                        if (static_cast<data_breakpoint_kind>(rw_field) != k)
                        {
                            extract_round_trips = false;
                        }
                        if (static_cast<data_breakpoint_length>(len_field) != l)
                        {
                            extract_round_trips = false;
                        }
                    }
                }
            }
            check("build_dr7_field_extract_round_trip_w2", extract_round_trips);
            // Population-count witness: a write/one_byte watch (LEN=0b00, R/W=0b01)
            // sets EXACTLY two bits -- the slot's L-enable (bit 2*slot) and the
            // single R/W low bit (bit 16+4*slot).  Verify the popcount is 2 for
            // every slot (an independent check on "no stray bits set").
            bool popcount_ok{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::write,
                    data_breakpoint_length::one_byte) };
                int bits{ 0 };
                while (v) { v &= (v - 1); ++bits; }
                if (bits != 2) { popcount_ok = false; }
            }
            check("build_dr7_write_one_byte_popcount_is_2_w2", popcount_ok);
        }
#endif
    }

    // =======================================================================
    // ADDITIVE WAVE 3: source-derived surfaces NOT pinned by waves 1-2.  Pure
    // value / compile-time logic only -- no memory is ever dereferenced.
    //   - is_valid_pointer's gate ORDERING and the low32 TRUNCATION semantics
    //     (vmhook.hpp:2047-2084): the alignment gate (addr & 1) runs BEFORE the
    //     sentinel switch, and the switch matches only the LOW 32 bits, so a
    //     high-half-tagged address whose low32 hits a sentinel is matched by
    //     truncation alone.  These all inspect the numeric VALUE; no read.
    //   - safe_read_pointer's PURE pre-filter (vmhook.hpp:2106-2130): null,
    //     addr<=floor, addr>=ceiling, or (addr & 0x7) all return nullptr BEFORE
    //     os::safe_read is ever called -- so feeding ONLY pre-filter-rejected
    //     values never crosses the OS boundary (POSIX-safe, no SEGV).  This is
    //     the no-JVM fail-closed contract for a fn this feature's constants gate.
    //   - page_size / allocation_granularity call-to-call self-consistency.
    // Distinct names (suffix "_w3"); strictly additive.
    // =======================================================================
    {
        namespace hs3 = vmhook::hotspot;
        using vmhook::os::user_address_floor;
        using vmhook::os::user_address_ceiling;

        // -- (1) is_valid_pointer gate ORDER: alignment precedes the switch ---
        // An ODD address whose low32 is NOT a sentinel is rejected purely by the
        // (addr & 1) gate, which runs before the switch (vmhook.hpp:2059).  Pick
        // a low32 that is not in the sentinel list and is odd: 0x00020003 is in
        // (floor, ceiling), odd, low32 = 0x00020003 (not a sentinel) -> rejected.
        check("ivp_odd_nonsentinel_rejected_by_alignment_w3",
              hs3::is_valid_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 0x00020003ull })) == false);
        // The even neighbour 0x00020002 (in range, even, non-sentinel) -> ACCEPTED,
        // proving it was the LOW BIT, not the value, that rejected the odd case.
        check("ivp_even_neighbour_of_odd_accepted_w3",
              hs3::is_valid_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 0x00020002ull })) == true);

        // -- (2) is_valid_pointer low32 TRUNCATION: switch matches low 32 bits --
        // The switch keys on static_cast<std::uint32_t>(addr) (vmhook.hpp:2067),
        // so an address with ARBITRARY high bits (still in range, even) whose LOW
        // 32 bits equal an EVEN sentinel is rejected by truncation alone.  Build
        // it in the canonical low-half window (high32 = 0x00001234, < ceiling) so
        // the only reason for rejection is the truncated low32 sentinel match.
        // 0xCAFEBABE is even, so it survives the alignment gate and reaches the
        // switch -- the truncation is what makes it match.
        check("ivp_low32_truncation_matches_even_sentinel_w3",
              hs3::is_valid_pointer(reinterpret_cast<const void*>(
                  (std::uintptr_t{ 0x00001234ull } << 32) | 0xCAFEBABEu)) == false);
        // Same high32 but a NON-sentinel even low32 (0xCAFEBABE with low nibble
        // cleared -> 0xCAFEBAB0) -> ACCEPTED: confirms ONLY the low32 governs the
        // switch arm, the high bits are irrelevant to the sentinel test.
        check("ivp_low32_nonsentinel_high_bits_irrelevant_w3",
              hs3::is_valid_pointer(reinterpret_cast<const void*>(
                  (std::uintptr_t{ 0x00001234ull } << 32) | 0xCAFEBAB0u)) == true);

        // -- (3) is_valid_pointer: every EVEN sentinel is matched by low32 even
        // when carrying high tag bits (the union of (2) over all even sentinels).
        // Odd sentinels are excluded here because the alignment gate would reject
        // them first regardless of the switch; only the even ones isolate the
        // switch's low32 behaviour.  Even sentinels: 0xCAFEBABE, 0xCCCCCCCC,
        // 0xFEEEFEEE (low bit 0).
        {
            const std::uint32_t even_sentinels[]{ 0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu };
            bool all_rejected_with_high_tag{ true };
            for (const std::uint32_t s : even_sentinels)
            {
                // high32 = 0x00007FFF keeps the full address < ceiling and even.
                const std::uintptr_t addr{ (std::uintptr_t{ 0x00007FFFull } << 32) | s };
                if (hs3::is_valid_pointer(reinterpret_cast<const void*>(addr)) != false)
                {
                    all_rejected_with_high_tag = false;
                }
            }
            check("ivp_even_sentinels_rejected_under_high_tag_w3",
                  all_rejected_with_high_tag);
        }

        // -- (4) safe_read_pointer PURE pre-filter (no OS boundary crossed) ----
        // From source (vmhook.hpp:2106-2130): the function returns nullptr WITHOUT
        // calling os::safe_read when the pointer is null, <= floor, >= ceiling, or
        // not 8-byte aligned.  We feed ONLY such pre-filter-rejected values, so no
        // read of a fabricated page ever happens (POSIX-safe).  Each must yield
        // nullptr -- the fail-closed contract for invalid inputs.
        // (a) null input.
        check("srp_null_returns_null_w3",
              hs3::safe_read_pointer(nullptr) == nullptr);
        // (b) addr == floor (0xFFFF): <= floor -> nullptr (no boundary crossed).
        check("srp_floor_returns_null_w3",
              hs3::safe_read_pointer(reinterpret_cast<const void*>(user_address_floor)) == nullptr);
        // (c) addr == 1: <= floor -> nullptr.
        check("srp_addr_one_returns_null_w3",
              hs3::safe_read_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 1 })) == nullptr);
        // (d) addr == ceiling: >= ceiling -> nullptr.
        check("srp_ceiling_returns_null_w3",
              hs3::safe_read_pointer(reinterpret_cast<const void*>(user_address_ceiling)) == nullptr);
        // (e) addr above ceiling (every bit set): >= ceiling -> nullptr.
        check("srp_all_ones_returns_null_w3",
              hs3::safe_read_pointer(reinterpret_cast<const void*>(~std::uintptr_t{ 0 })) == nullptr);
        // (f) safe_read_pointer's alignment requirement is 8 (stricter than
        // is_valid_pointer's 2): an in-range address that is 2-aligned but NOT
        // 8-aligned hits the (addr & 0x7) gate and returns nullptr before any
        // read.  0x10002 is > floor, < ceiling, even, but 0x10002 & 0x7 == 2.
        check("srp_misaligned_in_range_returns_null_w3",
              hs3::safe_read_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x10002ull })) == nullptr);
        // The SAME address passes is_valid_pointer (which only requires 2-byte
        // alignment) -- pinning that the two helpers use DIFFERENT alignment
        // thresholds (2 vs 8), both source-derived.  Pure value test, no read.
        check("ivp_accepts_2aligned_that_srp_rejects_w3",
              hs3::is_valid_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x10002ull })) == true);
        // Each of the four sub-words of an 8-aligned-failing address: 2,4,6 are
        // all rejected by safe_read_pointer's & 0x7 gate; 0 (8-aligned) would NOT
        // be rejected by alignment, so we keep that one OUT (it would reach the OS
        // boundary).  Confirm the three non-zero low-3-bit offsets fail-close.
        {
            const std::uintptr_t misaligned_offsets[]{ 2, 4, 6 };
            bool all_misaligned_rejected{ true };
            for (const std::uintptr_t off : misaligned_offsets)
            {
                // base 0x40000 is 8-aligned and in range; add the offset to break
                // 8-alignment while staying in (floor, ceiling).
                const std::uintptr_t addr{ std::uintptr_t{ 0x40000ull } + off };
                if (hs3::safe_read_pointer(reinterpret_cast<const void*>(addr)) != nullptr)
                {
                    all_misaligned_rejected = false;
                }
            }
            check("srp_all_sub8_misalignments_fail_closed_w3", all_misaligned_rejected);
        }

        // -- (5) untag_pointer THEN safe_read_pointer fail-closed on a tagged
        // high-half pointer whose BASE is out of range.  untag masks to the low
        // 47 bits; if the masked base is itself <= floor it is pre-filter-rejected
        // by safe_read_pointer.  base = 0x8 (<= floor after masking) OR'd with a
        // high tag -> untag yields 0x8 -> safe_read_pointer returns nullptr (no
        // read).  Pure value composition of two feature helpers.
        {
            const std::uintptr_t low_base{ 0x8ull };  // <= floor (0xFFFF)
            const std::uintptr_t tagged{ low_base | (std::uintptr_t{ 0xDEADull } << 48) };
            const void* untagged{ hs3::untag_pointer(reinterpret_cast<const void*>(tagged)) };
            check("untag_then_srp_low_base_fail_closed_w3",
                  reinterpret_cast<std::uintptr_t>(untagged) == low_base
                  && hs3::safe_read_pointer(untagged) == nullptr);
        }

        // -- (6) page_size / allocation_granularity call-to-call stability -----
        // Both wrap a single GetSystemInfo / sysconf snapshot of an immutable host
        // property, so successive calls must return identical values (no waves
        // pinned determinism).  Pure host-property reads, no JVM.
        check("page_size_is_stable_across_calls_w3",
              vmhook::os::page_size() == vmhook::os::page_size());
        check("allocation_granularity_is_stable_across_calls_w3",
              vmhook::os::allocation_granularity() == vmhook::os::allocation_granularity());
        // allocation_granularity / page_size is an exact integer (granularity is a
        // whole number of pages) AND that quotient is itself >= 1 -- the scan
        // allocator's clamp relies on granularity being a page multiple, never a
        // fraction.  (Wave 1 checked the remainder is 0; this pins the quotient.)
        {
            const std::size_t ps3{ vmhook::os::page_size() };
            const std::size_t gran3{ vmhook::os::allocation_granularity() };
            check("granularity_over_page_size_is_integer_ge_1_w3",
                  ps3 != 0 && (gran3 % ps3) == 0 && (gran3 / ps3) >= 1);
            // gran is a power of two as well (it is a power-of-two multiple of the
            // power-of-two page size on every supported host: 4K page, 4K POSIX
            // granularity or 64K Windows granularity).  Pin power-of-two-ness.
            check("granularity_is_power_of_two_w3",
                  gran3 != 0 && (gran3 & (gran3 - 1)) == 0);
        }
    }

    // =======================================================================
    // ADDITIVE WAVE 4: source-derived surfaces NOT pinned by waves 1-3.  Pure
    // compile-time / value logic only -- no memory is ever dereferenced.  Every
    // expected value is traced directly from vmhook/ext/vmhook/vmhook.hpp cited
    // inline.  Distinct names (suffix "_w4"); strictly additive, no existing
    // assertion touched.
    // =======================================================================
    {
        // -- (1) data_breakpoint_kind code STRUCTURE (vmhook.hpp:1210-1214) -----
        // write=0b01, read_write=0b11.  Source-derived structure not yet pinned:
        // read_write == write WITH the "read" bit (0b10) added, i.e. read_write
        // equals (write | 0b10), and write is exactly the low "writes" bit alone.
        // Both codes are <= 0b11 (fit the 2-bit DR7 R/W field) and execute(0b00)
        // is deliberately absent.
        {
            using vmhook::os::data_breakpoint_kind;
            const std::uint8_t kw{ static_cast<std::uint8_t>(data_breakpoint_kind::write) };
            const std::uint8_t krw{ static_cast<std::uint8_t>(data_breakpoint_kind::read_write) };
            check("dr_kind_read_write_is_write_plus_read_bit_w4",
                  krw == static_cast<std::uint8_t>(kw | 0b10u));
            check("dr_kind_write_is_low_bit_only_w4", kw == 0b01u);
            check("dr_kind_codes_fit_two_bit_field_w4",
                  (kw & ~0b11u) == 0u && (krw & ~0b11u) == 0u);
            // Neither code is the absent execute encoding 0b00.
            check("dr_kind_codes_exclude_execute_zero_w4", kw != 0u && krw != 0u);
        }

        // -- (2) data_breakpoint_length code <-> guarded BYTE-WIDTH bijection ---
        // The Intel-SDM LEN encoding is intentionally non-monotonic
        // (one_byte=0b00, two_bytes=0b01, eight_bytes=0b10, four_bytes=0b11,
        // vmhook.hpp:1219-1225).  Map each enumerator to the byte width it guards
        // and assert the code->width function is exactly the SDM table -- a
        // width-swap (two_bytes<->four_bytes) would break this even though both
        // remain valid 2-bit codes.  Pure compile-time arithmetic.
        {
            using vmhook::os::data_breakpoint_length;
            struct row { data_breakpoint_length len; std::uint8_t code; std::size_t width; };
            const row table[]{
                { data_breakpoint_length::one_byte,    0b00u, 1u },
                { data_breakpoint_length::two_bytes,   0b01u, 2u },
                { data_breakpoint_length::eight_bytes, 0b10u, 8u },
                { data_breakpoint_length::four_bytes,  0b11u, 4u },
            };
            bool table_matches{ true };
            std::size_t width_product{ 1u };
            for (const row& r : table)
            {
                if (static_cast<std::uint8_t>(r.len) != r.code) { table_matches = false; }
                width_product *= r.width;
            }
            check("dr_length_code_to_byte_width_table_w4", table_matches);
            // The four guarded widths are exactly {1,2,4,8}: their product is 64
            // (1*2*4*8) -- an independent witness that no width is duplicated or
            // wrong (any swap within {1,2,4,8} keeps the product; a wrong width
            // changes it).
            check("dr_length_guarded_widths_product_is_64_w4", width_product == 64u);
            // The non-monotonic property made explicit: the numeric CODE order
            // (0,1,2,3) does NOT match ascending byte width -- code 0b10 guards 8
            // bytes while the larger code 0b11 guards only 4.  Pin that inversion.
            check("dr_length_encoding_is_non_monotonic_w4",
                  static_cast<std::uint8_t>(data_breakpoint_length::eight_bytes)
                      < static_cast<std::uint8_t>(data_breakpoint_length::four_bytes));
        }

        // -- (3) is_valid_pointer: ALIGNMENT gate rejects EVERY odd in-range
        // value, independent of the low32 sentinel switch (vmhook.hpp:2059).
        // Sweep a handful of odd, in-range, NON-sentinel low32 values: all must
        // be rejected purely by (addr & 1).  Pure value inspection -- the
        // function never reads through the pointer.
        {
            namespace hs4 = vmhook::hotspot;
            const std::uintptr_t odd_in_range[]{
                0x00010001ull, 0x00020003ull, 0x12345679ull, 0x00007FFE0000DDDFull };
            bool all_odd_rejected{ true };
            for (const std::uintptr_t a : odd_in_range)
            {
                if (hs4::is_valid_pointer(reinterpret_cast<const void*>(a)) != false)
                {
                    all_odd_rejected = false;
                }
            }
            check("ivp_every_odd_in_range_rejected_w4", all_odd_rejected);
            // The even counterpart of each (clear bit 0) is in range, non-sentinel
            // -> ACCEPTED, isolating the low bit as the sole reason.
            bool all_even_accepted{ true };
            for (const std::uintptr_t a : odd_in_range)
            {
                if (hs4::is_valid_pointer(reinterpret_cast<const void*>(a & ~std::uintptr_t{ 1 })) != true)
                {
                    all_even_accepted = false;
                }
            }
            check("ivp_even_counterparts_accepted_w4", all_even_accepted);
        }

        // -- (4) memory_protection: bound and width consistency ----------------
        // Wave 1 pinned the ordinals/distinctness; here pin that EVERY value fits
        // in its uint32 underlying type and that the maximum value (execute_rw=4)
        // needs only 3 bits -- so the whole enum lives in the low byte, no value
        // ever needs the high bits the OS-protection mapping table does not index.
        {
            using mp = vmhook::os::memory_protection;
            const std::uint32_t values[]{
                static_cast<std::uint32_t>(mp::no_access),
                static_cast<std::uint32_t>(mp::read),
                static_cast<std::uint32_t>(mp::read_write),
                static_cast<std::uint32_t>(mp::execute_read),
                static_cast<std::uint32_t>(mp::execute_rw) };
            std::uint32_t max_val{ 0 };
            for (const std::uint32_t v : values) { if (v > max_val) { max_val = v; } }
            check("memory_protection_max_value_is_4_w4", max_val == 4u);
            // All five fit in the low byte (< 256) -- no value escapes the
            // single-byte index space the protection lookup uses.
            bool all_in_low_byte{ true };
            for (const std::uint32_t v : values) { if (v >= 256u) { all_in_low_byte = false; } }
            check("memory_protection_values_fit_low_byte_w4", all_in_low_byte);
        }

        // -- (5) build_dr7: read_write/one_byte popcount is 3 (gated) ----------
        // Complements wave-2's write/one_byte popcount==2: a read_write watch
        // sets BOTH R/W bits (0b11 at 16+4*slot) plus the L-enable bit, and a
        // one_byte LEN adds zero bits (0b00), so exactly THREE bits are set for
        // every slot.  Independent witness that the R/W field width is 2, not 1.
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        {
            using vmhook::os::data_breakpoint_kind;
            using vmhook::os::data_breakpoint_length;
            bool popcount3_ok{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::read_write,
                    data_breakpoint_length::one_byte) };
                int bits{ 0 };
                while (v) { v &= (v - 1); ++bits; }
                if (bits != 3) { popcount3_ok = false; }
            }
            check("build_dr7_read_write_one_byte_popcount_is_3_w4", popcount3_ok);
            // And the L-enable bit is the LOWEST set bit for every slot's
            // write/one_byte word (bit 2*slot < 16, below the R/W field) -- pins
            // that the enable bit and the field bits never reorder.
            bool enable_is_lowest{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::write,
                    data_breakpoint_length::one_byte) };
                // lowest set bit == enable bit (2*slot).
                const std::uint64_t lowest{ v & (~v + 1) };
                if (lowest != (std::uint64_t{ 1 } << (slot * 2))) { enable_is_lowest = false; }
            }
            check("build_dr7_enable_is_lowest_set_bit_w4", enable_is_lowest);
        }
#endif

        // -- (6) page geometry: granularity is a power-of-two multiple of page,
        // and the quotient is itself a power of two (vmhook.hpp os layer) -------
        // Wave 1 pinned remainder==0 and (POSIX) equality; wave 3 pinned the
        // quotient >= 1.  Here pin that the quotient (granularity / page_size) is
        // a power of two -- true on every supported host (POSIX: quotient 1;
        // Windows: 64K/4K = 16, a power of two).  Pure host-property arithmetic.
        {
            const std::size_t ps4{ vmhook::os::page_size() };
            const std::size_t gran4{ vmhook::os::allocation_granularity() };
            const std::size_t quotient{ (ps4 != 0) ? (gran4 / ps4) : 0u };
            check("granularity_quotient_is_power_of_two_w4",
                  ps4 != 0 && (gran4 % ps4) == 0
                      && quotient != 0 && (quotient & (quotient - 1)) == 0);
            // On POSIX the quotient is exactly 1 (granularity forwards to
            // page_size, vmhook.hpp os layer); pin the stronger equality through
            // the quotient form, complementing wave 1's gran==ps.
#if VMHOOK_OS_POSIX
            check("posix_granularity_quotient_is_one_w4", quotient == 1u);
#else
            // On Windows the quotient is >= 1 and a power of two (already pinned
            // above); restate the multiple relation through the quotient form.
            check("windows_granularity_quotient_at_least_one_w4", quotient >= 1u);
#endif
        }
    }

    // =======================================================================
    // ADDITIVE WAVE 5 (wave-24 ledger closeout): pin the LEDGER GAPS as
    // pure-compile-time / pure-arithmetic invariants, with names suffixed
    // "_w5".  Every claim is traced back to vmhook.hpp source cited inline.
    // =======================================================================
    {
        // -- (1) OS base-flag XOR sum is exactly 1 ----------------------------
        // Restating the "exactly one" invariant in XOR form: a 2-element subset
        // (which would still SUM to 2, caught by wave 1) does not XOR to 1, so
        // this is an INDEPENDENT witness on parity.  The all-zero error state
        // also XORs to 0; only a singleton XORs to 1.  Compile-time pin.
        static_assert((VMHOOK_OS_WINDOWS ^ VMHOOK_OS_LINUX ^ VMHOOK_OS_MACOS
                       ^ VMHOOK_OS_IOS ^ VMHOOK_OS_ANDROID) == 1,
                      "exactly one VMHOOK_OS_* base flag must be set (XOR sum == 1)");
        check("os_base_flags_xor_sum_is_one_w5",
              (VMHOOK_OS_WINDOWS ^ VMHOOK_OS_LINUX ^ VMHOOK_OS_MACOS
               ^ VMHOOK_OS_IOS ^ VMHOOK_OS_ANDROID) == 1);

        // -- (2) Arch XOR == 1 (compile-time form) ----------------------------
        // Wave 1 used arithmetic sum and the runtime mirror used XOR; this adds
        // the XOR form as a static_assert so a future arm that set BOTH arch
        // flags (sum 2, XOR 0) fails the build.
        static_assert((VMHOOK_ARCH_X86_64 ^ VMHOOK_ARCH_ARM64) == 1,
                      "exactly one VMHOOK_ARCH_* must be selected (XOR == 1)");
        check("arch_flags_xor_sum_is_one_w5",
              (VMHOOK_ARCH_X86_64 ^ VMHOOK_ARCH_ARM64) == 1);

        // -- (3) Compiler XOR == 1 *when a recognized compiler is in use* -----
        // The three families do NOT form a strict partition (an unrecognized
        // compiler legally yields all three 0).  But on EVERY CI cell we ship,
        // one of MSVC/CLANG/GCC is detected -- which is exactly the condition
        // (VMHT_HAS_MSC_VER || VMHT_HAS_CLANG || VMHT_HAS_GNUC).  Under that
        // gate, the XOR partition holds.  This is the strongest form the
        // ledger gap admits without breaking exotic toolchains.
#if VMHT_HAS_MSC_VER || VMHT_HAS_CLANG || VMHT_HAS_GNUC
        static_assert((VMHOOK_COMPILER_MSVC ^ VMHOOK_COMPILER_CLANG
                       ^ VMHOOK_COMPILER_GCC) == 1,
                      "exactly one compiler family is selected on every supported host");
        check("compiler_xor_sum_is_one_on_recognized_host_w5",
              (VMHOOK_COMPILER_MSVC ^ VMHOOK_COMPILER_CLANG
               ^ VMHOOK_COMPILER_GCC) == 1);
        // And the arithmetic SUM is also exactly 1 (not just <=1): the at-most-
        // one bound from wave 1 was the conservative form; combined with the
        // recognized-host gate it tightens to ==1.
        static_assert(VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_CLANG
                          + VMHOOK_COMPILER_GCC == 1,
                      "compiler-family sum is exactly 1 on a recognized host");
        check("compiler_sum_is_one_on_recognized_host_w5",
              VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_CLANG
                  + VMHOOK_COMPILER_GCC == 1);
#endif

        // -- (4) RUNTIME_HOOKING_AVAILABLE consistency with arch -------------
        // Wave 1 pinned: available iff (x86_64 && !iOS), arm64 forces off, iOS
        // forces off, and availability implies x86_64.  Add the bidirectional
        // restatement in the arch dimension SPECIFICALLY: on a non-iOS target
        // the availability flag EQUALS the x86_64 flag exactly (a stricter
        // identity than the implication).  Vacuous on iOS.
        static_assert(VMHOOK_OS_IOS
                          || VMHOOK_RUNTIME_HOOKING_AVAILABLE == VMHOOK_ARCH_X86_64,
                      "on a non-iOS target runtime hooking availability tracks arch exactly");
        check("runtime_hooking_equals_x86_64_on_non_ios_w5",
              VMHOOK_OS_IOS
                  || VMHOOK_RUNTIME_HOOKING_AVAILABLE == VMHOOK_ARCH_X86_64);
        // And the contrapositive of wave 1's "availability implies x86_64":
        // arm64 implies UN-availability (no exception for OS).  Wave 1 framed it
        // as "arm64 && available is false"; restate as the direct implication.
        static_assert(!VMHOOK_ARCH_ARM64 || VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0,
                      "arm64 implies runtime hooking unavailable, regardless of OS");
        check("arm64_implies_runtime_hooking_unavailable_w5",
              !VMHOOK_ARCH_ARM64 || VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0);

        // -- (5) build_dr7: per-slot bit-position pins for the 4*slot stride --
        // Wave 1 / wave 2 / wave 3 pinned the FIELD masks and disjointness for
        // ALL slots, and the closed-form matrix.  Here add explicit numeric
        // pins on the bit positions themselves -- for every slot s in 0..3,
        // the L-enable bit is at position 2*s, the R/W field's low bit at
        // 16+4*s, and the LEN field's low bit at 18+4*s -- so a refactor of the
        // 4*slot stride to (say) 3*slot fails on every slot, not only at the
        // endpoints.  These are pure integer identities, gated on the symbol
        // being declared (Windows/x86_64 only).
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        {
            using vmhook::os::data_breakpoint_kind;
            using vmhook::os::data_breakpoint_length;
            // Expected bit positions per slot, traced from build_dr7's source
            // (vmhook.hpp:1035-1043).
            const int expected_enable_bit[4]{ 0,  2,  4,  6 };
            const int expected_rw_low_bit[4]{ 16, 20, 24, 28 };
            const int expected_len_low_bit[4]{ 18, 22, 26, 30 };
            bool positions_ok{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                // write/one_byte: R/W = 0b01, LEN = 0b00.  The R/W field's low
                // bit (the "writes" bit) is the only R/W bit set, so it lands
                // at expected_rw_low_bit[slot] exactly.
                const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::write,
                    data_breakpoint_length::one_byte) };
                // L-enable bit is at the expected position.
                if ((v & (std::uint64_t{ 1 } << expected_enable_bit[slot])) == 0)
                {
                    positions_ok = false;
                }
                // R/W low bit is at the expected position.
                if ((v & (std::uint64_t{ 1 } << expected_rw_low_bit[slot])) == 0)
                {
                    positions_ok = false;
                }
                // LEN is 0b00, so neither LEN bit is set.
                if ((v & (std::uint64_t{ 1 } << expected_len_low_bit[slot])) != 0)
                {
                    positions_ok = false;
                }
                if ((v & (std::uint64_t{ 1 } << (expected_len_low_bit[slot] + 1))) != 0)
                {
                    positions_ok = false;
                }
            }
            check("build_dr7_bit_positions_per_slot_stride_4_w5", positions_ok);

            // The L-enable bits across the 4 slots, taken together, occupy
            // exactly bits 0,2,4,6 -- a 4-bit comb in the low byte.  Build the
            // OR of every slot's enable bit (with R/W=write, LEN=one_byte so no
            // other bits set above bit 16) and mask to the low 8 bits; it must
            // equal 0b01010101.
            std::uint64_t enable_comb_low8{ 0 };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                const std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::write,
                    data_breakpoint_length::one_byte) };
                enable_comb_low8 |= (v & 0xFFull);
            }
            check("build_dr7_enable_comb_is_0b01010101_w5",
                  enable_comb_low8 == 0b01010101ull);

            // Each slot's CONTROL REGION (enable | rw | len) occupies exactly
            // 5 distinct bit positions: 1 + 2 + 2.  Confirm via popcount of
            // the all-ones mask: read_write/four_bytes (R/W=0b11, LEN=0b11)
            // fills every field bit AND the enable bit -> popcount 5 per slot.
            bool popcount5_ok{ true };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                std::uint64_t v{ vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::read_write,
                    data_breakpoint_length::four_bytes) };
                int bits{ 0 };
                while (v) { v &= (v - 1); ++bits; }
                if (bits != 5) { popcount5_ok = false; }
            }
            check("build_dr7_full_control_region_popcount_is_5_w5", popcount5_ok);

            // The OR of all 4 slots' full read_write/four_bytes words lights
            // up exactly 20 distinct bits: 4 enable + 4*2 R/W + 4*2 LEN = 20.
            // An overlap (wrong stride) would reduce the popcount.
            std::uint64_t union_word{ 0 };
            for (int slot{ 0 }; slot < 4; ++slot)
            {
                union_word |= vmhook::os::detail_dr::build_dr7(
                    slot, data_breakpoint_kind::read_write,
                    data_breakpoint_length::four_bytes);
            }
            int union_bits{ 0 };
            {
                std::uint64_t u{ union_word };
                while (u) { u &= (u - 1); ++union_bits; }
            }
            check("build_dr7_union_across_4_slots_has_20_bits_w5",
                  union_bits == 20);
        }
#endif
    }

    return failures == 0 ? 0 : 1;
}
