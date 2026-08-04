// Standalone (no-JVM) unit test for vmhook::jni::global_ref and vmhook::pin().
//
// ===========================================================================
// !! READ THIS BEFORE EDITING — GC-SURVIVAL COVERAGE IS INTENTIONALLY ABSENT !!
// ===========================================================================
// vmhook::jni::global_ref is currently a NO-OP STUB.  The header says so in as
// many words: the constructor stores the raw OOP you hand it, the destructor
// does nothing, and oop() gives the stored address back verbatim.  There is no
// VM-side registration, so the holder
//
//   * does NOT keep the object alive (it is not a GC root), and
//   * does NOT track relocation (a moving collector leaves it stale).
//
// Therefore this file deliberately contains NO assertion — and no wording that
// could be read as one — about an object surviving a collection, about a handle
// being re-derived after a move, or about DeleteGlobalRef ever running.  Those
// behaviours do not exist today and a test claiming them would be a lie that
// passes.  What IS pinned down here is everything the stub genuinely promises:
//
//   * move-only, final, standard-layout, one-pointer-wide type shape,
//   * EXPLICIT construction from an oop_t (no implicit pin from a raw pointer),
//   * verbatim storage: oop() == handle() == the address passed in,
//   * empty/null behaviour: default ctor, nullptr ctor, and reset() all agree,
//   * reset() idempotence and dtor safety on both empty and armed holders,
//   * move semantics as VALUE TRANSFER: the destination ends up holding exactly
//     the source's address and the source ends up empty — never a resurrection,
//   * self-move / self-swap preserve the stored address (the `this != &other`
//     guard), and container round-trips (vector / deque / map / optional /
//     pair / tuple / array / move-iterators) preserve every stored address.
//
// >>> WHEN THE REAL PIN LANDS: a genuine GC root must ALSO be covered by a
// >>> live-JVM module (allocate, pin, drop all Java references, System.gc(),
// >>> then prove oop() still resolves and — for a relocating collector — that
// >>> it resolves to the NEW address).  That coverage belongs in
// >>> tests/jvm/modules/, not here; this file cannot host it.  At that point
// >>> the "verbatim storage" assertions below become WRONG and must be
// >>> rewritten to the handle-indirection contract, and the
// >>> is_trivially_destructible assertion in SECTION 1b must flip back to
// >>> !is_trivially_destructible (a real pin needs a releasing destructor).
// ===========================================================================
//
// WHY every check below is deterministic without a JVM: the stub never calls
// into the VM at all.  Construction, oop(), handle(), reset() and destruction
// are pure pointer bookkeeping, so they behave identically with and without a
// JVM present and no fabricated address is ever dereferenced by this file.
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
// accessor at once (operator bool, oop(), handle()).  Used to assert the "all
// three views agree" invariant after each lifecycle operation, so a bug that
// nulls one view but not another is caught.
static auto is_empty(const vmhook::jni::global_ref& g) -> bool
{
    return !static_cast<bool>(g) && g.oop() == nullptr && g.handle() == nullptr;
}

