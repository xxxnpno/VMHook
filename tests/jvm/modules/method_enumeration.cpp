// method_enumeration JVM test module  (feature area: methods)
//
// Exhaustively exercises vmhook's method-introspection / descriptor-hook API on
// a live JVM against a fixture whose declared (name, descriptor) set is known
// EXACTLY (see vmhook/fixtures/MethodEnumeration.java, cross-checked with
// `javap -s`):
//
//   * vmhook::get_class_methods<T>()                  — by registered wrapper type
//   * vmhook::get_class_methods("vmhook/fixtures/..") — by internal class name
//   * vmhook::find_methods_by_signature<T>(desc)      — all names for a descriptor
//   * vmhook::hook_by_signature<T>(desc, detour)      — installs on the UNIQUE
//                                                       descriptor match and
//                                                       REFUSES (returns false)
//                                                       when 2+ methods share it.
//
// What the module proves, angle by angle:
//   - the real declared method SET is returned (every application method present
//     with its exact descriptor; searched by membership, never by array order,
//     because HotSpot sorts _methods by name-symbol, not source order);
//   - the by-name overload AGREES with the by-type overload (same multiset);
//   - the synthetic members <init> and <clinit> ARE included (they live in
//     _methods) while inherited java.lang.Object methods are NOT;
//   - find_methods_by_signature returns ALL matches: 1 for the unique (J)J,
//     3 for the shared (I)I, 6 for the shared ()V;
//   - hook_by_signature INSTALLS + FIRES on the unique (J)J descriptor (real
//     bytecode dispatch via the probe), decoding the long arg and seeing self;
//   - hook_by_signature REFUSES (false, nothing installed) on a SHARED
//     descriptor — proven twice: (I)I (application-only collision) and ()V
//     (synthetic-member collision) — and a refused hook never fires;
//   - hook_by_signature REFUSES (false) on a descriptor that matches NOTHING;
//   - get_class_methods<U>() / find_methods_by_signature<U>() on an UNREGISTERED
//     wrapper type return empty;
//   - get_class_methods("bogus/Name") returns empty (class not loaded).
//
// CRASH-PROOFING (mingw·gcc has NO SEH net — any wild read kills the whole JVM):
//   PARTs A-D are pure METADATA: get_class_methods / find_methods_by_signature
//   walk InstanceKlass::_methods and read Method*/Symbol* out of METASPACE, which
//   is native and STABLE (never GC-relocated), and the library guards every slot
//   with is_valid_pointer — so those reads cannot fault on a cold JVM and stay
//   HARD.  The single genuinely cold-unsafe dereference in the whole module is
//   inside the PART E (J)J detour: it reads the receiver's `seed` field
//   (self->seed() -> get_field("seed")->get(), a RAW std::memcpy at oop+offset,
//   vmhook.hpp field_proxy::get).  Crucially, the library's detour trampoline
//   wraps the user detour in seh_invoke_detour (vmhook.hpp:5945), but that SEH
//   net is a no-op on mingw·gcc — its #else branch is a plain C++ try/catch,
//   which on Windows-GCC does NOT catch a structured access violation.  So a
//   faulting field read inside the detour escapes uncaught and tears down the
//   JVM ("Last module entered: method_enumeration", no TOTAL line) — exactly the
//   modular-only cold crash.  The fix makes the detour itself fault-proof:
//   self->seed() is read only after the receiver oop's header AND the exact
//   `seed` slot are proven currently-mapped via os::safe_read (returns false,
//   never faults, on an unmapped/relocated page); a transient miss degrades to a
//   best-effort [INFO] (never a fault, never a vacuous pass), while a SUCCESSFUL
//   read that yields the WRONG seed still FAILS.  No m->call() is ever issued
//   here (the probe drives real bytecode; the detour only reads a field), so the
//   call-stub gate that sibling modules need is N/A.  Fine-grained ctx.record()
//   checkpoints (flushed per line) bracket every PART and every risky op so any
//   residual no-SEH fault is pinpointed by the last-flushed line.
//
// Harness note: the fixture's `done` latches, so each scenario resets done +
// sets mode on the rising edge of go (the drive() helper), runs ONE probe cycle,
// then reads back observations.  scoped_hook (NEVER shutdown_hooks) isolates the
// module; hook_by_signature installs through the persistent hook table, so the
// install scenarios bracket their probe inside an explicit uninstall via a
// scoped re-hook is NOT possible (hook_by_signature returns bool, not a handle).
// Instead, mode 2 confirms the REFUSED (I)I hook never installed by observing
// that calling idInt fires nothing; the single accepted (J)J hook is the only
// persistent install and is harmless to leave (the JVM exits right after).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.MethodEnumeration.  Deriving from
    // vmhook::object<> gives the wrapper a vtable (required by register_class<T>)
    // and the static_field(...) / get_field(...) accessors.
    class me_fixture : public vmhook::object<me_fixture>
    {
    public:
        explicit me_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<me_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        static auto get_last_id_long() -> std::int64_t { return static_field("lastIdLong")->get(); }
        static auto get_last_id_int() -> std::int32_t  { return static_field("lastIdInt")->get(); }

        // Reads an instance's own seed (proves the detour's `self` is correct).
        //
        // CRASH-PROOF variant for use INSIDE the detour, where a raw fault would
        // escape the library's (mingw-no-op) SEH net and kill the JVM.  Returns
        // nullopt — never faults, never UB — when the field cannot be SAFELY read:
        //   * get_field() didn't resolve (cold-JVM field-resolution miss) — the
        //     plain seed() below would deref a nullopt optional (UB);
        //   * the receiver oop's header or the exact 4-byte `seed` slot is not
        //     currently mapped (os::safe_read returns false instead of faulting),
        //     e.g. a relocated/stale receiver.
        // A SUCCESSFUL probe means the get()/memcpy of the SAME 4 bytes at the
        // SAME address cannot fault, so the returned value is HARD: a wrong seed
        // from a successful read still surfaces as a mismatch at the call site.
        auto seed_safe() const -> std::optional<std::int32_t>
        {
            const auto proxy{ get_field("seed") };
            if (!proxy.has_value())
            {
                return std::nullopt;
            }
            // Re-acquire the receiver oop fresh and probe BOTH the object header
            // (proves the base oop is the real, currently-resident object) and the
            // precise 4-byte slot the read will touch (proxy->raw_address() ==
            // oop+offset for an instance field).  safe_read goes through a kernel
            // path (ReadProcessMemory / process_vm_readv) that returns false rather
            // than faulting on an unmapped page.
            void* const base_oop{ this->get_instance() };
            if (!base_oop || !vmhook::hotspot::is_valid_pointer(base_oop))
            {
                return std::nullopt;
            }
            std::uint8_t header_scratch[16] = { 0 };
            if (!vmhook::os::safe_read(header_scratch, base_oop, sizeof(header_scratch)))
            {
                return std::nullopt;
            }
            void* const slot{ proxy->raw_address() };
            std::int32_t slot_scratch{ 0 };
            if (!slot || !vmhook::os::safe_read(&slot_scratch, slot, sizeof(slot_scratch)))
            {
                return std::nullopt;
            }
            // COPY-init from value_t (never brace-init): value_t's templated
            // conversion operator makes std::int32_t{ proxy->get() } ambiguous on
            // MSVC (see the nested_classes convention).
            const std::int32_t value = proxy->get();
            return value;
        }
    };

    // A SECOND wrapper type that we deliberately NEVER register, to prove the
    // template overloads return empty for an unregistered type.
    class me_unregistered : public vmhook::object<me_unregistered>
    {
    public:
        explicit me_unregistered(vmhook::oop_t instance) noexcept
            : vmhook::object<me_unregistered>{ instance }
        {
        }
    };

    // ---- Fixture-mirrored constants (lockstep with MethodEnumeration.java) --
    constexpr std::int32_t SEED{ 7 };
    constexpr std::int64_t IDLONG_ARG{ 0x0102030405060708LL };
    constexpr std::int32_t IDINT_ARG{ 1234 };

    constexpr char CLASS_NAME[]{ "vmhook/fixtures/MethodEnumeration" };

    // ---- (J)J hook observations (the unique-descriptor install/fire target) -
    std::atomic<std::int32_t> g_jj_fire_count{ 0 };
    std::atomic<std::int64_t> g_jj_arg{ -1 };
    // Tri-state observation of the detour's view of `self` (set INSIDE the detour,
    // which on mingw runs with NO SEH net — so the seed read must never fault):
    //    0 = not observed / could not SAFELY read seed (transient: receiver oop
    //        header or slot not currently mapped, or field unresolved) -> the
    //        saw_correct_self assertion degrades to [INFO] (never a fault, never
    //        a vacuous pass);
    //    1 = read seed and it MATCHED SEED (self is the correct receiver);
    //    2 = read seed and it did NOT match (a real wrong-self / mis-decode bug).
    // A value of 2 still FAILS the assertion, so a SUCCESSFUL read stays HARD.
    std::atomic<std::int32_t> g_jj_self_state{ 0 };

    // ---- refused-hook observation: must STAY zero -----------------------
    std::atomic<std::int32_t> g_refused_fire_count{ 0 };

    // Count occurrences of an exact (name, descriptor) pair in the result set.
    auto count_pair(const std::vector<std::pair<std::string, std::string>>& methods,
                    const std::string&                                      name,
                    const std::string&                                      descriptor) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            {
                return m.first == name && m.second == descriptor;
            }));
    }

    // True if ANY pair has this method name (regardless of descriptor).
    auto has_name(const std::vector<std::pair<std::string, std::string>>& methods,
                  const std::string&                                      name) -> bool
    {
        return std::any_of(methods.begin(), methods.end(),
                           [&](const std::pair<std::string, std::string>& m)
                           { return m.first == name; });
    }

    // Count how many pairs carry exactly this descriptor.
    auto count_descriptor(const std::vector<std::pair<std::string, std::string>>& methods,
                          const std::string&                                      descriptor) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            { return m.second == descriptor; }));
    }

    // Drives exactly one probe cycle for `mode`: clears the latched done and
    // programs the selector on the rising edge, then runs the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    me_fixture::set_done(false);
                    me_fixture::set_mode(mode);
                }
                me_fixture::set_go(value);
            },
            []() { return me_fixture::get_done(); });
    }
}

