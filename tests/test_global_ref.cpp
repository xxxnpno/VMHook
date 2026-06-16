// Standalone (no-JVM) unit test for vmhook::jni::global_ref and vmhook::pin().
//
// global_ref is the GC-pin lifetime primitive ported from the NPNOQOL fork:
// it keeps a Java object alive across relocating GCs and re-derives the live
// (relocated) address via oop().  The cross-GC behaviour itself needs a live
// JVM (covered by the JVM integration suite / tests/jvm/modules/global_ref.cpp);
// here we pin down the parts that are FULLY testable without a JVM:
//   * move-only semantics (copy is statically disabled, move is available),
//   * null / empty / no-JVM construction is safe and inert,
//   * a moved-from pin is empty, the moved-to pin owns the (empty) state,
//   * move-assign self-assignment doesn't corrupt state or double-free,
//   * chained / repeated moves never resurrect a handle,
//   * a default / empty global_ref is null / falsy and safe to destroy & reset,
//   * with no JVM, constructing from ANY non-null fake OOP stays empty
//     (NewGlobalRef cannot run: current_jni_env is null), and destruction /
//     reset / oop() / handle() / operator bool never crash,
//   * pin() free helpers (oop overload + unique_ptr<wrapper> overload) compile
//     and produce empty pins without a JVM, and round-trip through containers.
//
// WHY this is deterministic without a JVM:
//   global_ref's ctor calls vmhook::detail::jni_new_global_ref(local_handle),
//   which resolves slot 21 off vmhook::hotspot::current_jni_env.  In a plain
//   test process current_jni_env is null, so jni_function<21> returns null and
//   jni_new_global_ref returns nullptr -> handle_ stays null.  A null raw_oop is
//   rejected even earlier (ctor early-return).  Therefore EVERY no-JVM-reachable
//   global_ref has handle_ == nullptr, oop() == nullptr, !bool, and reset()/dtor
//   hit the documented null no-op in jni_delete_global_ref.  oop()'s mask-and-
//   deref branch is unreachable here (it requires a non-null handle), so no test
//   below ever dereferences a fabricated handle -- there is no UB in this file.
//
// Anything requiring a real pinned object that survives System.gc() is out of
// scope here -- see the live-JVM module tests/jvm/modules/global_ref.cpp.
#include <vmhook/vmhook.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Returns true iff `g` is in the canonical empty state across EVERY observable
// no-JVM accessor at once (operator bool, oop(), handle()).  Used to assert the
// "all three views agree" invariant after each lifecycle operation, so a bug
// that nulls one view but not another is caught.
static auto is_empty(const vmhook::jni::global_ref& g) -> bool
{
    return !static_cast<bool>(g) && g.oop() == nullptr && g.handle() == nullptr;
}

// A minimal wrapper to exercise the pin(unique_ptr<T>) overload's compile path.
namespace
{
    class dummy_wrapper : public vmhook::object<dummy_wrapper>
    {
    public:
        explicit dummy_wrapper(vmhook::oop_t instance) noexcept
            : vmhook::object<dummy_wrapper>{ instance }
        {
        }
    };

    // A SECOND, distinct wrapper type so the templated pin(unique_ptr<T>)
    // overload is instantiated for more than one T (guards against the helper
    // accidentally hard-coding a single wrapper type).
    class other_wrapper : public vmhook::object<other_wrapper>
    {
    public:
        using vmhook::object<other_wrapper>::object;
    };

    // Performs `dst = std::move(src)` behind a function boundary.  Passing the
    // SAME object as both arguments exercises global_ref's `this != &other`
    // self-assignment guard at runtime WITHOUT any compiler seeing a syntactic
    // self-move at the call site -- so clang's -Wself-move never fires under
    // -Werror (it is a purely syntactic diagnostic).  Mirrors the technique in
    // the sibling test_jni_forwarders.cpp.
    auto launder_move_assign(vmhook::jni::global_ref& dst,
                             vmhook::jni::global_ref& src) -> void
    {
        dst = std::move(src);
    }

    // Returns a global_ref BY VALUE built from the given OOP.  Used to drive the
    // return-by-value / NRVO / guaranteed-elision paths through a real function
    // boundary (a prvalue returned from a named local).  With no JVM the result
    // is always empty; the point is that the move-on-return path is well-formed
    // and leaves no armed handle behind.
    auto make_ref(const std::uintptr_t bits) -> vmhook::jni::global_ref
    {
        vmhook::jni::global_ref local{ reinterpret_cast<vmhook::oop_t>(bits) };
        return local;  // NRVO / implicit move of a local on return
    }

    // Convenience: turn a uintptr_t bit pattern into an oop_t at a call site.
    auto oop_of(const std::uintptr_t bits) noexcept -> vmhook::oop_t
    {
        return reinterpret_cast<vmhook::oop_t>(bits);
    }
}

