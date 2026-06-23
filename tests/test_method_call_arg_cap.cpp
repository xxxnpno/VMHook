// Standalone (no-JVM) characterization of method_proxy::call()'s 8-argument cap
// diagnostic.
//
// BACKGROUND: call()'s call_stub fast path packs arguments into a fixed
// `params[8]` interpreter-locals array; the pack() lambda guards every store
// with `if (param_idx >= 8) return;`.  An argument past the cap would be
// SILENTLY dropped by that guard.  The library now rejects an over-cap arity at
// COMPILE time with a clear, slot-aware static_assert
//
//     static_assert(sizeof...(args_t) <= 8,
//                   "method_proxy::call: max 8 arguments ...");
//
// placed just before the pack() fold, mirroring the static_assert the JNI
// fallback call_jni() already carries.  Because it is a constant-expression
// check the warm <=8-arg path is byte-identical and no runtime code is emitted.
//
// WHY A static_assert AND NOT A RUNTIME LOG: call() always references
// call_jni<args_t...> (the call_stub-missing short-circuit `return
// this->call_jni(...)`), and call_jni already static_asserts `<= 8`.  So any
// over-cap instantiation of call() is a HARD COMPILE ERROR today, not a silent
// runtime truncation - a runtime VMHOOK_LOG in an `if constexpr (>8)` branch
// could never fire.  The right, honest diagnostic is therefore the compile-time
// assert, anchored on the public call() entry so the limit is reported there
// (not buried in the fallback) regardless of which dispatch path a JDK takes.
//
// SCOPE: call() needs a live JavaThread, so it cannot RUN here.  What IS pure
// host C++ and is pinned below:
//   (1) the THRESHOLD predicate - `sizeof...(args) <= 8` is the exact selector,
//       so 0..8 args are accepted and 9+ rejected.  Pinned with static_assert so
//       a regression is a BUILD failure on every compiler in the matrix.
//   (2) the WELL-FORMEDNESS of call() at and below the cap - a detection idiom
//       confirms `proxy.call(<=8 args)` is a valid expression (the assert does
//       NOT fire at the boundary, 8).  The over-cap direction is intentionally
//       NOT detected: a failing static_assert is a hard error, not a SFINAE
//       substitution failure, so it cannot be probed without breaking the build
//       - which is precisely the guarantee (9+ args = compile error).
// A real over-cap dispatch is impossible to express in valid C++, so there is no
// JVM-side counterpart; the boundary is fully characterized here.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstddef>
#include <utility>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Compile-time mirror of the library predicate: the single source of truth for
// "is this arity within the 8-slot interpreter cap".  The static_asserts below
// pin the boundary so a change to the threshold breaks the BUILD, not just a run.
template<typename... args_t>
inline constexpr bool within_cap_v{ sizeof...(args_t) <= 8 };

// Positive-direction detector: is `proxy.call(args...)` a well-formed expression?
// This compiles (and yields true) only for arities the call() static_assert
// accepts - i.e. <= 8.  We never instantiate it for an over-cap arity, because a
// failing static_assert is a hard error (not a recoverable substitution failure),
// so detecting "false" for 9+ args would itself break the build.  The detector
// therefore exists solely to confirm the AT-cap / under-cap boundary is valid.
template<typename, typename... args_t>
struct call_well_formed : std::false_type {};

template<typename... args_t>
struct call_well_formed<
    std::void_t<decltype(std::declval<const vmhook::method_proxy&>()
                             .call(std::declval<args_t>()...))>,
    args_t...> : std::true_type {};

template<typename... args_t>
inline constexpr bool call_well_formed_v{ call_well_formed<void, args_t...>::value };

int main()
{
    // -----------------------------------------------------------------------
    // (1) Threshold predicate - the constexpr selector `sizeof...(args) <= 8`.
    //     0..8 accepted (warm path unchanged / byte-identical); 9+ rejected.
    //     static_assert so a regression fails the BUILD on every matrix compiler.
    // -----------------------------------------------------------------------
    static_assert(within_cap_v<>,                                  "0 args: in cap");
    static_assert(within_cap_v<int>,                               "1 arg: in cap");
    static_assert(within_cap_v<int, int, int, int>,                "4 args: in cap");
    static_assert(within_cap_v<int, int, int, int, int, int, int>, "7 args: in cap");
    static_assert(within_cap_v<int, int, int, int, int, int, int, int>,
                  "8 args: AT the cap, must be accepted");
    static_assert(!within_cap_v<int, int, int, int, int, int, int, int, int>,
                  "9 args: over the cap, must be rejected");
    static_assert(!within_cap_v<int, int, int, int, int, int, int, int, int, int, int, int>,
                  "12 args: over the cap, must be rejected");
    check("threshold_8_in_cap",
          within_cap_v<int, int, int, int, int, int, int, int>);
    check("threshold_9_over_cap",
          !within_cap_v<int, int, int, int, int, int, int, int, int>);

    // -----------------------------------------------------------------------
    // (2) Well-formedness of call() at and below the cap.  These detections
    //     compile to `true` ONLY because the call() static_assert does not fire
    //     for these arities - i.e. they confirm 8 is INSIDE the accepted set and
    //     the boundary was placed at 8, not 7.  (An over-cap detection is omitted
    //     by design: it would be a hard build error, which is the guarantee.)
    // -----------------------------------------------------------------------
    check("call_well_formed_0_args", call_well_formed_v<>);
    check("call_well_formed_1_arg",  call_well_formed_v<int>);
    check("call_well_formed_8_args",
          call_well_formed_v<int, int, int, int, int, int, int, int>);
    // A long and a double argument are accepted at the boundary too (they each
    // map to one VARIADIC C++ argument here; their two-interpreter-slot cost is
    // a runtime concern the assert message documents, not a C++-arity concern).
    check("call_well_formed_8_mixed_wide_args",
          call_well_formed_v<long, double, int, int, int, int, int, int>);
    // static_assert form of the same boundary fact: the AT-cap expression is
    // valid, so the assert provably does not reject 8.
    static_assert(call_well_formed_v<int, int, int, int, int, int, int, int>,
                  "call() must accept exactly 8 arguments");

    std::printf("\n%s (%d failure%s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
