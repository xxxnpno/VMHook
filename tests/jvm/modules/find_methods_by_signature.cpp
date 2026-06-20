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
//                          (..String;)..Object; {boxStr} ; ()..String; {name}
//   ARRAYS               : ([I)[I {arr} ; ([[I)[[I {arr2} ; ([L..String;)[L..String; {arrStr}
//   ARRAY ELEM-TYPE      : ([J)[J {arrJ,sArrJ} ; ([D)[D {arrD} ; ([Z)[Z {arrZ} ; ([B)[B {arrB}
//   ARRAY DIM=3          : ([[[I)[[[I {arr3}  (dimension-count discriminator)
//   VARARGS              : ([I)I {sumArr,sVararg}  (T... compiles to [T, no vararg marker)
//   REF (non-String)     : (Lj.l.Object;)Lj.l.Object; {idObj} ; (Lj.l.String;Lj.l.Object;)V {twoRef}
//   MULTI-SLOT / WIDE    : (IJD)D {mix} ; (JJ)J {sUnique} ; (JD)V {wideVoid}
//   MANY-ARG             : (IJDLjava/lang/String;[IZ)V {many} ; (IIIIII)I {manyR}
//   STATIC == INSTANCE   : (I)I returns BOTH instance f AND static sf
//   N-WAY (N=3)          : (D)I returns ALL of { tri1, tri2, tri3 } (2 instance + 1 static)
//
// What it proves, angle by angle:
//   - find returns the FULL match SET (not just the first): (I)I -> {f, sf},
//     String-desc -> {f, sf}, AND (D)I -> {tri1, tri2, tri3} (N=3); size AND
//     membership asserted, order-independent.
//   - genuinely-unique descriptors resolve to exactly their one method (incl. the
//     task-named shapes (JD)V, (IJDLjava/lang/String;[IZ)V, (IIIIII)I,
//     (..String;)..Object;, and ()..String;).
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
    // ...and the newly-added shapes (task-named: wide-void, many-arg, String->Object,
    // no-arg String, and the three-way (D)I set).
    ctx.check("substrate_has_wideVoid_JDV", has_pair(all_methods, "wideVoid", "(JD)V"));
    ctx.check("substrate_has_many_V",
              has_pair(all_methods, "many", "(IJDLjava/lang/String;[IZ)V"));
    ctx.check("substrate_has_manyR_6I", has_pair(all_methods, "manyR", "(IIIIII)I"));
    ctx.check("substrate_has_boxStr_SO",
              has_pair(all_methods, "boxStr", "(Ljava/lang/String;)Ljava/lang/Object;"));
    ctx.check("substrate_has_name_S", has_pair(all_methods, "name", "()Ljava/lang/String;"));
    ctx.check("substrate_has_tri1_DI", has_pair(all_methods, "tri1", "(D)I"));
    ctx.check("substrate_has_tri2_DI", has_pair(all_methods, "tri2", "(D)I"));
    ctx.check("substrate_has_tri3_DI", has_pair(all_methods, "tri3", "(D)I"));
    // ...and the array element-type / dimension / varargs / reference shapes added
    // for this deepening pass (all verified with `javap -s` on JDK 8/11/17/21).
    ctx.check("substrate_has_arrJ_aJ",  has_pair(all_methods, "arrJ", "([J)[J"));
    ctx.check("substrate_has_sArrJ_aJ", has_pair(all_methods, "sArrJ", "([J)[J"));
    ctx.check("substrate_has_arrD_aD",  has_pair(all_methods, "arrD", "([D)[D"));
    ctx.check("substrate_has_arrZ_aZ",  has_pair(all_methods, "arrZ", "([Z)[Z"));
    ctx.check("substrate_has_arrB_aB",  has_pair(all_methods, "arrB", "([B)[B"));
    ctx.check("substrate_has_arr3_a3I", has_pair(all_methods, "arr3", "([[[I)[[[I"));
    ctx.check("substrate_has_sumArr_aII",  has_pair(all_methods, "sumArr", "([I)I"));
    ctx.check("substrate_has_sVararg_aII", has_pair(all_methods, "sVararg", "([I)I"));
    ctx.check("substrate_has_idObj_OO",
              has_pair(all_methods, "idObj", "(Ljava/lang/Object;)Ljava/lang/Object;"));
    ctx.check("substrate_has_twoRef_SOV",
              has_pair(all_methods, "twoRef", "(Ljava/lang/String;Ljava/lang/Object;)V"));
    // ...and the batch-20 deepening shapes: 2-D reference array, interface-typed
    // reference, the class's OWN type, the narrow/wide no-arg returns, and the
    // four-way (F)I set (all verified by reasoning the JVM descriptor grammar; the
    // descriptors are JDK-stable internal-form strings).
    ctx.check("substrate_has_arrStr2_a2S",
              has_pair(all_methods, "arrStr2",
                       "([[Ljava/lang/String;)[[Ljava/lang/String;"));
    ctx.check("substrate_has_seq_CSCS",
              has_pair(all_methods, "seq",
                       "(Ljava/lang/CharSequence;)Ljava/lang/CharSequence;"));
    ctx.check("substrate_has_self_TT",
              has_pair(all_methods, "self",
                       "(Lvmhook/fixtures/FindMethodsBySig;)Lvmhook/fixtures/FindMethodsBySig;"));
    ctx.check("substrate_has_retF_F", has_pair(all_methods, "retF", "()F"));
    ctx.check("substrate_has_retD_D", has_pair(all_methods, "retD", "()D"));
    ctx.check("substrate_has_retZ_Z", has_pair(all_methods, "retZ", "()Z"));
    ctx.check("substrate_has_retB_B", has_pair(all_methods, "retB", "()B"));
    ctx.check("substrate_has_retC_C", has_pair(all_methods, "retC", "()C"));
    ctx.check("substrate_has_retS_S", has_pair(all_methods, "retS", "()S"));
    ctx.check("substrate_has_q1_FI", has_pair(all_methods, "q1", "(F)I"));
    ctx.check("substrate_has_q2_FI", has_pair(all_methods, "q2", "(F)I"));
    ctx.check("substrate_has_q3_FI", has_pair(all_methods, "q3", "(F)I"));
    ctx.check("substrate_has_q4_FI", has_pair(all_methods, "q4", "(F)I"));

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
        // REFERENCE-RETURN-TYPE discrimination: ()Ljava/lang/Object; (makeObj) and
        // ()Ljava/lang/String; (name) share the no-arg shape but differ in return
        // type, so neither set may contain the other's method.
        ctx.check("noargObj_excludes_name", !contains(obj, "name"));
        ctx.check("noargObj_excludes_f", !contains(obj, "f"));
    }

    // =====================================================================
    //  5b. TASK-NAMED descriptor shapes not covered above, each a genuine
    //      SINGLETON -> exactly its one method:
    //        (JD)V                        wide args (long+double), VOID return
    //        (IJDLjava/lang/String;[IZ)V  many-arg, all slot-widths, VOID return
    //        (IIIIII)I                    many-arg, all single-slot, INT return
    //        (Ljava/lang/String;)Ljava/lang/Object;  String in -> Object out
    //        ()Ljava/lang/String;         no-arg String return (DECLARED name(),
    //                                     NOT the inherited Object#toString())
    // =====================================================================
    {
        // (JD)V -- two WIDE args, VOID return.  Distinct from (IJD)D {mix} and
        // from every no-arg ()V member.
        const name_list jdv{ find_sig("(JD)V") };
        ctx.check("JDV_size_1", jdv.size() == 1);
        ctx.check("JDV_is_wideVoid", jdv.size() == 1 && jdv.front() == "wideVoid");
        ctx.check("JDV_excludes_mix", !contains(jdv, "mix"));   // mix is (IJD)D
        ctx.check("JDV_excludes_f", !contains(jdv, "f"));       // f() is ()V

        // (IJDLjava/lang/String;[IZ)V -- 6-arg, all slot-width classes, VOID.
        const name_list manyV{ find_sig("(IJDLjava/lang/String;[IZ)V") };
        ctx.check("manyV_size_1", manyV.size() == 1);
        ctx.check("manyV_is_many", manyV.size() == 1 && manyV.front() == "many");

        // (IIIIII)I -- 6 single-slot ints, INT return.  A genuinely-present long
        // arg list (the (IIIIIIIIII)I 10-arg negative below stays absent).
        const name_list manyR{ find_sig("(IIIIII)I") };
        ctx.check("manyR_size_1", manyR.size() == 1);
        ctx.check("manyR_is_manyR", manyR.size() == 1 && manyR.front() == "manyR");
        // ARITY discrimination at high arity: (IIIIII)I != (II)I {g} and != (I)I.
        ctx.check("manyR_excludes_g", !contains(manyR, "g"));

        // (Ljava/lang/String;)Ljava/lang/Object; -- REFERENCE arg, DIFFERENT
        // reference return.  Must NOT be confused with the String->String set
        // { f, sf } that shares the arg type but differs in return type.
        const name_list so{ find_sig("(Ljava/lang/String;)Ljava/lang/Object;") };
        ctx.check("strObj_size_1", so.size() == 1);
        ctx.check("strObj_is_boxStr", so.size() == 1 && so.front() == "boxStr");
        ctx.check("strObj_excludes_f", !contains(so, "f"));    // f(String) is ..)..String;
        ctx.check("strObj_excludes_sf", !contains(so, "sf"));
        // ...and the String->String set must NOT contain boxStr (return differs).
        const name_list strstr{ find_sig("(Ljava/lang/String;)Ljava/lang/String;") };
        ctx.check("strStr_excludes_boxStr", !contains(strstr, "boxStr"));

        // ()Ljava/lang/String; -- a no-arg String return.  Object#toString() has
        // this EXACT descriptor, but find walks DECLARED methods only, so the
        // result is the declared name() and NEVER the inherited toString.
        const name_list ns{ find_sig("()Ljava/lang/String;") };
        ctx.check("noargStr_size_1", ns.size() == 1);
        ctx.check("noargStr_is_name", ns.size() == 1 && ns.front() == "name");
        ctx.check("noargStr_excludes_toString", !contains(ns, "toString"));
        ctx.check("noargStr_excludes_makeObj", !contains(ns, "makeObj")); // ()Lj.l.Object;
    }

    // =====================================================================
    //  5c. THREE-WAY shared descriptor (D)I -> { tri1, tri2, tri3 }.  This
    //      pushes the headline "return ALL matches, not just the first" past
    //      multiplicity 2 to N=3, and proves two INSTANCE methods (tri1, tri2)
    //      and one STATIC method (tri3) co-enumerate on a single descriptor
    //      (the walk ignores JVM_ACC_STATIC).
    // =====================================================================
    {
        const name_list di{ find_sig("(D)I") };
        ctx.check("DI_size_3", di.size() == 3);
        ctx.check("DI_has_tri1", contains(di, "tri1"));
        ctx.check("DI_has_tri2", contains(di, "tri2"));
        ctx.check("DI_has_tri3", contains(di, "tri3"));
        ctx.check("DI_is_exactly_tri123",
                  same_multiset(di, name_list{ "tri1", "tri2", "tri3" }));
        // Each name appears exactly once (distinct name+descriptor per method).
        ctx.check("DI_tri1_once", count_name(di, "tri1") == 1);
        ctx.check("DI_tri2_once", count_name(di, "tri2") == 1);
        ctx.check("DI_tri3_once", count_name(di, "tri3") == 1);
        // (D)I must not bleed into the (D)D {dfn} set (same arg, different return).
        ctx.check("DI_excludes_dfn", !contains(di, "dfn"));
        const name_list ddd{ find_sig("(D)D") };
        ctx.check("DD_excludes_tri1", !contains(ddd, "tri1"));
    }

    // =====================================================================
    //  5d. ARRAY ELEMENT-TYPE discrimination.  An array descriptor is a run of
    //      '[' followed by the element descriptor, so the ELEMENT TYPE is a
    //      discriminator: ([J)[J, ([D)[D, ([Z)[Z, ([B)[B are four distinct
    //      arrays even though they share the 1-D, in==out shape.  ([J)[J also
    //      SHARES across a static (sArrJ) and an instance (arrJ) method -- the
    //      headline full-set guarantee, on a WIDE-element array this time.
    // =====================================================================
    {
        // ([J)[J -- the SHARED wide-element-array set { arrJ, sArrJ }.
        const name_list aj{ find_sig("([J)[J") };
        ctx.check("arrJ_size_2", aj.size() == 2);
        ctx.check("arrJ_has_arrJ", contains(aj, "arrJ"));
        ctx.check("arrJ_has_sArrJ", contains(aj, "sArrJ"));
        ctx.check("arrJ_is_exactly_arrJ_sArrJ",
                  same_multiset(aj, name_list{ "arrJ", "sArrJ" }));
        ctx.check("arrJ_arrJ_once", count_name(aj, "arrJ") == 1);
        ctx.check("arrJ_sArrJ_once", count_name(aj, "sArrJ") == 1);

        // ([D)[D / ([Z)[Z / ([B)[B -- each a distinct SINGLETON by element type.
        const name_list ad{ find_sig("([D)[D") };
        ctx.check("arrD_size_1", ad.size() == 1);
        ctx.check("arrD_is_arrD", ad.size() == 1 && ad.front() == "arrD");

        const name_list az{ find_sig("([Z)[Z") };
        ctx.check("arrZ_size_1", az.size() == 1);
        ctx.check("arrZ_is_arrZ", az.size() == 1 && az.front() == "arrZ");

        const name_list ab{ find_sig("([B)[B") };
        ctx.check("arrB_size_1", ab.size() == 1);
        ctx.check("arrB_is_arrB", ab.size() == 1 && ab.front() == "arrB");

        // Element-type CROSS-matches must NOT happen: the long-array set must not
        // contain the double-array method, and vice-versa.
        ctx.check("arrJ_excludes_arrD", !contains(aj, "arrD"));
        ctx.check("arrD_excludes_arrJ", !contains(ad, "arrJ"));
        ctx.check("arrZ_excludes_arrB", !contains(az, "arrB"));
        // The original int-array ([I)[I {arr} must NOT bleed into any of these.
        ctx.check("arrJ_excludes_arr", !contains(aj, "arr"));
        ctx.check("arrD_excludes_arr", !contains(ad, "arr"));
    }

    // =====================================================================
    //  5e. ARRAY DIMENSION-COUNT discrimination at depth 3.  ([[[I)[[[I {arr3}
    //      must be exactly { arr3 } and must NOT be reachable by the 1-D ([I)[I
    //      {arr} or the 2-D ([[I)[[I {arr2} descriptors (and vice-versa).  This
    //      extends the 1-D-vs-2-D cross-match negative already present to a 3-D
    //      apex, pinning that EACH extra '[' is a distinct descriptor.
    // =====================================================================
    {
        const name_list a3{ find_sig("([[[I)[[[I") };
        ctx.check("arr3_size_1", a3.size() == 1);
        ctx.check("arr3_is_arr3", a3.size() == 1 && a3.front() == "arr3");

        const name_list a1{ find_sig("([I)[I") };
        const name_list a2{ find_sig("([[I)[[I") };
        // Cross-dimension matrix: no descriptor of one depth matches another depth.
        ctx.check("arr1_excludes_arr3", !contains(a1, "arr3"));
        ctx.check("arr2_excludes_arr3", !contains(a2, "arr3"));
        ctx.check("arr3_excludes_arr", !contains(a3, "arr"));
        ctx.check("arr3_excludes_arr2", !contains(a3, "arr2"));
        // A mismatched in/out dimension (2-D in, 1-D out) is declared by NOTHING.
        ctx.check("arr_2in_1out_empty", find_sig("([[I)[I").empty());
        ctx.check("arr_1in_2out_empty", find_sig("([I)[[I").empty());
        ctx.check("arr_3in_2out_empty", find_sig("([[[I)[[I").empty());
    }

    // =====================================================================
    //  5f. VARARGS is a plain ARRAY descriptor.  A `T... xs` parameter compiles
    //      to a `[T` descriptor with NO vararg marker, so ([I)I -- the descriptor
    //      of both sumArr(int...) and the static sVararg(int...) -- returns the
    //      full set { sumArr, sVararg }.  This proves (a) varargs carries no
    //      special descriptor bit, (b) a primitive-array ARG with a SCALAR return
    //      is distinct from ([I)[I {arr} (array return) and from (I)I {f, sf}
    //      (scalar arg), and (c) a static varargs co-enumerates with an instance.
    // =====================================================================
    {
        const name_list aii{ find_sig("([I)I") };
        ctx.check("varargAII_size_2", aii.size() == 2);
        ctx.check("varargAII_has_sumArr", contains(aii, "sumArr"));
        ctx.check("varargAII_has_sVararg", contains(aii, "sVararg"));
        ctx.check("varargAII_is_exactly_sumArr_sVararg",
                  same_multiset(aii, name_list{ "sumArr", "sVararg" }));
        // The array-arg/scalar-return shape must NOT collide with the
        // array-arg/array-return {arr} or the scalar-arg/scalar-return {f, sf}.
        ctx.check("varargAII_excludes_arr", !contains(aii, "arr"));   // arr is ([I)[I
        ctx.check("varargAII_excludes_f", !contains(aii, "f"));       // f is (I)I
        ctx.check("varargAII_excludes_sf", !contains(aii, "sf"));     // sf is (I)I
        // Reverse: the (I)I scalar-arg set must NOT contain the varargs methods.
        const name_list ii{ find_sig("(I)I") };
        ctx.check("II_excludes_sumArr", !contains(ii, "sumArr"));
        // ...and ([I)[I {arr} must NOT contain the varargs methods either.
        const name_list arr{ find_sig("([I)[I") };
        ctx.check("arrII_excludes_sumArr", !contains(arr, "sumArr"));
    }

    // =====================================================================
    //  5g. REFERENCE-TYPE discrimination beyond String.  The L...; tag matches
    //      the EXACT internal class name, so (Ljava/lang/Object;)Ljava/lang/Object;
    //      {idObj} is a distinct match from the String->String set {f, sf} and the
    //      String->Object {boxStr}.  A two-reference-arg method
    //      (Ljava/lang/String;Ljava/lang/Object;)V {twoRef} proves consecutive
    //      reference args parse as separate slots and is a unique singleton.
    // =====================================================================
    {
        const name_list oo{ find_sig("(Ljava/lang/Object;)Ljava/lang/Object;") };
        ctx.check("objObj_size_1", oo.size() == 1);
        ctx.check("objObj_is_idObj", oo.size() == 1 && oo.front() == "idObj");
        // Object->Object must NOT contain the String->String or String->Object set.
        ctx.check("objObj_excludes_f", !contains(oo, "f"));
        ctx.check("objObj_excludes_sf", !contains(oo, "sf"));
        ctx.check("objObj_excludes_boxStr", !contains(oo, "boxStr"));
        // ...and the String->String set must NOT contain idObj (arg type differs).
        const name_list ss{ find_sig("(Ljava/lang/String;)Ljava/lang/String;") };
        ctx.check("strStr_excludes_idObj", !contains(ss, "idObj"));

        // (Ljava/lang/String;Ljava/lang/Object;)V -- two reference args, VOID.
        const name_list two{ find_sig("(Ljava/lang/String;Ljava/lang/Object;)V") };
        ctx.check("twoRef_size_1", two.size() == 1);
        ctx.check("twoRef_is_twoRef", two.size() == 1 && two.front() == "twoRef");
        // Arg-ORDER matters: the swapped (Object, String) descriptor is absent.
        ctx.check("twoRef_swapped_args_empty",
                  find_sig("(Ljava/lang/Object;Ljava/lang/String;)V").empty());
        // Dropping one ref arg (single String -> void) is absent (no such method).
        ctx.check("twoRef_one_arg_empty", find_sig("(Ljava/lang/String;)V").empty());
    }

    // =====================================================================
    //  5h. MULTI-DIMENSIONAL REFERENCE array + INTERFACE + SELF reference types.
    //      ([[Ljava/lang/String;)[[Ljava/lang/String; {arrStr2} nests the '[' run
    //      AHEAD of the L...; element tag and must be distinct from the 1-D
    //      reference array {arrStr} and the 2-D PRIMITIVE array {arr2}.  An
    //      interface-typed reference (CharSequence) and the class's OWN type are
    //      just reference class names: each a unique singleton, neither reachable
    //      by a sibling reference descriptor even when the Java types are related
    //      (String IS-A CharSequence, but the descriptor names the STATIC type).
    // =====================================================================
    {
        const name_list as2{ find_sig("([[Ljava/lang/String;)[[Ljava/lang/String;") };
        ctx.check("arrStr2_size_1", as2.size() == 1);
        ctx.check("arrStr2_is_arrStr2", as2.size() == 1 && as2.front() == "arrStr2");
        // 2-D reference array must NOT collide with 1-D reference array {arrStr}
        // nor with the 2-D PRIMITIVE int array {arr2}.
        const name_list as1{ find_sig("([Ljava/lang/String;)[Ljava/lang/String;") };
        ctx.check("arrStr2_excludes_arrStr", !contains(as2, "arrStr"));
        ctx.check("arrStr_excludes_arrStr2", !contains(as1, "arrStr2"));
        ctx.check("arr2_prim_excludes_arrStr2",
                  !contains(find_sig("([[I)[[I"), "arrStr2"));
        // A dimension/element mismatch on the reference array is declared by nothing.
        ctx.check("refarr_1in_2out_empty",
                  find_sig("([Ljava/lang/String;)[[Ljava/lang/String;").empty());

        // Interface-typed reference: (Ljava/lang/CharSequence;)Ljava/lang/CharSequence;
        // {seq}, a unique singleton, NOT reachable via String or Object descriptors.
        const name_list cs{ find_sig("(Ljava/lang/CharSequence;)Ljava/lang/CharSequence;") };
        ctx.check("seq_size_1", cs.size() == 1);
        ctx.check("seq_is_seq", cs.size() == 1 && cs.front() == "seq");
        ctx.check("strStr_excludes_seq",
                  !contains(find_sig("(Ljava/lang/String;)Ljava/lang/String;"), "seq"));
        ctx.check("objObj_excludes_seq",
                  !contains(find_sig("(Ljava/lang/Object;)Ljava/lang/Object;"), "seq"));
        // The supertype-substitution near-miss: a String passed where CharSequence
        // is declared has descriptor (Ljava/lang/String;)Ljava/lang/CharSequence;,
        // declared by nothing (descriptors are invariant in the static type).
        ctx.check("seq_str_in_cs_out_empty",
                  find_sig("(Ljava/lang/String;)Ljava/lang/CharSequence;").empty());

        // Self-referential class type: (Lvmhook/fixtures/FindMethodsBySig;).. {self}.
        const name_list slf{ find_sig(
            "(Lvmhook/fixtures/FindMethodsBySig;)Lvmhook/fixtures/FindMethodsBySig;") };
        ctx.check("self_size_1", slf.size() == 1);
        ctx.check("self_is_self", slf.size() == 1 && slf.front() == "self");
        // The dotted (source) form of the self type matches nothing.
        ctx.check("self_dotted_empty",
                  find_sig("(Lvmhook.fixtures.FindMethodsBySig;)Lvmhook.fixtures.FindMethodsBySig;")
                      .empty());
    }

    // =====================================================================
    //  5i. NO-ARG NARROW/WIDE RETURN-TYPE discrimination completed.  Section 5
    //      covered ()V/()I/()J/()Lj.l.Object;/()Lj.l.String;; here are the
    //      remaining primitive return tags on the empty arg list: ()F ()D ()Z
    //      ()B ()C ()S -- each a genuine singleton (no synthetic returns these
    //      with zero args on any JDK), proving the RETURN-TYPE tag fully
    //      partitions the no-arg methods.
    // =====================================================================
    {
        struct ret_case { const char* desc; const char* name; };
        const ret_case cases[]{
            { "()F", "retF" }, { "()D", "retD" }, { "()Z", "retZ" },
            { "()B", "retB" }, { "()C", "retC" }, { "()S", "retS" }
        };
        for (const ret_case& rc : cases)
        {
            const name_list r{ find_sig(rc.desc) };
            ctx.check(std::string{ "noarg_" } + rc.name + "_size_1", r.size() == 1);
            ctx.check(std::string{ "noarg_" } + rc.name + "_is_expected",
                      r.size() == 1 && r.front() == rc.name);
        }
        // Cross-exclusion: the wide ()D set must not contain the narrow-return
        // methods and vice-versa; ()F (retF) and ()D (retD) are NOT the same set.
        ctx.check("noargD_excludes_retF", !contains(find_sig("()D"), "retF"));
        ctx.check("noargF_excludes_retD", !contains(find_sig("()F"), "retD"));
        ctx.check("noargF_excludes_retI", !contains(find_sig("()F"), "retI"));
        // ()Z is single-byte tag Z, distinct from ()I (retI) and ()B (retB).
        ctx.check("noargZ_excludes_retI", !contains(find_sig("()Z"), "retI"));
        ctx.check("noargZ_excludes_retB", !contains(find_sig("()Z"), "retB"));
        ctx.check("noargB_excludes_retZ", !contains(find_sig("()B"), "retZ"));
    }

    // =====================================================================
    //  5j. FOUR-WAY shared descriptor (F)I -> { q1, q2, q3, q4 }.  Pushes the
    //      headline "return ALL matches" past N=3 to N=4: three instance methods
    //      (q1, q2, q3) and one static (q4) co-enumerate on a single descriptor.
    //      Also proves the FLOAT arg tag (F) discriminates against the DOUBLE arg
    //      (D)I {tri1,tri2,tri3} set -- same int return, different arg width.
    // =====================================================================
    {
        const name_list fi{ find_sig("(F)I") };
        ctx.check("FI_size_4", fi.size() == 4);
        ctx.check("FI_has_q1", contains(fi, "q1"));
        ctx.check("FI_has_q2", contains(fi, "q2"));
        ctx.check("FI_has_q3", contains(fi, "q3"));
        ctx.check("FI_has_q4", contains(fi, "q4"));
        ctx.check("FI_is_exactly_q1234",
                  same_multiset(fi, name_list{ "q1", "q2", "q3", "q4" }));
        ctx.check("FI_q1_once", count_name(fi, "q1") == 1);
        ctx.check("FI_q4_once", count_name(fi, "q4") == 1);
        // FLOAT-arg vs DOUBLE-arg discrimination: (F)I and (D)I are disjoint sets.
        ctx.check("FI_excludes_tri1", !contains(fi, "tri1"));
        ctx.check("DI_excludes_q1", !contains(find_sig("(D)I"), "q1"));
        // (F)I must not bleed into (F)F {ffn} (same arg, different return).
        ctx.check("FI_excludes_ffn", !contains(fi, "ffn"));
    }

    // =====================================================================
    //  5k. OVERLOADED-NAME cross-descriptor invariant.  The name `f` is declared
    //      on FOUR distinct descriptors -- (I)I, (J)J, ()V, and
    //      (Ljava/lang/String;)Ljava/lang/String; -- so `f` must appear in EXACTLY
    //      those four result sets and in NO other.  This pins that find partitions
    //      a single overloaded NAME correctly across descriptors (the inverse view
    //      of the multi-method-per-descriptor sets above).
    // =====================================================================
    {
        const char* f_carriers[]{ "(I)I", "(J)J", "()V",
                                  "(Ljava/lang/String;)Ljava/lang/String;" };
        bool f_in_all_carriers{ true };
        for (const char* d : f_carriers)
        {
            if (!contains(find_sig(d), "f")) { f_in_all_carriers = false; }
        }
        ctx.check("name_f_in_all_four_carrier_descriptors", f_in_all_carriers);
        // `f` must NOT appear under any descriptor it does not declare.
        const char* f_non_carriers[]{ "(II)I", "(I)J", "()I", "()J", "(D)I",
                                      "([I)[I", "(Ljava/lang/Object;)Ljava/lang/Object;",
                                      "(JD)V", "(F)I" };
        bool f_in_no_other{ true };
        for (const char* d : f_non_carriers)
        {
            if (contains(find_sig(d), "f")) { f_in_no_other = false; }
        }
        ctx.check("name_f_absent_from_non_carrier_descriptors", f_in_no_other);
        // The total number of result sets containing `f` across ALL declared
        // descriptors equals 4 (one per distinct descriptor `f` declares).
        name_list distinct_descs{};
        for (const std::pair<std::string, std::string>& m : all_methods)
        {
            if (std::find(distinct_descs.begin(), distinct_descs.end(), m.second)
                == distinct_descs.end())
            {
                distinct_descs.push_back(m.second);
            }
        }
        std::size_t sets_with_f{ 0 };
        for (const std::string& d : distinct_descs)
        {
            if (contains(find_sig(d.c_str()), "f")) { ++sets_with_f; }
        }
        ctx.check("name_f_appears_in_exactly_4_sets", sets_with_f == 4);
    }

    // =====================================================================
    //  5l. GLOBAL COMPLETENESS invariant: every declared method is reachable by
    //      EXACTLY its own descriptor, and nothing is lost or double-counted.  The
    //      sum over DISTINCT descriptors of find(d).size() must equal the total
    //      declared-method count, and the union of all find results (as a multiset
    //      of names) must equal the substrate's full name multiset.  This is the
    //      strongest single statement that find is a faithful partition of
    //      get_class_methods<W>() by descriptor.
    // =====================================================================
    {
        name_list distinct_descs{};
        for (const std::pair<std::string, std::string>& m : all_methods)
        {
            if (std::find(distinct_descs.begin(), distinct_descs.end(), m.second)
                == distinct_descs.end())
            {
                distinct_descs.push_back(m.second);
            }
        }
        std::size_t sum_of_sets{ 0 };
        name_list union_names{};
        for (const std::string& d : distinct_descs)
        {
            const name_list got{ find_sig(d.c_str()) };
            sum_of_sets += got.size();
            for (const std::string& nm : got) { union_names.push_back(nm); }
        }
        ctx.check("sum_of_descriptor_sets_equals_total",
                  sum_of_sets == all_methods.size());
        // The union of all find results must equal the substrate name multiset.
        name_list substrate_names{};
        substrate_names.reserve(all_methods.size());
        for (const std::pair<std::string, std::string>& m : all_methods)
        {
            substrate_names.push_back(m.first);
        }
        ctx.check("union_of_find_results_equals_substrate_names",
                  same_multiset(union_names, substrate_names));
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
            "()V", "()I", "()J", "()Ljava/lang/Object;",
            // task-named shapes added to the fixture (singletons + the 3-way set):
            "(JD)V", "(IJDLjava/lang/String;[IZ)V", "(IIIIII)I",
            "(Ljava/lang/String;)Ljava/lang/Object;", "()Ljava/lang/String;",
            "(D)I",
            // deepening-pass shapes: array element type / dimension / varargs /
            // reference (incl. the two SHARED sets ([J)[J and ([I)I):
            "([J)[J", "([D)[D", "([Z)[Z", "([B)[B", "([[[I)[[[I", "([I)I",
            "(Ljava/lang/Object;)Ljava/lang/Object;",
            "(Ljava/lang/String;Ljava/lang/Object;)V",
            // batch-20 shapes: 2-D ref array, interface ref, self ref, the narrow/
            // wide no-arg returns, and the four-way (F)I set:
            "([[Ljava/lang/String;)[[Ljava/lang/String;",
            "(Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",
            "(Lvmhook/fixtures/FindMethodsBySig;)Lvmhook/fixtures/FindMethodsBySig;",
            "()F", "()D", "()Z", "()B", "()C", "()S", "(F)I"
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
        const char* probe_descs[]{ "(I)I", "(J)J", "()V", "([I)[I", "(JJ)J",
                                   "(D)I", "(JD)V", "()Ljava/lang/String;",
                                   "(Ljava/lang/String;)Ljava/lang/Object;",
                                   "([J)[J", "([D)[D", "([[[I)[[[I", "([I)I",
                                   "(Ljava/lang/Object;)Ljava/lang/Object;",
                                   "(Ljava/lang/String;Ljava/lang/Object;)V",
                                   "(F)I", "()D", "()F",
                                   "([[Ljava/lang/String;)[[Ljava/lang/String;",
                                   "(Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",
                                   "(Lvmhook/fixtures/FindMethodsBySig;)Lvmhook/fixtures/FindMethodsBySig;" };
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
        // near-misses on the NEWLY-added shapes: (JD)V is declared, so (JD)I /
        // (JD)D / (JD)J (same args, wrong return) must be absent; (D)I and (D)D
        // are declared, so (D)J / (D)S (same arg, wrong return) must be absent.
        ctx.check("nearmiss_JDtoI_empty", find_sig("(JD)I").empty());
        ctx.check("nearmiss_JDtoD_empty", find_sig("(JD)D").empty());
        ctx.check("nearmiss_JDtoJ_empty", find_sig("(JD)J").empty());
        ctx.check("nearmiss_DtoJ_empty", find_sig("(D)J").empty());
        ctx.check("nearmiss_DtoS_empty", find_sig("(D)S").empty());
        // a near-miss many-arg: drop one int from (IIIIII)I {manyR} -> absent.
        ctx.check("nearmiss_5I_empty", find_sig("(IIIII)I").empty());
        // String->Object {boxStr} is declared; the swap Object->String is absent.
        ctx.check("nearmiss_ObjToStr_empty",
                  find_sig("(Ljava/lang/Object;)Ljava/lang/String;").empty());

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

        // (l) PREFIX / SUBSTRING non-matching.  The compare is whole-string ==,
        //     not startswith/contains, so neither a strict prefix of a real
        //     descriptor nor a real descriptor with extra tail matches.
        ctx.check("prefix_of_II_empty", find_sig("(I)").empty());        // prefix of (I)I
        ctx.check("II_with_tail_empty", find_sig("(I)II").empty());      // (I)I + tail 'I'
        ctx.check("prefix_of_arr_empty", find_sig("([I)[").empty());     // prefix of ([I)[I
        ctx.check("strdesc_prefix_empty",
                  find_sig("(Ljava/lang/String;)Ljava/lang/String").empty()); // missing ';'

        // (m) ARRAY element-type / wide-element near-misses.  ([J)[J {arrJ,sArrJ}
        //     and ([D)[D {arrD} are declared; a wrong element-tag or a wrong
        //     in/out element-tag pairing matches NOTHING.
        ctx.check("arrJ_wrong_ret_elem_empty", find_sig("([J)[I").empty()); // long[] in, int[] out
        ctx.check("arrD_wrong_ret_elem_empty", find_sig("([D)[J").empty()); // double[] in, long[] out
        ctx.check("arrF_undeclared_empty", find_sig("([F)[F").empty());     // no float[] method
        ctx.check("arrC_undeclared_empty", find_sig("([C)[C").empty());     // no char[] method
        ctx.check("arrS_undeclared_empty", find_sig("([S)[S").empty());     // no short[] method
        // varargs is ([I)I; the wide-element variant ([J)J / ([J)I is absent.
        ctx.check("vararg_wide_elem_JJ_empty", find_sig("([J)J").empty());
        ctx.check("vararg_wide_elem_JI_empty", find_sig("([J)I").empty());

        // (n) REFERENCE-class near-misses.  The L...; tag is matched by EXACT
        //     internal class name: a wrong class, an Object<->String swap, or a
        //     reference array of the wrong element class matches nothing.
        //     idObj is (Ljava/lang/Object;)Ljava/lang/Object;; boxStr is
        //     (Ljava/lang/String;)Ljava/lang/Object;.
        ctx.check("wrong_ref_class_empty",
                  find_sig("(Ljava/lang/Integer;)Ljava/lang/Integer;").empty());
        ctx.check("obj_to_str_empty",
                  find_sig("(Ljava/lang/Object;)Ljava/lang/String;").empty()); // not declared
        // arrStr is ([Ljava/lang/String;)[Ljava/lang/String;; an Object[] variant
        // (different element class) is declared by nothing.
        ctx.check("objarr_undeclared_empty",
                  find_sig("([Ljava/lang/Object;)[Ljava/lang/Object;").empty());
        // capital-L typo inside the package path (case-sensitive class name).
        ctx.check("capital_L_in_package_empty",
                  find_sig("(Ljava/Lang/String;)Ljava/Lang/String;").empty());
        // dotted form of a reference ARRAY descriptor (internal form uses '/').
        ctx.check("dotted_array_form_empty",
                  find_sig("([Ljava.lang.String;)[Ljava.lang.String;").empty());
        // a 1-D reference array where the declared method is 2-D ({arrStr2} is
        // ([[Ljava/lang/String;)..), and the reverse, must both be absent.
        ctx.check("refarr_2in_1out_empty",
                  find_sig("([[Ljava/lang/String;)[Ljava/lang/String;").empty());
        // CharSequence is declared as a scalar reference, not as an array element.
        ctx.check("seq_as_array_empty",
                  find_sig("([Ljava/lang/CharSequence;)[Ljava/lang/CharSequence;").empty());
        // The self type carried as an ARRAY element is declared by nothing.
        ctx.check("self_as_array_empty",
                  find_sig("([Lvmhook/fixtures/FindMethodsBySig;)[Lvmhook/fixtures/FindMethodsBySig;")
                      .empty());

        // (o) ARITY near-misses on the multi-arg shapes.  twoRef is
        //     (Ljava/lang/String;Ljava/lang/Object;)V; adding/removing a slot or
        //     swapping arg order is absent.  manyR is (IIIIII)I; a 7-int variant
        //     is absent (the 5-int one is checked above).
        ctx.check("manyR_7I_empty", find_sig("(IIIIIII)I").empty());
        ctx.check("twoRef_extra_arg_empty",
                  find_sig("(Ljava/lang/String;Ljava/lang/Object;I)V").empty());
        // mixed-slot near-miss: (IJD)D {mix} with the int dropped -> (JD)D absent
        // (already covered as (JD)D), and with args reordered -> (DJI)D absent.
        ctx.check("mix_reordered_empty", find_sig("(DJI)D").empty());
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
        // Two of the deepening shapes too: the SHARED wide-array set ([J)[J and
        // the SHARED varargs set ([I)I -- proving the new array/varargs entries
        // are as stable across dispatch/JIT as the scalar ones.
        const name_list before_aj{ find_sig("([J)[J") };
        const name_list before_aii{ find_sig("([I)I") };
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
            const name_list after_aj{ find_sig("([J)[J") };
            const name_list after_aii{ find_sig("([I)I") };
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
            ctx.check("post_dispatch_arrJ_stable", same_multiset(before_aj, after_aj));
            ctx.check("post_dispatch_arrJ_still_arrJ_sArrJ",
                      same_multiset(after_aj, name_list{ "arrJ", "sArrJ" }));
            ctx.check("post_dispatch_vararg_stable", same_multiset(before_aii, after_aii));
            ctx.check("post_dispatch_vararg_still_sumArr_sVararg",
                      same_multiset(after_aii, name_list{ "sumArr", "sVararg" }));
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
        // The 3-way (D)I set is the strongest by-name cross-check: the klass
        // resolved by internal name must report multiplicity 3 for (D)I, exactly
        // matching what find returns through the registered wrapper.
        ctx.check("by_name_DI_count_matches_find",
                  count_descriptor(by_name, "(D)I") == find_sig("(D)I").size());
        ctx.check("by_name_DI_count_is_3", count_descriptor(by_name, "(D)I") == 3);
        // The two SHARED deepening sets are equally strong by-name cross-checks:
        // ([J)[J {arrJ,sArrJ} and ([I)I {sumArr,sVararg} must each report
        // multiplicity 2 through the internal-name klass, matching find exactly.
        ctx.check("by_name_arrJ_count_matches_find",
                  count_descriptor(by_name, "([J)[J") == find_sig("([J)[J").size());
        ctx.check("by_name_arrJ_count_is_2", count_descriptor(by_name, "([J)[J") == 2);
        ctx.check("by_name_vararg_count_matches_find",
                  count_descriptor(by_name, "([I)I") == find_sig("([I)I").size());
        ctx.check("by_name_vararg_count_is_2", count_descriptor(by_name, "([I)I") == 2);
        ctx.check("by_name_arr3_count_matches_find",
                  count_descriptor(by_name, "([[[I)[[[I") == find_sig("([[[I)[[[I").size());
        // The four-way (F)I set is the strongest batch-20 by-name cross-check: the
        // internal-name klass must report multiplicity 4 for (F)I, exactly matching
        // what find returns through the registered wrapper.
        ctx.check("by_name_FI_count_matches_find",
                  count_descriptor(by_name, "(F)I") == find_sig("(F)I").size());
        ctx.check("by_name_FI_count_is_4", count_descriptor(by_name, "(F)I") == 4);
        // ...and the self-referential descriptor resolves identically by name.
        ctx.check("by_name_self_count_matches_find",
                  count_descriptor(by_name,
                      "(Lvmhook/fixtures/FindMethodsBySig;)Lvmhook/fixtures/FindMethodsBySig;")
                      == find_sig(
                          "(Lvmhook/fixtures/FindMethodsBySig;)Lvmhook/fixtures/FindMethodsBySig;")
                          .size());
    }
}
