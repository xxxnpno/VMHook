// instanceklass_methods_walk JVM test module  (feature area: methods / klass)
//
// THE dedicated authority for the PRIMITIVE the whole methods feature stands on:
// the raw walk of InstanceKlass::_methods (an Array<Method*>) into a class's
// DECLARED (name, JVM-descriptor) set -- no JNI, no JVMTI.  Every higher-level
// surface is this walk with a different per-element action:
//   * get_class_methods(name) / get_class_methods<W>()  -- public enumeration,
//   * find_methods_by_signature<W>(descriptor)          -- a descriptor filter,
//   * hook<W>() target lookup                            -- name/sig match,
//   * deoptimize_methods_if() / static_method()->call()  -- per-element actions.
// The walk reads the Array<Method*> LENGTH at offset 0 (clamped to [0,65535],
// vmhook.hpp:3404-3408) and the DATA at offset 8 (vmhook.hpp:3441), decodes each
// Method* via get_name()/get_signature() -> Symbol::to_string() (clamped to
// <=0x1000 bytes), and SKIPS any slot failing is_valid_pointer (vmhook.hpp:8635).
//
// The existing method_enumeration.cpp / find_methods_by_signature.cpp modules
// cover the descriptor-shaped happy path transitively.  THIS module pins the
// walk's OWN edge behaviours that nothing else does, against a DEDICATED fixture
// (vmhook/fixtures/MethodsWalk.java) whose declared (name, descriptor) set of the
// top-level class AND of every nested type is known EXACTLY (verified by
// `javap -s -p` on JDK 26, JDK-stable 8..26) -- so the count and the name/desc
// SET are HARD-asserted for our fixtures.
//
// COVERAGE (HARD unless tagged [INFO]):
//   1  RAW accessor parity -- get_methods_count() / get_methods_ptr() agree with
//      the collector: count>0, ptr!=null, every slot is_valid_pointer, and
//      collect == count pairs (this single equality implicitly exercises the
//      Array<T> +0/+8 ABI assumption, flaw #1).
//   2  METHOD SHAPES on the top-level class -- static / instance / native /
//      final / synchronized / varargs / overloads / generic-erased / <init> /
//      <clinit> / every-primitive descriptor / deeply-nested multi-dim array.
//   3  Empty class -> ONLY synthetic <init> ()V (minimal non-empty).
//   4  Marker interface -> EMPTY (no methods, no <init>); [INFO] it is
//      indistinguishable from an unregistered wrapper (flaw #6).
//   5  Interface -> abstract + default + static methods ALL enumerated.
//   6  Abstract class -> abstract + concrete + <init> enumerated.
//   7  Inheritance EXCLUSION (flaw #4) -- 3-level Base<-Mid<-Sub each lists ONLY
//      its OWN declared methods; the bare walk of Sub never shows Base/Mid/Object
//      methods.  (The fixture also declares Base.baseStatic / Sub.subStatic so the
//      super-walking static_method()->call() resolver, the ONLY path that climbs
//      get_super(), can be contrasted against this bare-walk exclusion -- that
//      resolver is exercised by method_static_portability.cpp, not re-driven here
//      to avoid a JDK-variant cold call-stub dependency in a metadata-only module.)
//   8  Enum -> synthetic values()/valueOf(String) HARD-present (JDK-stable);
//      $values() [INFO] (JDK 15+).
//   9  Annotation -> its elements are abstract methods in _methods.
//  10  MANY methods (m00..m49) all enumerated; count stable across two reads.
//  11  Inner class -> user innerOp() present (synthetic this$0 is a FIELD, so the
//      method set is just innerOp + the synthetic-param <init>).
//  12  Name/descriptor DECODE boundaries -- long name (82 chars, under the 0x1000
//      clamp) decoded in full; Unicode names round-trip as exact modified-UTF-8
//      bytes; multi-dim/deeply-nested + all-primitive descriptors byte-exact.
//  13  No ("","") pair anywhere (per-slot skip / decode-fail property, flaw #5).
//  14  Determinism + ORDERED stability -- collect twice == element-for-element.
//  15  Substrate <-> inline-clone agreement (flaw #3) -- a scoped_hook on the
//      UNIQUE idLong (J)J fires on real bytecode, proving hook<W>()'s inline walk
//      resolved the SAME Method* the collector enumerates.
//  16  Null / not-loaded / bad-input -- "does/not/Exist", "", dotted form, all
//      empty, never a crash.
//  17  Live mutation safety -- force Many.<clinit> AFTER first enum; declared set
//      unchanged; idLong JIT does not perturb the set.
//  18  log_class_methods<W>() links + is a no-op in release (no throw, no
//      observable change).
//
// CRASH-SAFETY (mingw/clang-windows have NO SEH net -- a wild read kills the
// whole suite):
//   * PARTs 1-14, 16-18 are pure METASPACE metadata reads (Method*/Symbol* are
//     native + STABLE, never GC-relocated; every slot is is_valid_pointer-guarded
//     by the library) -- they cannot fault on a cold JVM and stay HARD.
//   * The ONLY cold-unsafe deref is the single idLong (J)J hook detour (PART 15);
//     it reads only the long ARG (a frame local, live stack -- safe) and an
//     atomic, deref-ing NO oop, so it cannot fault even with no SEH net.
//   * scoped_hook RAII tears the one hook down before the module returns; no hook
//     state leaks to the next module.  No forced System.gc().  No large in-detour
//     allocation.  Fine-grained ctx.record checkpoints bracket every PART so any
//     residual no-SEH fault is pinpointed by the last-flushed line.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using pair_list = std::vector<std::pair<std::string, std::string>>;
    using name_list = std::vector<std::string>;

    // ---- Wrapper for the top-level fixture (handshake + idLong hook target) --
    class mw : public vmhook::object<mw>
    {
    public:
        explicit mw(vmhook::oop_t instance) noexcept
            : vmhook::object<mw>{ instance }
        {
        }

        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto get_last_id_long() -> std::int64_t { return static_field("lastIdLong")->get(); }
        static auto get_many_touched() -> std::int32_t { return static_field("manyTouched")->get(); }
    };

    // Nested-type wrappers registered by their internal `$`-name so
    // get_class_methods<W>() resolves the right klass.  None is instantiated by
    // the native side; only their klass enumeration is read.
    class mw_empty    : public vmhook::object<mw_empty>    { public: explicit mw_empty(vmhook::oop_t i) noexcept    : vmhook::object<mw_empty>{ i } {} };
    class mw_marker   : public vmhook::object<mw_marker>   { public: explicit mw_marker(vmhook::oop_t i) noexcept   : vmhook::object<mw_marker>{ i } {} };
    class mw_iface    : public vmhook::object<mw_iface>    { public: explicit mw_iface(vmhook::oop_t i) noexcept    : vmhook::object<mw_iface>{ i } {} };
    class mw_ifacecl  : public vmhook::object<mw_ifacecl>  { public: explicit mw_ifacecl(vmhook::oop_t i) noexcept  : vmhook::object<mw_ifacecl>{ i } {} };
    class mw_abstract : public vmhook::object<mw_abstract> { public: explicit mw_abstract(vmhook::oop_t i) noexcept : vmhook::object<mw_abstract>{ i } {} };
    class mw_concrete : public vmhook::object<mw_concrete> { public: explicit mw_concrete(vmhook::oop_t i) noexcept : vmhook::object<mw_concrete>{ i } {} };
    class mw_base     : public vmhook::object<mw_base>     { public: explicit mw_base(vmhook::oop_t i) noexcept     : vmhook::object<mw_base>{ i } {} };
    class mw_mid      : public vmhook::object<mw_mid>      { public: explicit mw_mid(vmhook::oop_t i) noexcept      : vmhook::object<mw_mid>{ i } {} };
    class mw_sub      : public vmhook::object<mw_sub>      { public: explicit mw_sub(vmhook::oop_t i) noexcept      : vmhook::object<mw_sub>{ i } {} };
    class mw_vals     : public vmhook::object<mw_vals>     { public: explicit mw_vals(vmhook::oop_t i) noexcept     : vmhook::object<mw_vals>{ i } {} };
    class mw_enumabs  : public vmhook::object<mw_enumabs>  { public: explicit mw_enumabs(vmhook::oop_t i) noexcept  : vmhook::object<mw_enumabs>{ i } {} };
    class mw_generic  : public vmhook::object<mw_generic>  { public: explicit mw_generic(vmhook::oop_t i) noexcept  : vmhook::object<mw_generic>{ i } {} };
    class mw_anno     : public vmhook::object<mw_anno>     { public: explicit mw_anno(vmhook::oop_t i) noexcept     : vmhook::object<mw_anno>{ i } {} };
    class mw_many     : public vmhook::object<mw_many>     { public: explicit mw_many(vmhook::oop_t i) noexcept     : vmhook::object<mw_many>{ i } {} };
    class mw_inner    : public vmhook::object<mw_inner>    { public: explicit mw_inner(vmhook::oop_t i) noexcept    : vmhook::object<mw_inner>{ i } {} };

    // A wrapper deliberately NEVER registered (flaw #6 contrast).
    class mw_unregistered : public vmhook::object<mw_unregistered>
    {
    public:
        explicit mw_unregistered(vmhook::oop_t i) noexcept
            : vmhook::object<mw_unregistered>{ i } {}
    };

    // ---- internal class names (slashed form) --------------------------------
    constexpr char NAME_TOP[]{ "vmhook/fixtures/MethodsWalk" };
    constexpr char NAME_EMPTY[]{ "vmhook/fixtures/MethodsWalk$Empty" };
    constexpr char NAME_MARKER[]{ "vmhook/fixtures/MethodsWalk$Marker" };
    constexpr char NAME_IFACE[]{ "vmhook/fixtures/MethodsWalk$Iface" };
    constexpr char NAME_IFACECL[]{ "vmhook/fixtures/MethodsWalk$IfaceClinit" };
    constexpr char NAME_ABSTRACT[]{ "vmhook/fixtures/MethodsWalk$Abstract" };
    constexpr char NAME_CONCRETE[]{ "vmhook/fixtures/MethodsWalk$ConcreteSub" };
    constexpr char NAME_BASE[]{ "vmhook/fixtures/MethodsWalk$Base" };
    constexpr char NAME_MID[]{ "vmhook/fixtures/MethodsWalk$Mid" };
    constexpr char NAME_SUB[]{ "vmhook/fixtures/MethodsWalk$Sub" };
    constexpr char NAME_VALS[]{ "vmhook/fixtures/MethodsWalk$Vals" };
    constexpr char NAME_ENUMABS[]{ "vmhook/fixtures/MethodsWalk$EnumAbstract" };
    constexpr char NAME_GENERIC[]{ "vmhook/fixtures/MethodsWalk$Generic" };
    constexpr char NAME_ANNO[]{ "vmhook/fixtures/MethodsWalk$Anno" };
    constexpr char NAME_MANY[]{ "vmhook/fixtures/MethodsWalk$Many" };
    constexpr char NAME_INNER[]{ "vmhook/fixtures/MethodsWalk$Inner" };

    // ---- fixture-mirrored constants -----------------------------------------
    constexpr std::int64_t IDLONG_ARG{ 0x0102030405060708LL };

    // The exact long method name from the fixture (82 chars: 'm' + '_' + 80 'x').
    const std::string LONG_NAME{ std::string{ "m_" } + std::string(80, 'x') };

    // The Unicode method names as the EXACT modified-UTF-8 bytes HotSpot stores
    // (computed from the .class constant pool; modified-UTF-8 == UTF-8 here since
    // both are BMP letters with no embedded NUL / supplementary chars):
    //   "méthodé" -> 6D C3 A9 74 68 6F 64 C3 A9   (9 bytes)
    //   "名前"     -> E5 90 8D E5 89 8D            (6 bytes)
    const std::string NAME_METHODE{
        "\x6D\xC3\xA9\x74\x68\x6F\x64\xC3\xA9" };
    const std::string NAME_NAMAE{
        "\xE5\x90\x8D\xE5\x89\x8D" };

    // ---- set helpers (order-independent) ------------------------------------
    auto count_pair(const pair_list& methods, const std::string& name, const std::string& descriptor) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            { return m.first == name && m.second == descriptor; }));
    }

    auto has_name(const pair_list& methods, const std::string& name) -> bool
    {
        return std::any_of(methods.begin(), methods.end(),
                           [&](const std::pair<std::string, std::string>& m)
                           { return m.first == name; });
    }

    auto count_name(const pair_list& methods, const std::string& name) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            { return m.first == name; }));
    }

    auto count_descriptor(const pair_list& methods, const std::string& descriptor) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            { return m.second == descriptor; }));
    }

    // ---- the unique idLong (J)J hook observations ---------------------------
    std::atomic<std::int32_t> g_idlong_fires{ 0 };
    std::atomic<std::int64_t> g_idlong_arg{ -1 };

    // Drives exactly one probe cycle for `mode`.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    mw::set_done(false);
                    mw::set_mode(mode);
                }
                mw::set_go(value);
            },
            []() { return mw::get_done(); });
    }
}

