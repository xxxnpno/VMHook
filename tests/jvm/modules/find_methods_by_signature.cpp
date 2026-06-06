// find_methods_by_signature JVM test module  (feature area: methods)
//
// THE authority for vmhook::find_methods_by_signature<W>(descriptor) -- the
// obfuscated-build selector that, given a stable JVM descriptor, returns the
// NAMES of EVERY declared method on W's klass whose descriptor equals it
// (vmhook.hpp:7081-7094).  It is a thin EXACT-equality filter over
// get_class_methods<W>() (vmhook.hpp:7030-7048 -> detail::collect_klass_methods
// 6973-7004), which walks InstanceKlass::_methods DIRECTLY -- no JNI -- so it
// returns the class's DECLARED methods (including the synthetic <init>/<clinit>)
// and NOT inherited java.lang.Object methods.
//
// This module exercises every discrimination axis the matcher must respect,
// against fixture vmhook/fixtures/FindMethodsBySig.java whose (name -> descriptor)
// map is known EXACTLY (verified with `javap -s` on JDK 8/11/17/21):
//
//   ARG TYPE / WIDTH     : (I)I (J)J (S)S (B)B (C)C (Z)Z (F)F (D)D
//   ARITY                : ()V vs (I)I vs (II)I ; ()J no-arg long
//   RETURN-TYPE          : (I)I {f,sf} vs (I)J {fL} ; ()V {..} vs ()I {retI} vs ()J {g}
//   REFERENCE vs PRIM    : (..String;)..String; {f,sf} ; ()Ljava/lang/Object; {makeObj}
//   ARRAYS               : ([I)[I {arr} ; ([[I)[[I {arr2} ; ([L..String;)[L..String; {arrStr}
//   MULTI-SLOT           : (IJD)D {mix} ; (JJ)J {sUnique}
//   STATIC == INSTANCE   : (I)I returns BOTH instance f AND static sf
//
// What it proves, angle by angle:
//   - find returns the FULL match SET (not just the first): (I)I -> {f, sf},
//     String-desc -> {f, sf}; size AND membership asserted, order-independent.
//   - genuinely-unique descriptors resolve to exactly their one method.
//   - RETURN-TYPE discrimination: (I)I must NOT include fL ((I)J), and (I)J must
//     NOT include f/sf -- a same-arg different-return method is a distinct match.
//   - ARITY discrimination: (I)I excludes g(int,int); (II)I is exactly {g}.
//   - STATIC methods ARE enumerated alongside instance ones (descriptor walk
//     ignores JVM_ACC_STATIC): (I)I and (..String;).. each contain the static sf.
//   - find AGREES with get_class_methods (its substrate): for every descriptor,
//     find(...).size() == count of that descriptor in get_class_methods<W>(),
//     and every returned name actually carries that descriptor in the pair list.
//   - NEGATIVE / malformed inputs degrade gracefully to EMPTY, never crash:
//     a descriptor nothing declares, the empty string, whitespace, a near-miss
//     (right shape wrong type), lowercase type chars, missing parens, a method
//     NAME instead of a descriptor, a descriptor with trailing junk, a truncated
//     descriptor, and a valid descriptor for a method on a DIFFERENT class.
//   - UNREGISTERED wrapper type -> empty (type_to_class_map miss).
//   - every returned name is a REAL declared method name (cross-checked against
//     the get_class_methods name set) and never empty.
//   - LIVE post-dispatch stability: the probe dispatches f/g/arr/sf/sUnique
//     through REAL bytecode (invokevirtual + invokestatic), then the module
//     RE-RUNS find and asserts the result sets are byte-identical -- proving
//     calling/JITting a method does not perturb the _methods enumeration.
//
// No hooks are installed by this module, so there is nothing to tear down; the
// only Java interaction is the single run_probe handshake.  C++17 only.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.FindMethodsBySig.  Deriving from
    // vmhook::object<> gives the wrapper the vtable register_class<T> needs and
    // the static_field(...) accessor used for the go/done handshake.  All
    // handshake access is via static_field (the GCC-portable path -- the
    // deducing-this get_field static overloads do not exist on GCC).
    class fmbs : public vmhook::object<fmbs>
    {
    public:
        explicit fmbs(vmhook::oop_t instance) noexcept
            : vmhook::object<fmbs>{ instance }
        {
        }

        static auto set_go(bool value) -> void    { static_field("go")->set(value); }
        static auto set_done(bool value) -> void   { static_field("done")->set(value); }
        static auto get_done() -> bool             { return static_field("done")->get(); }

        // Witnesses written by driveDispatch() through real bytecode.
        static auto get_wFInt() -> std::int32_t    { return static_field("wFInt")->get(); }
        static auto get_wFLong() -> std::int64_t   { return static_field("wFLong")->get(); }
        static auto get_wGII() -> std::int32_t     { return static_field("wGII")->get(); }
        static auto get_wArrLen() -> std::int32_t  { return static_field("wArrLen")->get(); }
        static auto get_wSfInt() -> std::int32_t   { return static_field("wSfInt")->get(); }
        static auto get_wSUnique() -> std::int64_t { return static_field("wSUnique")->get(); }
    };

    // A SECOND wrapper type that is deliberately NEVER registered, to prove the
    // template overload returns empty for an unregistered type.
    class fmbs_unregistered : public vmhook::object<fmbs_unregistered>
    {
    public:
        explicit fmbs_unregistered(vmhook::oop_t instance) noexcept
            : vmhook::object<fmbs_unregistered>{ instance }
        {
        }
    };

    constexpr char CLASS_NAME[]{ "vmhook/fixtures/FindMethodsBySig" };

    // -- small set helpers (order-independent membership / equality) ----------
    using name_list = std::vector<std::string>;
    using pair_list = std::vector<std::pair<std::string, std::string>>;

    auto contains(const name_list& names, const std::string& needle) -> bool
    {
        return std::find(names.begin(), names.end(), needle) != names.end();
    }

    auto count_name(const name_list& names, const std::string& needle) -> std::size_t
    {
        return static_cast<std::size_t>(
            std::count(names.begin(), names.end(), needle));
    }

    // Count how many (name, descriptor) pairs carry exactly this descriptor.
    auto count_descriptor(const pair_list& methods, const std::string& descriptor) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            { return m.second == descriptor; }));
    }

    // True if some pair has exactly (name, descriptor).
    auto has_pair(const pair_list& methods, const std::string& name, const std::string& descriptor) -> bool
    {
        return std::any_of(methods.begin(), methods.end(),
                           [&](const std::pair<std::string, std::string>& m)
                           { return m.first == name && m.second == descriptor; });
    }

    // Two name multisets are equal regardless of order (sort + compare copies).
    auto same_multiset(name_list a, name_list b) -> bool
    {
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        return a == b;
    }

    // Shorthand: find_methods_by_signature on the registered fmbs wrapper.
    auto find_sig(const char* descriptor) -> name_list
    {
        return vmhook::find_methods_by_signature<fmbs>(descriptor);
    }
}

