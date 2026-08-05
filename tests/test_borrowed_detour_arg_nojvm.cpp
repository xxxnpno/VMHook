// Standalone (no-JVM) contract test for vmhook::borrowed<T> AS A DETOUR
// ARGUMENT -- the detail::extract_frame_arg arm plus the descriptor and
// slot-width traits that have to agree with it.
//
// ===========================================================================
// WHY THIS FILE EXISTS
// ===========================================================================
// extract_frame_arg is the single choke point that turns a raw interpreter
// local slot into whatever type the user's detour declared.  Every detour
// argument in the library passes through it, so it is where the "the user
// never handles a raw oop" rule is either enforced or lost.  Adding
// borrowed<T> there means three separate compile-time tables must agree:
//
//   1. extract_frame_arg      -- produces the handle
//   2. jvm_descriptor_for_arg  -- describes it to the JVM as Lclass;
//   3. is_java_double_slot_v   -- says it occupies ONE local slot
//
// Disagreement between (1) and (3) is the nastiest failure mode in this area
// and has happened before: a wrong slot width does not fail to compile and
// does not crash -- every argument AFTER the offending one silently reads the
// wrong slot, and the detour sees plausible garbage.  So the slot table is
// pinned here at compile time, against argument lists that mix borrows with
// the two-slot primitives (long / double) that expose the drift.
//
// ===========================================================================
// WHAT THIS FILE CAN AND CANNOT PROVE
// ===========================================================================
// No jvm.dll / libjvm.so is loaded, so gHotSpotVMStructs never resolves,
// vmhook::gc_epoch() reports an INVALID sample, and every borrow taken here is
// born expired (the fail-closed arm).  That makes this the TYPE-LEVEL and
// GRACEFUL-DEGRADATION test:
//
//   * the trait truth table, including the const-ref and cv-qualified spellings
//     a detour signature can legally use;
//   * the descriptor produced for a registered wrapper, an UNregistered
//     wrapper, and the untyped borrowed<void>;
//   * the slot-offset table across borrow/long/double/borrow orderings;
//   * that extract_frame_arg on a null frame yields an EMPTY borrow rather
//     than faulting, for every borrow spelling.
//
// It CANNOT prove that a real local slot decodes to the right object, that the
// epoch stamp tracks a real collection, or that a live detour receives a live
// receiver.  Those need a live VM and are covered by the JVM modules.
// ===========================================================================
#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A wrapper that IS registered (see main), and one that deliberately is not.
class bda_registered final : public vmhook::object<bda_registered>
{
public:
    explicit bda_registered(const vmhook::oop_t oop = nullptr) noexcept
        : vmhook::object<bda_registered>{ oop }
    {
    }
};

class bda_unregistered final : public vmhook::object<bda_unregistered>
{
public:
    explicit bda_unregistered(const vmhook::oop_t oop = nullptr) noexcept
        : vmhook::object<bda_unregistered>{ oop }
    {
    }
};

// ===========================================================================
// COMPILE-TIME PINS
// ===========================================================================

namespace detail = vmhook::detail;

// -- 1. trait truth table ---------------------------------------------------
static_assert(detail::is_borrowed_v<vmhook::borrowed<bda_registered>>,
              "borrowed<W> is a borrow");
static_assert(detail::is_borrowed_v<vmhook::borrowed<>>,
              "the untyped borrowed<> is a borrow");
// A detour may take its argument by value or by const reference; both must
// classify identically or the two spellings would decode differently.
static_assert(detail::is_borrowed_v<const vmhook::borrowed<bda_registered>&>,
              "const borrowed<W>& is a borrow");
static_assert(detail::is_borrowed_v<vmhook::borrowed<bda_registered>&&>,
              "borrowed<W>&& is a borrow");
static_assert(detail::is_borrowed_v<const volatile vmhook::borrowed<bda_registered>>,
              "cv-qualified borrowed<W> is a borrow");

// Neighbouring handle types must NOT be swept up: ref and root are anchored
// and outlive the frame, so they are deliberately not detour-argument types.
static_assert(!detail::is_borrowed_v<vmhook::ref<bda_registered>>, "ref is not a borrow");
static_assert(!detail::is_borrowed_v<vmhook::root<bda_registered>>, "root is not a borrow");
static_assert(!detail::is_borrowed_v<std::unique_ptr<bda_registered>>,
              "unique_ptr is not a borrow");
static_assert(!detail::is_borrowed_v<bda_registered>, "a bare wrapper is not a borrow");
static_assert(!detail::is_borrowed_v<std::int32_t>, "int is not a borrow");
static_assert(!detail::is_borrowed_v<std::string>, "string is not a borrow");
static_assert(!detail::is_borrowed_v<void*>, "void* is not a borrow");