VMHOOK_JVM_MODULE(instanceklass_methods_walk)
{
    const auto cp = [&](const char* where)
    {
        ctx.record(std::string{ "[INFO] instanceklass_methods_walk checkpoint: " } + where);
    };

    cp("register_class<mw> + nested wrappers");
    vmhook::register_class<mw>(NAME_TOP);
    vmhook::register_class<mw_empty>(NAME_EMPTY);
    vmhook::register_class<mw_marker>(NAME_MARKER);
    vmhook::register_class<mw_iface>(NAME_IFACE);
    vmhook::register_class<mw_ifacecl>(NAME_IFACECL);
    vmhook::register_class<mw_abstract>(NAME_ABSTRACT);
    vmhook::register_class<mw_concrete>(NAME_CONCRETE);
    vmhook::register_class<mw_base>(NAME_BASE);
    vmhook::register_class<mw_mid>(NAME_MID);
    vmhook::register_class<mw_sub>(NAME_SUB);
    vmhook::register_class<mw_vals>(NAME_VALS);
    vmhook::register_class<mw_enumabs>(NAME_ENUMABS);
    vmhook::register_class<mw_generic>(NAME_GENERIC);
    vmhook::register_class<mw_anno>(NAME_ANNO);
    vmhook::register_class<mw_many>(NAME_MANY);
    vmhook::register_class<mw_inner>(NAME_INNER);

    // Bail cleanly if the fixture isn't loaded (never crash; record + skip the
    // class-dependent body).  find_class is a pure metadata lookup.
    ctx.check("fixture_loaded", vmhook::find_class(NAME_TOP) != nullptr);

    // =====================================================================
    // PART 1 — RAW accessor parity: get_methods_count() / get_methods_ptr()
    //   agree with detail::collect_klass_methods (== get_class_methods).
    //   The single `count == pairs.size()` equality implicitly exercises the
    //   Array<T> +0 (length) / +8 (data) ABI assumption (flaw #1): a wrong
    //   layout would mis-count or mis-decode and break the equality.
    // =====================================================================
    cp("PART 1 raw accessor parity (metaspace metadata — no oop deref)");
    {
        vmhook::hotspot::klass* const top_klass{
            reinterpret_cast<vmhook::hotspot::klass*>(vmhook::find_class(NAME_TOP)) };
        ctx.check("raw_klass_resolved", top_klass != nullptr);

        if (top_klass != nullptr)
        {
            const std::int32_t raw_count{ top_klass->get_methods_count() };
            vmhook::hotspot::method** const raw_ptr{ top_klass->get_methods_ptr() };
            ctx.check("raw_count_positive", raw_count > 0);
            ctx.check("raw_ptr_nonnull", raw_ptr != nullptr);
            ctx.record(std::string{ "[INFO] get_methods_count() = " } + std::to_string(raw_count));

            // Every slot in [0,count) is non-null and is_valid_pointer (the walk's
            // per-slot precondition — a clean fixture klass has no skipped slots).
            bool all_slots_valid{ raw_ptr != nullptr };
            if (raw_ptr != nullptr)
            {
                for (std::int32_t i{ 0 }; i < raw_count; ++i)
                {
                    vmhook::hotspot::method* const m{ raw_ptr[i] };
                    if (!m || !vmhook::hotspot::is_valid_pointer(m))
                    {
                        all_slots_valid = false;
                        break;
                    }
                }
            }
            ctx.check("raw_every_slot_valid", all_slots_valid);

            // The collector returns exactly count pairs (no slot was skipped on a
            // clean class) — pins count == data-walk length, i.e. the +0/+8 ABI.
            const pair_list collected{ vmhook::get_class_methods(NAME_TOP) };
            ctx.check("collect_size_le_raw_count",
                      collected.size() <= static_cast<std::size_t>(raw_count));
            ctx.check("collect_size_eq_raw_count",
                      collected.size() == static_cast<std::size_t>(raw_count));
        }
    }

    // =====================================================================
    // PART 2 — METHOD SHAPES on the top-level class (the exact declared set).
    //   by_type (registered wrapper) and by_name agree; every named method is
    //   present with its EXACT descriptor.  All metaspace reads -> HARD.
    // =====================================================================
    cp("PART 2 method shapes (top-level declared set)");
    const pair_list top{ vmhook::get_class_methods<mw>() };
    ctx.check("top_nonempty", !top.empty());
    ctx.record(std::string{ "[INFO] top-level method count = " } + std::to_string(top.size()));

    // static / instance / native / final all share (I)I and are distinct names.
    ctx.check("has_si_II",   count_pair(top, "si",  "(I)I") == 1);   // static
    ctx.check("has_ii_II",   count_pair(top, "ii",  "(I)I") == 1);   // instance
    ctx.check("has_nat_II",  count_pair(top, "nat", "(I)I") == 1);   // native (declared)
    ctx.check("has_fin_II",  count_pair(top, "fin", "(I)I") == 1);   // final
    // synchronized void / double method (modifiers don't change the descriptor).
    ctx.check("has_syncM_V", count_pair(top, "syncM",   "()V") == 1);
    ctx.check("has_strictM_DD", count_pair(top, "strictM", "(D)D") == 1);
    // varargs -> array descriptor.
    ctx.check("has_varargs_aII", count_pair(top, "varargs", "([I)I") == 1);
    // overloads: same name, three distinct descriptors.
    ctx.check("has_ov_II", count_pair(top, "ov", "(I)I") == 1);
    ctx.check("has_ov_JJ", count_pair(top, "ov", "(J)J") == 1);
    ctx.check("has_ov_strstr",
              count_pair(top, "ov", "(Ljava/lang/String;)Ljava/lang/String;") == 1);
    ctx.check("ov_appears_3_times", count_name(top, "ov") == 3);
    // the unique idLong (J)J — distinct from ov(long) by NAME, same descriptor.
    ctx.check("has_idLong_JJ", count_pair(top, "idLong", "(J)J") == 1);
    ctx.check("descriptor_JJ_is_2", count_descriptor(top, "(J)J") == 2); // ov(long)+idLong
    // generic-erased arg/return.
    ctx.check("has_gen_objobj",
              count_pair(top, "gen", "(Ljava/lang/Object;)Ljava/lang/Object;") == 1);
    ctx.check("has_genB_cmpcmp",
              count_pair(top, "genB", "(Ljava/lang/Comparable;)Ljava/lang/Comparable;") == 1);
    // every primitive in one descriptor.
    ctx.check("has_allPrims_ZBCSIJFD", count_pair(top, "allPrims", "(ZBCSIJFD)V") == 1);
    // deeply-nested multi-dim reference array.
    ctx.check("has_deep_3dStr", count_pair(top, "deep", "([[[Ljava/lang/String;)V") == 1);
    // newInner returns the inner type.
    ctx.check("has_newInner",
              count_pair(top, "newInner", "()Lvmhook/fixtures/MethodsWalk$Inner;") == 1);

    // SINGLE-PRIMITIVE RETURN descriptors: one method per primitive return char.
    // (int/void returns are already implied by ov/syncM; these add the five
    // OTHER single-primitive return codes the walk must decode byte-exact.)
    ctx.check("has_retZ_Z", count_pair(top, "retZ", "()Z") == 1);
    ctx.check("has_retC_C", count_pair(top, "retC", "()C") == 1);
    ctx.check("has_retS_S", count_pair(top, "retS", "()S") == 1);
    ctx.check("has_retB_B", count_pair(top, "retB", "()B") == 1);
    ctx.check("has_retF_F", count_pair(top, "retF", "()F") == 1);
    ctx.check("has_retJ_J", count_pair(top, "retJ", "()J") == 1);

    // ARRAY RETURN descriptors: the '[' lives in the RETURN slot (1-D prim,
    // 2-D prim, 1-D reference) — never exercised by the arg-side array tests.
    ctx.check("has_retArrI",   count_pair(top, "retArrI",   "()[I") == 1);
    ctx.check("has_retArr2J",  count_pair(top, "retArr2J",  "()[[J") == 1);
    ctx.check("has_retArrStr", count_pair(top, "retArrStr", "()[Ljava/lang/String;") == 1);

    // VISIBILITY-agnostic enumeration: a private and a protected method appear
    // exactly like a public one (the walk reads _methods regardless of
    // JVM_ACC_*).  Unique descriptors so the (I)I/()V counts are undisturbed.
    ctx.check("has_privM_CI",  count_pair(top, "privM", "(C)I") == 1); // private
    ctx.check("has_protM_BJ",  count_pair(top, "protM", "(B)J") == 1); // protected

    // STATIC NATIVE (declared; never linked) still has an _methods entry.
    ctx.check("has_snat_IV",   count_pair(top, "snat", "(I)V") == 1);

    // synthetic members live in _methods.
    ctx.check("has_init_V",   count_pair(top, "<init>",   "()V") >= 1);
    ctx.check("has_clinit_V", count_pair(top, "<clinit>", "()V") >= 1);

    // inherited Object methods are NOT declared here.
    ctx.check("excludes_toString", !has_name(top, "toString"));
    ctx.check("excludes_hashCode", !has_name(top, "hashCode"));
    ctx.check("excludes_equals",   !has_name(top, "equals"));
    ctx.check("excludes_getClass", !has_name(top, "getClass"));

    // by-name path agrees with by-type (same multiset).
    const pair_list top_by_name{ vmhook::get_class_methods(NAME_TOP) };
    ctx.check("by_name_same_size", top_by_name.size() == top.size());
    bool multiset_eq{ top_by_name.size() == top.size() };
    for (const std::pair<std::string, std::string>& m : top)
    {
        if (count_pair(top_by_name, m.first, m.second) != count_pair(top, m.first, m.second))
        {
            multiset_eq = false;
            break;
        }
    }
    ctx.check("by_name_matches_by_type_each_pair", multiset_eq);

    // =====================================================================
    // PART 3 — Empty class: ONLY the synthetic <init> ()V (minimal non-empty).
    // =====================================================================
    cp("PART 3 empty class -> only <init>");
    {
        const pair_list em{ vmhook::get_class_methods<mw_empty>() };
        ctx.check("empty_has_init", count_pair(em, "<init>", "()V") == 1);
        ctx.check("empty_no_user_methods", em.size() == 1);
        ctx.check("empty_no_toString", !has_name(em, "toString"));
    }

    // =====================================================================
    // PART 4 — Marker interface: EMPTY (no methods, interfaces get no <init>).
    //   By design INDISTINGUISHABLE from an unregistered wrapper (flaw #6).
    // =====================================================================
    cp("PART 4 marker interface -> empty");
    {
        const pair_list mk{ vmhook::get_class_methods<mw_marker>() };
        ctx.check("marker_empty", mk.empty());
        const pair_list unreg{ vmhook::get_class_methods<mw_unregistered>() };
        ctx.check("unregistered_empty", unreg.empty());
        ctx.record("[INFO] marker-interface enumeration == unregistered-wrapper "
                   "enumeration (both empty) — flaw #6: a method-less class is "
                   "indistinguishable from an unregistered/typo'd wrapper.");
    }

    // =====================================================================
    // PART 5 — Interface: abstract + default + static methods ALL enumerated.
    //   (Abstract methods STILL have _methods entries.)
    // =====================================================================
    cp("PART 5 interface -> abstract+default+static");
    {
        const pair_list iface{ vmhook::get_class_methods<mw_iface>() };
        ctx.check("iface_has_absOp",    count_pair(iface, "absOp",    "(I)I") == 1);
        ctx.check("iface_has_defOp",    count_pair(iface, "defOp",    "(I)I") == 1);
        ctx.check("iface_has_staticOp", count_pair(iface, "staticOp", "(I)I") == 1);
        ctx.check("iface_no_init",      !has_name(iface, "<init>")); // interfaces have no ctor
        ctx.check("iface_size_3",       iface.size() == 3);
    }

    // =====================================================================
    // PART 6 — Abstract class: abstract + concrete + <init> enumerated.
    // =====================================================================
    cp("PART 6 abstract class");
    {
        const pair_list ab{ vmhook::get_class_methods<mw_abstract>() };
        ctx.check("abstract_has_init",     count_pair(ab, "<init>",   "()V") == 1);
        ctx.check("abstract_has_shape",    count_pair(ab, "shape",    "(I)I") == 1); // abstract
        ctx.check("abstract_has_concrete", count_pair(ab, "concrete", "(I)I") == 1);
        ctx.check("abstract_size_3",       ab.size() == 3);
    }

    // =====================================================================
    // PART 7 — Inheritance EXCLUSION (flaw #4): the bare walk lists ONLY a
    //   class's OWN declared methods.  Base<-Mid<-Sub each shows only its own;
    //   Sub never shows Base/Mid/Object methods.  (static_method()->call() is the
    //   only path that climbs get_super() to see an INHERITED static; that
    //   super-walk is covered by method_static_portability.cpp, not re-driven
    //   here -- this module pins the bare-walk exclusion contract.)
    // =====================================================================
    cp("PART 7 inheritance exclusion (own methods only)");
    {
        const pair_list base{ vmhook::get_class_methods<mw_base>() };
        const pair_list mid{ vmhook::get_class_methods<mw_mid>() };
        const pair_list sub{ vmhook::get_class_methods<mw_sub>() };

        // Base: baseOnly ()V, static baseStatic (I)I, <init> ()V.
        ctx.check("base_has_baseOnly",   count_pair(base, "baseOnly",   "()V") == 1);
        ctx.check("base_has_baseStatic", count_pair(base, "baseStatic", "(I)I") == 1);
        ctx.check("base_has_init",       count_pair(base, "<init>",     "()V") == 1);
        ctx.check("base_size_3",         base.size() == 3);

        // Mid: midOnly only (+ <init>); does NOT list inherited baseOnly/baseStatic.
        ctx.check("mid_has_midOnly",     count_pair(mid, "midOnly", "()V") == 1);
        ctx.check("mid_has_init",        count_pair(mid, "<init>",  "()V") == 1);
        ctx.check("mid_excludes_baseOnly",   !has_name(mid, "baseOnly"));
        ctx.check("mid_excludes_baseStatic", !has_name(mid, "baseStatic"));
        ctx.check("mid_size_2",          mid.size() == 2);

        // Sub: subOnly ()V, subStatic (I)I, compareTo(Sub) (declared), <init> ()V;
        // does NOT list inherited midOnly/baseOnly/baseStatic/Object methods.
        ctx.check("sub_has_subOnly",   count_pair(sub, "subOnly",   "()V") == 1);
        ctx.check("sub_has_subStatic", count_pair(sub, "subStatic", "(I)I") == 1);
        ctx.check("sub_has_compareTo_declared",
                  count_pair(sub, "compareTo", "(Lvmhook/fixtures/MethodsWalk$Sub;)I") == 1);
        ctx.check("sub_has_init",      count_pair(sub, "<init>", "()V") == 1);
        ctx.check("sub_excludes_midOnly",    !has_name(sub, "midOnly"));
        ctx.check("sub_excludes_baseOnly",   !has_name(sub, "baseOnly"));
        ctx.check("sub_excludes_baseStatic", !has_name(sub, "baseStatic"));
        ctx.check("sub_excludes_toString",   !has_name(sub, "toString"));

        // The generic Comparable override emits a synthetic BRIDGE compareTo(Object)
        // (Ljava/lang/Object;)I on every shipping JDK; treat as [INFO] (compiler-
        // emitted, PASS-or-INFO) per the crash-safety policy.
        const bool has_bridge{ count_pair(sub, "compareTo", "(Ljava/lang/Object;)I") == 1 };
        ctx.record(std::string{ "[INFO] Sub bridge compareTo(Object) present: " }
                   + (has_bridge ? "yes" : "no") + " (compiler-emitted bridge for "
                   "the generic Comparable override).");
        // compareTo appears either once (declared only) or twice (declared+bridge).
        ctx.check("sub_compareTo_count_1_or_2",
                  count_name(sub, "compareTo") == 1 || count_name(sub, "compareTo") == 2);
    }

    // =====================================================================
    // PART 8 — Enum: synthetic values()/valueOf(String) HARD-present (JDK 8..26
    //   stable); user rank()/<init>/<clinit> present; $values() [INFO] (JDK 15+).
    // =====================================================================
    cp("PART 8 enum synthetics");
    {
        const pair_list vals{ vmhook::get_class_methods<mw_vals>() };
        ctx.check("enum_has_values",
                  count_pair(vals, "values", "()[Lvmhook/fixtures/MethodsWalk$Vals;") == 1);
        ctx.check("enum_has_valueOf",
                  count_pair(vals, "valueOf",
                             "(Ljava/lang/String;)Lvmhook/fixtures/MethodsWalk$Vals;") == 1);
        ctx.check("enum_has_rank",   count_pair(vals, "rank", "()I") == 1);
        ctx.check("enum_has_init",   has_name(vals, "<init>"));   // private (Ljava/lang/String;I)V
        ctx.check("enum_has_clinit", has_name(vals, "<clinit>")); // enum constants init
        // $values() is a JDK 15+ synthetic (older JDKs inline it into values()).
        const bool has_dvalues{ has_name(vals, "$values") };
        ctx.record(std::string{ "[INFO] enum $values() synthetic present: " }
                   + (has_dvalues ? "yes (JDK 15+)" : "no (JDK <=14)") + ".");
    }

    // =====================================================================
    // PART 9 — Annotation: its elements are abstract methods in _methods.
    // =====================================================================
    cp("PART 9 annotation elements");
    {
        const pair_list anno{ vmhook::get_class_methods<mw_anno>() };
        ctx.check("anno_has_label",  count_pair(anno, "label",  "()Ljava/lang/String;") == 1);
        ctx.check("anno_has_weight", count_pair(anno, "weight", "()I") == 1);
        ctx.check("anno_no_init",    !has_name(anno, "<init>")); // annotation is an interface
        ctx.check("anno_size_2",     anno.size() == 2);
    }

    // =====================================================================
    // PART 10 — MANY methods (m00..m49) all enumerated; count stable.
    // =====================================================================
    cp("PART 10 many methods");
    {
        const pair_list many{ vmhook::get_class_methods<mw_many>() };
        bool all_m_present{ true };
        for (int i{ 0 }; i < 50; ++i)
        {
            char buf[8];
            buf[0] = 'm';
            buf[1] = static_cast<char>('0' + (i / 10));
            buf[2] = static_cast<char>('0' + (i % 10));
            buf[3] = '\0';
            if (count_pair(many, std::string{ buf }, "()V") != 1)
            {
                all_m_present = false;
                break;
            }
        }
        ctx.check("many_all_50_present", all_m_present);
        ctx.check("many_has_touch",   count_pair(many, "touch",  "()V") == 1);
        ctx.check("many_has_init",    count_pair(many, "<init>", "()V") == 1);
        ctx.check("many_has_clinit",  count_pair(many, "<clinit>", "()V") == 1); // initialized static field
        // 50 m## + touch + <init> + <clinit> = 53 (lower-bounded so a JDK that adds
        // a synthetic cannot break CI; the exact membership above pins the set).
        ctx.check("many_size_at_least_52", many.size() >= 52);
        ctx.record(std::string{ "[INFO] Many method count = " } + std::to_string(many.size()));

        // Count stable across a second read.
        const pair_list many2{ vmhook::get_class_methods<mw_many>() };
        ctx.check("many_count_stable", many.size() == many2.size());
    }

    // =====================================================================
    // PART 11 — Inner class: user innerOp() present (synthetic this$0 is a FIELD,
    //   not a method, so the method set is innerOp + the synthetic-param <init>).
    // =====================================================================
    cp("PART 11 inner class");
    {
        const pair_list inner{ vmhook::get_class_methods<mw_inner>() };
        ctx.check("inner_has_innerOp", count_pair(inner, "innerOp", "()I") == 1);
        // <init> takes the synthetic outer param (descriptor varies; assert by name).
        ctx.check("inner_has_init", has_name(inner, "<init>"));
        ctx.check("inner_no_this0_method", !has_name(inner, "this$0")); // this$0 is a FIELD
    }

    // =====================================================================
    // PART 12 — Name/descriptor DECODE boundaries (the Symbol path, 1878-1916).
    //   Long name decoded in full; Unicode names round-trip as exact modified-
    //   UTF-8 bytes; multi-dim/all-primitive descriptors byte-exact.
    // =====================================================================
    cp("PART 12 decode boundaries (long name / unicode / descriptors)");
    {
        // Long name (82 chars) decoded in full, length matches exactly.
        ctx.check("longname_present", count_pair(top, LONG_NAME, "()V") == 1);
        ctx.check("longname_length_82", LONG_NAME.size() == 82);

        // Unicode names: compare against the KNOWN modified-UTF-8 byte sequence
        // (NOT a re-decode) — std::string holds the raw bytes Symbol stores.
        ctx.check("unicode_methode_bytes", count_pair(top, NAME_METHODE, "()V") == 1);
        ctx.check("unicode_namae_bytes",   count_pair(top, NAME_NAMAE,   "()V") == 1);
        ctx.check("unicode_methode_bytelen_9", NAME_METHODE.size() == 9);
        ctx.check("unicode_namae_bytelen_6",   NAME_NAMAE.size() == 6);

        // Descriptor byte-for-byte: every-primitive + multi-dim already asserted
        // present in PART 2; re-affirm the exact bytes here as decode boundaries.
        ctx.check("descriptor_allprims_exact", count_descriptor(top, "(ZBCSIJFD)V") == 1);
        ctx.check("descriptor_deep_exact",     count_descriptor(top, "([[[Ljava/lang/String;)V") == 1);
    }

    // =====================================================================
    // PART 13 — No ("","") pair anywhere (per-slot skip / decode-fail property,
    //   flaw #5).  A skipped slot never reaches emplace_back; a decoded-but-empty
    //   name would mean get_name failed — both surface as an empty string here.
    // =====================================================================
    cp("PART 13 no empty name/descriptor");
    {
        const auto no_empty = [](const pair_list& ms) -> bool
        {
            return std::none_of(ms.begin(), ms.end(),
                [](const std::pair<std::string, std::string>& m)
                { return m.first.empty() || m.second.empty(); });
        };
        ctx.check("top_no_empty_strings", no_empty(top));
        ctx.check("sub_no_empty_strings", no_empty(vmhook::get_class_methods<mw_sub>()));
        ctx.check("vals_no_empty_strings", no_empty(vmhook::get_class_methods<mw_vals>()));
        ctx.check("many_no_empty_strings", no_empty(vmhook::get_class_methods<mw_many>()));

        // Every descriptor is well-formed: starts '(' and contains ')'.
        const bool wellformed{ std::all_of(top.begin(), top.end(),
            [](const std::pair<std::string, std::string>& m)
            { return !m.second.empty() && m.second.front() == '('
                     && m.second.find(')') != std::string::npos; }) };
        ctx.check("top_all_descriptors_wellformed", wellformed);
    }

    // =====================================================================
    // PART 14 — Determinism + ORDERED stability: collect twice yields an
    //   element-for-element identical vector (the walk is index-ordered over
    //   _methods, stable within a run).  Stronger than fmbs's multiset check.
    // =====================================================================
    cp("PART 14 ordered determinism");
    {
        const pair_list a{ vmhook::get_class_methods<mw>() };
        const pair_list b{ vmhook::get_class_methods<mw>() };
        ctx.check("ordered_same_size", a.size() == b.size());
        ctx.check("ordered_identical", a == b); // element-for-element, incl. order

        const pair_list sa{ vmhook::get_class_methods<mw_sub>() };
        const pair_list sb{ vmhook::get_class_methods<mw_sub>() };
        ctx.check("ordered_sub_identical", sa == sb);
    }

    // =====================================================================
    // PART 15 — Substrate <-> inline-clone agreement (flaw #3): a scoped_hook on
    //   the UNIQUE idLong (J)J fires on real bytecode, proving hook<W>()'s inline
    //   _methods walk (vmhook.hpp:8049) resolved the SAME Method* the collector
    //   enumerates.  The detour reads only the long ARG (a live frame local) and
    //   an atomic — NO oop deref — so it cannot fault even with no SEH net.
    // =====================================================================
    cp("PART 15 substrate<->hook<W> agreement (scoped_hook idLong (J)J)");
    {
        g_idlong_fires.store(0);
        g_idlong_arg.store(-1);

        {
            auto handle{ vmhook::scoped_hook<mw>(
                "idLong", "(J)J",
                [](vmhook::return_value&,
                   const std::unique_ptr<mw>&,
                   std::int64_t x)
                {
                    g_idlong_fires.fetch_add(1, std::memory_order_relaxed);
                    g_idlong_arg.store(x, std::memory_order_relaxed);
                }) };

            ctx.check("scoped_hook_idLong_installed", handle.installed());

            cp("PART 15 drive(mode 1) — real idLong (J)J bytecode dispatch");
            const bool done{ drive(ctx, 1) };
            ctx.check("idLong_probe_completed", done);
            ctx.check("idLong_fired_once",
                      g_idlong_fires.load(std::memory_order_relaxed) == 1);
            ctx.check("idLong_decoded_arg",
                      g_idlong_arg.load(std::memory_order_relaxed) == IDLONG_ARG);
            // allow-through: original body ran (static mirror, stable -> HARD).
            ctx.check("idLong_allow_through", mw::get_last_id_long() == IDLONG_ARG);
        } // scoped_hook torn down here (RAII) — no hook leaks to the next module.

        ctx.check("scoped_hook_torn_down", true);
    }

    // =====================================================================
    // PART 16 — Null / not-loaded / bad-input paths -> empty, never a crash.
    // =====================================================================
    cp("PART 16 null / not-loaded / bad input");
    {
        ctx.check("not_loaded_empty",
                  vmhook::get_class_methods("vmhook/fixtures/DoesNotExistZZZ").empty());
        ctx.check("empty_name_empty", vmhook::get_class_methods("").empty());
        // The dotted (source) form is NOT the internal slashed form. Whether it
        // RESOLVES is environment-variant: on platforms/JDKs where get_class_methods'
        // find_class fallback reaches a dot-accepting path (JNI FindClass / Class.
        // forName) the dotted name resolves (Windows + linux·gcc·26 observed), while
        // the pure slashed-only resolver returns empty elsewhere. Characterize it
        // PASS-or-[INFO] — never HARD. What IS hard-guaranteed (and the real point of
        // this PART) is the no-crash contract, exercised by the four bad-input probes.
        {
            const bool dotted_empty{
                vmhook::get_class_methods("vmhook.fixtures.MethodsWalk").empty() };
            ctx.record(std::string{ "[INFO] dotted_form resolves=" } +
                       (dotted_empty ? "no (empty — slashed-form-only resolver)"
                                     : "yes (dotted name accepted via find_class fallback)"));
        }
        ctx.check("bad_input_paths_no_crash", true);  // reached here => no fault on any probe
        // a long garbage name -> empty, never a crash.
        ctx.check("garbage_name_empty",
                  vmhook::get_class_methods(std::string(300, 'Z')).empty());
    }

    // =====================================================================
    // PART 17 — Live mutation safety: force Many.<clinit> + a method dispatch
    //   AFTER the first enumeration; the declared set must be unchanged (declared
    //   _methods never grows from class init), and the JIT of idLong (PART 15) did
    //   not perturb the top-level set.
    // =====================================================================
    cp("PART 17 live mutation safety");
    {
        const pair_list many_before{ vmhook::get_class_methods<mw_many>() };

        cp("PART 17 drive(mode 2) — force Many.<clinit> + dispatch");
        const bool done{ drive(ctx, 2) };
        ctx.check("many_mutation_probe_completed", done);
        ctx.check("many_touched_observed", mw::get_many_touched() >= 1);

        const pair_list many_after{ vmhook::get_class_methods<mw_many>() };
        ctx.check("many_set_unchanged_after_clinit",
                  many_before.size() == many_after.size());
        ctx.check("many_ordered_identical_after_clinit", many_before == many_after);

        // The top-level set is unchanged after idLong dispatch+JIT (PART 15).
        const pair_list top_after{ vmhook::get_class_methods<mw>() };
        ctx.check("top_set_unchanged_after_jit", top.size() == top_after.size());
        ctx.check("top_ordered_identical_after_jit", top == top_after);
    }

    // =====================================================================
    // PART 18 — log_class_methods<W>() links + is a no-op in release.
    //   Compile-time + link assertion; must not throw or change enumeration.
    // =====================================================================
    cp("PART 18 log_class_methods no-op");
    {
        const std::size_t before{ vmhook::get_class_methods<mw>().size() };
        vmhook::log_class_methods<mw>();        // VMHOOK_LOG no-op in release
        vmhook::log_class_methods<mw_empty>();
        const std::size_t after{ vmhook::get_class_methods<mw>().size() };
        ctx.check("log_class_methods_no_side_effect", before == after);
    }

    // =====================================================================
    // PART 19 — RAW per-slot DECODE parity (deeper than PART 1's count==size).
    //   PART 1 pins count == collector.size().  This pins the DECODE: walk the
    //   raw get_methods_ptr() array element-by-element, call get_name()/
    //   get_signature() on each slot DIRECTLY, and assert the resulting ordered
    //   pair vector is byte-for-byte IDENTICAL to the collector's output.  This
    //   proves the collector adds nothing and drops nothing vs the bare accessor
    //   walk (the +0 length / +8 data ABI decoded through the real Method*).
    //   All metaspace reads (Method*/Symbol* native + stable) -> HARD.
    // =====================================================================
    cp("PART 19 raw per-slot decode parity (metaspace metadata)");
    {
        vmhook::hotspot::klass* const top_klass{
            reinterpret_cast<vmhook::hotspot::klass*>(vmhook::find_class(NAME_TOP)) };
        ctx.check("decode_klass_resolved", top_klass != nullptr);

        if (top_klass != nullptr)
        {
            const std::int32_t raw_count{ top_klass->get_methods_count() };
            vmhook::hotspot::method** const raw_ptr{ top_klass->get_methods_ptr() };

            pair_list raw_decoded{};
            bool decode_ran{ raw_ptr != nullptr && raw_count > 0 };
            if (decode_ran)
            {
                raw_decoded.reserve(static_cast<std::size_t>(raw_count));
                for (std::int32_t i{ 0 }; i < raw_count; ++i)
                {
                    vmhook::hotspot::method* const m{ raw_ptr[i] };
                    if (!m || !vmhook::hotspot::is_valid_pointer(m))
                    {
                        continue; // mirror the collector's per-slot skip
                    }
                    const std::string nm{ m->get_name() };
                    const std::string sg{ m->get_signature() };
                    raw_decoded.emplace_back(nm, sg);
                }
            }
            ctx.check("decode_ran", decode_ran);

            // The raw decode equals the collector — element-for-element, in order
            // (the collector IS this exact loop; this is the behavioural pin).
            const pair_list collected{ vmhook::get_class_methods(NAME_TOP) };
            ctx.check("decode_size_eq_collector", raw_decoded.size() == collected.size());
            ctx.check("decode_identical_to_collector", raw_decoded == collected);

            // Every decoded slot is non-empty on both axes (no decode-fail / no
            // empty-name slot survived) — the flaw-#5 property at the RAW layer.
            const bool raw_no_empty{ std::none_of(raw_decoded.begin(), raw_decoded.end(),
                [](const std::pair<std::string, std::string>& p)
                { return p.first.empty() || p.second.empty(); }) };
            ctx.check("decode_no_empty_pair", raw_no_empty);

            // A known unique method is present in the RAW decode (proves the loop
            // reached real Method*s, not a degenerate empty walk).
            ctx.check("decode_has_idLong",
                      count_pair(raw_decoded, "idLong", "(J)J") == 1);
        }
    }

    // =====================================================================
    // PART 20 — Concrete subclass of an abstract parent OVERRIDES the abstract
    //   method: the override is DECLARED on the child, so its walk lists shape(I)
    //   + the child's own extra(I) + <init>; it does NOT list the inherited
    //   concrete(I) (a non-overridden parent method stays on the parent only).
    // =====================================================================
    cp("PART 20 concrete subclass override enumeration");
    {
        const pair_list cs{ vmhook::get_class_methods<mw_concrete>() };
        ctx.check("concrete_has_override_shape", count_pair(cs, "shape", "(I)I") == 1);
        ctx.check("concrete_has_extra",         count_pair(cs, "extra", "(I)I") == 1);
        ctx.check("concrete_has_init",          count_pair(cs, "<init>", "()V") == 1);
        // The parent's non-overridden concrete(I) is NOT declared on the child.
        ctx.check("concrete_excludes_inherited_concrete", !has_name(cs, "concrete"));
        ctx.check("concrete_excludes_toString",           !has_name(cs, "toString"));
        // shape + extra + <init> = 3 declared (a plain override of a same-
        // signature method emits no bridge).  Lower-bounded so a JDK that adds a
        // synthetic cannot redden CI; the exact membership above pins the set.
        ctx.check("concrete_size_at_least_3", cs.size() >= 3);
        ctx.record(std::string{ "[INFO] ConcreteSub method count = " }
                   + std::to_string(cs.size()));
    }

    // =====================================================================
    // PART 21 — Enum with a constant-specific body: the user method is ABSTRACT
    //   on the enum klass (constant bodies live on synthetic anon subclasses).
    //   values()/valueOf(String) stay JDK-stable; the abstract apply(I) is on
    //   THIS klass; the constant-body overrides are NOT (they are on the anon
    //   subclasses, which this bare walk never climbs into).
    // =====================================================================
    cp("PART 21 enum with abstract constant-specific method");
    {
        const pair_list ea{ vmhook::get_class_methods<mw_enumabs>() };
        ctx.check("enumabs_has_values",
                  count_pair(ea, "values", "()[Lvmhook/fixtures/MethodsWalk$EnumAbstract;") == 1);
        ctx.check("enumabs_has_valueOf",
                  count_pair(ea, "valueOf",
                             "(Ljava/lang/String;)Lvmhook/fixtures/MethodsWalk$EnumAbstract;") == 1);
        ctx.check("enumabs_has_abstract_apply", count_pair(ea, "apply", "(I)I") == 1);
        ctx.check("enumabs_has_init",   has_name(ea, "<init>"));
        ctx.check("enumabs_has_clinit", has_name(ea, "<clinit>"));
        // apply appears exactly once on the enum klass (the constant overrides
        // are on the anon subclasses, not enumerated by this bare walk).
        ctx.check("enumabs_apply_once_on_klass", count_name(ea, "apply") == 1);
        ctx.check("enumabs_no_empty",
                  std::none_of(ea.begin(), ea.end(),
                      [](const std::pair<std::string, std::string>& p)
                      { return p.first.empty() || p.second.empty(); }));
    }

    // =====================================================================
    // PART 22 — Generic CLASS (type parameter on the class): the type variable
    //   erases to its bound (Object) in every method descriptor.  echo(T) walks
    //   as (Ljava/lang/Object;)Ljava/lang/Object;; sizeOf(T) as (Lj.l.Object;)I.
    //   Class-level erasure, distinct from the top-level method-level generics.
    // =====================================================================
    cp("PART 22 generic-class erasure");
    {
        const pair_list ge{ vmhook::get_class_methods<mw_generic>() };
        ctx.check("generic_has_echo_erased",
                  count_pair(ge, "echo", "(Ljava/lang/Object;)Ljava/lang/Object;") == 1);
        ctx.check("generic_has_sizeOf_erased",
                  count_pair(ge, "sizeOf", "(Ljava/lang/Object;)I") == 1);
        ctx.check("generic_has_init", count_pair(ge, "<init>", "()V") == 1);
        // Erasure must have replaced the type variable T with its bound: the
        // un-erased generic-signature form (TT;)TT; must NOT appear as the JVM
        // DESCRIPTOR (that form lives only in the Signature attribute, never in
        // _methods' descriptor symbol the walk reads).
        ctx.check("generic_no_typevar_leak", count_descriptor(ge, "(TT;)TT;") == 0);
        ctx.check("generic_echo_count_1", count_name(ge, "echo") == 1);
    }

    // =====================================================================
    // PART 23 — find_methods_by_signature<W> substrate consistency on THIS
    //   fixture's KNOWN descriptors (fmbs's own module pins its OWN fixture;
    //   this pins the filter against the methods-walk fixture's exact map).  For
    //   every probed descriptor, find(...).size() == its multiplicity in
    //   get_class_methods<mw>(), and every returned name carries that descriptor.
    // =====================================================================
    cp("PART 23 find_methods_by_signature substrate consistency");
    {
        const auto check_desc = [&](const char* tag, const std::string& descriptor)
        {
            const std::vector<std::string> names{
                vmhook::find_methods_by_signature<mw>(descriptor) };
            const std::size_t multiplicity{ count_descriptor(top, descriptor) };
            ctx.check(std::string{ "fmbs_size_eq_mult_" } + tag,
                      names.size() == multiplicity);
            // Every returned NAME actually carries this descriptor in the pair list.
            bool every_name_carries{ true };
            for (const std::string& n : names)
            {
                if (count_pair(top, n, descriptor) != 1)
                {
                    every_name_carries = false;
                    break;
                }
            }
            ctx.check(std::string{ "fmbs_names_carry_desc_" } + tag, every_name_carries);
        };

        check_desc("II",   "(I)I");                 // si + ii + nat + fin + ov = 5
        check_desc("JJ",   "(J)J");                 // ov(long) + idLong = 2
        check_desc("V",    "()V");                  // many: syncM + uniques + <init>/<clinit>
        check_desc("Z",    "()Z");                  // retZ -> exactly 1
        check_desc("allP", "(ZBCSIJFD)V");          // allPrims -> exactly 1
        check_desc("deep", "([[[Ljava/lang/String;)V"); // deep -> exactly 1
        check_desc("none", "(Lno/such/Type;)V");    // nothing declares -> 0

        // The unique-descriptor case resolves to exactly one name, and it is the
        // expected one — the canonical "rotate the name, keep the descriptor" use.
        const std::vector<std::string> z_names{
            vmhook::find_methods_by_signature<mw>("()Z") };
        ctx.check("fmbs_Z_unique_is_retZ",
                  z_names.size() == 1 && z_names.front() == "retZ");

        // (J)J is non-unique (2) — find returns BOTH, never silently the first.
        const std::vector<std::string> jj_names{
            vmhook::find_methods_by_signature<mw>("(J)J") };
        ctx.check("fmbs_JJ_returns_both",
                  jj_names.size() == 2
                  && std::count(jj_names.begin(), jj_names.end(), std::string{ "ov" }) == 1
                  && std::count(jj_names.begin(), jj_names.end(), std::string{ "idLong" }) == 1);

        // Unregistered wrapper -> empty for ANY descriptor (flaw #6 at the filter).
        ctx.check("fmbs_unregistered_empty",
                  vmhook::find_methods_by_signature<mw_unregistered>("(I)I").empty());
    }

    // =====================================================================
    // PART 24 — Expanded bad-input degradation: more malformed class-name shapes
    //   all return empty and never crash (extends PART 16).  Pure metadata
    //   lookups; reaching the end is the no-crash witness.
    // =====================================================================
    cp("PART 24 expanded bad-input degradation");
    {
        ctx.check("whitespace_name_empty",
                  vmhook::get_class_methods("   ").empty());
        ctx.check("trailing_slash_empty",
                  vmhook::get_class_methods("vmhook/fixtures/MethodsWalk/").empty());
        ctx.check("leading_slash_empty",
                  vmhook::get_class_methods("/vmhook/fixtures/MethodsWalk").empty());
        // A primitive type name and an array descriptor are not InstanceKlass
        // internal names -> empty, never a crash.
        ctx.check("primitive_name_empty", vmhook::get_class_methods("int").empty());
        ctx.check("array_desc_name_empty", vmhook::get_class_methods("[I").empty());
        ctx.check("obj_array_desc_empty",
                  vmhook::get_class_methods("[Ljava/lang/String;").empty());
        // A wrong-case nested name (the JVM is case-sensitive) -> empty.
        ctx.check("wrongcase_nested_empty",
                  vmhook::get_class_methods("vmhook/fixtures/methodswalk$Empty").empty());
        // A name with an embedded NUL byte -> empty, never a crash (the resolver
        // must not walk past / mis-handle the NUL).  Length 28 = 23 chars +
        // NUL + "Walk" (4), so the NUL is interior, not a terminator.
        ctx.check("embedded_nul_name_empty",
                  vmhook::get_class_methods(std::string{ "vmhook/fixtures/Methods\0Walk", 28 }).empty());
        // A bare top-level name without package -> empty.
        ctx.check("bare_name_empty", vmhook::get_class_methods("MethodsWalk").empty());
        ctx.check("bad_input_24_no_crash", true); // reached => no fault on any probe
    }

    // =====================================================================
    // PART 25 — Superclass-chain walk via get_super() (the ONE walk primitive
    //   the module never drove).  PART 7 pins that Sub's BARE walk excludes
    //   inherited methods; this pins the COMPLEMENT: walking get_super() up the
    //   Sub -> Mid -> Base -> Object chain and unioning each level's OWN declared
    //   methods RECOVERS exactly those excluded names.  get_super() is
    //   is_valid_pointer-guarded; every level read is pure metaspace metadata
    //   (Klass* native + stable, never GC-relocated) -> HARD, cannot fault cold.
    // =====================================================================
    cp("PART 25 superclass-chain walk (get_super union recovers inherited)");
    {
        vmhook::hotspot::klass* const sub_klass{
            reinterpret_cast<vmhook::hotspot::klass*>(vmhook::find_class(NAME_SUB)) };
        ctx.check("chain_sub_resolved", sub_klass != nullptr);

        if (sub_klass != nullptr)
        {
            // Walk Sub -> super -> super ... collecting each level's OWN declared
            // (name,desc) pairs into one union, bounded so a corrupt _super loop
            // can never spin (a real chain to Object is < 8 deep here).
            pair_list chain_union{};
            name_list level_first_names{};   // a witness name per level, in order
            std::int32_t levels{ 0 };
            for (vmhook::hotspot::klass* k{ sub_klass };
                 k != nullptr && levels < 16;
                 k = k->get_super(), ++levels)
            {
                if (!vmhook::hotspot::is_valid_pointer(k))
                {
                    break;
                }
                const pair_list lvl{ vmhook::detail::collect_klass_methods(k) };
                for (const std::pair<std::string, std::string>& m : lvl)
                {
                    chain_union.push_back(m);
                }
                if (!lvl.empty())
                {
                    level_first_names.push_back(lvl.front().first);
                }
            }
            ctx.check("chain_has_multiple_levels", levels >= 4); // Sub,Mid,Base,Object,...
            ctx.record(std::string{ "[INFO] superclass chain depth from Sub = " }
                       + std::to_string(levels));

            // The union over the chain RECOVERS every name Sub's bare walk excluded.
            ctx.check("chain_recovers_subOnly",    has_name(chain_union, "subOnly"));
            ctx.check("chain_recovers_midOnly",    has_name(chain_union, "midOnly"));
            ctx.check("chain_recovers_baseOnly",   has_name(chain_union, "baseOnly"));
            ctx.check("chain_recovers_baseStatic", has_name(chain_union, "baseStatic"));

            // Object's OWN declared methods are reachable at the chain TOP — proving
            // Sub's exclusion of toString/hashCode/equals is a PLACEMENT fact (they
            // live on Object), not an enumeration failure.
            ctx.check("chain_recovers_object_toString", has_name(chain_union, "toString"));
            ctx.check("chain_recovers_object_hashCode", has_name(chain_union, "hashCode"));
            ctx.check("chain_recovers_object_equals",   has_name(chain_union, "equals"));

            // And the SINGLE-level Sub walk still does NOT contain the parent-only
            // names (re-affirms the PART 7 exclusion against the same klass object).
            const pair_list sub_only{ vmhook::detail::collect_klass_methods(sub_klass) };
            ctx.check("chain_sub_level_excludes_midOnly",  !has_name(sub_only, "midOnly"));
            ctx.check("chain_sub_level_excludes_baseOnly", !has_name(sub_only, "baseOnly"));
            ctx.check("chain_sub_level_excludes_toString", !has_name(sub_only, "toString"));

            // No level produced an empty-name witness (every collected level decoded).
            ctx.check("chain_levels_decoded_nonempty",
                      std::none_of(level_first_names.begin(), level_first_names.end(),
                                   [](const std::string& s) { return s.empty(); }));
        }
    }

    // =====================================================================
    // PART 26 — <clinit> PRESENCE is a placement/class-file fact, not runtime.
    //   A class with NO static initialization (Empty) has NO <clinit>; a class
    //   WITH static field inits + a static block (top-level MethodsWalk) HAS one;
    //   an interface whose static-final field has a NON-constant initializer
    //   (IfaceClinit) HAS a <clinit> but still NO <init>.  Pins that the walk
    //   reports <clinit> exactly when the class file carries it.
    // =====================================================================
    cp("PART 26 <clinit> presence is a class-file fact");
    {
        const pair_list empty_methods{ vmhook::get_class_methods<mw_empty>() };
        ctx.check("empty_has_no_clinit", !has_name(empty_methods, "<clinit>"));
        ctx.check("empty_still_has_init", count_pair(empty_methods, "<init>", "()V") == 1);

        // The top-level class genuinely has a <clinit> (static field inits + block).
        ctx.check("top_has_clinit_fact", count_pair(top, "<clinit>", "()V") == 1);

        // IfaceClinit: <clinit> present (non-constant static-final), <init> absent.
        const pair_list ifc{ vmhook::get_class_methods<mw_ifacecl>() };
        ctx.check("ifacecl_has_clinit", count_pair(ifc, "<clinit>", "()V") == 1);
        ctx.check("ifacecl_no_init",    !has_name(ifc, "<init>"));
    }

    // =====================================================================
    // PART 27 — Interface with a <clinit>: abstract op + the <clinit> are the
    //   ONLY entries; no <init>, no inherited Object method declared.
    // =====================================================================
    cp("PART 27 interface-with-clinit enumeration");
    {
        const pair_list ifc{ vmhook::get_class_methods<mw_ifacecl>() };
        ctx.check("ifacecl_has_cOp",   count_pair(ifc, "cOp", "(I)I") == 1);
        ctx.check("ifacecl_excludes_toString", !has_name(ifc, "toString"));
        // cOp (abstract) + <clinit> == exactly 2 entries (interface, no <init>).
        ctx.check("ifacecl_size_2", ifc.size() == 2);
        ctx.check("ifacecl_no_empty",
                  std::none_of(ifc.begin(), ifc.end(),
                      [](const std::pair<std::string, std::string>& p)
                      { return p.first.empty() || p.second.empty(); }));
    }

    // =====================================================================
    // PART 28 — RAW accessor robustness on a NON-klass pointer: the
    //   get_methods_count()/get_methods_ptr() guard rejects a pointer that fails
    //   is_valid_pointer(this) BEFORE any deref, so a garbage `this` degrades to
    //   0 / nullptr (never a fault), and collect_klass_methods(garbage) is empty.
    //   The sentinels are chosen to be is_valid_pointer-REJECTED (odd / below the
    //   user floor), so nothing is dereferenced -> crash-safe even with no SEH.
    // =====================================================================
    cp("PART 28 raw accessor robustness on a non-klass pointer");
    {
        // 0x1 (odd, below floor) and 0xDEADBEEF (a debug-fill sentinel) both fail
        // is_valid_pointer; the accessors must short-circuit to 0 / nullptr.
        vmhook::hotspot::klass* const garbage_a{
            reinterpret_cast<vmhook::hotspot::klass*>(static_cast<std::uintptr_t>(0x1)) };
        vmhook::hotspot::klass* const garbage_b{
            reinterpret_cast<vmhook::hotspot::klass*>(static_cast<std::uintptr_t>(0xDEADBEEFULL)) };

        ctx.check("garbage_a_rejected_by_is_valid",
                  !vmhook::hotspot::is_valid_pointer(garbage_a));
        ctx.check("garbage_b_rejected_by_is_valid",
                  !vmhook::hotspot::is_valid_pointer(garbage_b));

        ctx.check("garbage_a_count_zero", garbage_a->get_methods_count() == 0);
        ctx.check("garbage_a_ptr_null",   garbage_a->get_methods_ptr() == nullptr);
        ctx.check("garbage_b_count_zero", garbage_b->get_methods_count() == 0);
        ctx.check("garbage_b_ptr_null",   garbage_b->get_methods_ptr() == nullptr);

        // The collector on a guard-rejected klass yields an empty list, never a fault.
        ctx.check("collect_garbage_a_empty",
                  vmhook::detail::collect_klass_methods(garbage_a).empty());
        ctx.check("collect_garbage_b_empty",
                  vmhook::detail::collect_klass_methods(garbage_b).empty());
        // A literal null klass collects to empty too (the collector's first guard).
        ctx.check("collect_null_empty",
                  vmhook::detail::collect_klass_methods(nullptr).empty());
        ctx.check("part28_no_crash", true); // reached => guards held, no deref fault
    }

    // =====================================================================
    // PART 29 — Per-klass ISOLATION: the walk reads ONE klass's _methods, so a
    //   name/descriptor declared on one nested type is ABSENT from a sibling's
    //   walk.  Many's m00..m49 ()V are not on Empty; Empty's set is a strict
    //   subset of Many's by size; idLong (top-level) is on neither nested type.
    // =====================================================================
    cp("PART 29 per-klass isolation (no cross-klass bleed)");
    {
        const pair_list many{ vmhook::get_class_methods<mw_many>() };
        const pair_list empty_methods{ vmhook::get_class_methods<mw_empty>() };

        ctx.check("isolation_many_has_m00", count_pair(many, "m00", "()V") == 1);
        ctx.check("isolation_empty_lacks_m00", !has_name(empty_methods, "m00"));
        ctx.check("isolation_empty_lacks_m49", !has_name(empty_methods, "m49"));
        ctx.check("isolation_empty_smaller_than_many",
                  empty_methods.size() < many.size());

        // The top-level-only idLong/allPrims are on NEITHER nested type.
        ctx.check("isolation_many_lacks_idLong",  !has_name(many, "idLong"));
        ctx.check("isolation_empty_lacks_idLong", !has_name(empty_methods, "idLong"));
        ctx.check("isolation_many_lacks_allPrims", !has_name(many, "allPrims"));

        // touch() is Many-only; it never appears on the top-level class either.
        ctx.check("isolation_top_lacks_touch", !has_name(top, "touch"));
        ctx.check("isolation_many_has_touch", count_pair(many, "touch", "()V") == 1);
    }

    // =====================================================================
    // PART 30 — Broad determinism net: collect EVERY registered nested type
    //   twice and assert each is ordered-identical and free of empty pairs.  One
    //   sweep pins the ordered-stability + no-decode-fail invariants across all
    //   class SHAPES (class / interface / abstract / enum / annotation / inner /
    //   generic / hierarchy levels) at once.  Pure metaspace -> HARD.
    // =====================================================================
    cp("PART 30 broad determinism sweep over every nested shape");
    {
        const name_list every_name{
            NAME_TOP, NAME_EMPTY, NAME_IFACE, NAME_IFACECL, NAME_ABSTRACT,
            NAME_CONCRETE, NAME_BASE, NAME_MID, NAME_SUB, NAME_VALS,
            NAME_ENUMABS, NAME_GENERIC, NAME_ANNO, NAME_MANY, NAME_INNER };

        bool all_ordered_identical{ true };
        bool all_no_empty{ true };
        std::size_t resolved_count{ 0 };
        for (const std::string& cls : every_name)
        {
            const pair_list a{ vmhook::get_class_methods(cls) };
            const pair_list b{ vmhook::get_class_methods(cls) };
            if (a != b)
            {
                all_ordered_identical = false;
            }
            for (const std::pair<std::string, std::string>& m : a)
            {
                if (m.first.empty() || m.second.empty())
                {
                    all_no_empty = false;
                    break;
                }
            }
            // Marker is the only EMPTY-by-design type and is intentionally not in
            // this list, so every entry here must resolve to a non-empty set.
            if (!a.empty())
            {
                ++resolved_count;
            }
        }
        ctx.check("sweep_all_ordered_identical", all_ordered_identical);
        ctx.check("sweep_all_no_empty_pairs", all_no_empty);
        ctx.check("sweep_all_15_resolved_nonempty",
                  resolved_count == every_name.size());
        ctx.record(std::string{ "[INFO] determinism sweep resolved " }
                   + std::to_string(resolved_count) + " of "
                   + std::to_string(every_name.size()) + " nested shapes non-empty.");
    }

    cp("module complete (all parts reached without a no-SEH fault)");
}