VMHOOK_JVM_MODULE(find_methods_by_signature)
{
    vmhook::register_class<fmbs>("vmhook/fixtures/FindMethodsBySig");

    // =====================================================================
    //  0. Sanity: the class resolves and its method enumeration is non-empty.
    //     (find_methods_by_signature is a filter over this enumeration.)
    // =====================================================================
    ctx.check("fmbs_class_registered", fmbs::static_field("go").has_value());

    const pair_list all_methods{ vmhook::get_class_methods<fmbs>() };
    ctx.check("get_class_methods_nonempty", !all_methods.empty());
    ctx.record(std::string{ "[INFO] get_class_methods<fmbs>() returned " }
               + std::to_string(all_methods.size()) + " declared method(s).");

    // The substrate must contain the exact declared pairs we rely on below.
    ctx.check("substrate_has_f_II",   has_pair(all_methods, "f", "(I)I"));
    ctx.check("substrate_has_sf_II",  has_pair(all_methods, "sf", "(I)I"));
    ctx.check("substrate_has_f_JJ",   has_pair(all_methods, "f", "(J)J"));
    ctx.check("substrate_has_g_III",  has_pair(all_methods, "g", "(II)I"));
    ctx.check("substrate_has_fL_IJ",  has_pair(all_methods, "fL", "(I)J"));
    ctx.check("substrate_has_arr_aII", has_pair(all_methods, "arr", "([I)[I"));

    // =====================================================================
    //  1. SHARED descriptor (I)I -> the FULL set { f, sf }.  This is the
    //     headline "return ALL matches, not just the first" guarantee, AND it
    //     proves a STATIC method (sf) is enumerated next to an instance one (f).
    // =====================================================================
    {
        const name_list ii{ find_sig("(I)I") };
        ctx.check("II_size_2", ii.size() == 2);
        ctx.check("II_has_f", contains(ii, "f"));
        ctx.check("II_has_sf", contains(ii, "sf"));
        ctx.check("II_is_exactly_f_sf", same_multiset(ii, name_list{ "f", "sf" }));
        // No name appears twice (a class cannot declare two methods with the
        // SAME name+descriptor, so each match name is distinct).
        ctx.check("II_f_once", count_name(ii, "f") == 1);
        ctx.check("II_sf_once", count_name(ii, "sf") == 1);
        // RETURN-TYPE discrimination: fL is (I)J, NOT (I)I -- must be absent.
        ctx.check("II_excludes_fL", !contains(ii, "fL"));
        // ARITY discrimination: g(int,int) is (II)I, NOT (I)I -- must be absent.
        ctx.check("II_excludes_g", !contains(ii, "g"));
        // <init>/<clinit> are ()V, never (I)I.
        ctx.check("II_excludes_init", !contains(ii, "<init>"));
    }

    // =====================================================================
    //  2. SHARED reference descriptor (..String;)..String; -> { f, sf }.
    //     A second proof that static (sf) and instance (f) share a result set,
    //     this time on a REFERENCE-arg / REFERENCE-return descriptor.
    // =====================================================================
    {
        const name_list ss{ find_sig("(Ljava/lang/String;)Ljava/lang/String;") };
        ctx.check("strdesc_size_2", ss.size() == 2);
        ctx.check("strdesc_has_f", contains(ss, "f"));
        ctx.check("strdesc_has_sf", contains(ss, "sf"));
        ctx.check("strdesc_is_exactly_f_sf", same_multiset(ss, name_list{ "f", "sf" }));
    }

    // =====================================================================
    //  3. GENUINELY-UNIQUE descriptors -> exactly their one method.
    //     Each asserts size==1 AND the single name, plus that the OTHER same-shape
    //     methods are excluded where a near-collision exists.
    // =====================================================================
    {
        const name_list jj{ find_sig("(J)J") };
        ctx.check("JJ_size_1", jj.size() == 1);
        ctx.check("JJ_is_f", jj.size() == 1 && jj.front() == "f");

        const name_list iii{ find_sig("(II)I") };
        ctx.check("III_size_1", iii.size() == 1);
        ctx.check("III_is_g", iii.size() == 1 && iii.front() == "g");

        // RETURN-TYPE proof: (I)J is exactly { fL } and excludes f/sf.
        const name_list ij{ find_sig("(I)J") };
        ctx.check("IJ_size_1", ij.size() == 1);
        ctx.check("IJ_is_fL", ij.size() == 1 && ij.front() == "fL");
        ctx.check("IJ_excludes_f", !contains(ij, "f"));
        ctx.check("IJ_excludes_sf", !contains(ij, "sf"));

        const name_list ss_{ find_sig("(S)S") };
        ctx.check("SS_is_sFn", ss_.size() == 1 && ss_.front() == "sFn");

        const name_list bb{ find_sig("(B)B") };
        ctx.check("BB_is_bFn", bb.size() == 1 && bb.front() == "bFn");

        const name_list cc{ find_sig("(C)C") };
        ctx.check("CC_is_cFn", cc.size() == 1 && cc.front() == "cFn");

        const name_list zz{ find_sig("(Z)Z") };
        ctx.check("ZZ_is_zFn", zz.size() == 1 && zz.front() == "zFn");

        const name_list ff{ find_sig("(F)F") };
        ctx.check("FF_is_ffn", ff.size() == 1 && ff.front() == "ffn");

        const name_list dd{ find_sig("(D)D") };
        ctx.check("DD_is_dfn", dd.size() == 1 && dd.front() == "dfn");

        const name_list ijd{ find_sig("(IJD)D") };
        ctx.check("IJD_is_mix", ijd.size() == 1 && ijd.front() == "mix");

        const name_list jjj{ find_sig("(JJ)J") };
        ctx.check("JJJ_is_sUnique", jjj.size() == 1 && jjj.front() == "sUnique");
    }

    // =====================================================================
    //  4. ARRAY descriptors -> exactly their one method.
    //     1-D primitive, 2-D primitive, and 1-D reference arrays.
    // =====================================================================
    {
        const name_list a1{ find_sig("([I)[I") };
        ctx.check("arrII_size_1", a1.size() == 1);
        ctx.check("arrII_is_arr", a1.size() == 1 && a1.front() == "arr");

        const name_list a2{ find_sig("([[I)[[I") };
        ctx.check("arr2_is_arr2", a2.size() == 1 && a2.front() == "arr2");

        const name_list astr{ find_sig("([Ljava/lang/String;)[Ljava/lang/String;") };
        ctx.check("arrStr_is_arrStr", astr.size() == 1 && astr.front() == "arrStr");

        // A near-miss array descriptor (1-D vs 2-D) must not cross-match.
        ctx.check("arrII_not_matched_by_arr2_desc", !contains(a2, "arr"));
        ctx.check("arr2_not_matched_by_arrII_desc", !contains(a1, "arr2"));
    }

    // =====================================================================
    //  5. NO-ARG RETURN-TYPE discrimination on the same () arg list:
    //     ()V (a SET incl. <init>/<clinit>/f/uniqueVoid/driveDispatch),
    //     ()I  -> exactly { retI },
    //     ()J  -> exactly { g },
    //     ()Ljava/lang/Object; -> exactly { makeObj }.
    //  Only ()V is a set (synthetic members + JDK8's access$ synthetic land here);
    //  the distinctive-return no-arg descriptors are genuine singletons on every
    //  JDK (no synthetic returns int/long/Object with zero args).
    // =====================================================================
    {
        const name_list vv{ find_sig("()V") };
        // Universal members: the synthetic <init>/<clinit>, the real void no-arg
        // f() and uniqueVoid(), and the private static driveDispatch() -- all ()V.
        ctx.check("V_has_init", contains(vv, "<init>"));
        ctx.check("V_has_clinit", contains(vv, "<clinit>"));
        ctx.check("V_has_f", contains(vv, "f"));
        ctx.check("V_has_uniqueVoid", contains(vv, "uniqueVoid"));
        ctx.check("V_has_driveDispatch", contains(vv, "driveDispatch"));
        // Lower bound holds on every JDK (>=5 above); JDK 8 adds one synthetic
        // ()V accessor (access$000) for the inner Probe's private-method access,
        // so the EXACT count is 6 on JDK 8 and 5 on JDK 11+ -- recorded, not
        // hard-asserted, so the synthetic delta never breaks CI.
        ctx.check("V_size_at_least_5", vv.size() >= 5);
        ctx.record(std::string{ "[INFO] ()V match count = " } + std::to_string(vv.size())
                   + " (>=5 universal: <init>,<clinit>,f,uniqueVoid,driveDispatch; "
                     "JDK 8 javac adds one synthetic ()V accessor -> 6, JDK 11+ -> 5).");
        // ()V must NOT contain any method that returns a value.
        ctx.check("V_excludes_g", !contains(vv, "g"));      // g() is ()J
        ctx.check("V_excludes_retI", !contains(vv, "retI")); // retI() is ()I
        ctx.check("V_excludes_makeObj", !contains(vv, "makeObj")); // ()Lj.l.Object;

        const name_list ri{ find_sig("()I") };
        ctx.check("retI_size_1", ri.size() == 1);
        ctx.check("retI_is_retI", ri.size() == 1 && ri.front() == "retI");
        ctx.check("retI_excludes_f", !contains(ri, "f"));   // f() is ()V

        const name_list gj{ find_sig("()J") };
        ctx.check("noargJ_size_1", gj.size() == 1);
        ctx.check("noargJ_is_g", gj.size() == 1 && gj.front() == "g");
        ctx.check("noargJ_excludes_f", !contains(gj, "f"));

        const name_list obj{ find_sig("()Ljava/lang/Object;") };
        ctx.check("noargObj_size_1", obj.size() == 1);
        ctx.check("noargObj_is_makeObj", obj.size() == 1 && obj.front() == "makeObj");
        // A reference return must NOT match the String-returning no-arg... there
        // is none, but assert makeObj is not mistaken for a String method.
        ctx.check("noargObj_excludes_f", !contains(obj, "f"));
    }

    // =====================================================================
    //  6. CONSISTENCY with the substrate: for EVERY descriptor we test, the
    //     find(...) size equals the descriptor's multiplicity in
    //     get_class_methods<W>(), and every returned NAME actually carries that
    //     descriptor as a (name, descriptor) pair.  (find IS that filter, so the
    //     two views must agree exactly.)
    // =====================================================================
    {
        const char* descriptors[]{
            "(I)I", "(J)J", "(II)I", "(I)J", "(S)S", "(B)B", "(C)C", "(Z)Z",
            "(F)F", "(D)D", "(IJD)D", "(JJ)J", "([I)[I", "([[I)[[I",
            "([Ljava/lang/String;)[Ljava/lang/String;",
            "(Ljava/lang/String;)Ljava/lang/String;",
            "()V", "()I", "()J", "()Ljava/lang/Object;"
        };
        bool all_sizes_agree{ true };
        bool all_names_carry_descriptor{ true };
        for (const char* d : descriptors)
        {
            const name_list found{ find_sig(d) };
            if (found.size() != count_descriptor(all_methods, d))
            {
                all_sizes_agree = false;
            }
            for (const std::string& nm : found)
            {
                if (!has_pair(all_methods, nm, d))
                {
                    all_names_carry_descriptor = false;
                }
            }
        }
        ctx.check("find_size_matches_substrate_count_for_all", all_sizes_agree);
        ctx.check("find_names_all_carry_their_descriptor", all_names_carry_descriptor);
    }

    // =====================================================================
    //  7. Every returned name is a REAL declared method name (never empty,
    //     never inherited).  Cross-check the union of a few result sets against
    //     the substrate's name set.
    // =====================================================================
    {
        name_list substrate_names{};
        substrate_names.reserve(all_methods.size());
        for (const std::pair<std::string, std::string>& m : all_methods)
        {
            substrate_names.push_back(m.first);
        }
        const char* probe_descs[]{ "(I)I", "(J)J", "()V", "([I)[I", "(JJ)J" };
        bool every_name_real{ true };
        bool no_empty_name{ true };
        for (const char* d : probe_descs)
        {
            for (const std::string& nm : find_sig(d))
            {
                if (nm.empty()) { no_empty_name = false; }
                if (!contains(substrate_names, nm)) { every_name_real = false; }
            }
        }
        ctx.check("returned_names_are_declared_methods", every_name_real);
        ctx.check("returned_names_never_empty", no_empty_name);
        // Inherited java.lang.Object methods are NOT declared here, so a descriptor
        // that only Object methods carry must NOT match anything in this class.
        // toString/hashCode are ()Ljava/lang/String; / ()I -- ()I IS declared here
        // (retI), so use the Object-specific equals descriptor for the negative.
        const name_list eq{ find_sig("(Ljava/lang/Object;)Z") };
        ctx.check("inherited_equals_descriptor_absent", eq.empty());
        const name_list gc{ find_sig("()Ljava/lang/Class;") };
        ctx.check("inherited_getClass_descriptor_absent", gc.empty());
    }

    // =====================================================================
    //  8. NEGATIVE / MALFORMED descriptors -> EMPTY, never a crash.
    //     The match is EXACT string equality with NO normalization/validation,
    //     so anything that is not byte-for-byte a declared descriptor returns {}.
    // =====================================================================
    {
        // (a) a well-formed descriptor that NOTHING in this class declares.
        ctx.check("absent_DDshort_empty", find_sig("(D)V").empty());
        ctx.check("absent_VtoI_empty", find_sig("(V)I").empty()); // V is not a legal arg, no match
        ctx.check("absent_long_arglist_empty", find_sig("(IIIIIIIIII)I").empty());

        // (b) the empty descriptor.
        ctx.check("empty_descriptor_empty", find_sig("").empty());

        // (c) whitespace-only / whitespace-padded (no trimming).
        ctx.check("space_only_empty", find_sig(" ").empty());
        ctx.check("padded_II_leading_space_empty", find_sig(" (I)I").empty());
        ctx.check("padded_II_trailing_space_empty", find_sig("(I)I ").empty());
        ctx.check("padded_II_inner_space_empty", find_sig("(I) I").empty());

        // (d) near-miss: right shape, wrong type (no method has it).
        ctx.check("nearmiss_ItoF_empty", find_sig("(I)F").empty());
        ctx.check("nearmiss_JtoI_empty", find_sig("(J)I").empty());
        ctx.check("nearmiss_DtoF_empty", find_sig("(D)F").empty());

        // (e) lowercase type chars (descriptors are case-sensitive uppercase).
        ctx.check("lowercase_i_empty", find_sig("(i)i").empty());
        ctx.check("lowercase_j_empty", find_sig("(j)j").empty());

        // (f) structurally malformed: missing parens / unbalanced.
        ctx.check("no_parens_II_empty", find_sig("II").empty());
        ctx.check("no_open_paren_empty", find_sig("I)I").empty());
        ctx.check("no_close_paren_empty", find_sig("(II").empty());
        ctx.check("only_open_paren_empty", find_sig("(").empty());
        ctx.check("only_close_paren_empty", find_sig(")").empty());
        ctx.check("garbage_empty", find_sig("@#$%^&*").empty());

        // (g) a METHOD NAME passed where a descriptor is expected -> no match.
        ctx.check("name_not_descriptor_empty", find_sig("f").empty());
        ctx.check("name_uniqueVoid_not_descriptor_empty", find_sig("uniqueVoid").empty());

        // (h) a valid descriptor with trailing junk after the return type.
        ctx.check("trailing_junk_empty", find_sig("(I)IX").empty());
        ctx.check("trailing_desc_empty", find_sig("(I)I(I)I").empty());

        // (i) a truncated reference descriptor (missing terminating ';').
        ctx.check("truncated_objarg_empty", find_sig("(Ljava/lang/String)I").empty());
        ctx.check("missing_semicolon_ret_empty",
                  find_sig("(Ljava/lang/String;)Ljava/lang/String").empty());

        // (j) dotted (source) form instead of slashed (internal) form.
        ctx.check("dotted_form_empty",
                  find_sig("(Ljava.lang.String;)Ljava.lang.String;").empty());

        // (k) a descriptor that is valid for a method on a DIFFERENT class
        //     (java.lang.String#length ()I would match retI by descriptor, so use
        //     a String-specific descriptor that FindMethodsBySig does not declare).
        ctx.check("foreign_method_desc_empty", find_sig("(II)Ljava/lang/String;").empty());
    }

    // =====================================================================
    //  9. UNREGISTERED wrapper type -> empty (type_to_class_map miss), for both
    //     a normally-matching descriptor and a no-arg one.
    // =====================================================================
    {
        const name_list u1{ vmhook::find_methods_by_signature<fmbs_unregistered>("(I)I") };
        ctx.check("unregistered_type_II_empty", u1.empty());
        const name_list u2{ vmhook::find_methods_by_signature<fmbs_unregistered>("()V") };
        ctx.check("unregistered_type_V_empty", u2.empty());
        const name_list u3{ vmhook::find_methods_by_signature<fmbs_unregistered>("") };
        ctx.check("unregistered_type_empty_desc_empty", u3.empty());
    }

    // =====================================================================
    // 10. DETERMINISM: calling find twice for the same descriptor yields the
    //     same multiset (pure read, no side effects on the enumeration).
    // =====================================================================
    {
        const name_list a{ find_sig("(I)I") };
        const name_list b{ find_sig("(I)I") };
        ctx.check("II_repeatable_same_multiset", same_multiset(a, b));
        const name_list va{ find_sig("()V") };
        const name_list vb{ find_sig("()V") };
        ctx.check("V_repeatable_same_size", va.size() == vb.size());
        ctx.check("V_repeatable_same_multiset", same_multiset(va, vb));
    }

    // =====================================================================
    // 11. LIVE post-dispatch STABILITY.  Snapshot the result sets, drive REAL
    //     bytecode through f/g/arr/sf/sUnique (invokevirtual + invokestatic,
    //     which also makes those methods candidates for the JIT), then RE-RUN
    //     find and assert the sets are byte-identical.  This proves the
    //     _methods enumeration reflects live JVM state and that calling /
    //     compiling a method does not add, drop, or reorder entries.
    // =====================================================================
    {
        const name_list before_ii{ find_sig("(I)I") };
        const name_list before_jj{ find_sig("(J)J") };
        const name_list before_iii{ find_sig("(II)I") };
        const name_list before_arr{ find_sig("([I)[I") };
        const name_list before_jjj{ find_sig("(JJ)J") };
        const name_list before_vv{ find_sig("()V") };
        const std::size_t before_total{ vmhook::get_class_methods<fmbs>().size() };

        const bool probe_done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    fmbs::set_done(false);
                }
                fmbs::set_go(value);
            },
            []() { return fmbs::get_done(); }) };

        ctx.check("probe_completed", probe_done);

        if (probe_done)
        {
            // The probe really executed its bytecode (witness fields written).
            ctx.check("probe_wFInt_is_8", fmbs::get_wFInt() == 8);          // f(7)=8
            ctx.check("probe_wFLong_is_12", fmbs::get_wFLong() == 12);      // f(11L)=12
            ctx.check("probe_wGII_is_7", fmbs::get_wGII() == 7);            // g(3,4)=7
            ctx.check("probe_wArrLen_is_3", fmbs::get_wArrLen() == 3);      // arr({1,2,3}).length
            ctx.check("probe_wSfInt_is_18", fmbs::get_wSfInt() == 18);      // sf(9)=18
            ctx.check("probe_wSUnique_is_5", fmbs::get_wSUnique() == 5);    // sUnique(2,3)=5

            // Re-enumerate AFTER live dispatch/JIT: every set is unchanged.
            const name_list after_ii{ find_sig("(I)I") };
            const name_list after_jj{ find_sig("(J)J") };
            const name_list after_iii{ find_sig("(II)I") };
            const name_list after_arr{ find_sig("([I)[I") };
            const name_list after_jjj{ find_sig("(JJ)J") };
            const name_list after_vv{ find_sig("()V") };
            const std::size_t after_total{ vmhook::get_class_methods<fmbs>().size() };

            ctx.check("post_dispatch_II_stable", same_multiset(before_ii, after_ii));
            ctx.check("post_dispatch_II_still_f_sf",
                      same_multiset(after_ii, name_list{ "f", "sf" }));
            ctx.check("post_dispatch_JJ_stable", same_multiset(before_jj, after_jj));
            ctx.check("post_dispatch_JJ_still_singleton_f",
                      after_jj.size() == 1 && after_jj.front() == "f");
            ctx.check("post_dispatch_III_stable", same_multiset(before_iii, after_iii));
            ctx.check("post_dispatch_arr_stable", same_multiset(before_arr, after_arr));
            ctx.check("post_dispatch_JJJ_stable", same_multiset(before_jjj, after_jjj));
            ctx.check("post_dispatch_V_size_stable", before_vv.size() == after_vv.size());
            ctx.check("post_dispatch_V_stable", same_multiset(before_vv, after_vv));
            ctx.check("post_dispatch_total_count_stable", before_total == after_total);
        }
    }

    // =====================================================================
    // 12. by-NAME class resolution path: find_methods_by_signature resolves the
    //     klass through the registered wrapper, but get_class_methods(name)
    //     resolves the SAME klass by internal name -- the descriptor multiset
    //     it reports must match what find returns, proving find is anchored to
    //     the right klass.  (A final cross-substrate consistency check.)
    // =====================================================================
    {
        const pair_list by_name{ vmhook::get_class_methods(CLASS_NAME) };
        ctx.check("by_name_nonempty", !by_name.empty());
        ctx.check("by_name_II_count_matches_find",
                  count_descriptor(by_name, "(I)I") == find_sig("(I)I").size());
        ctx.check("by_name_JJ_count_matches_find",
                  count_descriptor(by_name, "(J)J") == find_sig("(J)J").size());
        ctx.check("by_name_strdesc_count_matches_find",
                  count_descriptor(by_name, "(Ljava/lang/String;)Ljava/lang/String;")
                      == find_sig("(Ljava/lang/String;)Ljava/lang/String;").size());
    }
}
