// Standalone (no-JVM) contract test for the IDENTITY and CONTAINER half of the
// anchored-reference model: vmhook::object_id, std::hash support, and
// vmhook::ref_vector / vmhook::elements_of.
//
// ===========================================================================
// WHAT THIS FILE CAN AND CANNOT PROVE
// ===========================================================================
// object_id is derived purely from an anchor CHAIN (root klass + offsets +
// indices) or, for an ephemeral ref, from the captured address paired with the
// collection epoch.  None of that needs a live VM to be well-defined, so every
// identity property below is provable here:
//
//   * an id is stable for the life of a ref and survives copies and moves;
//   * equal anchors give equal ids; different anchors give different ids;
//   * ids from different anchor KINDS live in disjoint spaces;
//   * hash and operator== agree, which is the invariant an unordered container
//     requires, and refs really do work as unordered_map / map keys;
//   * ref_vector cannot be handed a raw address by any route the type system
//     admits -- the "collect raw oops, then pin them afterwards" shape is
//     UNEXPRESSIBLE (axiom A4), which is asserted with static_asserts because
//     that is the only way to assert the absence of an API;
//   * elements_of() anchors by INDEX during the walk and degrades to an empty
//     vector when the array cannot be resolved.
//
// It CANNOT prove that two refs to the same LIVE object agree, or that an
// element ref survives a real relocation; both need a VM.
//
// SAFETY: anchors here are built over a real, mapped, aligned buffer this
// process owns.  Nothing is ever dereferenced as a Java object -- every walk
// stops at the first unresolvable VMStruct.
// ===========================================================================
#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

class ari_row final : public vmhook::object<ari_row>
{
public:
    explicit ari_row(const vmhook::oop_t oop = nullptr) noexcept
        : vmhook::object<ari_row>{ oop }
    {
    }
};

namespace
{
    alignas(16) std::uint8_t g_arena[512]{};

    auto fake_klass() noexcept -> vmhook::hotspot::klass*
    {
        return reinterpret_cast<vmhook::hotspot::klass*>(static_cast<void*>(g_arena));
    }

    auto other_klass() noexcept -> vmhook::hotspot::klass*
    {
        return reinterpret_cast<vmhook::hotspot::klass*>(static_cast<void*>(g_arena + 128));
    }

    auto arena_address(const std::size_t offset) noexcept -> vmhook::oop_t
    {
        return static_cast<vmhook::oop_t>(g_arena + offset);
    }
}

// ===========================================================================
// COMPILE-TIME PINS
// ===========================================================================

// -- object_id is a trivially copyable value.  It is copied into map keys and
//    snapshots by the thousand, so it must cost nothing to move around and must
//    need no destructor.
static_assert(std::is_trivially_copyable_v<vmhook::object_id>,
              "object_id must be trivially copyable (it is a map key)");
static_assert(std::is_standard_layout_v<vmhook::object_id>,
              "object_id must be standard-layout");
static_assert(std::is_trivially_destructible_v<vmhook::object_id>,
              "object_id must be trivially destructible");
static_assert(sizeof(vmhook::object_id) == sizeof(std::uint64_t),
              "object_id must be exactly its token -- no hidden state");
static_assert(std::is_nothrow_default_constructible_v<vmhook::object_id>,
              "a default object_id is the 'no identity' value and must be free");
static_assert(vmhook::object_id{}.value() == 0u,
              "a default-constructed object_id must be the zero token");
static_assert(!static_cast<bool>(vmhook::object_id{}),
              "a zero token must be falsy");
static_assert(static_cast<bool>(vmhook::object_id{ 1u }),
              "a non-zero token must be truthy");
// Constexpr-usable: ids can be compared and ordered at compile time.
static_assert(vmhook::object_id{ 7u } == vmhook::object_id{ 7u },
              "equal tokens compare equal");
static_assert(vmhook::object_id{ 7u } != vmhook::object_id{ 8u },
              "different tokens compare unequal");
static_assert(vmhook::object_id{ 7u } < vmhook::object_id{ 8u },
              "object_id must be totally ordered so std::map/std::set work");
// A raw integer must not silently become an identity.
static_assert(!std::is_convertible_v<std::uint64_t, vmhook::object_id>,
              "the object_id(token) constructor must be explicit");

