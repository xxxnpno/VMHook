// No-JVM contract checks for the vmhook::object<T> / object_base CRTP wrapper
// pattern.  Wave-24 deepening targets the ledger gaps:
//
//   (1) MULTI-LEVEL inherited hierarchy: Base : object<Base> -> Mid : object<Mid>
//       -> Leaf : object<Leaf>.  Each level is its OWN CRTP root (the library's
//       pattern — see test_object_factory.cpp where every wrapper does
//       `class W : object<W>`).  The chain still flattens through object_base,
//       and instantiates cleanly with the explicit T(oop_t) ctor at every layer.
//   (2) Wrapper with a USER-DEFINED non-trivial destructor: object<T> where T's
//       dtor does extra work.  Must remain destructible and the unique_ptr the
//       library hands back must release through it via the virtual object_base
//       dtor.
//   (3) The object_base value-semantics contract: copyable (raw-pointer alias)
//       AND movable (move nulls the source) — pinned with static_assert.
//   (4) unique_ptr<wrapper> deleter identity: vmhook::make_unique<W> returns
//       std::unique_ptr<W> (NOT std::unique_ptr<W, custom-deleter>), so the
//       deleter is the default and the unique_ptr is the same TYPE the user can
//       std::move into their own std::unique_ptr<W>.
//   (5) Null-OOP construction path: every wrapper layer is nothrow-constructible
//       from nullptr and reads back nullptr through get_instance() (no crash, no
//       VM touch) — characterizes the safe "wrapped null" state the field_proxy
//       value_t -> unique_ptr path produces when the decoded oop is null.
//
// All checks are pure-logic and run on every CI compiler/STL.  No JVM, no VM
// state, no pointer is ever dereferenced (we hold raw void* sentinels that are
// stored-and-read but never decoded).
#include <vmhook/vmhook.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// Three-level inherited wrapper hierarchy.  Each layer is its own object<T>
// CRTP root (mirroring how the library's wrappers are declared one-per-class).
// All three flatten through vmhook::object_base, so a single object_base*
// upcast walks the whole chain.
// ---------------------------------------------------------------------------
class wp_base : public vmhook::object<wp_base>
{
public:
    explicit wp_base(vmhook::oop_t oop) noexcept
        : vmhook::object<wp_base>{ oop }
    {
    }
};

class wp_mid : public vmhook::object<wp_mid>
{
public:
    explicit wp_mid(vmhook::oop_t oop) noexcept
        : vmhook::object<wp_mid>{ oop }
    {
    }
};

class wp_leaf : public vmhook::object<wp_leaf>
{
public:
    explicit wp_leaf(vmhook::oop_t oop) noexcept
        : vmhook::object<wp_leaf>{ oop }
    {
    }
};

// A wrapper with a USER-DEFINED non-trivial destructor.  The dtor flips a
// static counter so we can prove (a) the wrapper IS destructible (compile-time
// trait + runtime count) and (b) when the unique_ptr handed back by the
// library's factory machinery is destroyed, the derived dtor runs (which it
// does because object_base has a virtual dtor — pinned with static_assert).
static int wp_nontrivial_dtor_count{ 0 };

class wp_nontrivial_dtor : public vmhook::object<wp_nontrivial_dtor>
{
public:
    explicit wp_nontrivial_dtor(vmhook::oop_t oop) noexcept
        : vmhook::object<wp_nontrivial_dtor>{ oop }
    {
    }

    ~wp_nontrivial_dtor() override
    {
        ++wp_nontrivial_dtor_count;
    }
};

// ---------------------------------------------------------------------------
// COMPILE-TIME contracts.
// ---------------------------------------------------------------------------

