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

#include <cstdio>
#include <cstdint>
#include <memory>
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

    return failures == 0 ? 0 : 1;
}