// -- HASHABILITY.  These are the pins that make the type usable as a key.
static_assert(std::is_default_constructible_v<std::hash<vmhook::object_id>>,
              "std::hash<object_id> must be specialised");
static_assert(std::is_invocable_r_v<std::size_t, std::hash<vmhook::object_id>, vmhook::object_id>,
              "std::hash<object_id> must yield a size_t");
static_assert(noexcept(std::hash<vmhook::object_id>{}(vmhook::object_id{})),
              "hashing an id must be noexcept");
static_assert(std::is_invocable_r_v<std::size_t,
                                    std::hash<vmhook::ref<ari_row>>,
                                    const vmhook::ref<ari_row>&>,
              "std::hash<ref<T>> must be specialised so a ref is a usable key");
static_assert(std::is_invocable_r_v<std::size_t,
                                    std::hash<vmhook::borrowed<ari_row>>,
                                    const vmhook::borrowed<ari_row>&>,
              "std::hash<borrowed<T>> must be specialised too");

// -- ref_vector: A4 asserted as the ABSENCE of an API.  There is no route from
//    a raw address (or a container of them) into a ref_vector, so the
//    "decode everything to raw oops, then pin them one at a time" shape -- the
//    one that roots STALE siblings into the GC root set -- cannot be written.
static_assert(!std::is_constructible_v<vmhook::ref_vector<ari_row>, vmhook::oop_t>,
              "ref_vector must NOT be constructible from a raw oop");
static_assert(!std::is_constructible_v<vmhook::ref_vector<ari_row>, void*>,
              "ref_vector must NOT be constructible from a raw void*");
static_assert(!std::is_constructible_v<vmhook::ref_vector<ari_row>, std::vector<void*>>,
              "ref_vector must NOT be constructible from a collected vector of oops -- "
              "that is exactly the pin-loop race A4 exists to make unexpressible");
static_assert(!std::is_constructible_v<vmhook::ref_vector<ari_row>,
                                       std::vector<vmhook::ref<ari_row>>>,
              "not even from a pre-built vector of refs -- append() or elements_of() only");
static_assert(!std::is_invocable_v<decltype(&vmhook::ref_vector<ari_row>::append),
                                   vmhook::ref_vector<ari_row>&, vmhook::oop_t>,
              "append() must not accept a raw oop");
static_assert(std::is_invocable_r_v<bool,
                                    decltype(&vmhook::ref_vector<ari_row>::append),
                                    vmhook::ref_vector<ari_row>&,
                                    const vmhook::ref<ari_row>&>,
              "append() takes an already-rooted ref and reports whether it took it");
static_assert(std::is_same_v<typename vmhook::ref_vector<ari_row>::value_type,
                             vmhook::ref<ari_row>>,
              "ref_vector elements are ordinary refs the caller can copy out");
static_assert(std::is_copy_constructible_v<vmhook::ref_vector<ari_row>>,
              "a ref_vector must be copyable (a snapshot is just data)");
static_assert(std::is_nothrow_move_constructible_v<vmhook::ref_vector<ari_row>>,
              "a ref_vector must move cheaply");