// Returns true iff `g` reports EXACTLY `expected` across every observable
// accessor at once.  This is the positive counterpart of is_empty(): it pins
// the stub's verbatim-storage contract (oop() and handle() are the same stored
// address, and operator bool is precisely "that address is non-null").
static auto holds(const vmhook::jni::global_ref& g,
                  const vmhook::oop_t expected) -> bool
{
    return g.oop() == expected
        && g.handle() == expected
        && static_cast<bool>(g) == (expected != nullptr);
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
    // -Werror (it is a purely syntactic diagnostic).
    auto launder_move_assign(vmhook::jni::global_ref& dst,
                             vmhook::jni::global_ref& src) -> void
    {
        dst = std::move(src);
    }

    // Convenience: turn a uintptr_t bit pattern into an oop_t at a call site.
    auto oop_of(const std::uintptr_t bits) noexcept -> vmhook::oop_t
    {
        return reinterpret_cast<vmhook::oop_t>(bits);
    }

    // Returns a global_ref BY VALUE built from the given OOP.  Used to drive the
    // return-by-value / NRVO / guaranteed-elision paths through a real function
    // boundary (a prvalue returned from a named local).  The returned holder
    // must carry the address through the move-on-return unchanged.
    auto make_ref(const std::uintptr_t bits) -> vmhook::jni::global_ref
    {
        vmhook::jni::global_ref local{ oop_of(bits) };
        return local;  // NRVO / implicit move of a local on return
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
                  "global_ref default ctor must be noexcept (empty holder, no VM call)");
    // The from-OOP ctor is the only value-taking constructor: pin it noexcept.
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

    // pin() free-function return types: both overloads yield a global_ref BY
    // VALUE (never a reference / wrapper).  Pin the signatures.
    static_assert(
        std::is_same_v<decltype(vmhook::pin(std::declval<vmhook::oop_t>())), global_ref>,
        "pin(oop_t) must return a global_ref by value");
    static_assert(
        std::is_same_v<decltype(vmhook::pin(std::declval<const std::unique_ptr<dummy_wrapper>&>())),
                       global_ref>,
        "pin(unique_ptr<wrapper>&) must return a global_ref by value");
    static_assert(noexcept(vmhook::pin(std::declval<vmhook::oop_t>())),
                  "pin(oop_t) must be noexcept");
    static_assert(noexcept(vmhook::pin(std::declval<const std::unique_ptr<dummy_wrapper>&>())),
                  "pin(unique_ptr<wrapper>&) must be noexcept");
    check("pin_free_function_signatures", true);

    // ========================================================================
    // SECTION 1b -- DEEPER type properties: layout, triviality, swappability,
    //   constructibility/assignability value-category matrix (compile-time).
    //   These go beyond the basic move-only contract: they pin the EXACT
    //   special-member shape, the thin-wrapper memory layout, swap support, and
    //   the precise set of argument value-categories the (move-only) ctor /
    //   assignment will and won't bind.
    // ========================================================================

    // -- Layout: global_ref is a thin, standard-layout wrapper around ONE void*.
    //    Asserting sizeof/alignof RELATIVE to void* (never an absolute byte
    //    count) keeps this true on ILP32 and LP64 / LLP64 alike.
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
                  "global_ref is not empty (it stores a void* member)");
    static_assert(!std::is_polymorphic_v<global_ref>,
                  "global_ref must not be polymorphic (no virtual functions)");

    // -- Triviality.  The move operations are user-provided (they steal and null
    //    the source), so the type is deliberately non-trivial on those axes; a
    //    drift to a defaulted/trivial move (which would leave the source armed,
    //    duplicating the address) is a real bug and must fail to compile.
    static_assert(!std::is_trivially_copyable_v<global_ref>,
                  "global_ref must NOT be trivially copyable (custom move + deleted copy)");
    static_assert(!std::is_trivial_v<global_ref>,
                  "global_ref must NOT be trivial");
    static_assert(!std::is_trivially_move_constructible_v<global_ref>,
                  "global_ref move ctor is user-provided (steals + nulls source) -> not trivial");
    static_assert(!std::is_trivially_move_assignable_v<global_ref>,
                  "global_ref move assignment is user-provided (steals + nulls source) -> not trivial");
    // The default ctor is `= default` but the sole member carries an NSDMI
    // (`= nullptr`), so default-construction is NOT trivial -- an uninitialised
    // holder would be indistinguishable from an armed one.
    static_assert(!std::is_trivially_default_constructible_v<global_ref>,
                  "global_ref must value-initialise its member (NSDMI) -> not trivially default-constructible");
    // -- STUB-SPECIFIC (see the file header): the destructor is `= default` and
    //    releases nothing, because there is nothing registered with the VM to
    //    release.  This assertion is the tripwire for that fact: the day a real
    //    pin lands, its destructor MUST release the root, the type stops being
    //    trivially destructible, and this line fails -- which is exactly the
    //    signal to come back here and restore the GC-survival coverage that the
    //    file header says is missing.  Do not delete this assertion; flip it.
    static_assert(std::is_trivially_destructible_v<global_ref>,
                  "STUB TRIPWIRE: global_ref's dtor releases nothing today.  If this fails, a "
                  "real releasing destructor landed -- flip this to !is_trivially_destructible "
                  "and restore the GC-survival coverage described at the top of this file.");

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
    // Nor may a raw oop_t / nullptr be assigned in: there is no converting
    // assignment operator, and the converting ctor is explicit.
    static_assert(!std::is_assignable_v<global_ref&, vmhook::oop_t>,
                  "a raw oop_t must NOT be assignable to a global_ref (explicit ctor, no converting assign)");
    static_assert(!std::is_assignable_v<global_ref&, std::nullptr_t>,
                  "nullptr must NOT be assignable to a global_ref (use reset())");

    // -- Move ctor / move assign EXPRESSION noexcept (decltype-level, in
    //    addition to the is_nothrow_* traits above).
    static_assert(noexcept(global_ref{ std::declval<global_ref&&>() }),
                  "move-construction expression must be noexcept");
    static_assert(noexcept(std::declval<global_ref&>() = std::declval<global_ref&&>()),
                  "move-assignment expression must be noexcept");
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
    // SECTION 3 -- Null-OOP construction is empty / inert
    // ========================================================================
    {
        global_ref from_null{ static_cast<vmhook::oop_t>(nullptr) };
        check("null_oop_construct_is_falsy", !static_cast<bool>(from_null));
        check("null_oop_construct_oop_is_null", from_null.oop() == nullptr);
        check("null_oop_construct_handle_is_null", from_null.handle() == nullptr);
        check("null_oop_construct_all_views_agree", is_empty(from_null));
        check("null_oop_construct_matches_default_ctor", holds(from_null, nullptr));
        from_null.reset();
        check("null_oop_construct_reset_safe", is_empty(from_null));
    }

    // ========================================================================
    // SECTION 4 -- Non-null OOP: the holder stores the address VERBATIM.
    //   The stub performs no VM call and no tag masking, so every accessor must
    //   report back exactly the address that was passed in, for ANY bit
    //   pattern.  We sweep several distinct fake addresses to prove the result
    //   is a pure pass-through and does not depend on the bit pattern.
    //   (None of these addresses is ever dereferenced.)
    // ========================================================================
    {
        const std::uintptr_t fakes[]{
            0x1u, 0x8u, 0x1000u, 0xDEADBEEFu, 0xFFFFFFFFu,
            static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0)),  // all-ones
            static_cast<std::uintptr_t>(0x7u),  // low-bit-tagged-looking value
        };
        bool all_hold{ true };
        bool all_truthy{ true };
        for (const std::uintptr_t bits : fakes)
        {
            auto* const fake_oop{ oop_of(bits) };
            global_ref armed{ fake_oop };
            all_hold   = all_hold && holds(armed, fake_oop);
            all_truthy = all_truthy && static_cast<bool>(armed);
            // destructor runs here on an armed holder -> the stub's no-op
            // release path, once per iteration -> no crash, no double-free.
        }
        check("non_null_oop_is_stored_verbatim", all_hold);
        check("non_null_oop_is_truthy", all_truthy);
    }

    // oop() and handle() are the SAME stored address (not two different views),
    // and repeated reads are stable.
    {
        auto* const fake_oop{ oop_of(0x4100u) };
        const global_ref armed{ fake_oop };
        check("oop_and_handle_are_the_same_address",
              armed.oop() == armed.handle() && armed.oop() == fake_oop);
        check("armed_oop_reads_are_stable",
              armed.oop() == armed.oop() && armed.handle() == armed.handle());
    }

    // reset() on an armed holder clears it, and is idempotent afterwards.
    {
        auto* const fake_oop{ oop_of(0x4000u) };
        global_ref armed{ fake_oop };
        check("armed_before_reset", holds(armed, fake_oop));
        armed.reset();
        check("reset_clears_armed_holder", is_empty(armed));
        armed.reset();  // idempotent
        armed.reset();
        check("reset_on_armed_is_idempotent", is_empty(armed));
    }

    // ========================================================================
    // SECTION 5 -- Move construction is VALUE TRANSFER: the destination ends up
    //   holding exactly the source's address, and the source ends up empty.
    // ========================================================================
    {
        global_ref a{};                       // empty
        global_ref b{ std::move(a) };
        check("moved_from_empty_is_falsy", !static_cast<bool>(a));            // NOLINT(bugprone-use-after-move)
        check("moved_from_empty_oop_is_null", a.oop() == nullptr);           // NOLINT(bugprone-use-after-move)
        check("moved_from_empty_handle_is_null", a.handle() == nullptr);     // NOLINT(bugprone-use-after-move)
        check("moved_from_empty_all_views_agree", is_empty(a));              // NOLINT(bugprone-use-after-move)
        check("moved_to_from_empty_is_empty", is_empty(b));
    }

    // Move-construct from an ARMED holder: the address migrates, the source is
    // nulled (no duplicate holder of the same address exists afterwards).
    {
        auto* const fake_oop{ oop_of(0x2000u) };
        global_ref a{ fake_oop };
        global_ref b{ std::move(a) };
        check("move_ctor_src_is_emptied", is_empty(a));                      // NOLINT(bugprone-use-after-move)
        check("move_ctor_dst_takes_address", holds(b, fake_oop));
    }

    // A moved-from pin is a valid object: it can be reset, re-checked, and
    // re-assigned-into afterwards (no resurrection of the stolen address).
    {
        auto* const fake_oop{ oop_of(0x2100u) };
        global_ref a{ fake_oop };
        global_ref b{ std::move(a) };
        check("moved_from_before_reset_empty", is_empty(a));                 // NOLINT(bugprone-use-after-move)
        a.reset();                                  // legal on a moved-from object
        check("moved_from_can_be_reset", is_empty(a));
        a = global_ref{ oop_of(0x2200u) };          // re-arm the moved-from holder
        check("moved_from_can_be_rearmed", holds(a, oop_of(0x2200u)));
        check("move_ctor_dst_unaffected_by_src_rearm", holds(b, fake_oop));
    }

    // ========================================================================
    // SECTION 6 -- Move assignment: steals the source's address, nulls source
    // ========================================================================
    {
        global_ref a{};
        global_ref b{};
        b = std::move(a);
        check("move_assign_empty_source_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("move_assign_empty_dest_empty", is_empty(b));
    }

    // Move-assign returns *this (so `(c = std::move(b))` chains correctly), and
    // the reference it returns is the destination object itself.
    {
        auto* const fake_oop{ oop_of(0x2300u) };
        global_ref a{ fake_oop };
        global_ref b{};
        global_ref& result{ (b = std::move(a)) };
        check("move_assign_returns_this", &result == &b);
        check("move_assign_returns_dest_state", holds(result, fake_oop));
    }

    // Move-assign OVER an armed destination: the destination's prior address is
    // dropped (overwritten) and replaced by the source's; the source is nulled.
    {
        auto* const fake_a{ oop_of(0x3000u) };
        auto* const fake_b{ oop_of(0x5000u) };
        global_ref a{ fake_a };
        global_ref b{ fake_b };
        b = std::move(a);
        check("move_assign_armed_src_emptied", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("move_assign_armed_dst_takes_src_address", holds(b, fake_a));
        check("move_assign_armed_dst_dropped_old_address", b.oop() != fake_b);
    }

    // ========================================================================
    // SECTION 7 -- Self-move-assign must not corrupt the holder.
    //   The operator is guarded by `if (this != &other)`, so a self-move is a
    //   no-op: it must NOT null the stored address.  We launder the self-
    //   reference through a pointer so the compiler can't see it is a self-move
    //   (silences -Wself-move while still exercising the runtime guard).
    //   NOTE: with the stub storing addresses verbatim this is now a REAL
    //   assertion -- before the de-JNI change every holder was empty, so a
    //   broken guard would have gone unnoticed here.
    // ========================================================================
    {
        global_ref a{};
        global_ref* const self{ &a };
        *self = std::move(a);   // guarded self-move on an empty holder
        check("self_move_assign_empty_safe", is_empty(a));
    }
    {
        auto* const fake_oop{ oop_of(0x6000u) };
        global_ref a{ fake_oop };
        global_ref* const self{ &a };
        *self = std::move(a);   // guarded self-move; must NOT clear the address
        check("self_move_assign_preserves_address", holds(a, fake_oop));
    }
    // Repeated self-move-assign stays safe (no accumulating corruption).
    {
        auto* const fake_oop{ oop_of(0x6100u) };
        global_ref a{ fake_oop };
        global_ref* const self{ &a };
        *self = std::move(a);
        *self = std::move(a);
        *self = std::move(a);
        check("self_move_assign_repeated_preserves_address", holds(a, fake_oop));
    }

    // ========================================================================
    // SECTION 8 -- Chained / double moves carry the address exactly once
    // ========================================================================
    {
        auto* const fake_oop{ oop_of(0x7100u) };
        global_ref a{ fake_oop };
        global_ref b{ std::move(a) };
        global_ref c{ std::move(b) };  // chain the move-ctor
        check("double_move_ctor_first_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("double_move_ctor_middle_empty", is_empty(b));  // NOLINT(bugprone-use-after-move)
        check("double_move_ctor_last_holds_address", holds(c, fake_oop));
    }
    {
        auto* const fake_a{ oop_of(0x7200u) };
        auto* const fake_b{ oop_of(0x7300u) };
        global_ref a{ fake_a };
        global_ref b{ fake_b };
        global_ref c{};
        c = std::move(b);
        b = std::move(a);  // move-assign chain in the other direction
        check("chained_move_assign_a_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("chained_move_assign_b_holds_a", holds(b, fake_a));
        check("chained_move_assign_c_holds_b", holds(c, fake_b));
    }
    // Mixed: move-construct, then move-assign the result onward.
    {
        auto* const fake_oop{ oop_of(0x7400u) };
        global_ref a{ fake_oop };
        global_ref b{ std::move(a) };
        global_ref c{};
        c = std::move(b);
        check("move_ctor_then_move_assign_src_empty", is_empty(a));  // NOLINT(bugprone-use-after-move)
        check("move_ctor_then_move_assign_mid_empty", is_empty(b));  // NOLINT(bugprone-use-after-move)
        check("move_ctor_then_move_assign_dst_holds", holds(c, fake_oop));
    }

    // ========================================================================
    // SECTION 9 -- Shuffling ownership through ctor + assignment: exactly one
    //   holder ends up with each address, the rest are empty, and every dtor at
    //   scope exit is safe.
    // ========================================================================
    {
        auto* const fake_a{ oop_of(0x10u) };
        auto* const fake_b{ oop_of(0x20u) };
        global_ref a{ fake_a };
        global_ref b{ fake_b };
        global_ref c{ std::move(a) };   // c <- a's address, a empty
        b = std::move(c);               // b <- a's address (b's own dropped), c empty
        global_ref d{};
        d = std::move(b);               // d <- a's address, b empty
        check("shuffle_a_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("shuffle_b_empty", is_empty(b));   // NOLINT(bugprone-use-after-move)
        check("shuffle_c_empty", is_empty(c));   // NOLINT(bugprone-use-after-move)
        check("shuffle_d_holds_first_address", holds(d, fake_a));
    }

    // ========================================================================
    // SECTION 10 -- pin(oop_t) free helper: identical to direct construction
    // ========================================================================
    {
        auto pinned = vmhook::pin(static_cast<vmhook::oop_t>(nullptr));
        check("pin_null_oop_is_empty", !static_cast<bool>(pinned));
        check("pin_null_oop_all_views_agree", is_empty(pinned));
    }
    {
        // Non-null OOP -> pin() must produce the same verbatim-storage holder as
        // `global_ref{ oop }` does.
        auto* const fake_oop{ oop_of(0x7000u) };
        auto pinned = vmhook::pin(fake_oop);
        check("pin_nonnull_oop_stores_verbatim", holds(pinned, fake_oop));
        const global_ref direct{ fake_oop };
        check("pin_matches_direct_construction", pinned.oop() == direct.oop());
    }
    {
        // The pin() result is a prvalue we can move-construct from directly
        // (guaranteed-copy-elision path) and move-assign from; the address must
        // survive both.
        auto* const fake_oop{ oop_of(0x7500u) };
        global_ref moved_from_pin{ vmhook::pin(fake_oop) };
        check("pin_result_move_constructs_with_address", holds(moved_from_pin, fake_oop));

        global_ref sink{};
        sink = vmhook::pin(oop_of(0x7600u));  // move-assign from prvalue
        check("pin_result_move_assigns_with_address", holds(sink, oop_of(0x7600u)));

        global_ref null_sink{ vmhook::pin(static_cast<vmhook::oop_t>(nullptr)) };
        check("pin_null_result_move_constructs_empty", is_empty(null_sink));
    }

    // ========================================================================
    // SECTION 11 -- pin(unique_ptr<wrapper>) free helper: forwards the wrapper's
    //   instance OOP, and yields an EMPTY holder for a null wrapper.
    // ========================================================================
    {
        std::unique_ptr<dummy_wrapper> null_wrapper{};
        auto pinned_wrapper = vmhook::pin(null_wrapper);
        check("pin_null_wrapper_is_empty", !static_cast<bool>(pinned_wrapper));
        check("pin_null_wrapper_all_views_agree", is_empty(pinned_wrapper));
    }
    {
        // A *non-null* wrapper around a non-null fake OOP: pin() forwards
        // wrapper->get_instance() into the global_ref ctor, which stores it
        // verbatim -> the pin reports the wrapper's instance address.
        auto* const fake_oop{ oop_of(0x8000u) };
        std::unique_ptr<dummy_wrapper> live_wrapper{
            std::make_unique<dummy_wrapper>(fake_oop) };
        auto pinned_wrapper = vmhook::pin(live_wrapper);
        check("pin_nonnull_wrapper_forwards_instance", holds(pinned_wrapper, fake_oop));
        // pin() takes the unique_ptr by const&, so the wrapper is NOT consumed.
        check("pin_does_not_consume_wrapper", static_cast<bool>(live_wrapper));
        check("pin_wrapper_instance_preserved",
              live_wrapper->vmhook::object_base::get_instance() == fake_oop);
        // Pinning the same wrapper twice yields two holders on the same address
        // (the stub has no ownership semantics to violate here).
        auto pinned_again = vmhook::pin(live_wrapper);
        check("pin_wrapper_twice_agrees", holds(pinned_again, fake_oop));
    }
    {
        // A second, DIFFERENT wrapper type to prove the template isn't hard-wired.
        std::unique_ptr<other_wrapper> null_other{};
        auto pinned_other = vmhook::pin(null_other);
        check("pin_other_null_wrapper_is_empty", is_empty(pinned_other));

        auto* const fake_oop{ oop_of(0x9000u) };
        std::unique_ptr<other_wrapper> live_other{
            std::make_unique<other_wrapper>(fake_oop) };
        auto pinned_live_other = vmhook::pin(live_other);
        check("pin_other_nonnull_wrapper_forwards_instance", holds(pinned_live_other, fake_oop));
    }

    // ========================================================================
    // SECTION 12 -- Container round-trips (the snapshot / registry use case).
    //   Every relocation, erase, growth and move must carry each stored address
    //   to its new slot unchanged.
    // ========================================================================
    {
        std::vector<global_ref> pins;
        pins.emplace_back();
        pins.emplace_back(static_cast<vmhook::oop_t>(nullptr));
        pins.emplace_back(oop_of(0x1234u));
        pins.reserve(64);  // forces a move-relocation of existing elements
        check("vector_of_pins_relocates",
              pins.size() == 3 && is_empty(pins[0]) && is_empty(pins[1])
                  && holds(pins[2], oop_of(0x1234u)));
    }
    {
        // Grow across multiple reallocations (each push past capacity move-
        // relocates every element); every address must survive every relocation.
        std::vector<global_ref> pins;
        for (int i = 0; i < 50; ++i)
        {
            pins.emplace_back(oop_of(static_cast<std::uintptr_t>(0x100 + i)));
        }
        bool all_hold{ pins.size() == 50 };
        for (int i = 0; i < 50; ++i)
        {
            all_hold = all_hold
                && holds(pins[static_cast<std::size_t>(i)],
                         oop_of(static_cast<std::uintptr_t>(0x100 + i)));
        }
        check("vector_growth_preserves_addresses", all_hold);

        // Erase from the middle (shifts elements LEFT via move-assignment): the
        // remaining addresses shift with their elements, none are duplicated.
        pins.erase(pins.begin() + 10);
        bool shift_ok{ pins.size() == 49
            && holds(pins.front(), oop_of(0x100u))
            && holds(pins[10], oop_of(0x100u + 11u))     // 0x10A was erased
            && holds(pins.back(), oop_of(0x100u + 49u)) };
        check("vector_erase_shifts_addresses_left", shift_ok);

        // Clear releases all (every dtor runs on an armed holder).
        pins.clear();
        check("vector_clear_is_safe", pins.empty());
    }
    {
        // std::move the whole vector: pins follow the buffer, originals drained.
        std::vector<global_ref> src;
        src.emplace_back();
        src.emplace_back(oop_of(0x1250u));
        std::vector<global_ref> dst{ std::move(src) };
        check("vector_move_transfers_pins",
              dst.size() == 2 && is_empty(dst[0]) && holds(dst[1], oop_of(0x1250u)));
    }
    {
        // A std::unique_ptr<global_ref> -- heap-owned pin, deleted via dtor.
        auto heap_pin = std::make_unique<global_ref>();
        check("unique_ptr_global_ref_is_empty", is_empty(*heap_pin));
        *heap_pin = global_ref{ oop_of(0x1260u) };
        check("unique_ptr_global_ref_rearms", holds(*heap_pin, oop_of(0x1260u)));
        heap_pin->reset();
        check("unique_ptr_global_ref_reset_safe", is_empty(*heap_pin));
        heap_pin.reset();  // delete the heap global_ref -> dtor runs
        check("unique_ptr_global_ref_delete_safe", heap_pin == nullptr);
    }

    // ========================================================================
    // SECTION 13 -- Lifetime / scope nesting: many ctor+dtor cycles.
    //   A tight loop of build-then-destroy proves there is no leak / corruption
    //   path that accumulates across repeated RAII cycles.
    // ========================================================================
    {
        bool ok{ true };
        for (int i = 0; i < 1000; ++i)
        {
            auto* const bits{ oop_of(static_cast<std::uintptr_t>(0x10000 + i)) };
            global_ref scoped{ bits };
            ok = ok && holds(scoped, bits);
            scoped.reset();
            ok = ok && is_empty(scoped);
        }  // dtor each iteration
        check("repeated_raii_cycles_stable", ok);
    }
    {
        // Nested scopes with overlapping lifetimes, inner moved out to outer:
        // the address outlives the inner scope inside the outer holder.
        auto* const fake_oop{ oop_of(0x13100u) };
        global_ref outer{};
        {
            global_ref inner{ fake_oop };
            outer = std::move(inner);
            check("nested_inner_moved_out_empty", is_empty(inner));  // NOLINT(bugprone-use-after-move)
        }  // inner dtor
        check("nested_outer_holds_moved_address", holds(outer, fake_oop));
    }

    // ========================================================================
    // SECTION 14 -- std::swap: the move-only swap path (move-ctor + 2 move-
    //   assigns under the hood) must EXCHANGE the two stored addresses.
    // ========================================================================
    {
        auto* const fake_a{ oop_of(0xA000u) };
        auto* const fake_b{ oop_of(0xB000u) };
        global_ref a{ fake_a };
        global_ref b{ fake_b };
        using std::swap;
        swap(a, b);
        check("swap_exchanges_addresses_a", holds(a, fake_b));
        check("swap_exchanges_addresses_b", holds(b, fake_a));
        swap(a, b);  // swap back
        check("swap_roundtrip_restores_a", holds(a, fake_a));
        check("swap_roundtrip_restores_b", holds(b, fake_b));
    }
    {
        // Swap an armed holder with an empty one: the emptiness swaps too.
        auto* const fake_oop{ oop_of(0xB800u) };
        global_ref armed{ fake_oop };
        global_ref empty{};
        using std::swap;
        swap(armed, empty);
        check("swap_armed_with_empty_src_now_empty", is_empty(armed));
        check("swap_armed_with_empty_dst_now_armed", holds(empty, fake_oop));
    }
    {
        // Self-swap via std::swap: std::swap(a, a) routes both refs to the same
        // object internally (NOT a syntactic self-move, so -Wself-move stays
        // silent) and must leave the stored address intact.
        auto* const fake_oop{ oop_of(0xC000u) };
        global_ref a{ fake_oop };
        using std::swap;
        swap(a, a);
        check("self_swap_preserves_address", holds(a, fake_oop));
    }
    {
        // A 3-way rotation built from two swaps; track where each address lands.
        auto* const fake_b{ oop_of(0xD000u) };
        global_ref a{};
        global_ref b{ fake_b };
        global_ref c{ static_cast<vmhook::oop_t>(nullptr) };
        using std::swap;
        swap(a, b);   // a <- 0xD000, b <- empty
        swap(b, c);   // b <- empty,  c <- empty
        check("three_way_swap_a_holds", holds(a, fake_b));
        check("three_way_swap_b_empty", is_empty(b));
        check("three_way_swap_c_empty", is_empty(c));
    }

    // ========================================================================
    // SECTION 15 -- Return-by-value / NRVO / guaranteed-elision through a real
    //   function boundary.  make_ref() returns a prvalue global_ref; bind it,
    //   move-assign it, and feed it straight into a container.  The address must
    //   survive the move-on-return in every path.
    // ========================================================================
    {
        global_ref r{ make_ref(0xE000u) };   // move-on-return into a fresh pin
        check("return_by_value_carries_address", holds(r, oop_of(0xE000u)));

        global_ref sink{};
        sink = make_ref(0xE100u);             // move-assign from a returned prvalue
        check("return_by_value_move_assigns_address", holds(sink, oop_of(0xE100u)));

        // Returned prvalue consumed directly (a temporary that lives only for
        // the full expression, then its dtor runs).
        check("return_by_value_temporary_holds", holds(make_ref(0xE200u), oop_of(0xE200u)));
    }
    {
        // A returned pin pushed into a vector (the prvalue is moved into the
        // element slot -- exercises emplace-from-prvalue, not emplace-in-place).
        std::vector<global_ref> v;
        v.push_back(make_ref(0xE300u));
        v.push_back(make_ref(0xE400u));
        check("return_by_value_into_vector",
              v.size() == 2 && holds(v.front(), oop_of(0xE300u))
                  && holds(v.back(), oop_of(0xE400u)));
    }

    // ========================================================================
    // SECTION 16 -- Ternary / conditional yielding a prvalue global_ref.  The
    //   selected arm's address must be the one that survives into the binding.
    // ========================================================================
    {
        const bool take_left{ (failures % 2) == 0 };
        global_ref chosen{ take_left ? global_ref{ oop_of(0xF000u) }
                                     : global_ref{ static_cast<vmhook::oop_t>(nullptr) } };
        check("ternary_prvalue_selects_correct_arm",
              take_left ? holds(chosen, oop_of(0xF000u)) : is_empty(chosen));
    }

    // ========================================================================
    // SECTION 17 -- std::optional<global_ref>: a move-only payload in optional.
    //   emplace / reset / move-construct / move-assign the optional and confirm
    //   the contained holder's address survives each transition.
    // ========================================================================
    {
        std::optional<global_ref> opt;
        check("optional_starts_disengaged", !opt.has_value());

        opt.emplace(oop_of(0x11000u));           // construct a holder in place
        check("optional_emplaced_engaged", opt.has_value());
        check("optional_emplaced_value_holds", holds(*opt, oop_of(0x11000u)));

        opt.reset();                              // destroy the contained holder
        check("optional_reset_disengaged", !opt.has_value());

        // Move-construct an engaged optional into another; the address migrates
        // to the destination and the source's contained holder is emptied.
        std::optional<global_ref> src;
        src.emplace(oop_of(0x11100u));
        std::optional<global_ref> dst{ std::move(src) };
        check("optional_move_ctor_dst_holds", dst.has_value() && holds(*dst, oop_of(0x11100u)));
        check("optional_move_ctor_src_contained_emptied",
              src.has_value() && is_empty(*src));   // NOLINT(bugprone-use-after-move)

        // Move-assign a fresh engaged optional over an engaged one.
        std::optional<global_ref> reassigned;
        reassigned.emplace(oop_of(0x12000u));
        reassigned = std::optional<global_ref>{ global_ref{ oop_of(0x13000u) } };
        check("optional_move_assign_holds_new_address",
              reassigned.has_value() && holds(*reassigned, oop_of(0x13000u)));
    }

    // ========================================================================
    // SECTION 18 -- global_ref inside std::pair / std::tuple (move-only members
    //   in aggregates the STL moves as a unit).  Build, move the whole
    //   pair/tuple, and confirm each embedded address survives.
    // ========================================================================
    {
        std::pair<int, global_ref> p{ 7, global_ref{ oop_of(0x14000u) } };
        check("pair_member_holds", holds(p.second, oop_of(0x14000u)));
        std::pair<int, global_ref> moved{ std::move(p) };
        check("pair_moved_member_holds",
              moved.first == 7 && holds(moved.second, oop_of(0x14000u)));
        check("pair_moved_from_member_emptied", is_empty(p.second));  // NOLINT(bugprone-use-after-move)
    }
    {
        std::tuple<global_ref, int, global_ref> t{
            global_ref{ oop_of(0x15000u) }, 9, global_ref{ static_cast<vmhook::oop_t>(nullptr) } };
        check("tuple_members_hold",
              holds(std::get<0>(t), oop_of(0x15000u)) && std::get<1>(t) == 9
                  && is_empty(std::get<2>(t)));
        std::tuple<global_ref, int, global_ref> moved{ std::move(t) };
        check("tuple_moved_members_hold",
              holds(std::get<0>(moved), oop_of(0x15000u)) && std::get<1>(moved) == 9
                  && is_empty(std::get<2>(moved)));
        check("tuple_moved_from_member_emptied", is_empty(std::get<0>(t)));  // NOLINT(bugprone-use-after-move)
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
        bool all_hold{ dq.size() == 40 };
        for (int i = 0; i < 40; ++i)
        {
            all_hold = all_hold
                && holds(dq[static_cast<std::size_t>(i)],
                         oop_of(static_cast<std::uintptr_t>(0x16000 + i)));
        }
        check("deque_growth_preserves_addresses", all_hold);
        dq.pop_front();   // shrink from the front (deque-specific path)
        dq.pop_back();
        check("deque_pop_ends_safe",
              dq.size() == 38 && holds(dq.front(), oop_of(0x16001u))
                  && holds(dq.back(), oop_of(0x16000u + 38u)));
        dq.clear();
        check("deque_clear_safe", dq.empty());
    }
    {
        std::map<int, global_ref> m;
        m.emplace(1, global_ref{ oop_of(0x17000u) });
        m.emplace(2, global_ref{ static_cast<vmhook::oop_t>(nullptr) });
        m.emplace(3, global_ref{});
        // operator[] default-constructs then move-assigns a fresh holder in.
        m[4] = global_ref{ oop_of(0x17400u) };
        check("map_of_pins_holds_addresses",
              m.size() == 4 && holds(m.at(1), oop_of(0x17000u)) && is_empty(m.at(2))
                  && is_empty(m.at(3)) && holds(m.at(4), oop_of(0x17400u)));
        m.erase(2);   // node removal -> dtor
        check("map_erase_safe",
              m.size() == 3 && holds(m.at(1), oop_of(0x17000u))
                  && holds(m.at(4), oop_of(0x17400u)));
    }

    // ========================================================================
    // SECTION 20 -- Bulk move between containers via move-iterators.  Drains the
    //   source elements (each left empty) into the destination, which must end
    //   up holding every address in order.
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
        bool dst_ok{ dst.size() == 16 };
        for (int i = 0; i < 16; ++i)
        {
            dst_ok = dst_ok
                && holds(dst[static_cast<std::size_t>(i)],
                         oop_of(static_cast<std::uintptr_t>(0x18000 + i)));
        }
        check("move_iterator_transfer_dst_holds_all", dst_ok);
        // The source elements were moved-from: still alive, now empty (no
        // resurrection, no duplicated address in two containers at once).
        bool src_drained{ src.size() == 16 };
        for (const auto& p : src)
        {
            src_drained = src_drained && is_empty(p);   // NOLINT(bugprone-use-after-move)
        }
        check("move_iterator_transfer_src_drained", src_drained);
    }
    {
        // std::vector::insert in the MIDDLE move-shifts the tail RIGHT (distinct
        // from the erase/left-shift already covered) -- every address must land
        // in its new slot unchanged.
        std::vector<global_ref> v;
        for (int i = 0; i < 20; ++i)
        {
            v.emplace_back(oop_of(static_cast<std::uintptr_t>(0x19000 + i)));
        }
        v.insert(v.begin() + 5, global_ref{ oop_of(0x19500u) });
        bool ok{ v.size() == 21
            && holds(v[4], oop_of(0x19004u))
            && holds(v[5], oop_of(0x19500u))     // the inserted element
            && holds(v[6], oop_of(0x19005u))     // the shifted-right tail
            && holds(v.back(), oop_of(0x19013u)) };
        check("vector_insert_middle_shifts_addresses_right", ok);

        // resize UP (default-constructs new empty holders) then DOWN (destroys
        // the tail; the survivors keep their addresses).
        v.resize(30);
        check("vector_resize_up_appends_empty",
              v.size() == 30 && is_empty(v.back()) && holds(v[5], oop_of(0x19500u)));
        v.resize(3);
        check("vector_resize_down_destroys_tail",
              v.size() == 3 && holds(v.front(), oop_of(0x19000u))
                  && holds(v.back(), oop_of(0x19002u)));
    }

    // ========================================================================
    // SECTION 21 -- C-array / std::array of holders, and a deeper double-use-
    //   after-move chain (a moved-from holder used as BOTH a move source AND a
    //   move destination repeatedly).  No address is ever resurrected.
    // ========================================================================
    {
        // Double braces: std::array is an aggregate wrapping a C array, so a
        // single brace level draws clang's -Wmissing-braces under -Werror.
        std::array<global_ref, 4> arr{ {
            global_ref{ oop_of(0x1A000u) },
            global_ref{ static_cast<vmhook::oop_t>(nullptr) },
            global_ref{ oop_of(0x1A200u) },
            global_ref{} } };
        check("std_array_of_pins_holds",
              holds(arr[0], oop_of(0x1A000u)) && is_empty(arr[1])
                  && holds(arr[2], oop_of(0x1A200u)) && is_empty(arr[3]));
    }
    {
        // A moved-from holder is a valid empty object: move it AGAIN (as
        // source), then move INTO it (as destination), alternating several
        // times.  The address must never be duplicated or resurrected.
        auto* const fake_oop{ oop_of(0x1B000u) };
        global_ref a{ fake_oop };
        global_ref b{ std::move(a) };          // b takes 0x1B000, a moved-from
        global_ref c{ std::move(a) };          // move the moved-from a AGAIN  // NOLINT(bugprone-use-after-move)
        check("double_move_from_same_source_a_empty", is_empty(a));   // NOLINT(bugprone-use-after-move)
        check("double_move_from_same_source_b_holds", holds(b, fake_oop));
        check("double_move_from_same_source_c_empty", is_empty(c));

        a = std::move(b);                       // move INTO the moved-from a
        check("reassign_into_moved_from_a_holds", holds(a, fake_oop));
        check("reassign_into_moved_from_b_empty", is_empty(b));   // NOLINT(bugprone-use-after-move)

        a = std::move(c);                       // and again -- c is empty, so a is cleared
        check("reassign_into_moved_from_again_a_empty", is_empty(a));
        check("reassign_into_moved_from_again_c_empty", is_empty(c));   // NOLINT(bugprone-use-after-move)
    }

    // ========================================================================
    // SECTION 22 -- re-arm after reset / re-arm after move; and the laundered
    //   self-move over both the empty and armed states.  Confirms reset() does
    //   not poison the object for a subsequent re-arm.
    // ========================================================================
    {
        global_ref g{ oop_of(0x1C000u) };
        check("reset_then_repin_initial_holds", holds(g, oop_of(0x1C000u)));
        g.reset();
        check("reset_then_repin_reset_empty", is_empty(g));
        g = global_ref{ oop_of(0x1C100u) };     // re-arm via move-assign from prvalue
        check("reset_then_repin_rearmed_holds", holds(g, oop_of(0x1C100u)));
        g.reset();
        check("reset_then_repin_final_reset_empty", is_empty(g));
    }
    {
        // Move OUT of g (g becomes moved-from), then re-arm g via assignment, in
        // a loop -- proves an object can cycle move-out / re-arm indefinitely
        // and that each cycle's address lands where it should.
        global_ref g{};
        bool ok{ true };
        for (int i = 0; i < 64; ++i)
        {
            global_ref taken{ std::move(g) };
            ok = ok && is_empty(g);             // NOLINT(bugprone-use-after-move)
            auto* const bits{ oop_of(static_cast<std::uintptr_t>(0x1D000 + i)) };
            g = global_ref{ bits };
            ok = ok && holds(g, bits);
            (void)taken;                         // taken dtors here
        }
        check("move_out_then_rearm_cycle_stable", ok);
    }
    {
        // Laundered self-move-assign (no syntactic self-move -> no -Wself-move),
        // exercising the runtime `this != &other` guard on both states.
        global_ref empty_self{};
        launder_move_assign(empty_self, empty_self);
        check("laundered_self_move_empty_safe", is_empty(empty_self));

        auto* const fake_oop{ oop_of(0x1E000u) };
        global_ref oop_self{ fake_oop };
        launder_move_assign(oop_self, oop_self);
        check("laundered_self_move_preserves_address", holds(oop_self, fake_oop));

        // Repeated laundered self-moves accumulate no corruption.
        auto* const repeat_oop{ oop_of(0x1E100u) };
        global_ref repeat_self{ repeat_oop };
        launder_move_assign(repeat_self, repeat_self);
        launder_move_assign(repeat_self, repeat_self);
        launder_move_assign(repeat_self, repeat_self);
        check("laundered_self_move_repeated_preserves_address", holds(repeat_self, repeat_oop));
    }

    // ========================================================================
    // SECTION 23 -- Exhaustive bit-pattern sweep of the from-OOP ctor.  Drives
    //   the ctor with addresses that look like JNI-tagged handles (every low-3-
    //   bit tag), is_valid_pointer debug-fill sentinels, alignment edges, and
    //   pointer-width extremes.  The stub applies NO masking, NO untagging and
    //   NO validation, so every pattern must come back out bit-for-bit -- if a
    //   future change starts masking tag bits, these fail loudly instead of
    //   silently corrupting addresses.  (Nothing here is dereferenced.)
    // ========================================================================
    {
        // All eight low-3-bit "tag" values OR'd onto an otherwise-valid base, to
        // mimic JDK 9+ tagged JNI handles the ctor would receive on a live JVM.
        bool all_hold{ true };
        for (std::uintptr_t tag = 0; tag < 8u; ++tag)
        {
            const std::uintptr_t bits{ 0x20000u | tag };
            global_ref g{ oop_of(bits) };
            all_hold = all_hold && holds(g, oop_of(bits));
        }
        check("ctor_all_low3_tag_patterns_stored_unmasked", all_hold);
    }
    {
        // The debug-fill / sentinel low-32 patterns is_valid_pointer rejects.
        // The ctor does no validation, so all are stored verbatim.
        const std::uintptr_t sentinels[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool all_hold{ true };
        for (const std::uintptr_t bits : sentinels)
        {
            global_ref g{ oop_of(bits) };
            all_hold = all_hold && holds(g, oop_of(bits));
        }
        check("ctor_sentinel_patterns_stored_verbatim", all_hold);
    }
    {
        // Alignment edges and pointer-width extremes (all reinterpret-only;
        // never dereferenced).  Each is stored verbatim, and reset() clears it.
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
        bool all_ok{ true };
        for (const std::uintptr_t bits : edges)
        {
            global_ref g{ oop_of(bits) };
            all_ok = all_ok && holds(g, oop_of(bits));
            g.reset();
            all_ok = all_ok && is_empty(g);
        }
        check("ctor_alignment_and_width_edges_stored_then_reset", all_ok);
    }

    // ========================================================================
    // SECTION W28 -- LEDGER-driven deepening: re-pin the explicit "default ctor
    //   holds nullptr", "move-only via deleted copy", "reset()/dtor safe on
    //   null" and the noexcept triad on a FRESH set of static_asserts + runtime
    //   checks, so a regression cannot quietly remove ANY ONE of them.
    // ========================================================================

    // Move-only contract via the precise deleted-vs-defined trait pairing.
    static_assert(std::is_nothrow_move_constructible_v<global_ref>
                      && std::is_nothrow_move_assignable_v<global_ref>
                      && !std::is_copy_constructible_v<global_ref>
                      && !std::is_copy_assignable_v<global_ref>,
                  "global_ref must be move-only with noexcept moves");

    // Default ctor + dtor + reset() all noexcept (the "safe on null" trio).
    static_assert(std::is_nothrow_default_constructible_v<global_ref>
                      && std::is_nothrow_destructible_v<global_ref>
                      && noexcept(std::declval<global_ref&>().reset()),
                  "default ctor, destructor, and reset() must all be noexcept");

    // Runtime: default-construct holds nullptr across every accessor.
    {
        global_ref g{};
        check("w28_default_ctor_holds_nullptr",
              g.handle() == nullptr && g.oop() == nullptr && !static_cast<bool>(g));
    }
    // Runtime: reset() on an already-null holder is a no-op (still null, idempotent).
    {
        global_ref g{};
        g.reset();
        g.reset();
        g.reset();
        check("w28_reset_idempotent_on_null",
              g.handle() == nullptr && g.oop() == nullptr && !static_cast<bool>(g));
    }
    // Runtime: destructor on a null holder is safe (exits cleanly).
    {
        {
            global_ref g{};
            (void)g;
        }
        check("w28_dtor_safe_on_null", true);
    }
    // Runtime: nullptr ctor matches default ctor's empty state.
    {
        global_ref a{};
        global_ref b{ nullptr };
        check("w28_nullptr_ctor_matches_default",
              is_empty(a) && is_empty(b));
    }
    // Runtime: an armed holder is the exact complement of an empty one across
    // all three views at once (bool / oop() / handle()), so a future change that
    // desyncs one view from the others fails here.
    {
        auto* const fake_oop{ oop_of(0x1F000u) };
        global_ref g{ fake_oop };
        check("w28_armed_views_are_consistent",
              static_cast<bool>(g) && g.oop() == fake_oop && g.handle() == fake_oop
                  && !is_empty(g));
        g.reset();
        check("w28_armed_then_reset_flips_all_views",
              !static_cast<bool>(g) && g.oop() == nullptr && g.handle() == nullptr
                  && is_empty(g));
    }

    return failures == 0 ? 0 : 1;
}