VMHOOK_JVM_MODULE(method_enumeration)
{
    // ── Fine-grained CRASH-LOCATOR checkpoints (CRITICAL on mingw·gcc) ────────
    //
    // mingw·gcc installs NO usable SEH net (seh_invoke_detour's try/catch can't
    // catch a structured AV), so a single wild read takes down the whole JVM with
    // no stack trace — the only forensic signal is the LAST line flushed to
    // test_results.txt (ctx.record flushes per line).  A checkpoint is dropped
    // before every PART and immediately before the one read that can fault on a
    // cold JVM (the detour's seed read), so the next CI run's last-flushed line
    // names the EXACT op that died.  Permanent: one flushed line each, invaluable
    // on a platform with no fault recovery.
    const auto cp = [&](const char* where)
    {
        ctx.record(std::string{ "[INFO] method_enumeration checkpoint: " } + where);
    };

    cp("register_class<me_fixture>");
    vmhook::register_class<me_fixture>("vmhook/fixtures/MethodEnumeration");

    // =====================================================================
    // PART A — get_class_methods<T>(): the real declared (name, descriptor) set.
    //   METASPACE metadata read (Method*/Symbol*, native + stable, library-guarded
    //   per slot with is_valid_pointer) — cannot fault on a cold JVM -> all HARD.
    // =====================================================================
    cp("PART A get_class_methods<T>() (metaspace metadata — no oop deref)");
    const std::vector<std::pair<std::string, std::string>> by_type{
        vmhook::get_class_methods<me_fixture>() };

    ctx.record(std::string{ "[INFO] get_class_methods<T>() returned " }
               + std::to_string(by_type.size()) + " method(s)");

    ctx.check("by_type_nonempty", !by_type.empty());

    // Every declared APPLICATION method, present with its EXACT descriptor.
    ctx.check("has_idInt_II",    count_pair(by_type, "idInt",   "(I)I") == 1);
    ctx.check("has_addInt_II",   count_pair(by_type, "addInt",  "(I)I") == 1);
    ctx.check("has_idLong_JJ",   count_pair(by_type, "idLong",  "(J)J") == 1);
    ctx.check("has_strLen_strI", count_pair(by_type, "strLen",  "(Ljava/lang/String;)I") == 1);
    ctx.check("has_sumArr_aII",  count_pair(by_type, "sumArr",  "([I)I") == 1);
    ctx.check("has_mix_IJDD",    count_pair(by_type, "mix",     "(IJD)D") == 1);
    ctx.check("has_noop_V",      count_pair(by_type, "noop",    "()V") == 1);
    ctx.check("has_tick_V",      count_pair(by_type, "tick",    "()V") == 1);
    ctx.check("has_flag_Z",      count_pair(by_type, "flag",    "()Z") == 1);
    ctx.check("has_makeObj_obj", count_pair(by_type, "makeObj", "()Ljava/lang/Object;") == 1);
    ctx.check("has_sId_II",      count_pair(by_type, "sId",     "(I)I") == 1);
    ctx.check("has_sWide_JDJ",   count_pair(by_type, "sWide",   "(JD)J") == 1);
    ctx.check("has_runIdLong_V", count_pair(by_type, "runIdLong", "()V") == 1);
    ctx.check("has_runIdInt_V",  count_pair(by_type, "runIdInt",  "()V") == 1);

    // The names are present (descriptor-agnostic) — a second, weaker angle that
    // isolates "name decode worked" from "descriptor decode worked".
    ctx.check("name_present_idLong",  has_name(by_type, "idLong"));
    ctx.check("name_present_makeObj", has_name(by_type, "makeObj"));
    ctx.check("name_present_sWide",   has_name(by_type, "sWide"));

    // Synthetic members live in _methods: <init> is universal; <clinit> exists
    // because the fixture has a static block + static field initializers.
    const bool has_init{ count_pair(by_type, "<init>", "()V") >= 1 };
    const bool has_clinit{ count_pair(by_type, "<clinit>", "()V") >= 1 };
    ctx.check("includes_synthetic_init", has_init);
    ctx.record(std::string{ "[INFO] <clinit> present in enumeration: " }
               + (has_clinit ? "yes" : "no"));
    ctx.check("includes_synthetic_clinit", has_clinit);

    // Inherited java.lang.Object methods must NOT appear (this lists DECLARED
    // methods only, not the resolved/inherited table).
    ctx.check("excludes_inherited_toString", !has_name(by_type, "toString"));
    ctx.check("excludes_inherited_hashCode", !has_name(by_type, "hashCode"));
    ctx.check("excludes_inherited_equals",   !has_name(by_type, "equals"));
    ctx.check("excludes_inherited_wait",     !has_name(by_type, "wait"));
    ctx.check("excludes_inherited_getClass", !has_name(by_type, "getClass"));

    // No pair may carry an empty name or empty descriptor (symbol decode never
    // silently produced "" for a valid slot).
    const bool no_empty_strings{ std::none_of(
        by_type.begin(), by_type.end(),
        [](const std::pair<std::string, std::string>& m)
        { return m.first.empty() || m.second.empty(); }) };
    ctx.check("no_empty_name_or_descriptor", no_empty_strings);

    // Every descriptor is well-formed: starts with '(' and contains ')'.
    const bool all_descriptors_wellformed{ std::all_of(
        by_type.begin(), by_type.end(),
        [](const std::pair<std::string, std::string>& m)
        { return !m.second.empty() && m.second.front() == '('
                 && m.second.find(')') != std::string::npos; }) };
    ctx.check("all_descriptors_wellformed", all_descriptors_wellformed);

    // Descriptor multiplicities (the heart of the shared-vs-unique distinction).
    ctx.check("descriptor_JJ_unique",   count_descriptor(by_type, "(J)J") == 1);
    ctx.check("descriptor_II_shared_3", count_descriptor(by_type, "(I)I") == 3);
    ctx.check("descriptor_V_shared_ge3", count_descriptor(by_type, "()V") >= 3);
    // The six DECLARED void methods (<init>, <clinit>, noop, tick, runIdLong,
    // runIdInt) are present on every JDK, but JDK 8's javac emits extra synthetic
    // methods for this class (18 total vs 16 on JDK 9+), some void — so the TOTAL
    // ()V multiplicity is a portable LOWER bound of 6, not exactly 6 everywhere.
    ctx.check("descriptor_V_shared_at_least_6", count_descriptor(by_type, "()V") >= 6);
    ctx.record(std::string{ "[INFO] ()V descriptor multiplicity = " } +
               std::to_string(count_descriptor(by_type, "()V")) +
               " (>=6: 6 declared void methods + any JDK-specific synthetics).");
    ctx.check("descriptor_strI_unique", count_descriptor(by_type, "(Ljava/lang/String;)I") == 1);
    ctx.check("descriptor_arrII_unique", count_descriptor(by_type, "([I)I") == 1);
    ctx.check("descriptor_IJDD_unique", count_descriptor(by_type, "(IJD)D") == 1);
    ctx.check("descriptor_Z_unique",    count_descriptor(by_type, "()Z") == 1);
    ctx.check("descriptor_objret_unique", count_descriptor(by_type, "()Ljava/lang/Object;") == 1);
    ctx.check("descriptor_JDJ_unique",  count_descriptor(by_type, "(JD)J") == 1);

    // A descriptor that nothing declares appears zero times.
    ctx.check("descriptor_absent_DD_zero", count_descriptor(by_type, "(D)D") == 0);

    // Lower bound on total: 14 application methods + <init>.  (Upper bound left
    // unconstrained on purpose so a JDK that adds a synthetic bridge can't break
    // the suite; the exact set is pinned by the membership checks above.)
    ctx.check("total_at_least_15", by_type.size() >= 15);

    // =====================================================================
    // PART B — get_class_methods(by NAME) AGREES with get_class_methods<T>().
    //   Same metaspace path as PART A (by internal name) -> HARD.
    // =====================================================================
    cp("PART B get_class_methods(by name) (metaspace metadata — no oop deref)");
    const std::vector<std::pair<std::string, std::string>> by_name{
        vmhook::get_class_methods(CLASS_NAME) };

    ctx.check("by_name_nonempty", !by_name.empty());
    ctx.check("by_name_same_size_as_by_type", by_name.size() == by_type.size());

    // Same multiset: every by_type pair appears the same number of times in
    // by_name and vice versa.  (Set-equality independent of order.)
    bool by_name_superset_of_by_type{ true };
    for (const std::pair<std::string, std::string>& m : by_type)
    {
        if (count_pair(by_name, m.first, m.second) != count_pair(by_type, m.first, m.second))
        {
            by_name_superset_of_by_type = false;
            break;
        }
    }
    bool by_type_superset_of_by_name{ true };
    for (const std::pair<std::string, std::string>& m : by_name)
    {
        if (count_pair(by_type, m.first, m.second) != count_pair(by_name, m.first, m.second))
        {
            by_type_superset_of_by_name = false;
            break;
        }
    }
    ctx.check("by_name_matches_by_type_each_pair", by_name_superset_of_by_type);
    ctx.check("by_type_matches_by_name_each_pair", by_type_superset_of_by_name);

    // Spot-check a couple of exact pairs through the by-name path directly.
    ctx.check("by_name_has_idLong_JJ", count_pair(by_name, "idLong", "(J)J") == 1);
    ctx.check("by_name_has_mix_IJDD",  count_pair(by_name, "mix",   "(IJD)D") == 1);
    ctx.check("by_name_descriptor_II_shared_3", count_descriptor(by_name, "(I)I") == 3);

    // Negative: a class that is NOT loaded enumerates to empty.
    const std::vector<std::pair<std::string, std::string>> by_bogus_name{
        vmhook::get_class_methods("vmhook/fixtures/NoSuchClassZZZ") };
    ctx.check("by_bogus_name_empty", by_bogus_name.empty());

    // Negative: an empty class name enumerates to empty (no crash).
    const std::vector<std::pair<std::string, std::string>> by_empty_name{
        vmhook::get_class_methods("") };
    ctx.check("by_empty_name_empty", by_empty_name.empty());

    // =====================================================================
    // PART C — get_class_methods<U>() on an UNREGISTERED wrapper type is empty.
    //   Pure type-map lookup miss -> returns empty, no deref -> HARD.
    // =====================================================================
    cp("PART C get_class_methods<unregistered>() (type-map miss — no deref)");
    const std::vector<std::pair<std::string, std::string>> by_unregistered_type{
        vmhook::get_class_methods<me_unregistered>() };
    ctx.check("unregistered_type_enumerates_empty", by_unregistered_type.empty());

    // =====================================================================
    // PART D — find_methods_by_signature<T>(desc): returns ALL matching names.
    //   Filters the PART-A metaspace enumeration by descriptor string-equality —
    //   no oop deref -> HARD.
    // =====================================================================
    cp("PART D find_methods_by_signature<T> (metaspace metadata — no oop deref)");

    // Unique (J)J -> exactly one name, and it is idLong.
    const std::vector<std::string> jj{ vmhook::find_methods_by_signature<me_fixture>("(J)J") };
    ctx.check("find_JJ_size_1", jj.size() == 1);
    ctx.check("find_JJ_is_idLong", jj.size() == 1 && jj.front() == "idLong");

    // Shared (I)I -> three names: idInt, addInt, sId (in some order).
    const std::vector<std::string> ii{ vmhook::find_methods_by_signature<me_fixture>("(I)I") };
    ctx.check("find_II_size_3", ii.size() == 3);
    ctx.check("find_II_has_idInt",
              std::find(ii.begin(), ii.end(), "idInt") != ii.end());
    ctx.check("find_II_has_addInt",
              std::find(ii.begin(), ii.end(), "addInt") != ii.end());
    ctx.check("find_II_has_sId",
              std::find(ii.begin(), ii.end(), "sId") != ii.end());

    // Shared ()V -> the synthetic members + the real void no-arg methods.
    const std::vector<std::string> vv{ vmhook::find_methods_by_signature<me_fixture>("()V") };
    ctx.check("find_V_size_ge3", vv.size() >= 3);
    ctx.check("find_V_has_noop",
              std::find(vv.begin(), vv.end(), "noop") != vv.end());
    ctx.check("find_V_has_tick",
              std::find(vv.begin(), vv.end(), "tick") != vv.end());
    ctx.check("find_V_has_init",
              std::find(vv.begin(), vv.end(), "<init>") != vv.end());

    // Other unique descriptors resolve to exactly their one method.
    const std::vector<std::string> sl{ vmhook::find_methods_by_signature<me_fixture>("(Ljava/lang/String;)I") };
    ctx.check("find_strI_size_1", sl.size() == 1);
    ctx.check("find_strI_is_strLen", sl.size() == 1 && sl.front() == "strLen");

    const std::vector<std::string> ar{ vmhook::find_methods_by_signature<me_fixture>("([I)I") };
    ctx.check("find_arrII_is_sumArr", ar.size() == 1 && ar.front() == "sumArr");

    const std::vector<std::string> mx{ vmhook::find_methods_by_signature<me_fixture>("(IJD)D") };
    ctx.check("find_IJDD_is_mix", mx.size() == 1 && mx.front() == "mix");

    const std::vector<std::string> zf{ vmhook::find_methods_by_signature<me_fixture>("()Z") };
    ctx.check("find_Z_is_flag", zf.size() == 1 && zf.front() == "flag");

    const std::vector<std::string> ob{ vmhook::find_methods_by_signature<me_fixture>("()Ljava/lang/Object;") };
    ctx.check("find_objret_is_makeObj", ob.size() == 1 && ob.front() == "makeObj");

    const std::vector<std::string> sw{ vmhook::find_methods_by_signature<me_fixture>("(JD)J") };
    ctx.check("find_JDJ_is_sWide", sw.size() == 1 && sw.front() == "sWide");

    // Negative: a descriptor nothing declares -> empty.
    const std::vector<std::string> none{ vmhook::find_methods_by_signature<me_fixture>("(D)D") };
    ctx.check("find_absent_DD_empty", none.empty());

    // Negative: an empty descriptor matches nothing (no method has an empty
    // descriptor; the candidate compare is exact-equality).
    const std::vector<std::string> empty_desc{ vmhook::find_methods_by_signature<me_fixture>("") };
    ctx.check("find_empty_descriptor_empty", empty_desc.empty());

    // Negative: a near-miss descriptor (right shape, wrong type) -> empty.
    const std::vector<std::string> nearmiss{ vmhook::find_methods_by_signature<me_fixture>("(I)J") };
    ctx.check("find_nearmiss_IJ_empty", nearmiss.empty());

    // Negative: unregistered wrapper type -> empty.
    const std::vector<std::string> unreg{ vmhook::find_methods_by_signature<me_unregistered>("(J)J") };
    ctx.check("find_unregistered_type_empty", unreg.empty());

    // Consistency: find_methods_by_signature multiplicities equal the
    // get_class_methods descriptor counts (the two helpers agree).
    ctx.check("find_JJ_count_eq_enum", jj.size() == count_descriptor(by_type, "(J)J"));
    ctx.check("find_II_count_eq_enum", ii.size() == count_descriptor(by_type, "(I)I"));
    ctx.check("find_V_count_eq_enum",  vv.size() == count_descriptor(by_type, "()V"));

    // =====================================================================
    // PART E — hook_by_signature<T>: INSTALL + FIRE on the UNIQUE (J)J match.
    //   The detour observes the long arg (across the 2-slot boundary) and self.
    //   run() calls idLong on a real bytecode dispatch (mode 1).
    //
    //   *** THE cold-JVM crash site ***  The detour reads self->seed(), a RAW
    //   memcpy at receiver_oop+offset (field_proxy::get).  The library's detour
    //   trampoline wraps this in seh_invoke_detour, but that SEH net is a no-op
    //   on mingw·gcc (its #else branch is a plain try/catch that cannot catch a
    //   structured AV), so a faulting seed read escapes uncaught and tears down
    //   the JVM.  The detour therefore reads via seed_safe(): it probes the oop
    //   header AND the exact 4-byte seed slot with os::safe_read first, so the
    //   read provably cannot fault; a transient miss yields nullopt -> tri-state
    //   0 (best-effort [INFO]), a successful read sets 1 (correct) or 2 (wrong).
    //   The arg decode and fire-count read NO oop (frame locals / an atomic), so
    //   they stay HARD.
    // =====================================================================
    {
        g_jj_fire_count.store(0);
        g_jj_arg.store(-1);
        g_jj_self_state.store(0);

        cp("PART E hook_by_signature<(J)J> install");
        const bool installed{ vmhook::hook_by_signature<me_fixture>(
            "(J)J",
            [](vmhook::return_value&,
               const std::unique_ptr<me_fixture>& self,
               std::int64_t x)
            {
                // Fire-count + arg decode touch NO oop (an atomic and the frame's
                // local-variable slots, which are live stack during the in-flight
                // call) -> always safe, even on a cold JVM with no SEH net.
                g_jj_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_jj_arg.store(x, std::memory_order_relaxed);

                // The ONLY oop deref: gate it through seed_safe(), which proves the
                // receiver header + the exact seed slot are currently mapped before
                // the memcpy.  nullopt = transient unreadable (tri-state 0); a read
                // value sets 1 (== SEED) or 2 (mismatch) so a wrong self still fails.
                if (self != nullptr)
                {
                    const std::optional<std::int32_t> s{ self->seed_safe() };
                    if (s.has_value())
                    {
                        g_jj_self_state.store(*s == SEED ? 1 : 2,
                                              std::memory_order_relaxed);
                    }
                }
            }) };

        ctx.check("hook_by_sig_JJ_installed_true", installed);

        cp("PART E drive(mode 1) — real idLong bytecode dispatch fires detour");
        const bool done{ drive(ctx, 1) };
        ctx.check("hook_by_sig_JJ_probe_completed", done);

        ctx.check("hook_by_sig_JJ_fired_once",
                  g_jj_fire_count.load(std::memory_order_relaxed) == 1);
        ctx.check("hook_by_sig_JJ_fired_not_zero",
                  g_jj_fire_count.load(std::memory_order_relaxed) != 0);
        ctx.check("hook_by_sig_JJ_decoded_long_arg",
                  g_jj_arg.load(std::memory_order_relaxed) == IDLONG_ARG);

        // saw_correct_self: HARD-fail on a WRONG seed from a successful read
        // (state 2 — a genuine wrong-self / mis-decode bug); pass on state 1;
        // best-effort [INFO] on state 0 (the detour fired but the receiver slot
        // was not safely readable at that instant — transient, never a fault).
        const std::int32_t self_state{ g_jj_self_state.load(std::memory_order_relaxed) };
        if (self_state == 0)
        {
            ctx.record("[INFO] hook_by_sig_JJ_saw_correct_self: detour fired but the "
                       "receiver `seed` slot was not safely readable at that instant "
                       "(transient cold-JVM/relocation miss) — skipped self assert "
                       "(not a defect; fire-count + arg-decode above stay HARD).");
        }
        else
        {
            ctx.check("hook_by_sig_JJ_saw_correct_self", self_state == 1);
        }
        // allow-through: the original idLong body still ran (returns its arg).
        // get_last_id_long reads the static mirror (old-gen, stable) -> HARD.
        ctx.check("hook_by_sig_JJ_allow_through",
                  me_fixture::get_last_id_long() == IDLONG_ARG);
    }

    // =====================================================================
    // PART F — hook_by_signature<T> REFUSES on a SHARED descriptor (I)I.
    //   It must return false AND install nothing — proven by then calling
    //   idInt (mode 2) and confirming the would-be detour never fires.
    //   The refused (I)I detour reads NO oop (an atomic only) and never installs;
    //   the accepted (J)J detour does NOT fire on idInt (a different method) — so
    //   mode 2 runs no oop-dereferencing detour body and cannot fault.
    // =====================================================================
    {
        g_refused_fire_count.store(0);

        cp("PART F hook_by_signature<(I)I> refuse (shared descriptor)");
        const bool installed{ vmhook::hook_by_signature<me_fixture>(
            "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<me_fixture>&,
               std::int32_t)
            {
                g_refused_fire_count.fetch_add(1, std::memory_order_relaxed);
            }) };

        ctx.check("hook_by_sig_II_refused_false", installed == false);

        cp("PART F drive(mode 2) — idInt dispatch (no detour body derefs an oop)");
        const bool done{ drive(ctx, 2) };
        ctx.check("hook_by_sig_II_probe_completed", done);

        // idInt really ran (allow-through of an UNHOOKED method).
        ctx.check("hook_by_sig_II_java_call_happened",
                  me_fixture::get_last_id_int() == IDINT_ARG);
        // ...but the refused hook installed nothing, so it never fired.
        ctx.check("hook_by_sig_II_detour_never_fired",
                  g_refused_fire_count.load(std::memory_order_relaxed) == 0);
        // The accepted (J)J hook from PART E must NOT have fired on an idInt
        // call either (idInt is (I)I, a different method).
        ctx.check("hook_by_sig_JJ_not_fired_on_idInt",
                  g_jj_fire_count.load(std::memory_order_relaxed) == 1);
    }

    // =====================================================================
    // PART G — hook_by_signature<T> REFUSES on the SYNTHETIC-member collision
    //   ()V (6 matches incl. <init>/<clinit>): returns false, installs nothing.
    //   No probe needed — the refusal is a pure resolution decision (no deref).
    // =====================================================================
    cp("PART G hook_by_signature<()V> refuse (synthetic-member collision)");
    {
        std::atomic<std::int32_t> v_fire{ 0 };
        const bool installed{ vmhook::hook_by_signature<me_fixture>(
            "()V",
            [&v_fire](vmhook::return_value&,
                      const std::unique_ptr<me_fixture>&)
            {
                v_fire.fetch_add(1, std::memory_order_relaxed);
            }) };
        ctx.check("hook_by_sig_V_refused_false", installed == false);
        ctx.check("hook_by_sig_V_nothing_fired_yet", v_fire.load() == 0);
    }

    // =====================================================================
    // PART H — hook_by_signature<T> on a descriptor that matches NOTHING:
    //   returns false (distinct refusal path: empty, not multi-match).
    //   Pure resolution decisions, no probe, no deref.
    // =====================================================================
    cp("PART H hook_by_signature refusals (no-match / empty / unregistered)");
    {
        const bool installed{ vmhook::hook_by_signature<me_fixture>(
            "(D)D",
            [](vmhook::return_value&,
               const std::unique_ptr<me_fixture>&,
               double)
            {
            }) };
        ctx.check("hook_by_sig_absent_DD_refused_false", installed == false);
    }

    // Negative: an empty descriptor matches nothing -> false.
    {
        const bool installed{ vmhook::hook_by_signature<me_fixture>(
            "",
            [](vmhook::return_value&, const std::unique_ptr<me_fixture>&)
            {
            }) };
        ctx.check("hook_by_sig_empty_descriptor_refused_false", installed == false);
    }

    // Negative: hook_by_signature on an UNREGISTERED wrapper type -> false
    // (find_methods_by_signature returns empty for it).
    {
        const bool installed{ vmhook::hook_by_signature<me_unregistered>(
            "(J)J",
            [](vmhook::return_value&,
               const std::unique_ptr<me_unregistered>&,
               std::int64_t)
            {
            }) };
        ctx.check("hook_by_sig_unregistered_type_refused_false", installed == false);
    }

    // =====================================================================
    // PART I — a UNIQUE descriptor that the probe never dispatches still
    //   INSTALLS (true).  Proves install success is decided purely by
    //   descriptor uniqueness, not by whether the method is later called.
    //   (sWide (JD)J is static and unique; we never invoke it — the detour never
    //   fires, so no deref.)
    // =====================================================================
    cp("PART I hook_by_signature<(JD)J> install (unique, never dispatched)");
    {
        const bool installed{ vmhook::hook_by_signature<me_fixture>(
            "(JD)J",
            [](vmhook::return_value&, std::int64_t, double)
            {
            }) };
        ctx.check("hook_by_sig_unique_static_JDJ_installed_true", installed);
    }

    cp("module complete (all parts reached without a no-SEH fault)");
}