int main()
{
    // =======================================================================
    // SECTION 1 -- object_id basics.
    // =======================================================================
    {
        const vmhook::object_id none{};
        check("default_id_has_no_value", none.value() == 0u);
        check("default_id_is_falsy", !static_cast<bool>(none));
        check("default_id_renders", none.to_string() == "oid:0000000000000000");
        check("id_renders_hex_lowercase",
              vmhook::object_id{ 0xDEADBEEFCAFEB0BAull }.to_string() == "oid:deadbeefcafeb0ba");
        check("id_render_length_is_fixed", vmhook::object_id{ 1u }.to_string().size() == 20u);
    }

    // =======================================================================
    // SECTION 2 -- An anchored ref's id is PATH identity: derived from the root
    //   klass and every offset / index on the way down, and containing no heap
    //   address at all.  That is what makes it relocation-proof.
    // =======================================================================
    {
        const vmhook::ref<ari_row> a{ vmhook::ref<ari_row>::at_static(fake_klass(), 0x20u) };
        const vmhook::ref<ari_row> same{ vmhook::ref<ari_row>::at_static(fake_klass(), 0x20u) };
        const vmhook::ref<ari_row> other_offset{
            vmhook::ref<ari_row>::at_static(fake_klass(), 0x28u) };
        const vmhook::ref<ari_row> other_owner{
            vmhook::ref<ari_row>::at_static(other_klass(), 0x20u) };

        check("anchored_ref_has_an_identity", static_cast<bool>(a.id()));
        check("identical_anchors_share_an_id", a.id() == same.id());
        check("identical_anchors_compare_equal", a == same);
        check("a_different_offset_is_a_different_id", a.id() != other_offset.id());
        check("a_different_root_klass_is_a_different_id", a.id() != other_owner.id());
        check("different_anchors_compare_unequal", a != other_offset);

        // Stability: the id must not drift between calls (it is precomputed once
        // when the anchor is built, so nothing can make it wander).
        bool stable{ true };
        const vmhook::object_id first{ a.id() };
        for (int i{ 0 }; i < 5000; ++i)
        {
            stable = stable && a.id() == first;
        }
        check("an_id_is_stable_across_repeated_reads", stable);
    }

    // Chain components each contribute: the SAME hops in a different ORDER, and
    // the same shape with a different index, must not collide.
    {
        const vmhook::ref<void> base{ vmhook::ref<void>::at_static(fake_klass(), 0u) };
        const vmhook::ref<void> field_then_element{ base.field_at<void>(0x10u).element<void>(2) };
        const vmhook::ref<void> element_then_field{ base.element<void>(2).field_at<void>(0x10u) };
        const vmhook::ref<void> other_index{ base.field_at<void>(0x10u).element<void>(3) };

        check("hop_order_changes_the_identity",
              field_then_element.id() != element_then_field.id());
        check("element_index_participates_in_the_identity",
              field_then_element.id() != other_index.id());
        check("a_child_id_differs_from_its_parent",
              field_then_element.id() != base.id());
        // Rebuilding the same chain reproduces the same id -- identity is a pure
        // function of the path, which is why it survives a collection.
        const vmhook::ref<void> rebuilt{ base.field_at<void>(0x10u).element<void>(2) };
        check("rebuilding_the_same_chain_reproduces_the_id",
              rebuilt.id() == field_then_element.id());
    }

    // Kinds live in disjoint spaces: an ephemeral capture and a static root must
    // never collide just because their raw numbers happen to line up.
    {
        const vmhook::ref<void> anchored{ vmhook::ref<void>::at_static(fake_klass(), 0u) };
        const vmhook::ref<void> transient{ vmhook::ref<void>::ephemeral(arena_address(0)) };
        check("ephemeral_and_static_ids_are_disjoint", anchored.id() != transient.id());

        // Two ephemeral captures of the same address in the same epoch DO share
        // an id -- within one epoch, address equality is object identity.
        const vmhook::ref<void> transient_again{ vmhook::ref<void>::ephemeral(arena_address(0)) };
        const vmhook::ref<void> elsewhere{ vmhook::ref<void>::ephemeral(arena_address(64)) };
        check("same_address_same_epoch_shares_an_ephemeral_id",
              transient.id() == transient_again.id());
        check("different_addresses_give_different_ephemeral_ids",
              transient.id() != elsewhere.id());

        // A borrowed<T> uses the same derivation as an ephemeral ref, so the two
        // agree -- otherwise a detour argument and its pin() would key
        // differently in the same map.
        const vmhook::borrowed<ari_row> borrow{ arena_address(0) };
        check("borrowed_and_ephemeral_ref_ids_agree_for_one_address",
              borrow.id() == transient.id());
    }

    // Copies and moves carry the identity; a moved-from ref loses it.
    {
        const vmhook::ref<ari_row> original{ vmhook::ref<ari_row>::at_static(fake_klass(), 0x60u) };
        const vmhook::ref<ari_row> copy{ original };
        check("a_copy_keeps_the_identity", copy.id() == original.id());

        vmhook::ref<ari_row> source{ vmhook::ref<ari_row>::at_static(fake_klass(), 0x68u) };
        const vmhook::object_id token{ source.id() };
        const vmhook::ref<ari_row> moved{ std::move(source) };
        check("a_move_carries_the_identity", moved.id() == token);
        check("a_moved_from_ref_loses_its_identity",
              !static_cast<bool>(source.id()));      // NOLINT(bugprone-use-after-move)
    }

    // =======================================================================
    // SECTION 3 -- hash / equality consistency, and real container use.  This is
    //   the property that replaces the raw-address map key: the token is
    //   relocation-proof, so a key written before a collection still finds its
    //   entry after one.
    // =======================================================================
    {
        const vmhook::ref<ari_row> a{ vmhook::ref<ari_row>::at_static(fake_klass(), 0x70u) };
        const vmhook::ref<ari_row> a_copy{ a };
        const vmhook::ref<ari_row> b{ vmhook::ref<ari_row>::at_static(fake_klass(), 0x78u) };

        const std::hash<vmhook::ref<ari_row>> ref_hash{};
        const std::hash<vmhook::object_id>    id_hash{};

        check("equal_refs_hash_equal", ref_hash(a) == ref_hash(a_copy));
        check("ref_hash_matches_its_id_hash", ref_hash(a) == id_hash(a.id()));
        check("distinct_anchors_hash_differently", ref_hash(a) != ref_hash(b));

        const std::hash<vmhook::borrowed<ari_row>> borrow_hash{};
        const vmhook::borrowed<ari_row> borrow{ arena_address(16) };
        check("borrow_hash_matches_its_id_hash", borrow_hash(borrow) == id_hash(borrow.id()));
    }
    {
        // The Pattern-10 shape: a snapshot keyed by identity rather than by a
        // raw address, published for another thread to read.
        std::unordered_map<vmhook::object_id, std::string> lines;
        std::vector<vmhook::ref<ari_row>> rows;
        for (std::size_t i{ 0 }; i < 32u; ++i)
        {
            vmhook::ref<ari_row> row{ vmhook::ref<ari_row>::at_static(fake_klass(), i * 8u) };
            lines.emplace(row.id(), "row-" + std::to_string(i));
            rows.push_back(std::move(row));
        }
        check("identity_keys_do_not_collide_over_32_anchors", lines.size() == 32u);

        bool every_row_found{ true };
        for (std::size_t i{ 0 }; i < rows.size(); ++i)
        {
            const auto entry{ lines.find(rows[i].id()) };
            every_row_found = every_row_found
                           && entry != lines.end()
                           && entry->second == "row-" + std::to_string(i);
        }
        check("every_row_is_found_again_by_its_identity", every_row_found);

        // A ref rebuilt from scratch (not a copy) finds the same entry -- which
        // is what a re-walked collection produces on the next tick.
        const vmhook::ref<ari_row> rebuilt{ vmhook::ref<ari_row>::at_static(fake_klass(), 5u * 8u) };
        const auto found{ lines.find(rebuilt.id()) };
        check("a_rebuilt_anchor_finds_the_same_entry",
              found != lines.end() && found->second == "row-5");

        // An id that was never inserted is simply absent.
        const vmhook::ref<ari_row> stranger{ vmhook::ref<ari_row>::at_static(other_klass(), 0u) };
        check("an_unknown_identity_is_absent", lines.find(stranger.id()) == lines.end());
    }
    {
        // refs themselves as unordered_set keys, and ids in an ORDERED map --
        // the second only works because object_id has a total order.
        std::unordered_set<vmhook::ref<ari_row>> unique_refs;
        for (int round{ 0 }; round < 3; ++round)
        {
            for (std::size_t i{ 0 }; i < 8u; ++i)
            {
                unique_refs.insert(vmhook::ref<ari_row>::at_static(fake_klass(), i * 16u));
            }
        }
        check("a_ref_deduplicates_by_identity_in_an_unordered_set", unique_refs.size() == 8u);

        std::map<vmhook::object_id, int> ordered;
        std::set<vmhook::object_id>      ordered_set;
        for (std::size_t i{ 0 }; i < 8u; ++i)
        {
            const vmhook::ref<ari_row> r{ vmhook::ref<ari_row>::at_static(fake_klass(), i * 16u) };
            ordered.emplace(r.id(), static_cast<int>(i));
            ordered_set.insert(r.id());
        }
        check("object_id_works_as_an_ordered_map_key", ordered.size() == 8u);
        check("object_id_works_in_an_ordered_set", ordered_set.size() == 8u);
    }

    // =======================================================================
    // SECTION 4 -- ref_vector: built anchored, or not built at all.
    // =======================================================================
    {
        vmhook::ref_vector<ari_row> rows{};
        check("a_fresh_ref_vector_is_empty", rows.empty() && rows.size() == 0u);

        // append() refuses an EMPTY ref, so a walk that failed halfway cannot
        // smuggle a hole into the container.
        check("append_refuses_an_empty_ref", !rows.append(vmhook::ref<ari_row>{}));
        check("a_refused_append_leaves_the_vector_empty", rows.empty());

        bool all_taken{ true };
        for (std::size_t i{ 0 }; i < 16u; ++i)
        {
            all_taken = all_taken
                     && rows.append(vmhook::ref<ari_row>::at_static(fake_klass(), i * 8u));
        }
        check("append_takes_every_anchored_ref", all_taken);
        check("ref_vector_size_tracks_the_appends", rows.size() == 16u);
        check("ref_vector_is_not_empty_after_appends", !rows.empty());

        // Range-for is the documented consumption shape, and the elements are
        // ordinary refs the caller can copy out into a cache.
        std::size_t seen{ 0 };
        bool every_element_is_anchored{ true };
        for (const vmhook::ref<ari_row>& row : rows)
        {
            every_element_is_anchored = every_element_is_anchored
                                     && !row.empty()
                                     && row.kind() == vmhook::anchor_kind::static_root
                                     && row.resolve() == nullptr;
            ++seen;
        }
        check("range_for_visits_every_element", seen == 16u);
        check("every_element_is_anchored_not_a_raw_address", every_element_is_anchored);

        check("indexing_agrees_with_iteration", rows[3].id() == (*(rows.begin() + 3)).id());
        check("at_returns_the_same_element_in_range", rows.at(3).id() == rows[3].id());
        // Out of range is a value, not a throw -- the library never throws out.
        check("at_out_of_range_yields_an_empty_ref", rows.at(999).empty());

        const vmhook::ref_vector<ari_row> copied{ rows };
        check("a_ref_vector_copies", copied.size() == rows.size());
        check("a_copied_element_keeps_its_identity", copied[0].id() == rows[0].id());

        const std::vector<vmhook::ref<ari_row>> plain{ rows.to_vector() };
        check("to_vector_hands_back_the_anchored_elements",
              plain.size() == 16u && plain[7].id() == rows[7].id());

        rows.clear();
        check("clear_empties_the_ref_vector", rows.empty());
    }

    // =======================================================================
    // SECTION 5 -- elements_of(): the anchored-during-the-walk constructor.
    //   With no VM the array never resolves, so the result is an empty vector --
    //   never a vector of dead addresses.
    // =======================================================================
    {
        const vmhook::ref<void> array_ref{ vmhook::ref<void>::at_static(fake_klass(), 0x80u) };
        const vmhook::ref_vector<ari_row> walked{ vmhook::elements_of<ari_row>(array_ref) };
        check("elements_of_yields_an_empty_vector_when_the_array_does_not_resolve",
              walked.empty());

        const vmhook::ref_vector<ari_row> from_empty{
            vmhook::elements_of<ari_row>(vmhook::ref<void>{}) };
        check("elements_of_on_an_empty_ref_is_empty", from_empty.empty());

        // The max_elements clamp is part of the signature, not an afterthought.
        const vmhook::ref_vector<ari_row> clamped{ vmhook::elements_of<ari_row>(array_ref, 4u) };
        check("elements_of_accepts_an_explicit_clamp", clamped.empty());
    }

    // Element anchoring is by INDEX: two element refs off the same parent differ
    // by index alone, and neither carries an address.  (On a live VM this is
    // what lets an element survive relocation of both array and element.)
    {
        const vmhook::ref<void> array_ref{ vmhook::ref<void>::at_static(fake_klass(), 0x88u) };
        vmhook::ref_vector<ari_row> manual{};
        for (std::int32_t i{ 0 }; i < 8; ++i)
        {
            manual.append(array_ref.element<ari_row>(i));
        }
        check("index_anchored_elements_are_all_distinct",
              manual.size() == 8u && manual[0].id() != manual[1].id());
        check("index_anchored_elements_are_element_of",
              manual[0].kind() == vmhook::anchor_kind::element_of);
        check("index_anchored_elements_hang_off_the_array",
              manual[0].depth() == array_ref.depth() + 1u);
        check("index_anchored_elements_resolve_null_without_a_jvm",
              manual[5].resolve() == nullptr);
    }

    return failures == 0 ? 0 : 1;
}
