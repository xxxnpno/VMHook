// Standalone (no-JVM) contract test for the anchored-reference model:
// vmhook::ref / vmhook::root / vmhook::borrowed and the detail::access proxy.
//
// ===========================================================================
// WHAT THIS FILE CAN AND CANNOT PROVE
// ===========================================================================
// No jvm.dll / libjvm.so is loaded into this binary, so gHotSpotVMStructs and
// gHotSpotVMTypes never resolve, every klass lookup returns null, and
// vmhook::gc_epoch() reports an INVALID sample.  That makes this the
// GRACEFUL-DEGRADATION and VALUE-SEMANTICS test:
//
//   * an anchor chain can be BUILT with no VM, and resolving it yields nullptr
//     rather than a fault, a throw, or a plausible-looking address;
//   * an EPHEMERAL ref is born EXPIRED when the epoch cannot be vouched for --
//     the fail-closed arm of the design.  This is also the no-JVM spelling of
//     "epoch invalidation makes an ephemeral ref expire": gc_epoch_changed() is
//     unconditionally true here, which is exactly the state a real collection
//     produces on a live VM;
//   * copy / move / assignment behave as ordinary C++ values, and a moved-from
//     ref is empty rather than poisoned;
//   * an over-long anchor chain is refused at BUILD time (k_max_anchor_depth);
//   * a vmhook::root binds lazily and reports "not bound" instead of guessing;
//   * detail::access binds a wrapper for one expression and cannot be hoisted.
//
// It CANNOT prove that a ref survives a real relocation, that get_java_mirror()
// re-derivation actually tracks a moved mirror, or that the walk decodes real
// narrow oops.  All of that needs a live VM and is covered by the out-of-tree
// live harness.  Nothing below asserts any of it.
//
// SAFETY: the addresses used as anchors here are the addresses of REAL, mapped,
// aligned buffers owned by this process, so is_valid_pointer's range/alignment
// heuristic admits them and the library's fault-safe reads are exercised on
// genuinely readable memory.  Nothing in the library dereferences them as a
// Java object -- every hop stops at the first unresolvable VMStruct.
// ===========================================================================
#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// A minimal wrapper in the library's own CRTP shape.  Deliberately NOT
// register_class'd: an unregistered wrapper is the worst case for every
// name-driven path below, and none of them may crash because of it.
// ---------------------------------------------------------------------------
class ar_entity final : public vmhook::object<ar_entity>
{
public:
    explicit ar_entity(const vmhook::oop_t oop = nullptr) noexcept
        : vmhook::object<ar_entity>{ oop }
    {
    }
};

namespace
{
    // A real, mapped, 8-aligned block this process owns.  Used ONLY as an
    // anchor value; the library never treats it as a Java object because every
    // VMStruct lookup fails first.
    alignas(16) std::uint8_t g_block_a[256]{};
    alignas(16) std::uint8_t g_block_b[256]{};

    auto address_a() noexcept -> vmhook::oop_t { return static_cast<vmhook::oop_t>(g_block_a); }
    auto address_b() noexcept -> vmhook::oop_t { return static_cast<vmhook::oop_t>(g_block_b); }

    auto fake_klass() noexcept -> vmhook::hotspot::klass*
    {
        return reinterpret_cast<vmhook::hotspot::klass*>(static_cast<void*>(g_block_a));
    }
}

// ===========================================================================
// COMPILE-TIME PINS on the type properties.  These are the parts of the
// contract a runtime check cannot express, and the parts a refactor is most
// likely to break silently.
// ===========================================================================

// -- anchor_kind: a scoped, byte-wide enum whose `empty` enumerator is zero, so
//    a value-initialised ref reports "no anchor" rather than a named shape.
static_assert(std::is_enum_v<vmhook::anchor_kind>,
              "anchor_kind must be an enum");