// The trait exposes the wrapper it was parameterised on.
static_assert(std::is_same_v<detail::is_borrowed<vmhook::borrowed<bda_registered>>::value_type_t,
                             bda_registered>,
              "value_type_t is the wrapper");
static_assert(std::is_same_v<detail::is_borrowed<vmhook::borrowed<>>::value_type_t, void>,
              "the untyped borrow's value_type_t is void");

// -- 2. slot width ----------------------------------------------------------
// A reference occupies ONE local slot, exactly like every other non-long,
// non-double argument.
static_assert(!detail::is_java_double_slot_v<vmhook::borrowed<bda_registered>>,
              "a borrow is a one-slot argument");
static_assert(!detail::is_java_double_slot_v<const vmhook::borrowed<>&>,
              "a const-ref borrow is still one slot");

// -- 3. slot-offset table ---------------------------------------------------
// The table these expand to is what extract_frame_arg is indexed with.  Each
// case below puts a two-slot primitive next to a borrow, which is where a
// wrong width shows up as every LATER argument reading the wrong slot.
template<typename tuple_type>
inline constexpr auto offsets_of{ detail::java_slot_offsets<tuple_type>::value };

// (this, int) -> receiver at slot 0, int at slot 1.
static_assert(offsets_of<std::tuple<vmhook::borrowed<bda_registered>, std::int32_t>>[0] == 0);
static_assert(offsets_of<std::tuple<vmhook::borrowed<bda_registered>, std::int32_t>>[1] == 1);

// (this, long, borrow) -> the long eats slots 1 AND 2, so the second reference
// starts at slot 3.  Reading it at 2 is the classic silent-garbage bug.
using mixed_long_t = std::tuple<vmhook::borrowed<bda_registered>,
                                std::int64_t,
                                vmhook::borrowed<>>;
static_assert(offsets_of<mixed_long_t>[0] == 0);
static_assert(offsets_of<mixed_long_t>[1] == 1);
static_assert(offsets_of<mixed_long_t>[2] == 3);

// (this, double, borrow, long, borrow) -> 0, 1, 3, 4, 6.
using mixed_wide_t = std::tuple<vmhook::borrowed<bda_registered>,
                                double,
                                vmhook::borrowed<bda_registered>,
                                std::int64_t,
                                vmhook::borrowed<>>;
static_assert(offsets_of<mixed_wide_t>[0] == 0);
static_assert(offsets_of<mixed_wide_t>[1] == 1);
static_assert(offsets_of<mixed_wide_t>[2] == 3);
static_assert(offsets_of<mixed_wide_t>[3] == 4);
static_assert(offsets_of<mixed_wide_t>[4] == 6);

// -- 4. extract_frame_arg's declared return type ----------------------------
// It must hand back the borrow BY VALUE, cv-ref stripped, for every spelling —
// a detour taking `const borrowed<W>&` still binds to a fresh temporary.
static_assert(std::is_same_v<
                  decltype(detail::extract_frame_arg<vmhook::borrowed<bda_registered>>(
                      static_cast<vmhook::hotspot::frame*>(nullptr), 0)),
                  vmhook::borrowed<bda_registered>>,
              "extract_frame_arg<borrowed<W>> returns borrowed<W> by value");
static_assert(std::is_same_v<
                  decltype(detail::extract_frame_arg<const vmhook::borrowed<>&>(
                      static_cast<vmhook::hotspot::frame*>(nullptr), 0)),
                  vmhook::borrowed<>>,
              "extract_frame_arg<const borrowed<>&> returns borrowed<> by value");

// A borrow is a two-word value: cheap enough to pass through the detour
// signature by value with no allocation, which is the whole point of the type.
static_assert(std::is_trivially_copyable_v<vmhook::borrowed<bda_registered>>,
              "a borrow is trivially copyable");
static_assert(std::is_nothrow_default_constructible_v<vmhook::borrowed<bda_registered>>,
              "a borrow is nothrow default-constructible (the null-slot result)");

