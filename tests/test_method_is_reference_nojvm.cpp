// Standalone (no-JVM) unit test exhaustively pinning
// `vmhook::method_proxy::is_reference()` — the O(1), pure-metadata accessor
// that decides "is the resolved Java method's RETURN type a reference (L or
// [)?" purely from the cached descriptor string.
//
// The accessor never touches the `Method*`, never enters HotSpot, and never
// calls JNI, so we can drive its FULL truth table by HAND-CONSTRUCTING
// `method_proxy{nullptr, nullptr, std::string{"..."}}` proxies with crafted
// descriptors.  Every assertion below is deterministic, byte-identical
// across compilers/platforms/JDKs — no [INFO] gates, no platform variance.
//
// Coverage (HARD asserts only — no JVM, no live thread):
//   * Compile-time: return type `bool`, `noexcept`, parity with
//     field_proxy::is_reference() (both return bool, both noexcept).
//   * The 8 JVM primitive return descriptors (Z B S C I J F D)        -> false
//   * The `void` return (V)                                          -> false
//   * Reference returns: java/lang/{Object,String,Throwable,Class}    -> true
//   * Array returns: primitive 1-D (`[I` `[J` etc.) + reference 1-D
//     (`[Ljava/lang/Object;`) + multi-dim (`[[I`, `[[[Ljava/lang/String;`) -> true
//   * Defensive descriptors — empty / no-')' / ')' at end / two ')' /
//     leading-')' / args-only / nested-')' inside L<...>; — all return
//     false without faulting (no `Method*` deref because constructor
//     passes nullptr).
//   * Overload disambiguation by descriptor only: `dual(I)I`  -> false
//     vs `dual(Ljava/lang/Object;)Ljava/lang/Object;` -> true.
//
// This file complements tests/jvm/modules/method_is_reference.cpp (live JVM
// instance / static / overload paths) by exhaustively pinning the parser
// half of the contract with ZERO HotSpot dependency.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------
// Compile-time contracts.
// ---------------------------------------------------------------------

// (1) Return type is exactly `bool` — callers branch on it directly.
using is_ref_ret_t = decltype(std::declval<const vmhook::method_proxy&>().is_reference());
static_assert(std::is_same_v<is_ref_ret_t, bool>,
              "method_proxy::is_reference() must return bool");

// (2) noexcept — the docstring guarantees noexcept on malformed input.
static_assert(noexcept(std::declval<const vmhook::method_proxy&>().is_reference()),
              "method_proxy::is_reference() must be noexcept");

// (3) Callable on a const lvalue (it is a const member).
static_assert(std::is_invocable_r_v<bool,
                                    decltype(&vmhook::method_proxy::is_reference),
                                    const vmhook::method_proxy&>,
              "method_proxy::is_reference() must be invocable on const lvalue");

// (4) Parity with field_proxy::is_reference (same return type + noexcept).
//     Both accessors share a documented contract; pin them together so a
//     drift in one is caught at compile time.
using field_is_ref_ret_t = decltype(std::declval<const vmhook::field_proxy&>().is_reference());
static_assert(std::is_same_v<field_is_ref_ret_t, bool>,
              "field_proxy::is_reference() must return bool (parity)");
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().is_reference()),
              "field_proxy::is_reference() must be noexcept (parity)");

// (5) std::string_view literal-friendly construction (precondition for the
//     descriptor strings we feed to the constructor at runtime).
static_assert(std::is_constructible_v<std::string, const char*>,
              "std::string from C-literal must compile (descriptor build path)");

// Build a name-only proxy with an explicit descriptor.  Constructor body
// only stores members; with both pointer args null the accessor can never
// reach the `Method*`.
static auto make_proxy(std::string desc) -> vmhook::method_proxy
{
    return vmhook::method_proxy{ nullptr, nullptr, std::move(desc) };
}

// Independent oracle: re-derives the truth straight from the descriptor.
// Mirrors the documented contract (char after first ')').  Used as a
// cross-check — if the accessor agrees with the oracle on every input
// below, the parser logic and the documented contract are byte-identical.
static auto oracle(std::string_view sig) noexcept -> bool
{
    const auto close = sig.find(')');
    if (close == std::string_view::npos || close + 1 >= sig.size())
    {
        return false;
    }
    const char r = sig[close + 1];
    return r == 'L' || r == '[';
}

static auto pin(const char* label, const char* desc, bool expect) -> void
{
    const auto p = make_proxy(desc);
    const bool got = p.is_reference();
    const bool ok = (got == expect) && (oracle(desc) == expect);
    check(label, ok);
}