static_assert(!std::is_convertible_v<vmhook::anchor_kind, int>,
              "anchor_kind must be SCOPED (no implicit conversion to int)");
static_assert(std::is_same_v<std::underlying_type_t<vmhook::anchor_kind>, std::uint8_t>,
              "anchor_kind must be uint8_t-backed (it is stored in every anchor node)");
static_assert(static_cast<std::uint8_t>(vmhook::anchor_kind::empty) == 0u,
              "anchor_kind::empty must be the zero value so defaults fail closed");

// -- ref<T> is an ordinary, cheap C++ VALUE.  Copyable is the headline
//    difference from vmhook::oop_pin (which is move-only); a snapshot that
//    copies ~80 refs per tick depends on it.
static_assert(std::is_default_constructible_v<vmhook::ref<ar_entity>>,
              "ref must be default-constructible (the empty state)");
static_assert(std::is_nothrow_default_constructible_v<vmhook::ref<ar_entity>>,
              "ref's empty state must not allocate");
static_assert(std::is_copy_constructible_v<vmhook::ref<ar_entity>>,
              "ref MUST be copyable -- this is the whole point vs oop_pin");
static_assert(std::is_copy_assignable_v<vmhook::ref<ar_entity>>,
              "ref must be copy-assignable");
static_assert(std::is_nothrow_move_constructible_v<vmhook::ref<ar_entity>>,
              "ref move must be noexcept so it is cheap inside std::vector growth");
static_assert(std::is_nothrow_move_assignable_v<vmhook::ref<ar_entity>>,
              "ref move-assign must be noexcept");
static_assert(std::is_nothrow_destructible_v<vmhook::ref<ar_entity>>,
              "ref destruction releases nothing VM-side -> must be noexcept on ANY thread");

// -- THE ANTI-CONVERSION PINS.  A raw oop must never become a ref implicitly,
//    and a ref must never decay to a raw address or to bool.  The named
//    constructors (at_static / ephemeral) are the only doors in, precisely so
//    "wrap this pointer I happen to have" is a deliberate act.
static_assert(!std::is_constructible_v<vmhook::ref<ar_entity>, vmhook::oop_t>,
              "a ref must NOT be constructible from a raw oop -- use ref::ephemeral()");
static_assert(!std::is_constructible_v<vmhook::ref<ar_entity>, void*>,
              "a ref must NOT be constructible from a raw void*");
static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::ref<ar_entity>>,
              "a raw oop must NOT implicitly convert to a ref");
static_assert(!std::is_convertible_v<vmhook::ref<ar_entity>, vmhook::oop_t>,
              "a ref must NOT decay to a raw oop");
static_assert(!std::is_convertible_v<vmhook::ref<ar_entity>, bool>,
              "operator bool must be EXPLICIT (no accidental arithmetic/overload traps)");
static_assert(std::is_constructible_v<bool, vmhook::ref<ar_entity>>,
              "... but `if (r)` must still work");
// Different wrapper types are different ref types -- no silent cross-typing.
static_assert(!std::is_convertible_v<vmhook::ref<ar_entity>, vmhook::ref<void>>,
              "ref<T> must not implicitly convert to ref<void> -- use erased()");

// -- resolve() is the hot path and runs inside detours: it must be noexcept.
static_assert(noexcept(std::declval<const vmhook::ref<ar_entity>&>().resolve()),
              "ref::resolve() must be noexcept (it runs inside detours)");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::ref<ar_entity>&>().resolve()),
                             vmhook::oop_t>,
              "ref::resolve() yields a raw oop -- the single documented escape hatch");
static_assert(noexcept(std::declval<const vmhook::ref<ar_entity>&>().id()),
              "ref::id() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::ref<ar_entity>&>().expired()),
              "ref::expired() must be noexcept");

// -- access<T>: the one-expression binding proxy.  Deleting copy AND move is
//    what stops `auto a = ref.operator->();` from smuggling a bound wrapper
//    past the revalidation that produced it.
static_assert(!std::is_copy_constructible_v<vmhook::detail::access<ar_entity>>,
              "access must NOT be copyable -- it may not outlive its expression");