int main()
{
    check("static_asserts_compiled", true);

    // =======================================================================
    // SECTION 1 -- descriptors.  This is what method resolution matches on, so
    //   a wrong string here means the hook silently fails to find its method.
    // =======================================================================
    {
        // register_class() verifies the name against a LIVE JVM and refuses
        // (returning false, leaving the map empty) when there is none — so it
        // cannot be used here.  Seed the type map directly instead: the thing
        // under test is the descriptor builder's lookup, not registration.
        vmhook::type_to_class_map.emplace(std::type_index{ typeid(bda_registered) },
                                          "vmhook/test/BdaRegistered");

        check("sig_registered_borrow_is_its_class",
              detail::jvm_descriptor_for_arg<vmhook::borrowed<bda_registered>>()
              == "Lvmhook/test/BdaRegistered;");

        // Taking the argument by const-ref must not change the descriptor.
        check("sig_const_ref_borrow_matches_by_value",
              detail::jvm_descriptor_for_arg<const vmhook::borrowed<bda_registered>&>()
              == detail::jvm_descriptor_for_arg<vmhook::borrowed<bda_registered>>());

        // borrowed<W> and unique_ptr<W> name the SAME Java type — they differ in
        // lifetime model, not in what they describe.
        check("sig_borrow_agrees_with_unique_ptr",
              detail::jvm_descriptor_for_arg<vmhook::borrowed<bda_registered>>()
              == detail::jvm_descriptor_for_arg<std::unique_ptr<bda_registered>>());

        // The untyped borrow carries no wrapper, so java/lang/Object is its
        // exact descriptor rather than a degraded guess.
        check("sig_untyped_borrow_is_object",
              detail::jvm_descriptor_for_arg<vmhook::borrowed<>>() == "Ljava/lang/Object;");

        // An unregistered wrapper degrades to java/lang/Object with a warning
        // rather than failing to compile or emitting a wrong class name.
        check("sig_unregistered_borrow_degrades_to_object",
              detail::jvm_descriptor_for_arg<vmhook::borrowed<bda_unregistered>>()
              == "Ljava/lang/Object;");
    }

    // =======================================================================
    // SECTION 2 -- extraction with no frame.  A null frame is what every cold
    //   / torn-down / not-yet-live path presents; it must yield an EMPTY borrow
    //   and never a fault, for every spelling and every slot index.
    // =======================================================================
    {
        auto* const no_frame{ static_cast<vmhook::hotspot::frame*>(nullptr) };

        const auto typed{ detail::extract_frame_arg<vmhook::borrowed<bda_registered>>(no_frame, 0) };
        check("extract_null_frame_typed_is_empty", !static_cast<bool>(typed));
        check("extract_null_frame_typed_raw_is_null", typed.raw_unsafe() == nullptr);
        // Empty is NOT expired: nothing was ever borrowed, so there is nothing
        // to have gone stale.  Conflating the two would make a Java null look
        // like a GC casualty.
        check("extract_null_frame_typed_not_expired", !typed.expired());

        const auto untyped{ detail::extract_frame_arg<vmhook::borrowed<>>(no_frame, 0) };
        check("extract_null_frame_untyped_is_empty", !static_cast<bool>(untyped));

        const auto by_ref{ detail::extract_frame_arg<const vmhook::borrowed<>&>(no_frame, 0) };
        check("extract_null_frame_const_ref_is_empty", !static_cast<bool>(by_ref));

        // The slot-index bounds guard (JVM max_locals is a u2) must reject a
        // negative or past-max index the same way, without touching memory.
        const auto negative{ detail::extract_frame_arg<vmhook::borrowed<bda_registered>>(no_frame, -1) };
        check("extract_negative_index_is_empty", !static_cast<bool>(negative));
        const auto past_max{ detail::extract_frame_arg<vmhook::borrowed<bda_registered>>(no_frame, 0x10000) };
        check("extract_past_max_locals_is_empty", !static_cast<bool>(past_max));

        // Two empty borrows compare equal, and an empty borrow has a null id —
        // so an unset detour argument cannot be mistaken for a real identity.
        check("extract_empty_borrows_compare_equal", typed == typed);
        check("extract_empty_borrow_id_is_empty", !static_cast<bool>(untyped.id()));
    }

    // =======================================================================
    // SECTION 3 -- a borrow reached through the extraction path behaves like
    //   any other borrow: no VM means resolve() is null, and every accessor
    //   degrades instead of faulting.
    // =======================================================================
    {
        auto* const no_frame{ static_cast<vmhook::hotspot::frame*>(nullptr) };
        const auto b{ detail::extract_frame_arg<vmhook::borrowed<bda_registered>>(no_frame, 3) };

        check("cold_borrow_resolve_is_null", b.resolve() == nullptr);
        check("cold_borrow_instance_of_is_false", !b.instance_of("java/lang/Object"));
        check("cold_borrow_is_wrapper_is_false", !b.is<bda_registered>());
        // pin() must still produce a ref rather than throwing; with no VM the
        // ref is born expired, which is the fail-closed state.
        check("cold_borrow_pin_is_empty", !static_cast<bool>(b.pin()));
        check("cold_borrow_field_is_empty", !static_cast<bool>(b.field("anything")));
        // operator-> binds an access proxy for one expression; on a null
        // resolve it must still bind, and the wrapper it hands back must carry
        // the null instance rather than a stale or fabricated address.
        check("cold_borrow_arrow_binds_null_instance", b->get_instance() == nullptr);
        // operator* hands back the same access proxy (not the wrapper), so the
        // wrapper is one more hop away — pinned here so the two spellings are
        // known to agree rather than assumed to.
        check("cold_borrow_star_binds_null_instance", (*b)->get_instance() == nullptr);
    }

    // =======================================================================
    // SECTION 4 -- the two OTHER intercepts that hand out a borrow, so the
    //   whole "the user never sees a raw address" entry set is covered by one
    //   file: object<W>::self() (the wrapper's own instance) and
    //   field_proxy::value_t::to_borrowed<W>() (a reference FIELD).
    // =======================================================================
    {
        // -- object<W>::self() ---------------------------------------------
        // A wrapper built on null yields an EMPTY borrow: never borrowed, so
        // not expired either.  Conflating the two would tell the caller an
        // object moved when in fact there never was one.
        const bda_registered empty_wrapper{ nullptr };
        const auto empty_self{ empty_wrapper.self() };
        check("self_of_null_wrapper_is_empty", !static_cast<bool>(empty_self));
        check("self_of_null_wrapper_not_expired", !empty_self.expired());
        check("self_of_null_wrapper_raw_is_null", empty_self.raw_unsafe() == nullptr);
        static_assert(std::is_same_v<decltype(empty_wrapper.self()),
                                     vmhook::borrowed<bda_registered>>,
                      "self() is typed on the wrapper, not erased to borrowed<void>");
        static_assert(noexcept(empty_wrapper.self()), "self() is noexcept");

        // A wrapper built on a REAL, mapped, aligned address this process owns:
        // the borrow must carry that exact address.  resolve() is null with no
        // VM (the epoch cannot be vouched for — the fail-closed arm), which is
        // precisely why raw_unsafe() exists as the diagnostics-only spelling.
        alignas(16) static std::uint8_t block[64]{};
        const bda_registered live_wrapper{ static_cast<vmhook::oop_t>(block) };
        const auto live_self{ live_wrapper.self() };
        check("self_of_live_wrapper_carries_the_address",
              live_self.raw_unsafe() == static_cast<vmhook::oop_t>(block));
        // Two self() calls on the same wrapper describe the same object.
        check("self_is_stable_across_calls",
              live_wrapper.self().raw_unsafe() == live_self.raw_unsafe());

        // -- field_proxy::value_t::to_borrowed<W>() ------------------------
        // A reference field is the uint32_t (compressed-OOP) alternative.  A
        // zero narrow oop is Java null -> empty borrow, not expired.
        const vmhook::field_proxy::value_t null_ref{ std::uint32_t{ 0u },
                                                     "Ljava/lang/Object;" };
        const auto from_null{ null_ref.to_borrowed<bda_registered>() };
        check("to_borrowed_null_reference_is_empty", !static_cast<bool>(from_null));
        check("to_borrowed_null_reference_not_expired", !from_null.expired());
        check("to_borrowed_null_reference_raw_is_null", from_null.raw_unsafe() == nullptr);

        // A NON-reference alternative must yield an empty borrow rather than a
        // borrow of reinterpreted primitive bits.  This is the arm that would
        // silently manufacture an address if the visit fell through.
        const vmhook::field_proxy::value_t an_int{ std::int32_t{ 0x0BADF00D }, "I" };
        const auto from_int{ an_int.to_borrowed<bda_registered>() };
        check("to_borrowed_of_int_field_is_empty", !static_cast<bool>(from_int));
        check("to_borrowed_of_int_field_raw_is_null", from_int.raw_unsafe() == nullptr);

        const vmhook::field_proxy::value_t a_long{ std::int64_t{ -1 }, "J" };
        check("to_borrowed_of_long_field_is_empty",
              a_long.to_borrowed<bda_registered>().raw_unsafe() == nullptr);
        const vmhook::field_proxy::value_t a_double{ 2.5, "D" };
        check("to_borrowed_of_double_field_is_empty",
              a_double.to_borrowed<bda_registered>().raw_unsafe() == nullptr);
        const vmhook::field_proxy::value_t a_bool{ true, "Z" };
        check("to_borrowed_of_bool_field_is_empty",
              a_bool.to_borrowed<bda_registered>().raw_unsafe() == nullptr);

        // The untyped spelling compiles and behaves identically — a caller who
        // only wants identity and lifetime need not name a wrapper.
        check("to_borrowed_untyped_of_null_is_empty",
              !static_cast<bool>(null_ref.to_borrowed<>()));
        static_assert(std::is_same_v<decltype(null_ref.to_borrowed<>()),
                                     vmhook::borrowed<>>,
                      "to_borrowed<> defaults to the untyped borrow");
        static_assert(noexcept(null_ref.to_borrowed<bda_registered>()),
                      "to_borrowed is noexcept");
    }

    return failures == 0 ? 0 : 1;
}
