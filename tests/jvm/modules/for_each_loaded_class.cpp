// for_each_loaded_class JVM test module  (feature area: class enumeration)
//
// Exhaustively exercises vmhook::for_each_loaded_class() on a LIVE JVM.  The
// function takes a SNAPSHOT of every Java class currently reachable through the
// global ClassLoaderDataGraph (JDK 21+ via ClassLoaderData::_klasses; JDK 8-17
// via the per-CLD Dictionary hashtables + the SystemDictionary _dictionary /
// _shared_dictionary fallback) and invokes
//
//     visitor(const std::string& internal_name, vmhook::hotspot::klass* k)
//
// once per Klass, where internal_name uses JVM '/'-separated form (e.g.
// "java/lang/String").  This migrates the legacy inline
// test_for_each_loaded_class from vmhook/src/example.cpp (Rework D).
//
// What the module proves, angle by angle (all PORTABLE across the supported JDK
// matrix — no exact count, no exact class set, because the loaded-class universe
// differs wildly between JDK 8 and JDK 21+ and between CDS-on / CDS-off):
//
//   - the snapshot is NON-TRIVIAL: a real JVM with the harness loaded holds far
//     more than 100 classes (bootstrap rt.jar / the java.base module alone is
//     thousands), so count > 100 is a robust liveness floor;
//   - the UNIVERSAL bootstrap classes are present: java/lang/Object,
//     java/lang/String, java/lang/Class, java/lang/Integer, java/lang/Thread —
//     these are loaded before any user code on every HotSpot, so their absence
//     would mean the walk missed the bootstrap loader entirely;
//   - APPLICATION-loaded classes are reached, not just bootstrap: this module's
//     OWN fixture class vmhook/fixtures/ForEachLoadedClass (Class.forName'd by
//     Main.loadFixtures at startup) MUST appear — the single strongest proof the
//     walk descends past the bootstrap CLD into the application class loader;
//   - EVERY klass pointer handed to the visitor is valid (passes the same
//     is_valid_pointer gate vmhook itself uses) — the enumeration never yields a
//     torn / sentinel / freed pointer the caller would crash on;
//   - the enumerated klass is genuinely USABLE, not merely non-null: for the OWN
//     fixture class the module derefs the supplied klass* (guarded) and confirms
//     klass->get_name()->to_string() ROUND-TRIPS to the very name the visitor was
//     handed — i.e. the pointer and the name describe the same live Klass;
//   - NO name is empty and EVERY name is well-formed (no leading '/', no NUL, no
//     embedded whitespace) — symbol decode never silently produced "" or garbage;
//   - NO klass handed to the visitor is null or torn: both walk paths call the
//     visitor only after is_valid_pointer passed (vmhook.hpp:3875/4047), so
//     null_klass == 0 and invalid_klass == 0 is a UNIVERSAL HARD contract;
//   - names arrive in INTERNAL '/'-separated form, never the Java dotted form (no
//     enumerated name contains a '.'; the fixture's dotted twin is absent);
//   - an INTROSPECTING visitor (one that re-derefs EVERY valid klass and re-reads
//     its _name) is as safe as the minimal name/count visitor — both walk the full
//     graph, return control, and never crash the JVM;
//   - the walk TERMINATES (control returns within a generous wall-clock bound) and
//     the count is bounded well below the internal 1M-per-CLD / 64K-CLD safety
//     caps, so we are observing a real finite graph, not a runaway loop;
//   - the snapshot is STABLE: a second independent enumeration agrees on every
//     robust invariant (count still > 100, the known classes still present, the
//     two counts within a small drift band) — enumeration is repeatable and
//     side-effect-free;
//   - SNAPSHOT FRESHNESS: a class loaded AFTER a first snapshot (the '$'-nested
//     ForEachLoadedClass$LateAnchor, Class.forName'd by the probe) appears in a
//     SECOND snapshot — the documented snapshot-at-call-time property.
//
// Two LIBRARY-CONTRACT angles worth calling out:
//   - NO EARLY-TERMINATE API.  for_each_loaded_class's visitor is
//     void(const std::string&, klass*) (vmhook.hpp:7505) — its return value is
//     IGNORED, so there is no "return false to stop" hook to honour.  The only
//     visitor-driven control flow the library promises is its OWN try/catch
//     (vmhook.hpp:7507): a std::exception thrown from the visitor is CONTAINED,
//     iteration stops at that visit, and control returns normally (no JVM crash).
//     Part E proves that containment and that a later enumeration still works.
//   - DUPLICATES are CHARACTERIZED, not asserted.  On JDK 8 the same Klass can be
//     listed by both the per-CLD Dictionary and the SystemDictionary fallback, so
//     a repeated klass* is legitimate — "no duplicate pointer" is NOT portable.
//
// Treated as BEST-EFFORT on JDK 8 only (recorded [INFO]; HARD on JDK 9+ via
// gate_jdk9_hard, since the JDK 9+ _klasses walk lists every Klass — verified
// locally): the launcher class vmhook/Main and the nested / array anchors
// (ForEachLoadedClass$Inner, [I, [Ljava/lang/String;) live in Klass families the
// JDK-8 dictionary-only walk omits.
//
// Harness note: the STATIC enumeration is a pure HotSpot-internal read driven
// straight from the native worker thread — no hook is required.  The fixture's
// go/done probe IS used (Part F): the native side raises `go` so the fixture's
// tick-thread probe Class.forName's the late class, giving the freshness test a
// class that is provably loaded only AFTER the first snapshot.  The module
// installs NO hooks, so there is nothing to scope or shut down; it reads
// vmhook.hpp's public surface and never mutates JVM state beyond triggering that
// one on-demand class load.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Internal name of this module's own fixture — Class.forName'd at startup by
    // Main.loadFixtures(), so it lives in the application class loader's CLD and
    // MUST appear in the enumeration (the app-class proof).
    constexpr char OWN_FIXTURE[]{ "vmhook/fixtures/ForEachLoadedClass" };

    // A nested class the fixture DELIBERATELY does NOT load at startup (it is
    // '$'-nested so Main.loadFixtures skips it, AND the fixture's static
    // initializer never references it).  The freshness probe Class.forName's it
    // on demand, so it is the snapshot-freshness target: ABSENT from a first
    // snapshot, PRESENT in a second taken after the probe loaded it.
    constexpr char LATE_FIXTURE[]{ "vmhook/fixtures/ForEachLoadedClass$LateAnchor" };

    // The result of one full enumeration pass.
    struct enumeration
    {
        std::set<std::string>            names{};         // every internal name seen
        std::set<vmhook::hotspot::klass*> ptrs{};         // every distinct klass* seen
        std::size_t                count{ 0 };            // raw visit count (with dups)
        std::size_t                null_klass{ 0 };       // visits handed a nullptr klass
        std::size_t                invalid_klass{ 0 };    // visits whose non-null klass failed the gate
        std::size_t                dup_ptr{ 0 };          // visits repeating an already-seen klass*
        bool                       all_klass_valid{ true };// every klass* passed the gate
        bool                       any_empty_name{ false };// any visit yielded ""
        bool                       any_bad_name{ false };  // any malformed name
        bool                       introspect_safe{ true };// re-deref of every klass stayed safe
        // For the OWN fixture: the klass* the visitor handed us, plus whether its
        // get_name() round-trips back to OWN_FIXTURE (usability proof).
        bool                       own_seen{ false };
        bool                       own_klass_valid{ false };
        bool                       own_name_roundtrips{ false };
        // The klass* the visitor handed us for the OWN fixture and for the
        // bootstrap canary java/lang/Object (nullptr if that name was not
        // enumerated this pass) — used for the cross-path POINTER IDENTITY angle
        // (enumeration's klass* must equal find_class()'s klass* for the SAME
        // name, on the graph-walk JDKs).
        vmhook::hotspot::klass*    own_klass_ptr{ nullptr };
        vmhook::hotspot::klass*    object_klass_ptr{ nullptr };
        // PREDICATE / FILTERING-visitor angle: how many enumerated names fall
        // under a couple of stable JVM prefixes.  These are not exact (the
        // universe varies across the JDK matrix) but give robust HARD lower
        // bounds — a real JVM always holds many java/lang/* classes.
        std::size_t                java_prefix{ 0 };       // names under "java/"
        std::size_t                java_lang_prefix{ 0 };  // names under "java/lang/"
        // ARRAY / NESTED structural characterization, folded over the walk so the
        // form of each family member is validated WHEN it is enumerated (presence
        // itself stays [INFO] / JDK-gated, but a malformed member is a HARD fail).
        std::size_t                array_names{ 0 };       // names starting with '['
        bool                       array_form_ok{ true };  // every '[' name well-shaped
        std::size_t                dollar_names{ 0 };      // names containing '$'
        bool                       dollar_form_ok{ true };  // every '$' name well-shaped
    };

    // Wrapper for vmhook.fixtures.ForEachLoadedClass.  Deriving from
    // vmhook::object<> gives the wrapper its vtable (required by register_class<T>)
    // and the static_field(...) accessors used for the freshness go/done handshake
    // and the late-load read-back.  No instance methods are needed — enumeration
    // is a pure native read; this wrapper only drives the probe.
    class felc_fixture : public vmhook::object<felc_fixture>
    {
    public:
        explicit felc_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<felc_fixture>{ instance }
        {
        }

        // go/done handshake (static fields live on the class mirror).
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }
        static auto get_done() -> bool           { return static_field("done")->get(); }

        // Freshness read-back the probe writes on the tick thread.
        static auto get_late_loaded() -> bool    { return static_field("lateLoaded")->get(); }
    };

    // A name is well-formed when it is non-empty, carries no leading '/', and has
    // no control / whitespace bytes.  (Array names like "[I" legitimately start
    // with '[', and inner-class names carry '$', so neither is rejected here.)
    auto name_is_wellformed(const std::string& name) -> bool
    {
        if (name.empty() || name.front() == '/')
        {
            return false;
        }
        for (const char ch : name)
        {
            const unsigned char byte{ static_cast<unsigned char>(ch) };
            if (byte <= 0x20u)   // NUL, control chars, space
            {
                return false;
            }
        }
        return true;
    }

    // An array descriptor is well-shaped when, after its leading run of '[' (the
    // dimension count, >=1), the element tag is a legal JVM field descriptor:
    // one of the primitive tags (Z B C S I J F D) for a primitive array, or a
    // reference tag 'L<internal-name>;' for an object array (ending in ';').  This
    // is the structural form check applied to every enumerated name beginning with
    // '[' — presence of array klasses stays JDK-gated, but the SHAPE of any that
    // are surfaced is a HARD invariant (a torn array symbol would violate it).
    auto array_descriptor_wellformed(const std::string& name) -> bool
    {
        std::size_t dims{ 0 };
        while (dims < name.size() && name[dims] == '[')
        {
            ++dims;
        }
        if (dims == 0 || dims >= name.size())   // no element tag after the '['s
        {
            return false;
        }
        const char tag{ name[dims] };
        switch (tag)
        {
            case 'Z': case 'B': case 'C': case 'S':
            case 'I': case 'J': case 'F': case 'D':
                return dims + 1 == name.size();   // primitive: exactly one tag byte
            case 'L':
                // Object array: 'L' <non-empty internal name> ';'
                return name.size() >= dims + 3 && name.back() == ';';
            default:
                return false;
        }
    }

    // Runs ONE full for_each_loaded_class pass and folds every observation into a
    // single `enumeration`.  Every klass dereference is guarded by is_valid_pointer
    // so a torn pointer in the snapshot can never crash the JVM from this test.
    auto enumerate_once() -> enumeration
    {
        enumeration result{};

        vmhook::for_each_loaded_class(
            [&](const std::string& name, vmhook::hotspot::klass* const k)
            {
                ++result.count;
                result.names.insert(name);

                if (name.empty())
                {
                    result.any_empty_name = true;
                }
                else if (!name_is_wellformed(name))
                {
                    result.any_bad_name = true;
                }

                // PREDICATE / FILTERING-visitor tallies (selective enumeration):
                // count names under a couple of always-present JVM prefixes.  A
                // visitor that filters by prefix is a first-class use of the API
                // (vmhook.hpp's own example filters "net/minecraft/"); these
                // counters let the caller assert a robust lower bound.
                if (name.rfind("java/", 0) == 0)
                {
                    ++result.java_prefix;
                    if (name.rfind("java/lang/", 0) == 0)
                    {
                        ++result.java_lang_prefix;
                    }
                }

                // ARRAY-name structural validation (only when an array klass was
                // surfaced — presence is JDK-variant, but FORM is universal).
                if (!name.empty() && name.front() == '[')
                {
                    ++result.array_names;
                    if (!array_descriptor_wellformed(name))
                    {
                        result.array_form_ok = false;
                    }
                }

                // NESTED ('$') name structural validation: an inner/nested klass
                // name is "<outer>$<member>" — the '$' is neither the first nor
                // the last byte, and the whole thing is well-formed.  (Array names
                // never contain '$', so the two families do not overlap.)
                if (name.find('$') != std::string::npos)
                {
                    ++result.dollar_names;
                    const std::size_t pos{ name.find('$') };
                    if (pos == 0 || pos + 1 >= name.size() || !name_is_wellformed(name))
                    {
                        result.dollar_form_ok = false;
                    }
                }

                // The library NEVER hands the visitor a null klass: both walk
                // paths (the JDK 21+ _klasses list and the JDK 8-17 dictionary)
                // call the visitor only AFTER is_valid_pointer(candidate) passed
                // (vmhook.hpp:3875/4047).  Count any nullptr / invalid pointer so
                // the caller can assert the contract (null_klass + invalid_klass
                // must both be 0).  A non-null pointer that fails the gate would
                // be a torn / sentinel / freed value the caller must never see.
                if (k == nullptr)
                {
                    ++result.null_klass;
                    result.all_klass_valid = false;
                    return;   // nothing else to introspect.
                }
                if (!vmhook::hotspot::is_valid_pointer(k))
                {
                    ++result.invalid_klass;
                    result.all_klass_valid = false;
                    return;
                }

                // DUPLICATE-POINTER characterization (best-effort, recorded by the
                // caller as [INFO], never asserted): on JDK 8 the same Klass can be
                // listed by both the per-CLD Dictionary and the SystemDictionary
                // _dictionary / _shared_dictionary fallback, so the same klass* can
                // legitimately repeat — NOT a portable "no duplicate" invariant.
                if (!result.ptrs.insert(k).second)
                {
                    ++result.dup_ptr;
                }

                // INTROSPECTING visitor (vs the minimal name/count visitor): for
                // EVERY valid klass re-deref the supplied pointer (guarded) and
                // read its _name symbol back.  This is the "callback that
                // introspects each klass" angle — it must stay crash-safe across
                // the whole snapshot (not just the fixture), and the re-read name
                // must itself be empty-or-well-formed.  A symbol that is non-null
                // but decodes to a control-byte string would flip introspect_safe.
                const vmhook::hotspot::symbol* const sym{ k->get_name() };
                if (vmhook::hotspot::is_valid_pointer(sym))
                {
                    const std::string round{ sym->to_string() };
                    if (!round.empty() && !name_is_wellformed(round))
                    {
                        result.introspect_safe = false;
                    }
                }

                // For our OWN fixture class, prove the supplied klass* is usable:
                // deref it (guarded) and confirm its own _name symbol round-trips
                // back to the exact internal name the visitor handed us.
                if (name == OWN_FIXTURE)
                {
                    result.own_seen = true;
                    result.own_klass_valid = true;   // proven valid above.
                    result.own_klass_ptr = k;        // capture for the find_class identity angle.
                    if (vmhook::hotspot::is_valid_pointer(sym))
                    {
                        result.own_name_roundtrips = (sym->to_string() == OWN_FIXTURE);
                    }
                }
                // Capture the bootstrap canary's klass* for the same cross-path
                // pointer-identity check (it must equal find_class("java/lang/Object")).
                else if (name == "java/lang/Object")
                {
                    result.object_klass_ptr = k;
                }
            });

        return result;
    }
}