static_assert(!std::is_move_constructible_v<vmhook::detail::access<ar_entity>>,
              "access must NOT be movable -- same reason");
static_assert(!std::is_default_constructible_v<vmhook::detail::access<ar_entity>>,
              "access only exists bound to an address");

// -- borrowed<T>: the allocation-free detour-scoped view.
static_assert(std::is_trivially_copyable_v<vmhook::borrowed<ar_entity>>,
              "borrowed must be trivially copyable -- it allocates no root");
static_assert(std::is_trivially_destructible_v<vmhook::borrowed<ar_entity>>,
              "borrowed must be trivially destructible");
static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::borrowed<ar_entity>>,
              "the borrowed(oop) constructor must be explicit");
static_assert(std::is_constructible_v<vmhook::borrowed<ar_entity>, vmhook::oop_t>,
              "... but explicitly constructible from a detour-scoped address");

// -- root<T>: a long-lived singleton handle, deliberately NOT copyable (it owns
//    the bind mutex; get() hands out the cheap copies instead).
static_assert(!std::is_copy_constructible_v<vmhook::root<ar_entity>>,
              "root must not be copyable -- copy the ref it hands out instead");
static_assert(!std::is_move_constructible_v<vmhook::root<ar_entity>>,
              "root must not be movable");
static_assert(std::is_default_constructible_v<vmhook::root<ar_entity>>,
              "an unnamed root must be constructible and simply never bind");
static_assert(std::is_constructible_v<vmhook::root<ar_entity>, std::string_view, std::string_view>,
              "root(class, field) must exist");
static_assert(std::is_constructible_v<vmhook::root<ar_entity>, std::string_view>,
              "root(field) must exist for the wrapper's own registered class");
static_assert(!std::is_convertible_v<std::string_view, vmhook::root<ar_entity>>,
              "the one-argument root constructor must be explicit");

// -- ref<void> is the type-erased form: it still resolves and still has an
//    identity, but it must NOT offer operator-> (there is no wrapper to bind).
//    Expressed through a named concept rather than an inline requires-expression
//    because GCC diagnoses `r.operator->()` written inline as a hard error.
template<typename handle_type>
concept ar_binds_a_wrapper = requires(const handle_type& handle) { *handle; };

static_assert(!ar_binds_a_wrapper<vmhook::ref<void>>,
              "ref<void> must not bind a wrapper -- there is nothing to bind");
static_assert(ar_binds_a_wrapper<vmhook::ref<ar_entity>>,
              "ref<T> must bind a wrapper for a type constructible from an oop");
static_assert(!ar_binds_a_wrapper<vmhook::borrowed<void>>,
              "borrowed<void> must not bind a wrapper either");
static_assert(ar_binds_a_wrapper<vmhook::borrowed<ar_entity>>,
              "borrowed<T> must bind a wrapper");