int main()
{
    // -----------------------------------------------------------------
    // Primitive returns — all 8 JVM primitive descriptor chars + void.
    // -----------------------------------------------------------------
    pin("prim_Z_boolean",            "()Z",                            false);
    pin("prim_B_byte",               "()B",                            false);
    pin("prim_S_short",              "()S",                            false);
    pin("prim_C_char",               "()C",                            false);
    pin("prim_I_int",                "()I",                            false);
    pin("prim_J_long",               "()J",                            false);
    pin("prim_F_float",              "()F",                            false);
    pin("prim_D_double",             "()D",                            false);
    pin("prim_V_void",               "()V",                            false);

    // -----------------------------------------------------------------
    // Reference returns — direct L<class>; for the common JDK roots.
    // -----------------------------------------------------------------
    pin("ref_Object",                "()Ljava/lang/Object;",           true);
    pin("ref_String",                "()Ljava/lang/String;",           true);
    pin("ref_Throwable",             "()Ljava/lang/Throwable;",        true);
    pin("ref_Class",                 "()Ljava/lang/Class;",            true);
    pin("ref_user_pkg",              "()Lcom/example/Widget;",         true);

    // -----------------------------------------------------------------
    // Array returns — both primitive element and reference element,
    // single and multi-dim.  Arrays ARE reference types in the JVM.
    // -----------------------------------------------------------------
    pin("arr_int",                   "()[I",                           true);
    pin("arr_long",                  "()[J",                           true);
    pin("arr_byte",                  "()[B",                           true);
    pin("arr_double",                "()[D",                           true);
    pin("arr_String",                "()[Ljava/lang/String;",          true);
    pin("arr_2d_int",                "()[[I",                          true);
    pin("arr_3d_String",             "()[[[Ljava/lang/String;",        true);

    // -----------------------------------------------------------------
    // Args present — char after first ')' is what matters.
    // -----------------------------------------------------------------
    pin("with_args_prim_ret",        "(I)I",                           false);
    pin("with_args_void_ret",        "(IJ)V",                          false);
    pin("with_args_ref_ret",         "(I)Ljava/lang/Object;",          true);
    pin("with_args_arr_ret",         "(Ljava/lang/String;)[I",         true);
    pin("with_args_2d_ret",          "(IJF)[[Ljava/lang/Object;",      true);

    // -----------------------------------------------------------------
    // Defensive — malformed descriptors must return false without UB.
    // The hand-built proxy has Method*=nullptr, so any erroneous deref
    // would trap; the accessor never touches it.
    // -----------------------------------------------------------------
    pin("def_empty",                 "",                               false);
    pin("def_no_close_paren",        "(",                              false);
    pin("def_no_close_paren_full",   "(IJ",                            false);
    pin("def_close_at_end",          "()",                             false);
    pin("def_close_at_end_args",     "(I)",                            false);
    pin("def_only_close",            ")",                              false);
    pin("def_leading_close_then_I",  ")I",                             false);  // close+1 == 'I' (primitive)
    pin("def_unknown_ret_char",      "()X",                            false);
    pin("def_unknown_ret_Q",         "()Q",                            false);

    // -----------------------------------------------------------------
    // Two ')' — the accessor uses FIND (first ')').  Pin it.
    //   "(L)I)Ljava/lang/Object;"
    //     ^first ')'-->'I'  (primitive) => is_reference()==false.
    //   Documented in the brief as a divergence vs the call_jni
    //   path's rfind-based `cached_ret_char`.  Real HotSpot
    //   descriptors only ever contain ONE ')' so this is unreachable
    //   in practice; pinning it locks the documented behaviour.
    // -----------------------------------------------------------------
    pin("two_close_parens_first_wins", "(L)I)Ljava/lang/Object;",      false);

    // -----------------------------------------------------------------
    // Overload disambiguation by descriptor — same NAME, different
    // is_reference().  This is the headline gotcha the JVM-test
    // module asserts live; we pin the parser side of it here.
    // -----------------------------------------------------------------
    {
        const auto prim_overload = make_proxy("(I)I");
        const auto ref_overload  = make_proxy("(Ljava/lang/Object;)Ljava/lang/Object;");
        check("overload_prim_is_not_reference", prim_overload.is_reference() == false);
        check("overload_ref_is_reference",       ref_overload.is_reference() == true);
        check("overload_distinct_truth",         prim_overload.is_reference() != ref_overload.is_reference());
    }

    // -----------------------------------------------------------------
    // Move / copy stability — is_reference() reads `signature_text`
    // by value, so the answer must SURVIVE a move/copy of the proxy.
    // -----------------------------------------------------------------
    {
        auto a = make_proxy("()Ljava/lang/String;");
        const bool before = a.is_reference();
        auto b = std::move(a);
        check("move_preserves_truth", b.is_reference() == before && before == true);

        auto c = make_proxy("()I");
        const bool before_prim = c.is_reference();
        auto d = std::move(c);
        check("move_preserves_truth_prim", d.is_reference() == before_prim && before_prim == false);
    }

    // -----------------------------------------------------------------
    // Independent-oracle agreement on a large permutation set — the
    // accessor isn't just self-consistent, it matches the documented
    // contract on every shape exercised above (already checked inline
    // by pin()).  This final sweep adds a few mixed-shape descriptors
    // a real fixture might emit.
    // -----------------------------------------------------------------
    constexpr const char* sweep[] = {
        "()Z", "()B", "()S", "()C", "()I", "()J", "()F", "()D", "()V",
        "()Ljava/lang/Object;", "()[I", "()[[Ljava/lang/String;",
        "(I)I", "(IJ)V", "(Ljava/lang/String;)Ljava/lang/Object;",
        "", "(", "()", "(I)", ")", ")I", "()X",
    };
    for (const char* s : sweep)
    {
        const auto p = make_proxy(s);
        const bool acc = p.is_reference();
        const bool orc = oracle(s);
        if (acc != orc)
        {
            std::printf("[FAIL] sweep mismatch desc=\"%s\" acc=%d orc=%d\n", s, acc, orc);
            ++failures;
        }
    }
    check("sweep_accessor_matches_oracle_on_all_inputs", failures == 0);

    std::printf("method_is_reference_nojvm: %s (failures=%d)\n",
                failures == 0 ? "OK" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