int main()
{
    using vmhook::jni::global_ref;

    // ========================================================================
    // SECTION 1 -- Type properties: move-only, exactly (compile-time)
    // ========================================================================
    static_assert(!std::is_copy_constructible_v<global_ref>,
                  "global_ref must not be copy-constructible (single ownership)");
    static_assert(!std::is_copy_assignable_v<global_ref>,
                  "global_ref must not be copy-assignable (single ownership)");
    static_assert(std::is_move_constructible_v<global_ref>,
                  "global_ref must be move-constructible");
    static_assert(std::is_move_assignable_v<global_ref>,
                  "global_ref must be move-assignable");
    static_assert(std::is_nothrow_move_constructible_v<global_ref>,
                  "global_ref move ctor must be noexcept (vector relocation must not throw)");
    static_assert(std::is_nothrow_move_assignable_v<global_ref>,
                  "global_ref move assignment must be noexcept");
    static_assert(std::is_nothrow_destructible_v<global_ref>,
                  "global_ref destructor must be noexcept (RAII release must not throw)");
    static_assert(std::is_nothrow_default_constructible_v<global_ref>,
                  "global_ref default ctor must be noexcept (empty handle, no JNI call)");
    // The from-OOP ctor is the one JNI-touching constructor: pin it noexcept.
    static_assert(std::is_nothrow_constructible_v<global_ref, vmhook::oop_t>,
                  "global_ref(oop_t) must be noexcept");
    // The from-OOP ctor must be explicit: an oop_t (void*) must NOT implicitly
    // convert to a global_ref (that would silently pin in surprising contexts).
    static_assert(!std::is_convertible_v<vmhook::oop_t, global_ref>,
                  "global_ref(oop_t) must be explicit (no implicit pin from a raw OOP)");
    // operator bool is explicit: a global_ref must not implicitly convert to bool
    // / int (it would let `pin + 1` etc. compile).  It IS contextually usable.
    static_assert(!std::is_convertible_v<global_ref, bool>,
                  "operator bool must be explicit (no implicit bool conversion)");
    static_assert(std::is_constructible_v<bool, global_ref>,
                  "operator bool must still allow explicit/contextual bool conversion");
    // It is a final class (the header marks it `final`); pin that down.
    static_assert(std::is_final_v<global_ref>,
                  "global_ref is declared final");
    // Accessor return types are exactly as documented.
    static_assert(std::is_same_v<decltype(std::declval<const global_ref&>().oop()), vmhook::oop_t>,
                  "oop() returns vmhook::oop_t");
    static_assert(std::is_same_v<decltype(std::declval<const global_ref&>().handle()), void*>,
                  "handle() returns void*");
    static_assert(std::is_same_v<decltype(std::declval<global_ref&>().reset()), void>,
                  "reset() returns void");
    // oop() / handle() / operator bool are const-callable (usable on a const pin).
    static_assert(noexcept(std::declval<const global_ref&>().oop()),
                  "oop() must be noexcept");
    static_assert(noexcept(std::declval<const global_ref&>().handle()),
                  "handle() must be noexcept");
    static_assert(noexcept(static_cast<bool>(std::declval<const global_ref&>())),
                  "operator bool must be noexcept");
    static_assert(noexcept(std::declval<global_ref&>().reset()),
                  "reset() must be noexcept");
    check("move_only_type_traits", true);

    // pin() free-function return types: both overloads yield an owning global_ref
    // BY VALUE (never a reference / wrapper).  Pin the signatures.
    static_assert(
        std::is_same_v<decltype(vmhook::pin(std::declval<vmhook::oop_t>())), global_ref>,
        "pin(oop_t) must return a global_ref by value");
    static_assert(
        std::is_same_v<decltype(vmhook::pin(std::declval<const std::unique_ptr<dummy_wrapper>&>())),
                       global_ref>,
        "pin(unique_ptr<wrapper>&) must return a global_ref by value");
    static_assert(noexcept(vmhook::pin(std::declval<vmhook::oop_t>())),
                  "pin(oop_t) must be noexcept");
    check("pin_free_function_signatures", true);

    // ========================================================================
    // SECTION 1b -- DEEPER type properties: layout, triviality, swappability,
    //   constructibility/assignability value-category matrix (compile-time).
    //   These go beyond the basic move-only contract: they pin the EXACT
    //   special-member shape (user-provided, hence non-trivial), the thin-
    //   wrapper memory layout, swap support, and the precise set of argument
    //   value-categories the (move-only) ctor / assignment will and won't bind.
    // ========================================================================

    // -- Layout: global_ref is a thin, standard-layout wrapper around ONE void*
    //    handle.  Asserting sizeof/alignof RELATIVE to void* (never an absolute
    //    byte count) keeps this true on ILP32 and LP64 / LLP64 alike.
    static_assert(std::is_standard_layout_v<global_ref>,
                  "global_ref must be standard-layout (a single void* member, no vtable)");
    static_assert(sizeof(global_ref) == sizeof(void*),
                  "global_ref must be exactly one pointer wide (no hidden state)");
    static_assert(alignof(global_ref) == alignof(void*),
                  "global_ref must have pointer alignment (thin wrapper over void*)");
    // Not an aggregate (it has user-declared constructors / private members).
    static_assert(!std::is_aggregate_v<global_ref>,
                  "global_ref must not be an aggregate (it has user-declared ctors)");
    static_assert(!std::is_empty_v<global_ref>,
                  "global_ref is not empty (it stores a void* handle)");
    static_assert(!std::is_polymorphic_v<global_ref>,
                  "global_ref must not be polymorphic (no virtual functions)");

    // -- Triviality: EVERY special member that matters is user-provided, so the
    //    type is deliberately non-trivial in each of these axes.  A drift to a
    //    defaulted/trivial move or dtor (which would skip DeleteGlobalRef) is a
    //    real bug; pin the non-triviality so such a drift fails to compile.
    static_assert(!std::is_trivially_copyable_v<global_ref>,
                  "global_ref must NOT be trivially copyable (custom move + deleted copy + custom dtor)");
    static_assert(!std::is_trivial_v<global_ref>,
                  "global_ref must NOT be trivial");
    static_assert(!std::is_trivially_destructible_v<global_ref>,
                  "global_ref dtor is user-provided (it must run DeleteGlobalRef) -> not trivially destructible");
    static_assert(!std::is_trivially_move_constructible_v<global_ref>,
                  "global_ref move ctor is user-provided (steals + nulls source) -> not trivial");
    static_assert(!std::is_trivially_move_assignable_v<global_ref>,
                  "global_ref move assignment is user-provided (releases old, steals, nulls) -> not trivial");
    // Default ctor IS trivial in effect? No -- it is `= default` but the class
    // has a non-trivial dtor, so the type is not trivially-default-constructible.
    static_assert(!std::is_trivially_default_constructible_v<global_ref>,
                  "global_ref is not trivially default-constructible (non-trivial dtor present)");

    // -- Swappability: a move-only type that is nothrow-move-constructible AND
    //    nothrow-move-assignable is itself nothrow-swappable via std::swap.
    static_assert(std::is_swappable_v<global_ref>,
                  "global_ref must be swappable (move-constructible + move-assignable)");
    static_assert(std::is_nothrow_swappable_v<global_ref>,
                  "global_ref swap must be noexcept (both move ops are noexcept)");

    // -- Constructibility value-category matrix.  Move-construct binds an
    //    rvalue; copy-construct from an lvalue / const-lvalue is DELETED, so
    //    is_constructible from those must be false.
    static_assert(std::is_constructible_v<global_ref, global_ref&&>,
                  "global_ref must be constructible from an rvalue global_ref (move)");
    static_assert(!std::is_constructible_v<global_ref, global_ref&>,
                  "global_ref must NOT be constructible from a non-const lvalue (copy deleted)");
    static_assert(!std::is_constructible_v<global_ref, const global_ref&>,
                  "global_ref must NOT be constructible from a const lvalue (copy deleted)");
    // The from-OOP ctor accepts a std::nullptr_t (nullptr -> void* is fine) but
    // is explicit, so it is constructible-but-not-convertible from nullptr_t.
    static_assert(std::is_constructible_v<global_ref, std::nullptr_t>,
                  "global_ref must be explicitly constructible from nullptr_t (oop_t is void*)");
    static_assert(!std::is_convertible_v<std::nullptr_t, global_ref>,
                  "nullptr_t must NOT implicitly convert to global_ref (the OOP ctor is explicit)");
    // A bare int must NOT construct a global_ref: there is no int -> void*
    // implicit conversion, so the only candidate (the oop_t ctor) does not apply.
    static_assert(!std::is_constructible_v<global_ref, int>,
                  "global_ref must NOT be constructible from int (no int->oop_t conversion)");
    static_assert(!std::is_constructible_v<global_ref, std::uintptr_t>,
                  "global_ref must NOT be constructible from a raw uintptr_t (no integer->oop_t conversion)");

    // -- Assignability value-category matrix.  Move-assign binds an rvalue;
    //    copy-assign from an lvalue / const-lvalue is DELETED.
    static_assert(std::is_assignable_v<global_ref&, global_ref&&>,
                  "global_ref must be move-assignable from an rvalue");
    static_assert(!std::is_assignable_v<global_ref&, global_ref&>,
                  "global_ref must NOT be assignable from a non-const lvalue (copy-assign deleted)");
    static_assert(!std::is_assignable_v<global_ref&, const global_ref&>,
                  "global_ref must NOT be assignable from a const lvalue (copy-assign deleted)");
    // You cannot assign anything to a CONST global_ref (no member is const-
    // qualified for assignment); pin that a const lvalue is not an assign target.
    static_assert(!std::is_assignable_v<const global_ref&, global_ref&&>,
                  "a const global_ref must not be an assignment target");

    // -- Move ctor / move assign EXPRESSION noexcept (decltype-level, in
    //    addition to the is_nothrow_* traits above): constructing/assigning from
    //    std::move(...) must be a noexcept expression.
    static_assert(noexcept(global_ref{ std::declval<global_ref&&>() }),
                  "move-construction expression must be noexcept");
    static_assert(noexcept(std::declval<global_ref&>() = std::declval<global_ref&&>()),
                  "move-assignment expression must be noexcept");
    // std::swap on two global_refs must be a noexcept expression too.
    static_assert(noexcept(std::swap(std::declval<global_ref&>(), std::declval<global_ref&>())),
                  "std::swap(global_ref&, global_ref&) must be noexcept");

    // -- handle() / oop() / operator bool are usable on an rvalue pin as well as
    //    a const lvalue (they are const member functions -> callable on prvalues).
    static_assert(std::is_same_v<decltype(make_ref(0u).oop()), vmhook::oop_t>,
                  "oop() must be callable on an rvalue global_ref and yield oop_t");
    static_assert(std::is_same_v<decltype(make_ref(0u).handle()), void*>,
                  "handle() must be callable on an rvalue global_ref and yield void*");

    // -- pin(unique_ptr<T>)'s base-of contract: the wrappers we instantiate it
    //    with genuinely satisfy `is_base_of<object_base, T>` (so the in-template
    //    static_assert is exercised on the satisfying side for >1 type).
    static_assert(std::is_base_of_v<vmhook::object_base, dummy_wrapper>,
                  "dummy_wrapper must derive from object_base (pin(unique_ptr<T>) requirement)");
    static_assert(std::is_base_of_v<vmhook::object_base, other_wrapper>,
                  "other_wrapper must derive from object_base (pin(unique_ptr<T>) requirement)");

    // -- A container of global_ref keeps the move-only contract: the value type
    //    of a std::vector<global_ref> is exactly global_ref (not a copy-wrapped
    //    proxy), and the vector is move-constructible.
    //    NOTE: we deliberately do NOT assert !is_copy_constructible_v<vector<...>>
    //    -- std::vector's copy constructor is declared UNCONDITIONALLY (it is not
    //    SFINAE-constrained on the element type), so the trait reports `true` even
    //    though instantiating the copy of a vector<move-only> is ill-formed.  That
    //    is a property of std::vector's declaration, not of global_ref, so it
    //    belongs nowhere in this feature's contract.  std::optional, by contrast,
    //    DOES conditionally delete its copy members, so the optional asserts below
    //    faithfully reflect global_ref's move-only-ness.
    static_assert(std::is_same_v<std::vector<global_ref>::value_type, global_ref>,
                  "vector<global_ref>::value_type must be global_ref");
    static_assert(std::is_move_constructible_v<std::vector<global_ref>>,
                  "vector<global_ref> must be move-constructible");
    // std::optional<global_ref> preserves move-only-ness (optional conditionally
    // deletes its copy members based on the contained type).
    static_assert(!std::is_copy_constructible_v<std::optional<global_ref>>,
                  "optional<global_ref> must be move-only (no copy ctor)");
    static_assert(std::is_move_constructible_v<std::optional<global_ref>>,
                  "optional<global_ref> must be move-constructible");
    static_assert(std::is_nothrow_move_constructible_v<std::optional<global_ref>>,
                  "optional<global_ref> move must stay noexcept");
    check("deeper_type_traits", true);

    // ========================================================================
    // SECTION 2 -- Default construction is empty / inert
    // ========================================================================
    {
        global_ref empty{};
        check("default_constructed_is_falsy", !static_cast<bool>(empty));
        check("default_constructed_oop_is_null", empty.oop() == nullptr);
        check("default_constructed_handle_is_null", empty.handle() == nullptr);
        check("default_constructed_all_views_agree", is_empty(empty));

        // oop() is a pure accessor: calling it repeatedly must be stable and
        // must not mutate the (already empty) state.
        check("default_oop_is_idempotent",
              empty.oop() == nullptr && empty.oop() == nullptr && empty.oop() == empty.oop());
        check("default_handle_matches_oop_nullness",
              (empty.handle() == nullptr) == (empty.oop() == nullptr));

        empty.reset();  // reset on empty must be safe
        check("reset_on_empty_is_safe", is_empty(empty));

        empty.reset();  // reset is idempotent -- a second reset is still safe
        empty.reset();
        check("reset_is_idempotent_when_empty", is_empty(empty));
    }

    // A const default-constructed pin: every const accessor must work.
    {
        const global_ref const_empty{};
        check("const_default_is_falsy", !static_cast<bool>(const_empty));
        check("const_default_oop_is_null", const_empty.oop() == nullptr);
        check("const_default_handle_is_null", const_empty.handle() == nullptr);
    }

    // ========================================================================
    // SECTION 3 -- Null-OOP construction is empty / inert (no NewGlobalRef)
    // ========================================================================
    {
        global_ref from_null{ static_cast<vmhook::oop_t>(nullptr) };
        check("null_oop_construct_is_falsy", !static_cast<bool>(from_null));
        check("null_oop_construct_oop_is_null", from_null.oop() == nullptr);
        check("null_oop_construct_handle_is_null", from_null.handle() == nullptr);
        check("null_oop_construct_all_views_agree", is_empty(from_null));
        from_null.reset();
        check("null_oop_construct_reset_safe", is_empty(from_null));
    }

    // ========================================================================
    // SECTION 4 -- Non-null fake OOP with NO JVM: NewGlobalRef can't run.
    //   current_jni_env is null in this process, so jni_new_global_ref returns
    //   nullptr; the pin must end up EMPTY, never holding a bogus handle.
    //   We sweep several distinct fake addresses to make sure the result does
    //   not depend on the particular bit pattern of the (rejected) OOP.
    // ========================================================================
    {
        const std::uintptr_t fakes[]{
            0x1u, 0x8u, 0x1000u, 0xDEADBEEFu, 0xFFFFFFFFu,
            static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0)),  // all-ones
            static_cast<std::uintptr_t>(0x7u),  // low-bit-tagged-looking value
        };
        bool all_empty{ true };
        bool all_oop_null{ true };
        bool all_handle_null{ true };
        for (const std::uintptr_t bits : fakes)
        {
            auto* const fake_oop{ reinterpret_cast<vmhook::oop_t>(bits) };
            global_ref no_jvm{ fake_oop };
            all_empty       = all_empty && !static_cast<bool>(no_jvm);
            all_oop_null    = all_oop_null && (no_jvm.oop() == nullptr);
            all_handle_null = all_handle_null && (no_jvm.handle() == nullptr);
            // destructor runs here on an empty handle -> exercises the null
            // no-op path of jni_delete_global_ref each iteration -> no crash.
        }
        check("non_null_oop_without_jvm_is_empty", all_empty);
        check("non_null_oop_without_jvm_oop_is_null", all_oop_null);
        check("non_null_oop_without_jvm_handle_is_null", all_handle_null);
    }

    // reset() on a (no-JVM) pin built from a non-null OOP is still safe.
    {
        auto* const fake_oop{ reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0x4000)) };
        global_ref no_jvm{ fake_oop };
        no_jvm.reset();
        check("non_null_oop_without_jvm_reset_safe", is_empty(no_jvm));
        no_jvm.reset();  // idempotent again
        check("non_null_oop_without_jvm_reset_idempotent", is_empty(no_jvm));
    }

    // ========================================================================
    // SECTION 5 -- Move construction transfers ownership; source becomes empty
    // ========================================================================
    {
        global_ref a{};                       // empty (no JVM anyway)
        global_ref b{ std::move(a) };
        check("moved_from_is_falsy", !static_cast<bool>(a));            // NOLINT(bugprone-use-after-move)
        check("moved_from_oop_is_null", a.oop() == nullptr);           // NOLINT(bugprone-use-after-move)
        check("moved_from_handle_is_null", a.handle() == nullptr);     // NOLINT(bugprone-use-after-move)
        check("moved_from_all_views_agree", is_empty(a));              // NOLINT(bugprone-use-after-move)
        check("moved_to_is_consistent", is_empty(b));  // both empty here
    }

    // Move-construct from a no-JVM pin built off a non-null OOP: still both empty
    // (because the source was already empty), and no double-free at scope end.
    {
        auto* const fake_oop{ reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0x2000)) };
        global_ref a{ fake_oop };          // empty: NewGlobalRef no-op'd
        global_ref b{ std::move(a) };
        check("move_ctor_from_nonnull_oop_src_empty", is_empty(a));    // NOLINT(bugprone-use-after-move)
        check("move_ctor_from_nonnull_oop_dst_empty", is_empty(b));
    }

    // A moved-from pin is a valid object: it can be reset, re-checked, and
    // re-assigned-into afterwards (no resurrection of a stale handle).
    {
        global_ref a{};
        global_ref b{ std::move(a) };
        (void)b;
        a.reset();                                  // legal on a moved-from object
        check("moved_from_can_be_reset", is_empty(a));   // NOLINT(bugprone-use-after-move)
        a = global_ref{};                           // re-assign into moved-from
        check("moved_from_can_be_reassigned", is_empty(a));
    }

    // ========================================================================
    // SECTION 6 -- Move assignment: releases old, steals source, nulls source
    // ========================================================================
    {
        global_ref a{};
        global_ref b{};
        b = std::move(a);
        check("move_assign_source_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("move_assign_dest_consistent", is_empty(b));
    }

    // Move-assign returns *this (so `(c = std::move(b))` chains correctly), and
    // the reference it returns is the destination object itself.
    {
        global_ref a{};
        global_ref b{};
        global_ref& result{ (b = std::move(a)) };
        check("move_assign_returns_this", &result == &b);
        check("move_assign_returns_dest_state", is_empty(result));
    }

    // Move-assign over a (no-JVM) pin that itself was built from a non-null OOP:
    // the destination's prior handle is released (here: already null) before the
    // steal -- exercises the `jni_delete_global_ref(this->handle_)` line in the
    // assignment operator with both operands deterministically empty.
    {
        auto* const fake_a{ reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0x3000)) };
        auto* const fake_b{ reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0x5000)) };
        global_ref a{ fake_a };   // empty
        global_ref b{ fake_b };   // empty
        b = std::move(a);
        check("move_assign_nonnull_src_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("move_assign_nonnull_dst_empty", is_empty(b));
    }

    // ========================================================================
    // SECTION 7 -- Self-move-assign must not corrupt or double-free
    //   The operator is guarded by `if (this != &other)`, so a self-move is a
    //   no-op: it must NOT delete-then-steal-from-itself (which would null the
    //   handle).  We launder the self-reference through a pointer so the
    //   compiler can't see it is a self-move (silences -Wself-move while still
    //   exercising the runtime guard).
    // ========================================================================
    {
        global_ref a{};
        global_ref* const self{ &a };
        *self = std::move(a);   // guarded self-move on an empty pin
        check("self_move_assign_empty_safe", is_empty(a));
    }
    {
        auto* const fake_oop{ reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0x6000)) };
        global_ref a{ fake_oop };   // empty (no JVM)
        global_ref* const self{ &a };
        *self = std::move(a);   // guarded self-move; must not double-free
        check("self_move_assign_nonnull_oop_safe", is_empty(a));
    }
    // Repeated self-move-assign stays safe (no accumulating corruption).
    {
        global_ref a{};
        global_ref* const self{ &a };
        *self = std::move(a);
        *self = std::move(a);
        *self = std::move(a);
        check("self_move_assign_repeated_safe", is_empty(a));
    }

    // ========================================================================
    // SECTION 8 -- Chained / double moves never resurrect a handle
    // ========================================================================
    {
        global_ref a{};
        global_ref b{ std::move(a) };
        global_ref c{ std::move(b) };  // chain the move-ctor
        check("double_move_ctor_first_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("double_move_ctor_middle_empty", is_empty(b));  // NOLINT(bugprone-use-after-move)
        check("double_move_ctor_last_empty", is_empty(c));
    }
    {
        global_ref a{};
        global_ref b{};
        global_ref c{};
        c = std::move(b);
        b = std::move(a);  // move-assign chain in the other direction
        check("chained_move_assign_a_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("chained_move_assign_b_empty", is_empty(b));
        check("chained_move_assign_c_empty", is_empty(c));
    }
    // Mixed: move-construct, then move-assign the result onward.
    {
        global_ref a{};
        global_ref b{ std::move(a) };
        global_ref c{};
        c = std::move(b);
        check("move_ctor_then_move_assign_src_empty", is_empty(a));  // NOLINT(bugprone-use-after-move)
        check("move_ctor_then_move_assign_mid_empty", is_empty(b));  // NOLINT(bugprone-use-after-move)
        check("move_ctor_then_move_assign_dst_empty", is_empty(c));
    }

    // ========================================================================
    // SECTION 9 -- std::move on a non-null-OOP pin (no JVM) drops cleanly
    //   Build several no-JVM pins from non-null OOPs and shuffle ownership
    //   through ctor + assignment; at scope exit every dtor sees a null handle.
    // ========================================================================
    {
        auto fake = [](std::uintptr_t v) {
            return reinterpret_cast<vmhook::oop_t>(v);
        };
        global_ref a{ fake(0x10) };
        global_ref b{ fake(0x20) };
        global_ref c{ std::move(a) };
        b = std::move(c);
        global_ref d{};
        d = std::move(b);
        check("shuffle_a_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("shuffle_b_empty", is_empty(b));   // NOLINT(bugprone-use-after-move)
        check("shuffle_c_empty", is_empty(c));   // NOLINT(bugprone-use-after-move)
        check("shuffle_d_empty", is_empty(d));
    }

    // ========================================================================
    // SECTION 10 -- pin(oop_t) free helper: inert without a JVM
    // ========================================================================
    {
        auto pinned = vmhook::pin(static_cast<vmhook::oop_t>(nullptr));
        check("pin_null_oop_is_empty", !static_cast<bool>(pinned));
        check("pin_null_oop_all_views_agree", is_empty(pinned));
    }
    {
        // Non-null OOP, no JVM -> pin() must also be empty (NewGlobalRef no-op).
        auto pinned = vmhook::pin(reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0x7000)));
        check("pin_nonnull_oop_without_jvm_is_empty", is_empty(pinned));
    }
    {
        // The pin() result is a prvalue we can move-construct from directly
        // (guaranteed-copy-elision path + an explicit move both end empty).
        global_ref moved_from_pin{ vmhook::pin(static_cast<vmhook::oop_t>(nullptr)) };
        check("pin_result_move_constructs_empty", is_empty(moved_from_pin));

        global_ref sink{};
        sink = vmhook::pin(static_cast<vmhook::oop_t>(nullptr));  // move-assign from prvalue
        check("pin_result_move_assigns_empty", is_empty(sink));
    }

    // ========================================================================
    // SECTION 11 -- pin(unique_ptr<wrapper>) free helper: inert without a JVM
    // ========================================================================
    {
        std::unique_ptr<dummy_wrapper> null_wrapper{};
        auto pinned_wrapper = vmhook::pin(null_wrapper);
        check("pin_null_wrapper_is_empty", !static_cast<bool>(pinned_wrapper));
        check("pin_null_wrapper_all_views_agree", is_empty(pinned_wrapper));
    }
    {
        // A *non-null* wrapper around a non-null fake OOP: pin() forwards
        // wrapper->get_instance() into the global_ref ctor, which still no-ops
        // without a JVM -> empty pin.  (MSVC copy-init for the unique_ptr.)
        std::unique_ptr<dummy_wrapper> live_wrapper{
            std::make_unique<dummy_wrapper>(
                reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x8000))) };
        auto pinned_wrapper = vmhook::pin(live_wrapper);
        check("pin_nonnull_wrapper_without_jvm_is_empty", is_empty(pinned_wrapper));
        // pin() takes the unique_ptr by const&, so the wrapper is NOT consumed.
        check("pin_does_not_consume_wrapper", static_cast<bool>(live_wrapper));
        check("pin_wrapper_instance_preserved",
              live_wrapper->vmhook::object_base::get_instance() ==
                  reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x8000)));
    }
    {
        // A second, DIFFERENT wrapper type to prove the template isn't hard-wired.
        std::unique_ptr<other_wrapper> null_other{};
        auto pinned_other = vmhook::pin(null_other);
        check("pin_other_null_wrapper_is_empty", is_empty(pinned_other));

        std::unique_ptr<other_wrapper> live_other{
            std::make_unique<other_wrapper>(
                reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x9000))) };
        auto pinned_live_other = vmhook::pin(live_other);
        check("pin_other_nonnull_wrapper_without_jvm_is_empty", is_empty(pinned_live_other));
    }

    // ========================================================================
    // SECTION 12 -- Container round-trips (the unordered_map / snapshot use case)
    // ========================================================================
    {
        std::vector<global_ref> pins;
        pins.emplace_back();
        pins.emplace_back(static_cast<vmhook::oop_t>(nullptr));
        pins.reserve(64);  // forces a move-relocation of existing elements
        check("vector_of_pins_relocates",
              pins.size() == 2 && is_empty(pins[0]) && is_empty(pins[1]));
    }
    {
        // Grow across multiple reallocations (each push past capacity move-
        // relocates every element); every pin stays empty and nothing double-frees.
        std::vector<global_ref> pins;
        for (int i = 0; i < 50; ++i)
        {
            pins.emplace_back(reinterpret_cast<vmhook::oop_t>(
                static_cast<std::uintptr_t>(0x100 + i)));
        }
        bool all_empty{ pins.size() == 50 };
        for (const auto& p : pins)
        {
            all_empty = all_empty && is_empty(p);
        }
        check("vector_growth_keeps_pins_empty", all_empty);

        // Erase from the middle (shifts elements via move-assignment).
        pins.erase(pins.begin() + 10);
        check("vector_erase_shifts_cleanly",
              pins.size() == 49 && is_empty(pins.front()) && is_empty(pins.back()));

        // Clear releases all (every dtor sees a null handle).
        pins.clear();
        check("vector_clear_is_safe", pins.empty());
    }
    {
        // std::move the whole vector: pins follow the buffer, originals drained.
        std::vector<global_ref> src;
        src.emplace_back();
        src.emplace_back(static_cast<vmhook::oop_t>(nullptr));
        std::vector<global_ref> dst{ std::move(src) };
        check("vector_move_transfers_pins",
              dst.size() == 2 && is_empty(dst[0]) && is_empty(dst[1]));
    }
    {
        // A std::unique_ptr<global_ref> -- heap-owned pin, deleted via dtor.
        auto heap_pin = std::make_unique<global_ref>();
        check("unique_ptr_global_ref_is_empty", is_empty(*heap_pin));
        heap_pin->reset();
        check("unique_ptr_global_ref_reset_safe", is_empty(*heap_pin));
        heap_pin.reset();  // delete the heap global_ref -> dtor on null handle
        check("unique_ptr_global_ref_delete_safe", heap_pin == nullptr);
    }

    // ========================================================================
    // SECTION 13 -- Lifetime / scope nesting: many ctor+dtor cycles
    //   A tight loop of build-then-destroy proves there is no leak / corruption
    //   path that accumulates across repeated RAII cycles (all on null handles).
    // ========================================================================
    {
        bool ok{ true };
        for (int i = 0; i < 1000; ++i)
        {
            global_ref scoped{ reinterpret_cast<vmhook::oop_t>(
                static_cast<std::uintptr_t>(0x10000 + i)) };
            ok = ok && is_empty(scoped);
            scoped.reset();
            ok = ok && is_empty(scoped);
        }  // dtor each iteration
        check("repeated_raii_cycles_stable", ok);
    }
    {
        // Nested scopes with overlapping lifetimes, inner moved out to outer.
        global_ref outer{};
        {
            global_ref inner{ static_cast<vmhook::oop_t>(nullptr) };
            outer = std::move(inner);
            check("nested_inner_moved_out_empty", is_empty(inner));  // NOLINT(bugprone-use-after-move)
        }  // inner dtor on null handle
        check("nested_outer_holds_empty", is_empty(outer));
    }

    // ========================================================================
    // SECTION 14 -- std::swap: the move-only swap path (move-ctor + 2 move-
    //   assigns under the hood).  Both operands empty in the no-JVM state, so
    //   the swap must leave them both empty and never double-free a handle.
    // ========================================================================
    {
        global_ref a{ oop_of(0xA000u) };   // empty (no JVM)
        global_ref b{ oop_of(0xB000u) };   // empty (no JVM)
        using std::swap;
        swap(a, b);
        check("swap_two_empties_a_empty", is_empty(a));
        check("swap_two_empties_b_empty", is_empty(b));
    }
    {
        // Self-swap via std::swap: std::swap(a, a) routes both refs to the same
        // object internally (NOT a syntactic self-move, so -Wself-move stays
        // silent) and must leave the pin intact / empty -- no double-free.
        global_ref a{ oop_of(0xC000u) };
        using std::swap;
        swap(a, a);
        check("self_swap_is_safe", is_empty(a));
    }
    {
        // A 3-way rotation built from two swaps; every pin stays empty.
        global_ref a{};
        global_ref b{ oop_of(0xD000u) };
        global_ref c{ static_cast<vmhook::oop_t>(nullptr) };
        using std::swap;
        swap(a, b);
        swap(b, c);
        check("three_way_swap_a_empty", is_empty(a));
        check("three_way_swap_b_empty", is_empty(b));
        check("three_way_swap_c_empty", is_empty(c));
    }

    // ========================================================================
    // SECTION 15 -- Return-by-value / NRVO / guaranteed-elision through a real
    //   function boundary.  make_ref() returns a prvalue global_ref; bind it,
    //   move-assign it, and feed it straight into a container.  Every path is
    //   well-formed and ends empty (no JVM) with nothing left armed.
    // ========================================================================
    {
        global_ref r{ make_ref(0xE000u) };   // move-on-return into a fresh pin
        check("return_by_value_binds_empty", is_empty(r));

        global_ref sink{};
        sink = make_ref(0xE100u);             // move-assign from a returned prvalue
        check("return_by_value_move_assigns_empty", is_empty(sink));

        // Returned prvalue consumed directly by is_empty() (a temporary that
        // lives only for the full expression, then its dtor runs on a null handle).
        check("return_by_value_temporary_empty", is_empty(make_ref(0xE200u)));
    }
    {
        // A returned pin pushed into a vector (the prvalue is moved into the
        // element slot -- exercises emplace-from-prvalue, not emplace-in-place).
        std::vector<global_ref> v;
        v.push_back(make_ref(0xE300u));
        v.push_back(make_ref(0xE400u));
        check("return_by_value_into_vector",
              v.size() == 2 && is_empty(v.front()) && is_empty(v.back()));
    }

    // ========================================================================
    // SECTION 16 -- Ternary / conditional yielding a prvalue global_ref.
    //   Both arms produce an (empty) pin; the selected prvalue is bound and
    //   must be empty regardless of which arm ran.
    // ========================================================================
    {
        const bool take_left{ (failures % 2) == 0 };  // value-dependent, both arms empty
        global_ref chosen{ take_left ? global_ref{ oop_of(0xF000u) }
                                     : global_ref{ static_cast<vmhook::oop_t>(nullptr) } };
        check("ternary_prvalue_is_empty", is_empty(chosen));
    }

    // ========================================================================
    // SECTION 17 -- std::optional<global_ref>: a move-only payload in optional.
    //   emplace / reset / move-construct / move-assign the optional and confirm
    //   the contained pin's empty contract survives each transition.
    // ========================================================================
    {
        std::optional<global_ref> opt;
        check("optional_starts_disengaged", !opt.has_value());

        opt.emplace(oop_of(0x11000u));           // construct a pin in place
        check("optional_emplaced_engaged", opt.has_value());
        check("optional_emplaced_value_empty", is_empty(*opt));

        opt.reset();                              // destroy the contained pin
        check("optional_reset_disengaged", !opt.has_value());

        // Move-construct an engaged optional into another; source becomes a
        // disengaged-or-empty optional, destination holds an empty pin.
        std::optional<global_ref> src;
        src.emplace(static_cast<vmhook::oop_t>(nullptr));
        std::optional<global_ref> dst{ std::move(src) };
        check("optional_move_ctor_dst_has_empty", dst.has_value() && is_empty(*dst));

        // Move-assign a fresh engaged optional over an engaged one.
        std::optional<global_ref> reassigned;
        reassigned.emplace(oop_of(0x12000u));
        reassigned = std::optional<global_ref>{ global_ref{ oop_of(0x13000u) } };
        check("optional_move_assign_holds_empty", reassigned.has_value() && is_empty(*reassigned));
    }

    // ========================================================================
    // SECTION 18 -- global_ref inside std::pair / std::tuple (move-only members
    //   in aggregates the STL move as a unit).  Build, move the whole pair/tuple,
    //   and confirm the embedded pin stays empty.
    // ========================================================================
    {
        std::pair<int, global_ref> p{ 7, global_ref{ oop_of(0x14000u) } };
        check("pair_member_pin_empty", is_empty(p.second));
        std::pair<int, global_ref> moved{ std::move(p) };
        check("pair_moved_member_pin_empty", moved.first == 7 && is_empty(moved.second));
    }
    {
        std::tuple<global_ref, int, global_ref> t{
            global_ref{ oop_of(0x15000u) }, 9, global_ref{ static_cast<vmhook::oop_t>(nullptr) } };
        check("tuple_members_pins_empty",
              is_empty(std::get<0>(t)) && std::get<1>(t) == 9 && is_empty(std::get<2>(t)));
        std::tuple<global_ref, int, global_ref> moved{ std::move(t) };
        check("tuple_moved_members_pins_empty",
              is_empty(std::get<0>(moved)) && std::get<1>(moved) == 9 && is_empty(std::get<2>(moved)));
    }

    // ========================================================================
    // SECTION 19 -- Other containers: std::deque (block relocation differs from
    //   vector) and std::map<int, global_ref> (move-only mapped type, node-based
    //   so no element relocation but exercises piecewise construction + erase).
    // ========================================================================
    {
        std::deque<global_ref> dq;
        for (int i = 0; i < 40; ++i)
        {
            dq.emplace_back(oop_of(static_cast<std::uintptr_t>(0x16000 + i)));
        }
        bool all_empty{ dq.size() == 40 };
        for (const auto& p : dq)
        {
            all_empty = all_empty && is_empty(p);
        }
        check("deque_growth_keeps_pins_empty", all_empty);
        dq.pop_front();   // shrink from the front (deque-specific path)
        dq.pop_back();
        check("deque_pop_ends_safe",
              dq.size() == 38 && is_empty(dq.front()) && is_empty(dq.back()));
        dq.clear();
        check("deque_clear_safe", dq.empty());
    }
    {
        std::map<int, global_ref> m;
        m.emplace(1, global_ref{ oop_of(0x17000u) });
        m.emplace(2, global_ref{ static_cast<vmhook::oop_t>(nullptr) });
        m.emplace(3, global_ref{});
        // operator[] default-constructs then move-assigns a fresh pin in.
        m[4] = global_ref{ oop_of(0x17400u) };
        bool all_empty{ m.size() == 4 };
        for (const auto& kv : m)
        {
            all_empty = all_empty && is_empty(kv.second);
        }
        check("map_of_pins_all_empty", all_empty);
        m.erase(2);   // node removal -> dtor on a null handle
        check("map_erase_safe", m.size() == 3 && is_empty(m.at(1)) && is_empty(m.at(4)));
    }

    // ========================================================================
    // SECTION 20 -- Bulk move between containers via move-iterators.  Drains the
    //   source elements (each left empty / moved-from) into the destination.
    // ========================================================================
    {
        std::vector<global_ref> src;
        for (int i = 0; i < 16; ++i)
        {
            src.emplace_back(oop_of(static_cast<std::uintptr_t>(0x18000 + i)));
        }
        std::vector<global_ref> dst;
        dst.reserve(src.size());
        dst.insert(dst.end(),
                   std::make_move_iterator(src.begin()),
                   std::make_move_iterator(src.end()));
        bool dst_empty{ dst.size() == 16 };
        for (const auto& p : dst)
        {
            dst_empty = dst_empty && is_empty(p);
        }
        check("move_iterator_transfer_dst_empty", dst_empty);
        // The source elements were moved-from: still alive, still empty (no
        // resurrection, no double-free at either vector's teardown).
        bool src_drained{ src.size() == 16 };
        for (const auto& p : src)
        {
            src_drained = src_drained && is_empty(p);   // NOLINT(bugprone-use-after-move)
        }
        check("move_iterator_transfer_src_drained", src_drained);
    }
    {
        // std::vector::insert in the MIDDLE move-shifts the tail RIGHT (distinct
        // from the erase/left-shift already covered) -- every pin stays empty.
        std::vector<global_ref> v;
        for (int i = 0; i < 20; ++i)
        {
            v.emplace_back(oop_of(static_cast<std::uintptr_t>(0x19000 + i)));
        }
        v.insert(v.begin() + 5, global_ref{ oop_of(0x19500u) });
        bool ok{ v.size() == 21 };
        for (const auto& p : v)
        {
            ok = ok && is_empty(p);
        }
        check("vector_insert_middle_shifts_cleanly", ok);

        // resize UP (default-constructs new empty pins) then DOWN (destroys tail).
        v.resize(30);
        check("vector_resize_up_appends_empty",
              v.size() == 30 && is_empty(v.back()));
        v.resize(3);
        check("vector_resize_down_destroys_tail",
              v.size() == 3 && is_empty(v.front()) && is_empty(v.back()));
    }

    // ========================================================================
    // SECTION 21 -- C-array / std::array of pins, and a deeper double-use-after-
    //   move chain (a moved-from pin used as BOTH a move source AND a move
    //   destination repeatedly).  No handle is ever resurrected.
    // ========================================================================
    {
        // Double braces: std::array is an aggregate wrapping a C array, so a
        // single brace level draws clang's -Wmissing-braces under -Werror.
        std::array<global_ref, 4> arr{ {
            global_ref{ oop_of(0x1A000u) },
            global_ref{ static_cast<vmhook::oop_t>(nullptr) },
            global_ref{ oop_of(0x1A200u) },
            global_ref{} } };
        bool all_empty{ true };
        for (const auto& p : arr)
        {
            all_empty = all_empty && is_empty(p);
        }
        check("std_array_of_pins_empty", all_empty);
    }
    {
        // A moved-from pin is a valid empty object: move it AGAIN (as source),
        // then move INTO it (as destination), alternating several times.  Each
        // step must keep every participant empty.
        global_ref a{ oop_of(0x1B000u) };
        global_ref b{ std::move(a) };          // a moved-from
        global_ref c{ std::move(a) };          // move the moved-from a AGAIN  // NOLINT(bugprone-use-after-move)
        check("double_move_from_same_source_a_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("double_move_from_same_source_b_empty", is_empty(b));
        check("double_move_from_same_source_c_empty", is_empty(c));

        a = std::move(b);                       // move INTO the moved-from a
        check("reassign_into_moved_from_a_empty", is_empty(a));
        check("reassign_into_moved_from_b_empty", is_empty(b));   // NOLINT(bugprone-use-after-move)

        a = std::move(c);                       // and again
        check("reassign_into_moved_from_again_a_empty", is_empty(a));
        check("reassign_into_moved_from_again_c_empty", is_empty(c));   // NOLINT(bugprone-use-after-move)
    }

    // ========================================================================
    // SECTION 22 -- re-pin after reset / re-arm after move; and the laundered
    //   self-move (matching the sibling forwarder test's technique) over both
    //   the default and non-null-OOP states.  Confirms reset() does not poison
    //   the object for a subsequent (no-JVM, still-empty) re-pin.
    // ========================================================================
    {
        global_ref g{ oop_of(0x1C000u) };
        g.reset();
        check("reset_then_repin_reset_empty", is_empty(g));
        g = global_ref{ oop_of(0x1C100u) };     // re-pin via move-assign from prvalue
        check("reset_then_repin_reassigned_empty", is_empty(g));
        g.reset();
        check("reset_then_repin_final_reset_empty", is_empty(g));
    }
    {
        // Move OUT of g (g becomes moved-from), then re-arm g via assignment, in
        // a loop -- proves an object can cycle move-out / re-arm indefinitely.
        global_ref g{};
        bool ok{ true };
        for (int i = 0; i < 64; ++i)
        {
            global_ref taken{ std::move(g) };
            ok = ok && is_empty(g);             // NOLINT(bugprone-use-after-move)
            g = global_ref{ oop_of(static_cast<std::uintptr_t>(0x1D000 + i)) };
            ok = ok && is_empty(g);
            (void)taken;                         // taken dtors here on null handle
        }
        check("move_out_then_rearm_cycle_stable", ok);
    }
    {
        // Laundered self-move-assign (no syntactic self-move -> no -Wself-move),
        // exercising the runtime `this != &other` guard on both states.
        global_ref empty_self{};
        launder_move_assign(empty_self, empty_self);
        check("laundered_self_move_empty_safe", is_empty(empty_self));

        global_ref oop_self{ oop_of(0x1E000u) };
        launder_move_assign(oop_self, oop_self);
        check("laundered_self_move_nonnull_oop_safe", is_empty(oop_self));

        // Repeated laundered self-moves accumulate no corruption.
        global_ref repeat_self{ oop_of(0x1E100u) };
        launder_move_assign(repeat_self, repeat_self);
        launder_move_assign(repeat_self, repeat_self);
        launder_move_assign(repeat_self, repeat_self);
        check("laundered_self_move_repeated_safe", is_empty(repeat_self));
    }

    // ========================================================================
    // SECTION 23 -- Exhaustive bit-pattern sweep of the no-JVM from-OOP ctor.
    //   Drives the ctor with addresses that look like JNI-tagged handles (every
    //   low-3-bit tag), is_valid_pointer debug-fill sentinels, alignment edges,
    //   and pointer-width extremes.  Without a JVM the NewGlobalRef table slot is
    //   unresolved, so EVERY pattern must yield an empty pin (handle stays null)
    //   -- the result must not depend on the bit pattern of the rejected OOP, and
    //   oop()'s mask-and-deref branch is never reached (handle_ is null).
    // ========================================================================
    {
        // All eight low-3-bit "tag" values OR'd onto an otherwise-valid base, to
        // mimic JDK 9+ tagged JNI handles the ctor would receive on a live JVM.
        bool all_empty{ true };
        for (std::uintptr_t tag = 0; tag < 8u; ++tag)
        {
            const std::uintptr_t bits{ 0x20000u | tag };
            global_ref g{ oop_of(bits) };
            all_empty = all_empty && is_empty(g);
        }
        check("ctor_all_low3_tag_patterns_empty", all_empty);
    }
    {
        // The debug-fill / sentinel low-32 patterns is_valid_pointer rejects;
        // the ctor still no-ops without a JVM regardless, so all stay empty.
        const std::uintptr_t sentinels[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool all_empty{ true };
        for (const std::uintptr_t bits : sentinels)
        {
            global_ref g{ oop_of(bits) };
            all_empty = all_empty && is_empty(g);
        }
        check("ctor_sentinel_patterns_empty", all_empty);
    }
    {
        // Alignment edges and pointer-width extremes (all reinterpret-only; never
        // dereferenced because the handle stays null without a JVM).
        const std::uintptr_t edges[]{
            std::uintptr_t{ 1u },
            std::uintptr_t{ 2u },
            std::uintptr_t{ 4u },
            std::uintptr_t{ 7u },
            std::uintptr_t{ 8u },
            static_cast<std::uintptr_t>(~std::uintptr_t{ 0 }),          // all-ones
            static_cast<std::uintptr_t>(~std::uintptr_t{ 0 }) & ~std::uintptr_t{ 0b111 },  // all-ones, aligned
            static_cast<std::uintptr_t>(std::uintptr_t{ 1 } << (sizeof(void*) * 8u - 1u)), // top bit only
        };
        bool all_empty{ true };
        for (const std::uintptr_t bits : edges)
        {
            global_ref g{ oop_of(bits) };
            all_empty = all_empty && is_empty(g);
            g.reset();                            // reset on each is a no-op
            all_empty = all_empty && is_empty(g);
        }
        check("ctor_alignment_and_width_edges_empty", all_empty);
    }

    return failures == 0 ? 0 : 1;
}