// (1) Multi-level: every layer derives from object_base AND its own object<T>.
//     sizeof equality across layers proves no layer adds extra state.  The
//     library only stores `oop_type_t instance` in object_base; every layer is
//     a thin CRTP veneer.
static_assert(std::is_base_of_v<vmhook::object_base, wp_base>);
static_assert(std::is_base_of_v<vmhook::object_base, wp_mid>);
static_assert(std::is_base_of_v<vmhook::object_base, wp_leaf>);
static_assert(std::is_base_of_v<vmhook::object<wp_base>, wp_base>);
static_assert(std::is_base_of_v<vmhook::object<wp_mid>, wp_mid>);
static_assert(std::is_base_of_v<vmhook::object<wp_leaf>, wp_leaf>);
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::object<wp_leaf>>);
static_assert(sizeof(wp_base) == sizeof(vmhook::object_base));
static_assert(sizeof(wp_mid)  == sizeof(vmhook::object_base));
static_assert(sizeof(wp_leaf) == sizeof(vmhook::object_base));

// (2) Non-trivial-dtor wrapper still derives from object_base and is destructible.
static_assert(std::is_base_of_v<vmhook::object_base, wp_nontrivial_dtor>);
static_assert(std::is_destructible_v<wp_nontrivial_dtor>);
static_assert(std::is_nothrow_destructible_v<wp_nontrivial_dtor>);
// object_base's virtual dtor reaches the derived dtor through a object_base*.
static_assert(std::has_virtual_destructor_v<vmhook::object_base>);
static_assert(std::has_virtual_destructor_v<wp_nontrivial_dtor>);
// The derived dtor is NOT trivial — user-defined — but the type still polymorphic.
static_assert(!std::is_trivially_destructible_v<wp_nontrivial_dtor>);
static_assert(std::is_polymorphic_v<wp_nontrivial_dtor>);

// (3) object_base value-semantics: copyable AND movable (the contract the
//     header documents at object_base:18075-18103).  Both ops noexcept on move.
static_assert(std::is_copy_constructible_v<vmhook::object_base>);
static_assert(std::is_copy_assignable_v<vmhook::object_base>);
static_assert(std::is_move_constructible_v<vmhook::object_base>);
static_assert(std::is_move_assignable_v<vmhook::object_base>);
static_assert(std::is_nothrow_move_constructible_v<vmhook::object_base>);
static_assert(std::is_nothrow_move_assignable_v<vmhook::object_base>);
// Every derived wrapper inherits the same semantics.
static_assert(std::is_copy_constructible_v<wp_leaf>);
static_assert(std::is_move_constructible_v<wp_leaf>);
static_assert(std::is_nothrow_move_constructible_v<wp_leaf>);

// (4) unique_ptr<wrapper> deleter identity: the library's factory returns a
//     PLAIN unique_ptr<W> — the deleter is std::default_delete<W>, not a custom
//     deleter type.  Pin both the unique_ptr type and the deleter equality.
static_assert(std::is_same_v<
    decltype(vmhook::make_unique<wp_leaf>()),
    std::unique_ptr<wp_leaf>>);
static_assert(std::is_same_v<
    std::unique_ptr<wp_leaf>::deleter_type,
    std::default_delete<wp_leaf>>);
static_assert(std::is_same_v<
    std::unique_ptr<wp_nontrivial_dtor>::deleter_type,
    std::default_delete<wp_nontrivial_dtor>>);
// The make_unique<W>() return type and a hand-written std::unique_ptr<W> are
// the SAME TYPE (so a user can `std::unique_ptr<W> u = make_unique<W>();`).
static_assert(std::is_assignable_v<
    std::unique_ptr<wp_leaf>&,
    decltype(vmhook::make_unique<wp_leaf>())>);

// (5) Null-OOP construction: every wrapper layer is nothrow-constructible from
//     nullptr_t / oop_t.  This is the type-system half of "wrapped null safe".
static_assert(std::is_nothrow_constructible_v<wp_base, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<wp_mid,  vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<wp_leaf, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<wp_nontrivial_dtor, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<wp_leaf, std::nullptr_t>);