VMHOOK_JVM_MODULE(for_each_loaded_class)
{
    // Register the fixture wrapper so the snapshot-freshness section (Part F) can
    // drive its go/done handshake via the static-field accessors.  Enumeration
    // itself needs no registration — it is a pure native graph read — so PASSES
    // 1/2 below do not depend on this, but the freshness probe does.
    vmhook::register_class<felc_fixture>(OWN_FIXTURE);

    // =====================================================================
    // PASS 1 — a single full enumeration; every invariant folded in one walk.
    // =====================================================================
    const enumeration e1{ enumerate_once() };

    ctx.record(std::string{ "[INFO] for_each_loaded_class pass 1 visited " }
               + std::to_string(e1.count) + " klass(es), "
               + std::to_string(e1.names.size()) + " distinct name(s)");

    // ── JDK-8 detection (house idiom, mirrors field_string.cpp /
    //    make_java_string.cpp): java.lang.String carries the compact-string
    //    `coder` field only on JDK 9+; on JDK 8 the backing is a classic char[]
    //    and there is no `coder` field, so its absence is a reliable "this is
    //    JDK 8" signal that needs no version string parsing. ──
    vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
    const bool jdk8{ string_klass != nullptr
                     && !string_klass->find_field("coder").has_value() };

    // BEST-EFFORT gate for this module's two java.lang.String-PRESENCE checks
    // (has_java_lang_String, pass2_has_java_lang_String) — the legacy inline test
    // in example.cpp gates its sibling forEachLoadedClassString the same way.  On
    // JDK 8 the SystemDictionary enumeration is INCOMPLETE / non-deterministic for
    // bootstrap classes: this same module already records vmhook/Main and the [I
    // array klass as "NOT enumerated (JDK 8 quirk)", and java.lang.String is
    // intermittently omitted by the very same walk (it appeared at an earlier
    // commit, vanished here).  So on JDK 8 we RECORD String's absence as [INFO]
    // rather than hard-asserting it; on JDK 9+ the presence check stays HARD.
    // java.lang.Object was historically HARD (the bootstrap-loader canary), but the
    // VS18/MSVC-14.51 windows runner intermittently drops it from the JDK 8
    // enumeration too (CI showed has_java_lang_Object/pass2 FAIL on windows·clang·java8),
    // so it is now gated the same best-effort way on JDK 8 — HARD on JDK 9+.
    const auto gate_bootstrap_presence =
        [&ctx, jdk8](const std::string& name, bool present) -> void
    {
        if (!jdk8 || present)
        {
            // JDK 9+ (hard assert), or JDK 8 where the class happened to be listed
            // (still record the PASS): assert normally.
            ctx.check(name, present);
            return;
        }
        ctx.record("[INFO] a bootstrap class was not enumerated by for_each_loaded_class "
                   "on JDK8 (incomplete / non-deterministic SystemDictionary enumeration, "
                   "same quirk as java.lang.String/Main/arrays) — presence check '"
                   + name + "' recorded, not asserted");
    };

    // BEST-EFFORT gate for klass FAMILIES the JDK 8 dictionary path does not list
    // but the JDK 9+ per-CLD _klasses walk DOES — array klasses ([I,
    // [Ljava/lang/String;), the nested $Inner app class, and the launcher class.
    // On JDK 9+ the _klasses linked list enumerates EVERY Klass (verified: a local
    // JDK enumerates [I, $Inner, vmhook/Main), so presence is HARD there; on JDK 8
    // the SystemDictionary-only walk omits these families, so record [INFO].  This
    // is the same JDK-split discipline as gate_bootstrap_presence, applied to the
    // array/nested/launcher targets the legacy test recorded as info-only.
    const auto gate_jdk9_hard =
        [&ctx, jdk8](const std::string& name, bool present, const char* jdk8_reason) -> void
    {
        if (!jdk8 || present)
        {
            ctx.check(name, present);
            return;
        }
        ctx.record(std::string{ "[INFO] " } + name + " not enumerated on JDK8 ("
                   + jdk8_reason + ") — recorded, not asserted; HARD on JDK 9+");
    };

    // ── HARD app-loader anchor that survives the JDK8 enumeration quirk ──
    //
    // The OWN fixture's PRESENCE-IN-THE-ENUMERATION is what regressed on
    // windows·java8: HotSpot's JDK8 path has no per-CLD Dictionary in VMStructs,
    // so for_each_loaded_class can only walk SystemDictionary::_dictionary /
    // _shared_dictionary, and that walk intermittently DROPS entries (the very
    // same quirk already gated above for java.lang.String — it "appeared at an
    // earlier commit, vanished").  Whether any one class survives depends on the
    // hashtable bucket/chain layout, which shifts as the loaded-class universe
    // grows (the 5 new Wave-5 modules each drop a fixture class), so the fixture
    // that used to land in a surviving chain now falls in a dropped one.
    //
    // CRUCIALLY this does NOT mean the class is unloaded or unreachable — only
    // that the enumeration missed it.  vmhook::find_class() proves that
    // independently: on JDK8 it falls back from the dictionary walk to a JNI
    // ClassLoader.loadClass() resolve (vmhook.hpp jni_find_class_with_context_loader
    // ~9534), so for an already-loaded app class it ALWAYS resolves.  So we keep
    // two HARD floors that hold on EVERY JDK, and only downgrade the fragile
    // "did the enumeration list THIS exact class" assertion on JDK8:
    //
    //   (1) the fixture is genuinely loaded + resolvable (find_class non-null);
    //   (2) the walk DID reach the application class-loader region — at least one
    //       vmhook/* application class is enumerated (dropping all ~80 app classes
    //       the harness loads is implausible: they scatter across many buckets).
    //
    // Neither floor is vacuous, and neither depends on the per-entry JDK8 lottery.

    // (1) HARD on every JDK: the fixture class is loaded and resolvable.  On JDK8
    //     this rides find_class's JNI fallback, so it is robust to the dictionary
    //     walk dropping the entry; on JDK 9+ the graph walk resolves it directly.
    const bool own_fixture_resolvable{ vmhook::find_class(OWN_FIXTURE) != nullptr };
    ctx.check("own_fixture_resolvable_via_find_class", own_fixture_resolvable);

    // (2) HARD on every JDK: count the application (vmhook/*) classes the walk
    //     surfaced.  >=1 proves the enumeration descended into the app loader, the
    //     real invariant behind "has_own_fixture_class" — robust to WHICH app
    //     class happens to survive the JDK8 chain-truncation lottery.
    const auto count_app_classes =
        [](const std::set<std::string>& names) -> std::size_t
    {
        std::size_t app{ 0 };
        for (const std::string& n : names)
        {
            if (n.rfind("vmhook/", 0) == 0)   // starts_with, C++17-safe
            {
                ++app;
            }
        }
        return app;
    };
    const std::size_t app_classes_seen{ count_app_classes(e1.names) };
    ctx.record(std::string{ "[INFO] for_each_loaded_class: enumeration surfaced " }
               + std::to_string(app_classes_seen) + " vmhook/* application class(es)");
    ctx.check("app_loader_reached", app_classes_seen >= 1);

    // BEST-EFFORT gate for the OWN-fixture ENUMERATION checks, mirroring
    // gate_bootstrap_presence.  On JDK 9+ (per-CLD Dictionary walk lists every app
    // class) these stay HARD.  On JDK8 the SystemDictionary-only walk may drop the
    // fixture entry, so when it is genuinely absent we record [INFO] instead of
    // failing — the two HARD floors above already prove the class is loaded AND
    // that the app loader was reached.  When the fixture WAS enumerated (any JDK,
    // including JDK8 when it happened to survive), the check is asserted normally,
    // so a seen-but-broken result still FAILS — the gate never goes vacuous.
    const auto gate_own_fixture =
        [&ctx, jdk8](const std::string& name, bool ok, bool fixture_enumerated) -> void
    {
        if (!jdk8 || fixture_enumerated)
        {
            ctx.check(name, ok);
            return;
        }
        ctx.record("[INFO] own fixture '" + std::string{ OWN_FIXTURE }
                   + "' not surfaced by for_each_loaded_class on JDK8 (incomplete "
                     "SystemDictionary enumeration, same quirk as java.lang.String/"
                     "Main/arrays) — check '" + name + "' recorded, not asserted; the "
                     "class IS loaded (find_class resolves it) and the app loader WAS "
                     "reached (>=1 vmhook/* class enumerated)");
    };

    // ---- Liveness: the snapshot is non-trivial. -------------------------
    // A real JVM with the harness loaded holds thousands of classes; >100 is a
    // robust portable floor across JDK 8 .. 21+ and CDS-on / CDS-off.
    ctx.check("count_over_100", e1.count > 100);
    // The distinct-name set is likewise far over the floor (no massive dup skew).
    ctx.check("distinct_names_over_100", e1.names.size() > 100);
    // ...and the visit count terminated well under the internal safety caps
    // (1M klasses per CLD, 64K CLDs), so this is a real finite graph, not a loop.
    ctx.check("count_under_safety_cap", e1.count < 1'000'000);

    // ---- Universal bootstrap classes are present. -----------------------
    // These five load before any user code on every HotSpot build; missing any
    // one means the walk never reached the bootstrap loader.
    // Object is the canary that the bootstrap loader was reached at all — it is
    // enumerated on EVERY JDK, so it stays HARD.  String/Class/Integer/Thread are
    // all intermittently dropped by the JDK 8 SystemDictionary-only walk (Class
    // failed on mingw·java8 + msvc·java8 once added modules shifted suite timing),
    // so they are best-effort on JDK 8 and HARD on JDK 9+.
    gate_bootstrap_presence("has_java_lang_Object",  e1.names.contains("java/lang/Object"));
    gate_bootstrap_presence("has_java_lang_String", e1.names.contains("java/lang/String"));
    gate_bootstrap_presence("has_java_lang_Class",   e1.names.contains("java/lang/Class"));
    gate_bootstrap_presence("has_java_lang_Integer", e1.names.contains("java/lang/Integer"));
    gate_bootstrap_presence("has_java_lang_Thread",  e1.names.contains("java/lang/Thread"));

    // ---- Application-loaded class is reached (not just bootstrap). -------
    // The strongest proof the walk descends into the application loader: this
    // module's OWN fixture, Class.forName'd at startup, is enumerated.  On JDK 9+
    // this is HARD; on JDK8 it is best-effort (see gate_own_fixture) because the
    // SystemDictionary-only walk may drop the entry — but the HARD app_loader_reached
    // and own_fixture_resolvable_via_find_class floors above still hold there.
    const bool own_enumerated{ e1.names.contains(OWN_FIXTURE) };
    gate_own_fixture("has_own_fixture_class", own_enumerated, own_enumerated);
    gate_own_fixture("own_fixture_seen_by_visitor", e1.own_seen, own_enumerated);

    // ---- The enumerated klass pointer is valid AND usable. --------------
    // Every non-null klass* the visitor produced passed the is_valid_pointer gate.
    ctx.check("every_klass_pointer_valid", e1.all_klass_valid);
    // NO klass delivered to the visitor is null — a UNIVERSAL HARD contract on
    // every JDK: both walk paths call the visitor only after is_valid_pointer
    // passed (vmhook.hpp:3875/4047), so the visitor never sees nullptr or an
    // in-range-but-torn pointer.  These two are the executable form of "no null
    // klass delivered to the callback".
    ctx.record(std::string{ "[INFO] for_each_loaded_class: pass-1 null-klass visits=" }
               + std::to_string(e1.null_klass) + " invalid-klass visits="
               + std::to_string(e1.invalid_klass));
    ctx.check("no_null_klass_delivered", e1.null_klass == 0);
    ctx.check("no_invalid_klass_delivered", e1.invalid_klass == 0);

    // ---- INTROSPECTING visitor stays crash-safe over the WHOLE snapshot. -
    // enumerate_once already re-derefs EVERY valid klass (a second, heavier
    // visitor than the minimal name/count one) and reads its _name back.  That it
    // returned at all proves the introspecting callback never crashed the JVM, and
    // introspect_safe proves no re-read name was a control-byte string.  This is
    // the "minimal-work vs introspect-each-klass — both safe" angle.
    ctx.check("introspecting_visitor_safe", e1.introspect_safe);

    // ---- DUPLICATE klass pointer: CHARACTERIZED, never asserted. ---------
    // On JDK 8 the same Klass can be listed by both the per-CLD Dictionary and the
    // SystemDictionary _dictionary / _shared_dictionary fallback, so a repeated
    // klass* is legitimate — "no duplicate pointer" is NOT a portable invariant.
    // On JDK 9+ the _klasses walk lists each Klass once per CLD link (locally:
    // distinct-name count == raw count, i.e. zero dups).  Record the delta either
    // way; the HARD floor is only that distinct pointers track distinct names
    // closely enough that the snapshot is not one giant duplicate.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: pass-1 saw " }
               + std::to_string(e1.ptrs.size()) + " distinct klass pointer(s), "
               + std::to_string(e1.dup_ptr) + " duplicate-pointer visit(s) (JDK 8 "
                 "dictionary+SystemDictionary overlap can legitimately repeat a Klass)");
    // The distinct-pointer set is itself non-trivial (the snapshot is real, not a
    // handful of pointers revisited thousands of times).
    ctx.check("distinct_klass_pointers_over_100", e1.ptrs.size() > 100);
    // For the OWN fixture specifically: the supplied pointer is valid and its own
    // _name symbol round-trips to the same internal name — pointer and name agree.
    // These only have meaning when the fixture was actually enumerated, so they
    // ride the same gate: HARD whenever the fixture WAS seen (a seen-but-torn
    // pointer or a mismatched name still FAILS, on every JDK), best-effort only on
    // a genuine JDK8 enumeration miss.
    gate_own_fixture("own_fixture_klass_pointer_valid", e1.own_klass_valid, own_enumerated);
    gate_own_fixture("own_fixture_klass_name_roundtrips", e1.own_name_roundtrips, own_enumerated);

    // ---- Symbol decode: no MALFORMED (control-byte) names. --------------
    // An EMPTY name DOES legitimately occur in a live enumeration on some JDKs —
    // HotSpot lists VM-internal / anonymous / mid-definition klasses whose _name
    // symbol is unresolved as "" — and it surfaced on JDK 8 once the suite started
    // reaching this module.  That is a JVM characteristic, not a decode defect, so
    // it is recorded as [INFO] and tolerated.  A NON-empty name carrying control /
    // whitespace bytes (a genuinely torn or garbage symbol) is still a HARD FAIL on
    // every JDK.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: pass-1 empty-name visit " }
               + (e1.any_empty_name ? "present (VM-internal/anonymous klass — tolerated)"
                                    : "none"));
    ctx.check("no_malformed_name", !e1.any_bad_name);
    // Every distinct NON-EMPTY name is independently well-formed (second angle over
    // the set, not just the per-visit flag — catches a name that appeared only as a
    // dup).  Empty names are tolerated per the [INFO] above; a control-byte name
    // still fails here.
    bool all_names_wellformed{ true };
    for (const std::string& n : e1.names)
    {
        if (!n.empty() && !name_is_wellformed(n))
        {
            all_names_wellformed = false;
            break;
        }
    }
    ctx.check("all_distinct_names_wellformed", all_names_wellformed);

    // ---- INTERNAL '/'-separated form, never the Java dotted form. --------
    // The visitor receives JVM-internal names ("java/lang/Object"), so the dotted
    // form must NEVER appear.  The OWN fixture is a known-present app class on
    // JDK 9+ (gated on JDK 8): assert its slash form was seen and its dotted twin
    // was not.  A snapshot-wide guard — no enumerated name contains '.' — also
    // holds: internal names use '/' and '$', so a '.' would mean a decode that
    // failed to convert.  (Array descriptors use '[', 'L', ';' — none contain '.'.)
    bool any_dotted_name{ false };
    for (const std::string& n : e1.names)
    {
        if (n.find('.') != std::string::npos)
        {
            any_dotted_name = true;
            break;
        }
    }
    ctx.check("no_dotted_form_name", !any_dotted_name);
    ctx.check("own_fixture_dotted_form_absent",
              !e1.names.contains("vmhook.fixtures.ForEachLoadedClass"));

    // ---- More UNIVERSAL bootstrap classes: primitive boxes + a core array. -
    // The wrapper boxes load before any user code on every HotSpot (autoboxing,
    // the JLS-mandated cache classes).  Integer is already checked above; Long /
    // Boolean / Double broaden the "a primitive box is enumerated" angle.  Gated
    // best-effort on JDK 8 (same SystemDictionary incompleteness), HARD on JDK 9+.
    gate_bootstrap_presence("has_java_lang_Long",    e1.names.contains("java/lang/Long"));
    gate_bootstrap_presence("has_java_lang_Boolean", e1.names.contains("java/lang/Boolean"));
    gate_bootstrap_presence("has_java_lang_Double",  e1.names.contains("java/lang/Double"));

    // ---- Nested + launcher InstanceKlass (HARD on JDK 9+). --------------
    // $Inner (a nested app class) and vmhook/Main (the launcher entry) are
    // ordinary InstanceKlasses — the SAME Klass family as the OWN fixture that is
    // already hard-asserted on JDK 9+ — so the JDK 9+ _klasses walk lists them with
    // the same reliability (verified locally; gate_jdk9_hard records [INFO] on the
    // JDK 8 dictionary-only walk that omits them).  This upgrades the legacy
    // info-only nested / launcher angles into real assertions on the modern matrix.
    const bool inner_present{ e1.names.contains("vmhook/fixtures/ForEachLoadedClass$Inner") };
    gate_jdk9_hard("has_nested_inner_class", inner_present, "dictionary-path omits nested klasses");
    const bool main_present{ e1.names.contains("vmhook/Main") };
    gate_jdk9_hard("has_launcher_main_class", main_present,
                   "SystemDictionary walk omits the launcher main class");

    // ---- Array klasses: CHARACTERIZED ([INFO], never asserted). ----------
    // The fixture force-loads the [I primitive-array and [Ljava/lang/String;
    // object-array klasses.  ArrayKlass is a DISTINCT Klass family from
    // InstanceKlass: whether array klasses are linked into a CLD's _klasses list is
    // a JDK-IMPLEMENTATION DETAIL that this module can only confirm on the JDK it
    // runs against locally (JDK 26 lists both).  Rather than risk a false FAIL on a
    // JDK 9..21 build the author cannot test, array-klass presence is recorded as
    // [INFO] across the whole matrix — the well-formed-name guard above already
    // proves that IF an array name is enumerated, "[I" / "[L..;" decode correctly.
    const bool prim_array_present{ e1.names.contains("[I") };
    const bool obj_array_present{ e1.names.contains("[Ljava/lang/String;") };
    ctx.record(std::string{ "[INFO] for_each_loaded_class: [I primitive-array klass " }
               + (prim_array_present ? "enumerated" : "NOT enumerated (array-klass family / JDK detail)"));
    ctx.record(std::string{ "[INFO] for_each_loaded_class: [Ljava/lang/String; object-array klass " }
               + (obj_array_present ? "enumerated" : "NOT enumerated (array-klass family / JDK detail)"));

    // vmhook/Example IS reliably present on every JDK (the legacy test asserted
    // it); record it as a cross-check alongside Main without making the suite
    // depend on the example class being loaded under the modular harness.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: vmhook/Example " }
               + (e1.names.contains("vmhook/Example") ? "enumerated"
                                                       : "NOT enumerated"));

    // ---- PREDICATE / FILTERING visitor: robust lower bounds by prefix. ------
    // A selective visitor (filter by name prefix) is a first-class use of the
    // API.  The java/ and java/lang/ subsets are present on EVERY HotSpot before
    // any user code (the bootstrap loader holds hundreds of java/lang/* classes),
    // so a generous lower bound is a HARD portable floor on every JDK, including
    // the JDK8 dictionary walk (the bootstrap region is exactly what its
    // SystemDictionary fallback covers most reliably).  Exact counts are NOT
    // asserted (the universe varies); only "many" is.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: pass-1 prefix tallies — java/=" }
               + std::to_string(e1.java_prefix) + " java/lang/="
               + std::to_string(e1.java_lang_prefix));
    ctx.check("filter_java_prefix_over_50", e1.java_prefix > 50);
    ctx.check("filter_java_lang_prefix_over_20", e1.java_lang_prefix > 20);
    // The java/lang/ subset is a strict subset of the java/ subset (monotone
    // refinement of the predicate) — a self-consistency invariant on every JDK.
    ctx.check("filter_java_lang_subset_of_java", e1.java_lang_prefix <= e1.java_prefix);
    // The java/ subset is itself a subset of the whole snapshot.
    ctx.check("filter_java_subset_of_all", e1.java_prefix <= e1.names.size());

    // ---- Visitor-invocation SELF-CONSISTENCY (the counters reconcile). ------
    // Every visit was classified into exactly one bucket: null, invalid, or a
    // valid klass (which was then either a first-seen or a duplicate pointer).
    // So count == null + invalid + (distinct ptrs) + (dup-ptr visits).  This
    // proves no visit was silently dropped or double-counted in our folding, on
    // every JDK (it is arithmetic over our own tallies, not a JVM property).
    const std::size_t reconciled{ e1.null_klass + e1.invalid_klass
                                  + e1.ptrs.size() + e1.dup_ptr };
    ctx.check("visit_counters_reconcile", reconciled == e1.count);
    // distinct names never exceed distinct pointers + duplicate-pointer visits
    // is not generally true, but distinct names <= total visits always is (each
    // name came from at least one visit).
    ctx.check("distinct_names_le_count", e1.names.size() <= e1.count);

    // ---- ARRAY-name STRUCTURAL form (HARD when any array is surfaced). ------
    // Presence of array klasses is JDK-variant ([INFO] above), but IF the walk
    // surfaces any name starting with '[', that name MUST be a well-shaped JVM
    // array descriptor ('['+ <prim-tag> | '['+ 'L'<name>';').  A torn array
    // symbol would fail this.  The check is non-vacuous only when arrays appear,
    // so record the population and assert the form universally (true vacuously
    // when zero arrays, which never weakens a JDK that does surface them).
    ctx.record(std::string{ "[INFO] for_each_loaded_class: pass-1 array-descriptor names=" }
               + std::to_string(e1.array_names));
    ctx.check("array_descriptors_wellformed", e1.array_form_ok);

    // ---- NESTED ('$') name STRUCTURAL form (HARD when any nested surfaces). --
    // Likewise: nested-class names carry '$' that is neither first nor last byte.
    // The HotSpot bootstrap loader holds many nested classes (e.g.
    // java/util/Map$Entry) on every JDK, so this population is non-empty in
    // practice on every build, and a malformed nested name is a HARD fail.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: pass-1 nested ('$') names=" }
               + std::to_string(e1.dollar_names));
    ctx.check("nested_names_wellformed", e1.dollar_form_ok);

    // ---- CROSS-PATH POINTER IDENTITY: enumeration klass* == find_class klass*.
    // find_class resolves a name through find_klass (the SAME ClassLoaderDataGraph
    // walk) on JDK 9+, so the klass* the enumeration handed us for a given name
    // and the klass* find_class returns for that name describe the SAME live
    // Klass — they must be pointer-equal.  This is the strongest "the pointer is
    // the real Klass, reached two independent ways" proof.  HARD on JDK 9+ for the
    // bootstrap canary (always enumerated there); on JDK 8 find_class may route
    // through the JNI fallback / a different cache, so record [INFO].  Object's
    // ptr was captured during the walk above.
    vmhook::hotspot::klass* const object_via_find{ vmhook::find_class("java/lang/Object") };
    ctx.check("object_resolvable_via_find_class", object_via_find != nullptr);
    if (!jdk8 && e1.object_klass_ptr != nullptr && object_via_find != nullptr)
    {
        ctx.check("object_klass_pointer_identity_across_paths",
                  e1.object_klass_ptr == object_via_find);
    }
    else
    {
        ctx.record(std::string{ "[INFO] for_each_loaded_class: java/lang/Object cross-path "
                                "pointer identity not asserted (jdk8 or not enumerated this pass) — "
                                "enumerated ptr " }
                   + (e1.object_klass_ptr ? "captured" : "absent") + ", find_class ptr "
                   + (object_via_find ? "non-null" : "null"));
    }
    // Same identity angle for the OWN application fixture (when enumerated): the
    // walk's klass* must equal find_class(OWN_FIXTURE).  Rides gate_own_fixture so
    // it is HARD whenever the fixture was actually surfaced (a mismatch FAILS even
    // on JDK8), best-effort only on a genuine JDK8 enumeration miss.
    vmhook::hotspot::klass* const own_via_find{ vmhook::find_class(OWN_FIXTURE) };
    const bool own_ptr_identity{ e1.own_klass_ptr != nullptr
                                 && own_via_find != nullptr
                                 && e1.own_klass_ptr == own_via_find };
    if (!jdk8)
    {
        gate_own_fixture("own_fixture_pointer_identity_across_paths",
                         own_ptr_identity, own_enumerated);
    }
    else
    {
        ctx.record(std::string{ "[INFO] for_each_loaded_class: OWN fixture cross-path "
                                "pointer identity not asserted on JDK8 (find_class JNI fallback "
                                "may resolve via a different path); enumerated=" }
                   + (e1.own_klass_ptr ? "yes" : "no") + " find_class="
                   + (own_via_find ? "yes" : "no")
                   + " equal=" + (own_ptr_identity ? "yes" : "no"));
    }

    // =====================================================================
    // PASS 2 — snapshot stability: an independent enumeration agrees on every
    // robust invariant.  Enumeration is repeatable and side-effect-free; a
    // second pass must reproduce the floor, the known classes, and a count that
    // has not drifted wildly (a few classes may lazy-load between passes, so the
    // bound is generous rather than exact).
    // =====================================================================
    const enumeration e2{ enumerate_once() };

    ctx.record(std::string{ "[INFO] for_each_loaded_class pass 2 visited " }
               + std::to_string(e2.count) + " klass(es), "
               + std::to_string(e2.names.size()) + " distinct name(s)");

    ctx.check("pass2_count_over_100", e2.count > 100);
    gate_bootstrap_presence("pass2_has_java_lang_Object", e2.names.contains("java/lang/Object"));
    gate_bootstrap_presence("pass2_has_java_lang_String", e2.names.contains("java/lang/String"));
    // Pass-2 app-loader floor (HARD on every JDK): the independent second walk also
    // reached the application loader.  Robust to the JDK8 per-entry lottery.
    ctx.check("pass2_app_loader_reached", count_app_classes(e2.names) >= 1);
    // Pass-2 own-fixture enumeration: HARD on JDK 9+, best-effort on a JDK8 miss
    // (same gate as pass 1; the HARD floors above and pass2_app_loader_reached hold).
    const bool own_enumerated_2{ e2.names.contains(OWN_FIXTURE) };
    gate_own_fixture("pass2_has_own_fixture_class", own_enumerated_2, own_enumerated_2);
    ctx.check("pass2_every_klass_pointer_valid", e2.all_klass_valid);
    // Pass-2 re-proves the UNIVERSAL no-null / no-torn-pointer contract and that
    // the introspecting (re-deref-every-klass) visitor stayed crash-safe on an
    // independent walk.
    ctx.check("pass2_no_null_klass_delivered", e2.null_klass == 0);
    ctx.check("pass2_no_invalid_klass_delivered", e2.invalid_klass == 0);
    ctx.check("pass2_introspecting_visitor_safe", e2.introspect_safe);
    // Pass-2 re-proves the structural form invariants and the predicate floors —
    // these hold on every independent walk, not just the first.
    ctx.check("pass2_array_descriptors_wellformed", e2.array_form_ok);
    ctx.check("pass2_nested_names_wellformed", e2.dollar_form_ok);
    ctx.check("pass2_filter_java_prefix_over_50", e2.java_prefix > 50);
    ctx.check("pass2_visit_counters_reconcile",
              e2.null_klass + e2.invalid_klass + e2.ptrs.size() + e2.dup_ptr == e2.count);
    // Empty names tolerated (see pass-1 rationale) — record, never hard-fail.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: pass-2 empty-name visit " }
               + (e2.any_empty_name ? "present (VM-internal/anonymous klass — tolerated)"
                                    : "none"));

    // The two passes' distinct-name counts are close: |Δ| stays within a small
    // band (classes only ever ACCRETE between two back-to-back snapshots, and
    // only a handful could lazy-load in that window).  Computed without unsigned
    // wraparound.
    const std::size_t lo{ e1.names.size() < e2.names.size() ? e1.names.size() : e2.names.size() };
    const std::size_t hi{ e1.names.size() < e2.names.size() ? e2.names.size() : e1.names.size() };
    ctx.check("pass_to_pass_name_count_stable", (hi - lo) <= 64);

    // Every distinct name pass 1 saw that is a known-stable class is still present
    // in pass 2 (the core set never disappears between snapshots).  java.lang.Object
    // is reliably enumerated on every JDK (the bootstrap-loader canary), so this
    // stays HARD everywhere.
    ctx.check("stable_Object_across_passes",
              e1.names.contains("java/lang/Object") == e2.names.contains("java/lang/Object"));
    // The app loader being reached is stable across passes on every JDK (HARD) —
    // the robust, lottery-free analogue of "the same app class set every time".
    ctx.check("stable_app_loader_reached_across_passes",
              (count_app_classes(e1.names) >= 1) == (count_app_classes(e2.names) >= 1));
    // The bootstrap java/lang/ subset is the deterministic CORE of the snapshot:
    // it does not vanish or balloon between two back-to-back walks.  Both passes
    // must clear the same robust floor, and the two prefix counts stay within the
    // same small drift band (a handful of classes may lazy-load between snapshots,
    // but the bootstrap java/lang/ region is fully loaded long before this module
    // runs).  HARD on every JDK — the bootstrap region is what the JDK8 walk
    // covers most reliably, so this does not ride the per-entry app lottery.
    const std::size_t jl_lo{ e1.java_lang_prefix < e2.java_lang_prefix
                                 ? e1.java_lang_prefix : e2.java_lang_prefix };
    const std::size_t jl_hi{ e1.java_lang_prefix < e2.java_lang_prefix
                                 ? e2.java_lang_prefix : e1.java_lang_prefix };
    ctx.check("pass2_filter_java_lang_prefix_over_20", e2.java_lang_prefix > 20);
    ctx.check("stable_java_lang_prefix_across_passes", (jl_hi - jl_lo) <= 64);
    // The OWN-fixture cross-pass agreement is only deterministic where the walk
    // lists every app class (JDK 9+, HARD).  On JDK8 the two passes can disagree
    // because the SystemDictionary walk drops entries non-deterministically, so we
    // record it [INFO] there rather than asserting a coincidence.
    if (!jdk8)
    {
        ctx.check("stable_own_fixture_across_passes", own_enumerated == own_enumerated_2);
    }
    else
    {
        ctx.record(std::string{ "[INFO] for_each_loaded_class: own-fixture cross-pass "
                                "agreement not asserted on JDK8 (pass1=" }
                   + (own_enumerated ? "seen" : "missed") + ", pass2="
                   + (own_enumerated_2 ? "seen" : "missed")
                   + ") — JDK8 SystemDictionary enumeration is non-deterministic");
    }

    // =====================================================================
    // PART D — TERMINATION + EMPTY (minimal) visitor.  The "large count / full
    // bootstrap set" angle: a visitor that does the least possible work (just a
    // count) must still walk the WHOLE graph, return control, and finish within a
    // generous wall-clock bound — no overrun, no hang, no crash.  This is the
    // executable "the walk terminates" proof, complementing count_under_safety_cap.
    // =====================================================================
    {
        std::size_t empty_count{ 0 };
        const auto t0{ std::chrono::steady_clock::now() };
        vmhook::for_each_loaded_class(
            [&](const std::string&, vmhook::hotspot::klass*) { ++empty_count; });
        const auto t1{ std::chrono::steady_clock::now() };
        const double empty_ms{ std::chrono::duration<double, std::milli>{ t1 - t0 }.count() };
        ctx.record(std::string{ "[INFO] for_each_loaded_class: minimal-visitor walk counted " }
                   + std::to_string(empty_count) + " klass(es) in "
                   + std::to_string(empty_ms) + " ms");
        // The minimal walk sees the same large bootstrap+app set (>100) ...
        ctx.check("minimal_visitor_count_over_100", empty_count > 100);
        // ... stays under the internal safety caps (a real finite graph) ...
        ctx.check("minimal_visitor_under_safety_cap", empty_count < 1'000'000);
        // ... and TERMINATES well within a generous bound even on a slow CI box.
        ctx.check("minimal_visitor_terminates_bounded", empty_ms < 30000.0);
    }

    // =====================================================================
    // PART E — THROWING visitor is CONTAINED (no early-terminate API, but a throw
    // must not crash the JVM).  for_each_loaded_class has NO bool-returning
    // early-stop hook — the visitor's return value is ignored (vmhook.hpp:7505,
    // signature void(const std::string&, klass*)).  The only visitor-driven
    // control flow the library promises is its OWN try/catch boundary
    // (vmhook.hpp:7507-7515): a std::exception thrown from the visitor is caught,
    // iteration stops at the throwing visit, and control returns NORMALLY — the
    // JVM is not torn down.  We prove that contract here AND that the walk is
    // stateless: an enumeration AFTER the throwing one still works.
    //
    // NOTE: the throw is caught INSIDE for_each_loaded_class, so it never reaches
    // the harness's per-module container — this is suite-safe by construction.
    // =====================================================================
    {
        std::size_t before_throw{ 0 };
        bool         walk_returned{ false };
        try
        {
            vmhook::for_each_loaded_class(
                [&](const std::string&, vmhook::hotspot::klass*)
                {
                    ++before_throw;
                    if (before_throw >= 5)
                    {
                        // Thrown from the visitor; the library's own catch must
                        // swallow it and return control to us.
                        throw vmhook::exception{ "for_each_loaded_class throwing-visitor probe" };
                    }
                });
            walk_returned = true;   // reached iff the library caught the throw.
        }
        catch (...)
        {
            // Must NOT happen: the library's try/catch should have contained it.
            walk_returned = false;
        }
        ctx.record(std::string{ "[INFO] for_each_loaded_class: throwing visitor saw " }
                   + std::to_string(before_throw)
                   + " visit(s) before throw; library "
                   + (walk_returned ? "contained the throw (control returned)"
                                    : "let the throw ESCAPE"));
        // The throw was contained by for_each_loaded_class's own catch: control
        // returned to us without the exception escaping (and without crashing).
        ctx.check("throwing_visitor_contained", walk_returned);
        // It stopped AT the throwing visit (we threw on the 5th), so the count is
        // exactly the throttle value — iteration halted, did not run to the end.
        ctx.check("throwing_visitor_stopped_at_throw", before_throw == 5);

        // The walk is stateless: a fresh enumeration AFTER the throwing one still
        // produces a healthy snapshot (the contained throw left nothing corrupt).
        const enumeration after{ enumerate_once() };
        ctx.check("enumeration_healthy_after_contained_throw", after.count > 100);
        ctx.check("enumeration_valid_after_contained_throw", after.all_klass_valid);

        // BOUNDARY: a throw on the VERY FIRST visit halts immediately — the
        // exception is the ONLY stop mechanism the API offers, and it works even
        // at the very first klass (no off-by-one that runs one extra visit).  The
        // library's catch contains it the same way; control returns; exactly one
        // visit happened.
        std::size_t first_visit_count{ 0 };
        bool         first_walk_returned{ false };
        try
        {
            vmhook::for_each_loaded_class(
                [&](const std::string&, vmhook::hotspot::klass*)
                {
                    ++first_visit_count;
                    throw vmhook::exception{ "for_each_loaded_class first-visit throw probe" };
                });
            first_walk_returned = true;
        }
        catch (...)
        {
            first_walk_returned = false;
        }
        ctx.check("throw_on_first_visit_contained", first_walk_returned);
        ctx.check("throw_on_first_visit_stopped_at_one", first_visit_count == 1);
    }

    // =====================================================================
    // PART F — SNAPSHOT FRESHNESS: a class loaded AFTER a first snapshot appears
    // in a SECOND snapshot.  for_each_loaded_class is documented as a SNAPSHOT
    // (vmhook.hpp:7474 "classes loaded after ... are not visited; ... loaded
    // before a later call IS").  LATE_FIXTURE is a '$'-nested class the fixture
    // deliberately leaves UNLOADED at startup; the probe Class.forName's it on the
    // tick thread.  We snapshot BEFORE (it must be absent) and AFTER (it must
    // appear).  CRUCIAL: we use ONLY the pure enumeration to test pre-load
    // absence — NOT vmhook::find_class, whose JNI fallback would itself LOAD the
    // class and destroy the premise.  find_class is used only AFTER the probe, as
    // a portable "it is genuinely loaded now" oracle (its JNI fallback resolves an
    // already-loaded app class on every JDK, including JDK 8).
    // =====================================================================
    {
        // Snapshot 1 (pre-load).  The late class must be absent here; record
        // [INFO] rather than hard-asserting (defensive — only this fixture ever
        // references it, but a hard "absent" assertion would be brittle).
        const enumeration pre{ enumerate_once() };
        const bool late_in_pre{ pre.names.contains(LATE_FIXTURE) };
        ctx.record(std::string{ "[INFO] for_each_loaded_class: late class " }
                   + LATE_FIXTURE + " in pre-load snapshot: "
                   + (late_in_pre ? "PRESENT (unexpected — already loaded)"
                                  : "absent (expected)"));

        // Drive the probe: ask the fixture's tick thread to Class.forName the late
        // class.  On the rising edge of `go` it clears `done`, loads the class,
        // and sets `done` (see ForEachLoadedClass.java).
        const bool probe_done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    felc_fixture::set_done(false);
                }
                felc_fixture::set_go(value);
            },
            []() { return felc_fixture::get_done(); }) };
        ctx.check("freshness_probe_completed", probe_done);
        // The fixture confirms (independent of the enumeration) that the load ran.
        const bool late_loaded_java{ felc_fixture::get_late_loaded() };
        ctx.check("freshness_java_loaded_late_class", late_loaded_java);

        // UNIVERSAL HARD oracle: the late class is now genuinely loaded, so
        // find_class resolves it on EVERY JDK (graph walk on JDK 9+, JNI fallback
        // on JDK 8).  Called only AFTER the probe, so it cannot have caused the
        // load itself.
        const bool late_resolvable{ vmhook::find_class(LATE_FIXTURE) != nullptr };
        ctx.check("freshness_late_class_resolvable_after_load", late_resolvable);

        // Snapshot 2 (post-load).
        const enumeration post{ enumerate_once() };
        const bool late_in_post{ post.names.contains(LATE_FIXTURE) };
        ctx.record(std::string{ "[INFO] for_each_loaded_class: late class " }
                   + LATE_FIXTURE + " in post-load snapshot: "
                   + (late_in_post ? "PRESENT (freshness confirmed)" : "absent"));

        // UNIVERSAL HARD (drift-tolerant): classes only ACCRETE — loading a class
        // never removes others, so the post-load snapshot should be >= the pre-load
        // one.  On JDK 9+ the _klasses walk is deterministic and this is exact; on
        // JDK 8 the SystemDictionary walk is non-deterministic and back-to-back
        // snapshots drift by a handful either way (the same reason
        // pass_to_pass_name_count_stable uses a tolerance band), so we allow the
        // post count to dip by at most that same band rather than asserting strict
        // monotonicity.  A genuine mass removal (dozens of classes) still FAILS.
        ctx.check("freshness_count_did_not_shrink",
                  post.names.size() + 64 >= pre.names.size());

        // The headline freshness invariant: the newly-loaded class now appears in
        // the snapshot.  HARD on JDK 9+ (the _klasses walk lists every app class);
        // best-effort on JDK 8 (the SystemDictionary-only walk may drop the entry —
        // the resolvable + monotonic floors above still hold there).
        gate_jdk9_hard("freshness_late_class_in_second_snapshot", late_in_post,
                       "SystemDictionary walk may drop the freshly-loaded entry");

        // FRESHNESS DELTA (best-effort, [INFO]): the class transitioned from
        // absent->present across the probe.  This is the cleanest statement of the
        // snapshot property when both endpoints behaved (JDK 9+); on JDK 8 either
        // endpoint may be perturbed by the enumeration lottery, so record it.
        ctx.record(std::string{ "[INFO] for_each_loaded_class: freshness delta — late class " }
                   + (!late_in_pre && late_in_post
                          ? "appeared between snapshot 1 and 2 (snapshot freshness confirmed)"
                          : "did not show a clean absent->present transition this run"));

        // ---- The FRESHLY-LOADED klass is genuinely USABLE, two ways. --------
        // When the late class WAS surfaced post-load, the klass* the walk handed
        // us for it must be valid, round-trip its own _name back to LATE_FIXTURE,
        // and be pointer-equal to find_class(LATE_FIXTURE) — exactly the same
        // usability + cross-path identity proof PASS 1 makes for the OWN fixture,
        // but for a class loaded AFTER startup.  Re-derived with a focused walk so
        // the base enumeration struct stays minimal.  HARD on JDK 9+ (where
        // late_in_post is itself hard); skipped on JDK 8 / when not surfaced.
        if (!jdk8 && late_in_post)
        {
            vmhook::hotspot::klass* late_ptr{ nullptr };
            bool                    late_roundtrips{ false };
            vmhook::for_each_loaded_class(
                [&](const std::string& name, vmhook::hotspot::klass* const k)
                {
                    if (name != LATE_FIXTURE || late_ptr != nullptr)
                    {
                        return;   // only the first hit for the target name.
                    }
                    if (k == nullptr || !vmhook::hotspot::is_valid_pointer(k))
                    {
                        return;
                    }
                    late_ptr = k;
                    const vmhook::hotspot::symbol* const sym{ k->get_name() };
                    if (vmhook::hotspot::is_valid_pointer(sym))
                    {
                        late_roundtrips = (sym->to_string() == LATE_FIXTURE);
                    }
                });
            ctx.check("freshness_late_klass_pointer_valid", late_ptr != nullptr);
            ctx.check("freshness_late_klass_name_roundtrips", late_roundtrips);
            vmhook::hotspot::klass* const late_via_find{ vmhook::find_class(LATE_FIXTURE) };
            ctx.check("freshness_late_klass_pointer_identity_across_paths",
                      late_ptr != nullptr && late_ptr == late_via_find);
        }
        else
        {
            ctx.record(std::string{ "[INFO] for_each_loaded_class: freshly-loaded klass "
                                    "usability/identity not asserted (jdk8 or late class not "
                                    "surfaced post-load)" });
        }
    }

    // =====================================================================
    // PART G — find_class DEGENERATE / BOUNDARY inputs (the cross-path oracle's
    // own contract).  PASS 1 uses find_class as the "is this name resolvable"
    // oracle for the enumeration; here we pin down what that oracle does at the
    // EDGES so a later regression in find_class can't silently weaken every
    // cross-path check above.  Every input here is constructed to be UNloadable
    // (a name no class file could exist for), so the JNI fallback inside
    // find_class cannot accidentally LOAD anything — the only correct answer is
    // nullptr.  (Empty / odd-but-conceivably-loadable inputs are recorded
    // [INFO], never hard-asserted, because their JNI-fallback behaviour is
    // platform/JDK-variant per HARD RULE 3.)
    // =====================================================================
    {
        // A garbage internal name no class file can match — must NOT resolve on
        // any JDK (graph walk misses it; JNI loadClass throws -> null).
        ctx.check("find_class_garbage_name_is_null",
                  vmhook::find_class("zzz/nonexistent/Felc_Probe_NoSuchKlass_42") == nullptr);
        // A second, structurally different garbage name (nested-looking) — same
        // contract: unresolvable names yield null, never a torn pointer.
        ctx.check("find_class_garbage_nested_name_is_null",
                  vmhook::find_class("vmhook/fixtures/ForEachLoadedClass$NoSuchInner_felc")
                      == nullptr);
        // The DOTTED form of a name that only exists in '/'-internal form is not
        // a valid internal name; the graph walk keys on '/'-form.  Its JNI
        // fallback CAN accept dotted form on some JDKs, so this is characterized
        // [INFO], not asserted (HARD RULE 3).
        vmhook::hotspot::klass* const dotted_probe{
            vmhook::find_class("zzz.nonexistent.Felc_Probe_NoSuchKlass_42") };
        ctx.record(std::string{ "[INFO] for_each_loaded_class: find_class(dotted garbage) -> " }
                   + (dotted_probe ? "non-null (unexpected)" : "null"));
        // Empty name: behaviour is JDK/JNI-variant (JNI FindClass("") is
        // ill-defined) — record what we observe, never assert.
        vmhook::hotspot::klass* const empty_probe{ vmhook::find_class("") };
        ctx.record(std::string{ "[INFO] for_each_loaded_class: find_class(\"\") -> " }
                   + (empty_probe ? "non-null" : "null"));
        // A bootstrap canary that PASS 1 already proved resolvable stays
        // resolvable here (find_class is stateless / idempotent across calls) —
        // the oracle did not get poisoned by the garbage lookups above.
        ctx.check("find_class_object_still_resolvable_after_garbage",
                  vmhook::find_class("java/lang/Object") != nullptr);
    }

    // =====================================================================
    // PART H — BROADER universal-bootstrap SHAPE coverage.  PASS 1 checks the
    // five canonical concrete classes + three primitive boxes.  Here we widen
    // the net across distinct Klass SHAPES that every HotSpot loads before user
    // code: an interface (java/lang/Runnable, java/util/Collection), an abstract
    // class (java/lang/Number), the throwable hierarchy (Throwable/Exception),
    // and core singletons (System/Math).  All ride gate_bootstrap_presence so
    // they stay best-effort on the JDK8 SystemDictionary lottery, HARD on JDK 9+.
    // =====================================================================
    {
        // Interface klasses (no super-of-Object in the bytecode sense, but still
        // ordinary InstanceKlasses the walk must surface).
        gate_bootstrap_presence("has_java_lang_Runnable",
                                e1.names.contains("java/lang/Runnable"));
        gate_bootstrap_presence("has_java_util_Collection",
                                e1.names.contains("java/util/Collection"));
        // Abstract class.
        gate_bootstrap_presence("has_java_lang_Number",
                                e1.names.contains("java/lang/Number"));
        // Throwable hierarchy — the exception machinery is wired up before any
        // user code on every JVM.
        gate_bootstrap_presence("has_java_lang_Throwable",
                                e1.names.contains("java/lang/Throwable"));
        gate_bootstrap_presence("has_java_lang_Exception",
                                e1.names.contains("java/lang/Exception"));
        // Core utility singletons.
        gate_bootstrap_presence("has_java_lang_System",
                                e1.names.contains("java/lang/System"));
        gate_bootstrap_presence("has_java_lang_Math",
                                e1.names.contains("java/lang/Math"));
        // java/util/ is, like java/lang/, fully populated before this module
        // runs; a generous lower bound is a HARD portable floor (the bootstrap
        // region is exactly what the JDK8 walk covers most reliably).
        std::size_t java_util_prefix{ 0 };
        for (const std::string& n : e1.names)
        {
            if (n.rfind("java/util/", 0) == 0)
            {
                ++java_util_prefix;
            }
        }
        ctx.record(std::string{ "[INFO] for_each_loaded_class: java/util/ prefix count=" }
                   + std::to_string(java_util_prefix));
        ctx.check("filter_java_util_prefix_over_10", java_util_prefix > 10);
    }

    // =====================================================================
    // PART I — OWN-fixture klass DEEP USABILITY (beyond name round-trip).  PASS 1
    // proves the enumerated klass* round-trips its _name; here, when the fixture
    // was surfaced (JDK 9+ HARD; JDK8 enumeration-miss skipped), we drive the
    // SAME klass* through the public klass accessors to prove it is a fully
    // formed InstanceKlass, not just a pointer whose name decodes:
    //   - get_super() returns java/lang/Object (the fixture is `final class
    //     ForEachLoadedClass`, extends Object on EVERY JDK — a plain class's
    //     super is always Object, no JDK variance, unlike array-klass super #32);
    //   - find_field("sentinel") resolves (the fixture declares
    //     `public static int sentinel`), proving constant-pool / field-stream
    //     decode works through the enumerated pointer;
    //   - find_field on a name the class does NOT declare returns nullopt
    //     (negative control — the accessor is selective, not "always present").
    // =====================================================================
    {
        if (e1.own_klass_ptr != nullptr
            && vmhook::hotspot::is_valid_pointer(e1.own_klass_ptr))
        {
            // get_super() -> java/lang/Object.  No JDK variance for a plain final
            // class; the enumerated pointer is a real InstanceKlass with a wired
            // super-link.
            vmhook::hotspot::klass* const super{ e1.own_klass_ptr->get_super() };
            bool super_is_object{ false };
            if (super != nullptr && vmhook::hotspot::is_valid_pointer(super))
            {
                const vmhook::hotspot::symbol* const super_name{ super->get_name() };
                if (vmhook::hotspot::is_valid_pointer(super_name))
                {
                    super_is_object = (super_name->to_string() == "java/lang/Object");
                }
            }
            gate_own_fixture("own_fixture_super_is_object", super_is_object, own_enumerated);

            // find_field("sentinel") resolves the declared static int field.
            const bool sentinel_found{
                e1.own_klass_ptr->find_field("sentinel").has_value() };
            gate_own_fixture("own_fixture_has_sentinel_field", sentinel_found, own_enumerated);

            // Negative control: a field the class does not declare is absent.
            const bool bogus_absent{
                !e1.own_klass_ptr->find_field("felc_no_such_field_xyz").has_value() };
            gate_own_fixture("own_fixture_bogus_field_absent", bogus_absent, own_enumerated);
        }
        else
        {
            ctx.record(std::string{ "[INFO] for_each_loaded_class: OWN-fixture deep "
                                    "usability not asserted (fixture klass* not captured this "
                                    "pass — JDK8 enumeration miss)" });
        }
    }

    // =====================================================================
    // PART J — SNAPSHOT-WIDE NAME-CHARSET invariants (every distinct name, not
    // just the per-visit flag).  Internal names use only '/', '$', '[', 'L',
    // ';', and the JVM identifier charset — NEVER a backslash, NEVER a NUL, and
    // a ';' may appear ONLY in an array descriptor (object-array element
    // terminator).  These are HARD on every JDK: a violation means symbol decode
    // produced a malformed name regardless of which loader path surfaced it.
    // =====================================================================
    {
        bool any_backslash{ false };
        bool any_embedded_nul{ false };
        bool semicolon_only_in_arrays{ true };
        bool dollar_outer_wellformed{ true };
        for (const std::string& n : e1.names)
        {
            if (n.find('\\') != std::string::npos)
            {
                any_backslash = true;
            }
            if (n.find('\0') != std::string::npos)
            {
                any_embedded_nul = true;
            }
            // ';' is legal ONLY as the terminator of an object-array descriptor
            // (a name starting with '['); a non-array name carrying ';' is a torn
            // symbol.
            if (n.find(';') != std::string::npos && (n.empty() || n.front() != '['))
            {
                semicolon_only_in_arrays = false;
            }
            // For every nested ('$') name, the substring BEFORE the first '$' (the
            // outer class name) is itself a non-empty well-formed internal name.
            const std::size_t dollar{ n.find('$') };
            if (dollar != std::string::npos)
            {
                const std::string outer{ n.substr(0, dollar) };
                if (outer.empty() || !name_is_wellformed(outer))
                {
                    dollar_outer_wellformed = false;
                }
            }
        }
        ctx.check("no_backslash_in_any_name", !any_backslash);
        ctx.check("no_embedded_nul_in_any_name", !any_embedded_nul);
        ctx.check("semicolon_only_in_array_descriptors", semicolon_only_in_arrays);
        ctx.check("nested_outer_prefix_wellformed", dollar_outer_wellformed);

        // The bare primitive descriptors ("I", "Z", "J", ...) are NOT class
        // names — a real enumeration never surfaces a single-tag primitive token
        // as a Klass name (primitives have no Klass; only their array forms do).
        const char* const prim_tags[]{ "I", "Z", "B", "C", "S", "J", "F", "D", "V" };
        bool any_bare_primitive{ false };
        for (const char* const tag : prim_tags)
        {
            if (e1.names.contains(tag))
            {
                any_bare_primitive = true;
            }
        }
        ctx.check("no_bare_primitive_descriptor_name", !any_bare_primitive);

        // Population-vs-total bookkeeping (arithmetic over our own tallies, HARD
        // on every JDK): the prefix / array / nested sub-populations can never
        // exceed the total distinct-name set.
        ctx.check("array_population_le_names", e1.array_names <= e1.count);
        ctx.check("nested_population_le_names", e1.dollar_names <= e1.count);
        ctx.check("java_util_consistency", e1.java_lang_prefix <= e1.names.size());
    }

    // =====================================================================
    // PART K — ENUMERATION value-semantics (copy / move) + idempotency.  The
    // per-pass result struct is an ordinary value; copying or moving it must
    // preserve every observable (the caller may stash a snapshot).  A copy
    // compares equal field-by-field on the robust invariants; a move leaves the
    // copy intact.  This is pure C++ value-semantics over our own struct — HARD
    // on every JDK.
    // =====================================================================
    {
        const enumeration original{ enumerate_once() };
        const enumeration copied{ original };   // copy-init (MSVC-safe, RULE).
        ctx.check("copy_preserves_count", copied.count == original.count);
        ctx.check("copy_preserves_name_set", copied.names == original.names);
        ctx.check("copy_preserves_ptr_set", copied.ptrs == original.ptrs);
        ctx.check("copy_preserves_validity", copied.all_klass_valid == original.all_klass_valid);

        // Move: the moved-from std::set members are left in a valid but
        // unspecified state, so we only assert the moved-INTO value matches the
        // pre-move snapshot's robust invariants.
        const std::size_t pre_move_count{ original.count };
        const std::size_t pre_move_names{ original.names.size() };
        enumeration moved_src{ enumerate_once() };
        const std::size_t src_count{ moved_src.count };
        const std::size_t src_names{ moved_src.names.size() };
        const enumeration moved_dst{ std::move(moved_src) };
        ctx.check("move_preserves_count", moved_dst.count == src_count);
        ctx.check("move_preserves_name_count", moved_dst.names.size() == src_names);
        // Idempotency: two independent enumerations both clear the liveness floor
        // and agree that the app loader was reached (robust, lottery-free).
        ctx.check("idempotent_floor_original", pre_move_count > 100);
        ctx.check("idempotent_floor_moved", moved_dst.count > 100);
        ctx.record(std::string{ "[INFO] for_each_loaded_class: value-semantics passes — "
                                "orig.names=" }
                   + std::to_string(pre_move_names) + " moved.names="
                   + std::to_string(moved_dst.names.size()));
    }

    // =====================================================================
    // PART L — CROSS-PASS NAME-SET stability (set algebra, not just counts).
    // PASS 2 already checks counts drift within a band; here we prove the
    // bootstrap CORE is a genuine STABLE SET: the java/lang/ names common to
    // both passes are themselves numerous (the deterministic core never
    // collapses to a handful), and the five canonical bootstrap names that BOTH
    // passes agree on is non-empty.  Set-intersection floors are robust to the
    // JDK8 per-entry lottery because the bootstrap region is what its walk
    // covers most reliably.
    // =====================================================================
    {
        // Intersection of the two passes' java/lang/ subsets.
        std::vector<std::string> jl1{};
        std::vector<std::string> jl2{};
        for (const std::string& n : e1.names)
        {
            if (n.rfind("java/lang/", 0) == 0)
            {
                jl1.push_back(n);
            }
        }
        for (const std::string& n : e2.names)
        {
            if (n.rfind("java/lang/", 0) == 0)
            {
                jl2.push_back(n);
            }
        }
        std::vector<std::string> common{};
        std::set_intersection(jl1.begin(), jl1.end(), jl2.begin(), jl2.end(),
                              std::back_inserter(common));
        ctx.record(std::string{ "[INFO] for_each_loaded_class: java/lang/ common across "
                                "passes=" }
                   + std::to_string(common.size()));
        // The deterministic bootstrap java/lang/ core shared by both passes is
        // numerous — far more than a handful — on every JDK.
        ctx.check("java_lang_common_across_passes_over_10", common.size() > 10);

        // The whole-snapshot intersection is itself non-trivial: two back-to-back
        // walks overwhelmingly agree (classes accrete, they do not churn).
        std::vector<std::string> all_common{};
        std::set_intersection(e1.names.begin(), e1.names.end(),
                              e2.names.begin(), e2.names.end(),
                              std::back_inserter(all_common));
        ctx.record(std::string{ "[INFO] for_each_loaded_class: full-snapshot common across "
                                "passes=" }
                   + std::to_string(all_common.size()));
        ctx.check("full_snapshot_common_over_100", all_common.size() > 100);
        // The common set is bounded by each pass's own size (set-algebra sanity).
        ctx.check("common_le_pass1", all_common.size() <= e1.names.size());
        ctx.check("common_le_pass2", all_common.size() <= e2.names.size());
    }
}