int main()
{
    // =======================================================================
    // SECTION 1 -- The empty state.  A value-initialised ref names nothing and
    //   says so on every axis, without touching the VM at all.
    // =======================================================================
    {
        const vmhook::ref<ar_entity> nothing{};
        check("empty_ref_is_empty", nothing.empty());
        check("empty_ref_is_not_anchored", !nothing.anchored());
        check("empty_ref_kind_is_empty", nothing.kind() == vmhook::anchor_kind::empty);
        check("empty_ref_depth_is_zero", nothing.depth() == 0u);
        check("empty_ref_resolves_to_null", nothing.resolve() == nullptr);
        check("empty_ref_is_falsy", !static_cast<bool>(nothing));
        // `expired` is deliberately DISTINCT from `empty`: an empty ref was
        // never given an object, so it has not expired -- it has nothing to
        // expire.  Collapsing the two would hide the difference between "you
        // never set this" and "the object went away".
        check("empty_ref_has_not_expired", !nothing.expired());
        check("empty_ref_has_no_identity", !static_cast<bool>(nothing.id()));
        check("empty_ref_class_name_is_empty", nothing.class_name().empty());
        check("empty_ref_raw_unsafe_is_null", nothing.raw_unsafe() == nullptr);
    }

    // =======================================================================
    // SECTION 2 -- EPHEMERAL refs: born stamped with the collection epoch, and
    //   refused the moment that epoch cannot be vouched for.  With no VM the
    //   epoch is permanently unreadable, which is the same state a real
    //   relocating collection produces -- so this section IS the no-JVM
    //   spelling of "epoch invalidation makes an ephemeral ref expire".
    // =======================================================================
    {
        const vmhook::ref<ar_entity> transient{ vmhook::ref<ar_entity>::ephemeral(address_a()) };

        check("ephemeral_ref_is_not_empty", !transient.empty());
        check("ephemeral_ref_kind_is_ephemeral",
              transient.kind() == vmhook::anchor_kind::ephemeral);
        check("ephemeral_ref_is_not_anchored", !transient.anchored());
        // THE CONTRACT: expired, never dangling.
        check("ephemeral_ref_expires_when_the_epoch_is_unreadable", transient.expired());
        check("ephemeral_ref_resolves_to_null_once_expired", transient.resolve() == nullptr);
        check("ephemeral_ref_is_falsy_once_expired", !static_cast<bool>(transient));
        check("ephemeral_ref_never_hands_back_the_captured_address",
              transient.resolve() != address_a());
        check("ephemeral_ref_raw_unsafe_still_shows_the_capture",
              transient.raw_unsafe() == address_a());
        check("ephemeral_ref_still_has_an_identity", static_cast<bool>(transient.id()));
    }

    // A null address yields an EMPTY ref, not an expired one -- wrapping
    // "nothing" must not manufacture an anchor.
    {
        const vmhook::ref<ar_entity> from_null{
            vmhook::ref<ar_entity>::ephemeral(vmhook::oop_t{ nullptr }) };
        check("ephemeral_of_null_is_empty", from_null.empty());
        check("ephemeral_of_null_has_no_identity", !static_cast<bool>(from_null.id()));
    }

    // Sweep bit patterns: neither the capture nor the refusal may depend on the
    // value.  (None of these is dereferenced by the test or by the library --
    // every walk stops at the first unresolvable VMStruct.)
    {
        const std::uintptr_t patterns[]{ 0x8u, 0x1000u, 0xDEADBEE0u, 0xFFFFFFF8u };
        bool all_captured{ true };
        bool all_refused{ true };
        for (const std::uintptr_t bits : patterns)
        {
            auto* const fake{ reinterpret_cast<vmhook::oop_t>(bits) };
            const vmhook::ref<ar_entity> r{ vmhook::ref<ar_entity>::ephemeral(fake) };
            all_captured = all_captured && r.raw_unsafe() == fake;
            all_refused  = all_refused && r.resolve() == nullptr && !static_cast<bool>(r);
        }
        check("every_ephemeral_bit_pattern_is_captured_verbatim", all_captured);
        check("every_ephemeral_bit_pattern_is_refused", all_refused);
    }

    // =======================================================================
    // SECTION 3 -- ANCHORED refs: a chain can be BUILT with no VM, and walking
    //   it degrades to nullptr instead of faulting.  This is the path that, on a
    //   live VM, re-derives through Klass::_java_mirror.
    // =======================================================================
    {
        const vmhook::ref<ar_entity> anchored{
            vmhook::ref<ar_entity>::at_static(fake_klass(), 0x18u) };

        check("static_anchor_is_buildable_without_a_jvm", !anchored.empty());
        check("static_anchor_kind_is_static_root",
              anchored.kind() == vmhook::anchor_kind::static_root);
        check("static_anchor_reports_anchored", anchored.anchored());
        check("static_anchor_depth_is_zero", anchored.depth() == 0u);
        // No VM -> Klass::_java_mirror never resolves -> the walk stops at the
        // root and yields null.  No fault, no throw, no invented address.
        check("static_anchor_resolves_to_null_without_a_jvm", anchored.resolve() == nullptr);
        check("static_anchor_is_falsy_without_a_jvm", !static_cast<bool>(anchored));
        check("static_anchor_reports_expired_without_a_jvm", anchored.expired());

        // Repeated resolution must stay stable and keep not-faulting: this is
        // the call pattern operator-> produces, once per dereference.
        bool stable{ true };
        for (int i{ 0 }; i < 20000; ++i)
        {
            stable = stable && anchored.resolve() == nullptr;
        }
        check("static_anchor_resolution_is_stable_over_many_calls", stable);
    }

    // A null / unusable klass must produce an EMPTY ref, not an anchor that
    // resolves through garbage.
    {
        const vmhook::ref<ar_entity> bad{ vmhook::ref<ar_entity>::at_static(nullptr, 8u) };
        check("static_anchor_on_a_null_klass_is_empty", bad.empty());
        check("static_anchor_on_a_null_klass_resolves_null", bad.resolve() == nullptr);
    }

    // Named lookup: with no VM the class is not loaded, so the ref comes back
    // EMPTY -- a value, never an exception and never a half-built anchor.
    {
        const vmhook::ref<ar_entity> by_name{
            vmhook::ref<ar_entity>::at_static("java/lang/System", "out") };
        check("named_static_anchor_is_empty_when_the_class_is_not_loaded", by_name.empty());
        check("named_static_anchor_resolves_null", by_name.resolve() == nullptr);

        const vmhook::ref<ar_entity> free_fn{
            vmhook::static_ref<ar_entity>("java/lang/System", "out") };
        check("static_ref_free_function_agrees", free_fn.empty());
    }

    // =======================================================================
    // SECTION 4 -- CHAINING.  field_at / element extend a chain; the depth is
    //   recorded so an over-long chain is refused at BUILD time rather than
    //   recursing without bound inside a detour.
    // =======================================================================
    {
        const vmhook::ref<ar_entity> base{
            vmhook::ref<ar_entity>::at_static(fake_klass(), 0x10u) };
        const vmhook::ref<void> child{ base.field_at<void>(0x20u) };
        const vmhook::ref<void> grandchild{ child.element<void>(3) };

        check("field_hop_extends_the_chain", !child.empty());
        check("field_hop_kind_is_field_of", child.kind() == vmhook::anchor_kind::field_of);
        check("field_hop_depth_is_one", child.depth() == 1u);
        check("element_hop_kind_is_element_of",
              grandchild.kind() == vmhook::anchor_kind::element_of);
        check("element_hop_depth_is_two", grandchild.depth() == 2u);
        check("chain_resolves_to_null_without_a_jvm",
              child.resolve() == nullptr && grandchild.resolve() == nullptr);

        // A hop off an EMPTY parent has no root, so it must not produce an
        // anchor at all -- a rootless chain is exactly what this design refuses
        // to represent.
        const vmhook::ref<void> orphan{ vmhook::ref<ar_entity>{}.field_at<void>(8u) };
        check("hop_off_an_empty_parent_is_empty", orphan.empty());

        // A negative index is rejected up front rather than at walk time.
        check("negative_element_index_is_refused", base.element<void>(-1).empty());
    }

    // Depth ceiling: build past k_max_anchor_depth and watch the chain refuse to
    // grow.  Every ref up to the ceiling is valid; the one past it is empty.
    {
        vmhook::ref<void> chain{ vmhook::ref<void>::at_static(fake_klass(), 0u) };
        bool grew_to_the_ceiling{ true };
        for (std::uint32_t hop{ 0 }; hop < vmhook::k_max_anchor_depth; ++hop)
        {
            chain = chain.field_at<void>(8u);
            grew_to_the_ceiling = grew_to_the_ceiling && !chain.empty() && chain.depth() == hop + 1u;
        }
        check("chain_grows_up_to_the_documented_ceiling", grew_to_the_ceiling);
        check("chain_is_exactly_k_max_anchor_depth_deep",
              chain.depth() == vmhook::k_max_anchor_depth);

        const vmhook::ref<void> too_deep{ chain.field_at<void>(8u) };
        check("chain_refuses_to_grow_past_the_ceiling", too_deep.empty());
        check("over_deep_chain_resolves_null_not_recursively", too_deep.resolve() == nullptr);
    }

    // =======================================================================
    // SECTION 5 -- VALUE SEMANTICS.  A ref is an ordinary C++ value: copies are
    //   independent, moves leave an empty (not poisoned) source, and none of it
    //   needs a JVM or a particular thread.
    // =======================================================================
    {
        const vmhook::ref<ar_entity> original{
            vmhook::ref<ar_entity>::at_static(fake_klass(), 0x30u) };
        const vmhook::ref<ar_entity> copy{ original };

        check("copy_shares_the_anchor_identity", copy.id() == original.id());
        check("copy_shares_the_anchor_kind", copy.kind() == original.kind());
        check("copy_compares_equal_to_the_original", copy == original);
        check("copy_resolves_the_same_way", copy.resolve() == original.resolve());
    }
    {
        vmhook::ref<ar_entity> source{ vmhook::ref<ar_entity>::at_static(fake_klass(), 0x40u) };
        const vmhook::object_id token{ source.id() };
        const vmhook::ref<ar_entity> destination{ std::move(source) };

        check("move_ctor_transfers_the_anchor", destination.id() == token);
        check("move_ctor_empties_the_source", source.empty());              // NOLINT(bugprone-use-after-move)
        check("moved_from_ref_resolves_null", source.resolve() == nullptr); // NOLINT(bugprone-use-after-move)
        check("moved_from_ref_has_no_identity",
              !static_cast<bool>(source.id()));                             // NOLINT(bugprone-use-after-move)

        // A moved-from ref is an ordinary empty ref, not a poisoned one.
        source = vmhook::ref<ar_entity>::at_static(fake_klass(), 0x48u);    // NOLINT(bugprone-use-after-move)
        check("moved_from_ref_can_be_rearmed", !source.empty());
        check("rearm_does_not_disturb_the_move_destination", destination.id() == token);
    }
    {
        vmhook::ref<ar_entity> a{ vmhook::ref<ar_entity>::at_static(fake_klass(), 0x50u) };
        vmhook::ref<ar_entity> b{ vmhook::ref<ar_entity>::at_static(fake_klass(), 0x58u) };
        const vmhook::object_id token_a{ a.id() };
        b = a;
        check("copy_assign_takes_the_source_anchor", b.id() == token_a);
        b = vmhook::ref<ar_entity>{};
        check("assigning_an_empty_ref_clears_it", b.empty());
        a.reset();
        check("reset_empties_the_ref", a.empty() && a.resolve() == nullptr);
    }
    {
        // Container round-trip: refs survive a vector reallocation with their
        // anchors intact, which is the shape a per-tick snapshot takes.
        std::vector<vmhook::ref<ar_entity>> refs;
        for (std::size_t i{ 0 }; i < 128u; ++i)
        {
            refs.push_back(vmhook::ref<ar_entity>::at_static(fake_klass(), i * 8u));
        }
        bool ok{ true };
        for (std::size_t i{ 0 }; i < refs.size(); ++i)
        {
            ok = ok && !refs[i].empty()
                    && refs[i].kind() == vmhook::anchor_kind::static_root
                    && refs[i].resolve() == nullptr;
        }
        check("vector_growth_preserves_every_anchor", ok);
    }

    // =======================================================================
    // SECTION 6 -- operator-> binds a wrapper for ONE expression.  With no VM
    //   the binding is null and the wrapper's own accessors degrade the way they
    //   always have, without crashing.
    // =======================================================================
    {
        const vmhook::ref<ar_entity> r{ vmhook::ref<ar_entity>::ephemeral(address_a()) };
        check("arrow_binds_a_wrapper_without_crashing", r->get_instance() == nullptr);
        check("star_binds_the_same_way", (*r)->get_instance() == nullptr);

        // read() must NOT invoke the visitor when the ref does not resolve --
        // that is what stops a caller reading fields off a bound null.
        bool visited{ false };
        const bool ran{ r.read([&visited](ar_entity&) noexcept { visited = true; }) };
        check("read_refuses_to_bind_an_unresolvable_ref", !ran && !visited);
    }

    // =======================================================================
    // SECTION 7 -- CLASSIFICATION helpers degrade quietly with no VM and with an
    //   unregistered wrapper type.  Neither may crash, and neither may claim a
    //   match it cannot substantiate.
    // =======================================================================
    {
        const vmhook::ref<ar_entity> r{ vmhook::ref<ar_entity>::at_static(fake_klass(), 8u) };
        check("is_on_an_unregistered_wrapper_is_false", !r.is<ar_entity>());
        check("instance_of_by_name_is_false_without_a_jvm",
              !r.instance_of("java/lang/Object"));
        check("instance_of_empty_name_is_false", !r.instance_of(""));
        check("class_name_is_empty_without_a_jvm", r.class_name().empty());
        check("as_on_a_failed_instance_test_yields_empty", r.as<ar_entity>().empty());
        // erasure always succeeds and keeps the identity -- it drops the C++
        // type, not the anchor.
        check("erased_keeps_the_anchor_identity", r.erased().id() == r.id());

        // same_object_as is a POINT-IN-TIME test: two refs that both fail to
        // resolve are NOT the same object.  Reporting "true" for two nulls would
        // make every dead ref equal to every other dead ref.
        const vmhook::ref<ar_entity> other{ vmhook::ref<ar_entity>::at_static(fake_klass(), 8u) };
        check("same_object_as_is_false_when_neither_resolves", !r.same_object_as(other));

        // A field lookup that cannot resolve a holder class yields an EMPTY ref
        // (a value), never an exception and never a half-built anchor.
        check("field_by_name_without_a_holder_class_is_empty",
              r.field<ar_entity>("anything").empty());
    }

    // =======================================================================
    // SECTION 8 -- borrowed<T>: the allocation-free detour-scoped view.
    // =======================================================================
    {
        const vmhook::borrowed<ar_entity> nothing{};
        check("empty_borrow_resolves_null", nothing.resolve() == nullptr);
        check("empty_borrow_is_falsy", !static_cast<bool>(nothing));
        check("empty_borrow_has_not_expired", !nothing.expired());
        check("empty_borrow_has_no_identity", !static_cast<bool>(nothing.id()));

        const vmhook::borrowed<ar_entity> live{ address_b() };
        check("borrow_captures_the_address", live.raw_unsafe() == address_b());
        check("borrow_expires_when_the_epoch_is_unreadable", live.expired());
        check("borrow_refuses_to_hand_the_address_back", live.resolve() == nullptr);
        check("borrow_is_falsy_once_expired", !static_cast<bool>(live));
        check("borrow_arrow_binds_without_crashing", live->get_instance() == nullptr);

        // Two borrows of the same address in the same epoch share an identity;
        // borrows of different addresses do not.
        const vmhook::borrowed<ar_entity> same{ address_b() };
        const vmhook::borrowed<ar_entity> different{ address_a() };
        check("borrows_of_the_same_address_share_an_id", same.id() == live.id());
        check("borrows_of_different_addresses_differ", !(different == live));

        // pin() promotes to a ref with the SAME expiry -- it does not
        // manufacture durability that the mechanism cannot provide.
        const vmhook::ref<ar_entity> pinned{ live.pin() };
        check("pin_of_an_expired_borrow_is_empty", pinned.empty());
        check("pin_result_resolves_null", pinned.resolve() == nullptr);

        check("borrow_is_on_an_unregistered_wrapper_is_false", !live.is<ar_entity>());
        check("borrow_instance_of_is_false_without_a_jvm", !live.instance_of("java/lang/Object"));

        const vmhook::borrowed<ar_entity> from_free_fn{ vmhook::borrow<ar_entity>(address_b()) };
        check("borrow_free_function_agrees", from_free_fn.raw_unsafe() == address_b());
    }

    // =======================================================================
    // SECTION 9 -- root<T>: lazy binding.  The class need not be loaded when the
    //   root is constructed (that is what makes a namespace-scope root legal),
    //   and an unbindable root reports so instead of guessing.
    // =======================================================================
    {
        const vmhook::root<ar_entity> minecraft{ "net/minecraft/client/Minecraft", "theMinecraft" };
        check("root_records_its_declaring_class",
              minecraft.declaring_class() == "net/minecraft/client/Minecraft");
        check("root_records_its_field_name", minecraft.field_name() == "theMinecraft");
        check("root_does_not_bind_without_a_jvm", !minecraft.bound());
        check("root_get_yields_an_empty_ref", minecraft.get().empty());
        check("root_resolves_null", minecraft.resolve() == nullptr);
        check("root_is_falsy", !static_cast<bool>(minecraft));
        check("root_arrow_binds_without_crashing", minecraft->get_instance() == nullptr);

        // Re-binding is attempted on every call until it succeeds, so a class
        // loaded late is picked up -- it must not latch "failed" permanently.
        // Kept to a handful of rounds because each failed bind logs through
        // VMHOOK_LOG, and a debug build would otherwise drown the test output.
        bool stable{ true };
        for (int i{ 0 }; i < 8; ++i)
        {
            stable = stable && !minecraft.bound() && minecraft.resolve() == nullptr;
        }
        check("root_retries_binding_and_stays_quiet", stable);
    }
    {
        // The one-argument form defers to the wrapper type's REGISTERED class
        // name.  ar_entity was never registered, so it can never bind -- and
        // must say so rather than inventing a name.
        const vmhook::root<ar_entity> by_type{ "theInstance" };
        check("type_driven_root_has_no_explicit_class", by_type.declaring_class().empty());
        check("type_driven_root_does_not_bind_when_unregistered", !by_type.bound());
        check("type_driven_root_resolves_null", by_type.resolve() == nullptr);

        // A root with no field name at all is inert rather than ill-formed.
        const vmhook::root<ar_entity> unnamed{};
        check("default_root_is_inert", !unnamed.bound() && unnamed.resolve() == nullptr);
    }

    // =======================================================================
    // SECTION 10 -- anchor_kind_name() covers every enumerator (it ends up in
    //   user-facing logs, so an unnamed kind is a real defect).
    // =======================================================================
    {
        check("kind_name_empty", vmhook::anchor_kind_name(vmhook::anchor_kind::empty) == "empty");
        check("kind_name_static_root",
              vmhook::anchor_kind_name(vmhook::anchor_kind::static_root) == "static_root");
        check("kind_name_field_of",
              vmhook::anchor_kind_name(vmhook::anchor_kind::field_of) == "field_of");
        check("kind_name_element_of",
              vmhook::anchor_kind_name(vmhook::anchor_kind::element_of) == "element_of");
        check("kind_name_ephemeral",
              vmhook::anchor_kind_name(vmhook::anchor_kind::ephemeral) == "ephemeral");
    }

    return failures == 0 ? 0 : 1;
}