int main()
{
    // =====================================================================
    // Multi-level hierarchy: instantiate every layer with the same sentinel
    // OOP, prove get_instance() round-trips, prove the object_base* upcast
    // works for every layer, and prove each layer's dynamic type is distinct.
    // =====================================================================
    void* const sentinel{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xC0FFEE00)) };
    {
        wp_base  b{ sentinel };
        wp_mid   m{ sentinel };
        wp_leaf  l{ sentinel };

        check("multi_level_base_round_trips_oop", b.vmhook::object_base::get_instance() == sentinel);
        check("multi_level_mid_round_trips_oop",  m.vmhook::object_base::get_instance() == sentinel);
        check("multi_level_leaf_round_trips_oop", l.vmhook::object_base::get_instance() == sentinel);

        // Upcast to object_base* and read back via the base accessor.
        vmhook::object_base* const pb{ &b };
        vmhook::object_base* const pm{ &m };
        vmhook::object_base* const pl{ &l };
        check("multi_level_base_upcast_ok", pb != nullptr && pb->get_instance() == sentinel);
        check("multi_level_mid_upcast_ok",  pm != nullptr && pm->get_instance() == sentinel);
        check("multi_level_leaf_upcast_ok", pl != nullptr && pl->get_instance() == sentinel);

        // Three distinct typeids — the layers are different C++ types, so the
        // type_to_class_map keying (which is by std::type_index) can distinguish
        // them even though they have identical layout.
        check("multi_level_base_mid_typeid_differ",  typeid(b) != typeid(m));
        check("multi_level_mid_leaf_typeid_differ",  typeid(m) != typeid(l));
        check("multi_level_base_leaf_typeid_differ", typeid(b) != typeid(l));
    }

    // =====================================================================
    // Non-trivial destructor: build through new wp_nontrivial_dtor{p}, upcast
    // to object_base*, delete, and prove the derived dtor ran (the bumped
    // static counter is the witness).  This is the exact pattern register_class
    // factories rely on at delete-time (see test_object_factory.cpp's
    // factory_builds<T>).
    // =====================================================================
    {
        const int before{ wp_nontrivial_dtor_count };
        vmhook::object_base* const w{ new wp_nontrivial_dtor{ sentinel } };
        check("nontrivial_dtor_construct_ok", w != nullptr && w->get_instance() == sentinel);
        delete w;
        check("nontrivial_dtor_ran_via_virtual_delete",
              wp_nontrivial_dtor_count == before + 1);
    }
    {
        // And through std::unique_ptr<W> — the default deleter calls
        // operator delete on the W* via the virtual dtor chain.
        const int before{ wp_nontrivial_dtor_count };
        {
            std::unique_ptr<wp_nontrivial_dtor> u{ new wp_nontrivial_dtor{ sentinel } };
            check("nontrivial_dtor_unique_ptr_holds_oop",
                  u && u->vmhook::object_base::get_instance() == sentinel);
        }
        check("nontrivial_dtor_ran_via_unique_ptr_scope_exit",
              wp_nontrivial_dtor_count == before + 1);
    }

    // =====================================================================
    // object_base move-only-transfer / copy-aliases contract (the documented
    // value semantics at vmhook.hpp:18075-18103).  Pinned at runtime to
    // complement the static_assert block above.
    // =====================================================================
    {
        wp_leaf a{ sentinel };
        wp_leaf copy{ a };
        check("copy_ctor_aliases_oop",
              a.vmhook::object_base::get_instance() == sentinel &&
              copy.vmhook::object_base::get_instance() == sentinel);

        wp_leaf assigned{ nullptr };
        assigned = a;
        check("copy_assign_aliases_oop",
              a.vmhook::object_base::get_instance() == sentinel &&
              assigned.vmhook::object_base::get_instance() == sentinel);
    }
    {
        wp_leaf src{ sentinel };
        wp_leaf moved{ std::move(src) };
        check("move_ctor_transfers_oop",
              moved.vmhook::object_base::get_instance() == sentinel);
        check("move_ctor_nulls_source",
              src.vmhook::object_base::get_instance() == nullptr);

        wp_leaf src2{ sentinel };
        wp_leaf dst{ nullptr };
        dst = std::move(src2);
        check("move_assign_transfers_oop",
              dst.vmhook::object_base::get_instance() == sentinel);
        check("move_assign_nulls_source",
              src2.vmhook::object_base::get_instance() == nullptr);
    }
    {
        // Self-move on assignment: the operator= guards `if (this != &other)`,
        // so a self-move is a no-op and leaves the OOP intact.  Pin it so the
        // guard doesn't silently regress.
        wp_leaf self{ sentinel };
        wp_leaf& self_ref{ self };
        self = std::move(self_ref);
        check("move_assign_self_is_noop",
              self.vmhook::object_base::get_instance() == sentinel);
    }

    // =====================================================================
    // unique_ptr deleter type identity at runtime: the type returned by the
    // library's make_unique<W> is std::unique_ptr<W> with the default deleter,
    // so a hand-built unique_ptr<W> can hold the same pointer and release it
    // safely.  We never call vmhook::make_unique without a JVM (it returns
    // null), but the TYPE check is the load-bearing one.
    // =====================================================================
    {
        std::unique_ptr<wp_leaf> u{ new wp_leaf{ sentinel } };
        check("unique_ptr_default_deleter_holds_oop",
              u && u->vmhook::object_base::get_instance() == sentinel);
        // Release into a raw pointer then re-adopt — proves the deleter is the
        // default and operator delete works on the W* with no custom deleter
        // state to carry.
        wp_leaf* const raw{ u.release() };
        check("unique_ptr_release_yields_same_oop",
              raw != nullptr && raw->vmhook::object_base::get_instance() == sentinel);
        std::unique_ptr<wp_leaf> readopted{ raw };
        check("unique_ptr_readopt_holds_oop",
              readopted && readopted->vmhook::object_base::get_instance() == sentinel);
    }

    // =====================================================================
    // Null-OOP construction path: every layer is constructible from nullptr,
    // does NOT crash, and reads back nullptr through the base accessor.  This
    // characterizes the safe "wrapped null" state — the same state that
    // field_proxy::value_t -> unique_ptr<T> would produce for a decoded null
    // OOP (which the value_t paths route through is_valid_pointer first; a
    // null result means the wrapper just holds nullptr).
    // =====================================================================
    {
        wp_base  b{ nullptr };
        wp_mid   m{ nullptr };
        wp_leaf  l{ nullptr };
        wp_nontrivial_dtor n{ nullptr };
        check("null_oop_base_get_instance_is_null",
              b.vmhook::object_base::get_instance() == nullptr);
        check("null_oop_mid_get_instance_is_null",
              m.vmhook::object_base::get_instance() == nullptr);
        check("null_oop_leaf_get_instance_is_null",
              l.vmhook::object_base::get_instance() == nullptr);
        check("null_oop_nontrivial_dtor_get_instance_is_null",
              n.vmhook::object_base::get_instance() == nullptr);
    }
    {
        // Default-constructed object_base directly (reaches object_base's
        // default-arg ctor; derived layers require an explicit oop arg).
        vmhook::object_base def{};
        check("default_ctor_object_base_is_null",
              def.get_instance() == nullptr);
    }
    {
        // Move-from a null-OOP wrapper: target stays null, source stays null.
        wp_leaf src{ nullptr };
        wp_leaf dst{ std::move(src) };
        check("move_from_null_wrapper_dst_null",
              dst.vmhook::object_base::get_instance() == nullptr);
        check("move_from_null_wrapper_src_null",
              src.vmhook::object_base::get_instance() == nullptr);
    }
    {
        // Polymorphic delete of a null-OOP wrapper through object_base*.  The
        // counter must still bump exactly once — the user-defined dtor runs
        // regardless of the wrapped-null state.
        const int before{ wp_nontrivial_dtor_count };
        vmhook::object_base* const w{ new wp_nontrivial_dtor{ nullptr } };
        check("null_oop_nontrivial_construct_ok",
              w != nullptr && w->get_instance() == nullptr);
        delete w;
        check("null_oop_nontrivial_dtor_still_runs",
              wp_nontrivial_dtor_count == before + 1);
    }

    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
