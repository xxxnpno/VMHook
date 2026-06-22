// method_is_reference JVM test module  (feature area: methods)
//
// THE authority for method_proxy::is_reference() — the O(1) introspection
// accessor that reports whether a Java method's RETURN type is a reference
// (object / array: the descriptor char after ')' is 'L' or '[') versus a
// primitive (Z B S C I J F D) or void (V).  vmhook.hpp implements it by reading
// the character that follows the closing ')' of the cached signature_text:
//
//     auto is_reference() const noexcept -> bool
//     {
//         const auto close{ signature_text.find(')') };
//         if (close == npos || close + 1 >= signature_text.size()) return false;
//         const char ret{ signature_text[close + 1] };
//         return ret == 'L' || ret == '[';
//     }
//
// Because it reads ONLY the cached descriptor it needs NO live bytecode dispatch
// — the proxy is never call()'d, so no current_java_thread is required.  Every
// assertion below is therefore made straight from resolved proxies in the module
// body (the SINGLETON is fetched only so the INSTANCE get_method("name") path is
// exercised alongside the STATIC static_method("name") path).
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * is_reference() is TRUE for a String return, an Object return, and BOTH a
//     primitive array (int[] -> "()[I") and a reference array (String[] ->
//     "()[Ljava/lang/String;") return — i.e. every 'L'/'[' return descriptor.
//   * is_reference() is FALSE for EVERY primitive return (Z B S C I J F D) AND
//     for a void return (V is the close+1 == 'V' branch, not a reference).
//   * is_reference() is INDEPENDENT of static-ness: each instance method has a
//     static twin with the identical return descriptor and the two agree.
//   * is_reference() AGREES, method-for-method, with an INDEPENDENT oracle that
//     parses signature() by hand (the char after ')'), so the accessor is not
//     merely self-consistent but matches the descriptor the JVM reports.
//   * is_reference() tracks the SPECIFIC RESOLVED OVERLOAD, not the bare name:
//     dual(I)I (primitive) and dual(String)Object (reference) share the name
//     "dual" yet resolve — by EXACT explicit descriptor via get_method(name,sig)
//     / static_method(name,sig) — to proxies whose is_reference() differ.
//   * malformed / empty descriptors are handled without UB: a proxy built with
//     "" , "(" , "()" all report is_reference()==false (the npos / close+1>=size
//     guards), and a null-Method* proxy is safe (raw_method()==nullptr; any deref
//     is gated behind vmhook::hotspot::is_valid_pointer).
//   * EVERY reference return CLASS is swept on both paths: BOXED wrapper types
//     (Boolean..Double, and the headline java.lang.Void which is a REFERENCE
//     despite the word "void", opposite the primitive 'V'); USER types (a nested
//     class + the interface it implements + a user-type array); array element
//     kinds [Z..[D + Object[]; multi-dim [[I / [[[B and a 5-D / 7-deep array.
//   * signature() round-trips the EXACT canonical JVM descriptor for the array /
//     multi-dim / boxed returns (pins '[' depth + element char), and instance vs
//     static twins share that descriptor AND agree on is_reference().
//
// SAFETY: is_reference()/signature() touch only signature_text (a std::string),
// never the Method*; the one place this module reads the Method* (raw_method())
// is gated with vmhook::hotspot::is_valid_pointer.  No hooks are installed, so
// there is nothing to tear down.  All value_t / std::string extraction uses
// COPY-INITIALISATION (std::string s = mp->signature();) to stay MSVC-unambiguous.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.IsReference.
    //
    // Resolution-only: nothing here calls a Java method.  Accessors expose both
    // the STATIC path (static_method) and, via an acquired SINGLETON instance,
    // the INSTANCE path (get_method on an object).
    class isref : public vmhook::object<isref>
    {
    public:
        explicit isref(vmhook::oop_t instance) noexcept
            : vmhook::object<isref>{ instance }
        {
        }

        // ---- handshake (present per harness contract; is_reference() needs no
        //      dispatch, so these are only touched by the optional no-op probe) -
        static auto set_go(bool value) -> void  { static_field("go")->set(value); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }
        static auto get_done() -> bool           { return static_field("done")->get(); }

        // ---- resolve a STATIC-path proxy by name (name-only) ----------------
        static auto static_proxy(const char* name) -> std::optional<vmhook::method_proxy>
        {
            return static_method(name);
        }

        // ---- resolve a STATIC-path proxy by EXACT descriptor (pins overload) -
        static auto static_proxy(const char* name, const char* sig)
            -> std::optional<vmhook::method_proxy>
        {
            return static_method(name, sig);
        }

        // ---- acquire the published SINGLETON instance (reference-field decode;
        //      no live thread needed — mirrors field_static::acquire) ----------
        static auto acquire_singleton() -> std::unique_ptr<isref> { return static_field("SINGLETON")->get(); }
    };

    // Independent oracle: parse the JVM descriptor's RETURN type by hand and
    // decide reference-vs-not exactly the way the spec defines it, WITHOUT
    // calling is_reference().  Used to cross-check the accessor against the
    // descriptor the JVM actually reports.
    //   reference  <=> return descriptor (char after ')') is 'L' or '['
    //   primitive  <=> one of Z B S C I J F D
    //   void / V   => NOT a reference
    auto oracle_is_reference(std::string_view signature) -> bool
    {
        const std::size_t close{ signature.find(')') };
        if (close == std::string_view::npos || close + 1 >= signature.size())
        {
            return false;
        }
        const char ret{ signature[close + 1] };
        return ret == 'L' || ret == '[';
    }

    // The single return char after ')', or '\0' if the signature is malformed.
    // Purely for diagnostics ([INFO]) when an assertion is characterised.
    auto return_char(std::string_view signature) -> char
    {
        const std::size_t close{ signature.find(')') };
        if (close == std::string_view::npos || close + 1 >= signature.size())
        {
            return '\0';
        }
        return signature[close + 1];
    }

    // The trichotomy a future call()'s value_t would fall into, derived ONLY
    // from the descriptor (no call(), no value_t).  EXACTLY ONE of these three
    // is true for ANY descriptor, well-formed or not — they PARTITION the input:
    //   reference  : char after ')' is 'L' or '['  (value_t would carry an oop)
    //   primitive  : char after ')' is one of Z B S C I J F D (numeric value_t)
    //   void       : EVERYTHING ELSE — the literal 'V', a malformed / empty
    //                descriptor (no ')' / nothing after ')'), AND any unknown /
    //                garbage return char.  A future call() of any such descriptor
    //                produces value_t::is_void() (monostate), so "void" is the
    //                catch-all that makes the partition exhaustive.
    // These mirror, on the descriptor side, what value_t::is_reference() /
    // (numeric) / is_void() would report after a real dispatch — asserted here
    // WITHOUT calling, so the proxy is never call()'d.
    auto oracle_is_primitive(std::string_view signature) -> bool
    {
        const std::size_t close{ signature.find(')') };
        if (close == std::string_view::npos || close + 1 >= signature.size())
        {
            return false;
        }
        switch (signature[close + 1])
        {
        case 'Z': case 'B': case 'S': case 'C':
        case 'I': case 'J': case 'F': case 'D':
            return true;
        default:
            return false;
        }
    }

    // void = neither reference nor primitive (the partition's catch-all, so the
    // three legs are mutually exclusive AND exhaustive over EVERY descriptor).
    auto oracle_is_void(std::string_view signature) -> bool
    {
        return !oracle_is_reference(signature) && !oracle_is_primitive(signature);
    }

    // Descriptor-side analogue of value_t::is_string(): the return type is
    // EXACTLY java.lang.String.  A future call() of such a method would set
    // value_t::is_string() true; we assert the descriptor shape WITHOUT calling.
    auto oracle_returns_string(std::string_view signature) -> bool
    {
        const std::size_t close{ signature.find(')') };
        if (close == std::string_view::npos)
        {
            return false;
        }
        return signature.substr(close + 1) == "Ljava/lang/String;";
    }

    // One method's expectation: its name and the truth is_reference() must report.
    struct expectation
    {
        const char* name;
        bool        expect_reference;
    };

    // The primitive + void returners (is_reference() must be FALSE) and the
    // reference returners (TRUE).  Instance and static name sets are parallel.
    constexpr expectation k_instance_methods[]{
        { "retBool",        false },
        { "retByte",        false },
        { "retShort",       false },
        { "retChar",        false },
        { "retInt",         false },
        { "retLong",        false },
        { "retFloat",       false },
        { "retDouble",      false },
        { "retVoid",        false },   // V: not a reference
        { "retString",      true  },   // Ljava/lang/String;
        { "retObject",      true  },   // Ljava/lang/Object;
        { "retIntArray",    true  },   // [I
        { "retStringArray", true  },   // [Ljava/lang/String;
    };

    constexpr expectation k_static_methods[]{
        { "sRetBool",        false },
        { "sRetByte",        false },
        { "sRetShort",       false },
        { "sRetChar",        false },
        { "sRetInt",         false },
        { "sRetLong",        false },
        { "sRetFloat",       false },
        { "sRetDouble",      false },
        { "sRetVoid",        false },
        { "sRetString",      true  },
        { "sRetObject",      true  },
        { "sRetIntArray",    true  },
        { "sRetStringArray", true  },
    };

    // Every PRIMITIVE-ELEMENT array return (plus Object[]).  ALL are references
    // ('[' leads the return descriptor) even though the element is a primitive —
    // is_reference() is "is this a Java reference", and arrays always are.  This
    // is the deliberate "[I is true" semantic, swept across every element kind.
    constexpr expectation k_instance_primitive_arrays[]{
        { "retBoolArray",   true },   // [Z
        { "retByteArray",   true },   // [B
        { "retShortArray",  true },   // [S
        { "retCharArray",   true },   // [C
        { "retLongArray",   true },   // [J
        { "retFloatArray",  true },   // [F
        { "retDoubleArray", true },   // [D
        { "retObjectArray", true },   // [Ljava/lang/Object;
    };

    // Multi-dimensional arrays — leading '[' regardless of depth, so TRUE.
    constexpr expectation k_instance_multidim_arrays[]{
        { "retInt2DArray",    true },   // [[I
        { "retString2DArray", true },   // [[Ljava/lang/String;
        { "retByte3DArray",   true },   // [[[B
    };

    // Reference returns that are NOT String/Object: a concrete JDK type and an
    // interface type.  Both descriptors are 'L...;', so TRUE.  Only the
    // is_reference() truth and oracle-agreement are HARD here; the EXACT
    // descriptor text (java/util/List vs the erased return) is NOT asserted
    // verbatim because generic-return descriptors are spec-fixed but verbose —
    // the truth ('L' after ')') is the invariant.
    constexpr expectation k_instance_other_refs[]{
        { "retList",      true },   // Ljava/util/List;
        { "retInterface", true },   // Ljava/lang/CharSequence;
    };

    // STATIC twins of EVERY reference return kind (arrays, multi-dim, the
    // collection + interface).  is_reference() must be TRUE for each, agreeing
    // with the oracle, proving static-ness never changes the verdict for the
    // '[' / 'L' return descriptors — not just the scalars section 2 already
    // covered.
    constexpr expectation k_static_reference_returns[]{
        { "sRetBoolArray",   true },   // [Z
        { "sRetByteArray",   true },   // [B
        { "sRetShortArray",  true },   // [S
        { "sRetCharArray",   true },   // [C
        { "sRetLongArray",   true },   // [J
        { "sRetFloatArray",  true },   // [F
        { "sRetDoubleArray", true },   // [D
        { "sRetObjectArray", true },   // [Ljava/lang/Object;
        { "sRetInt2DArray",  true },   // [[I
        { "sRetByte3DArray", true },   // [[[B
        { "sRetList",        true },   // Ljava/util/List;
        { "sRetInterface",   true },   // Ljava/lang/CharSequence;
    };

    // BOXED wrapper-type returns: every boxed primitive is a Java REFERENCE
    // ('L...;'), so is_reference() is TRUE — INCLUDING java.lang.Void
    // ('()Ljava/lang/Void;'), which is the sharp contrast against the
    // PRIMITIVE void '()V' (FALSE).  The boxed-Void vs primitive-void pair is
    // the headline of this block: same English word "void", opposite verdict.
    constexpr expectation k_instance_boxed[]{
        { "retBoxedBool",   true },   // Ljava/lang/Boolean;
        { "retBoxedByte",   true },   // Ljava/lang/Byte;
        { "retBoxedShort",  true },   // Ljava/lang/Short;
        { "retBoxedChar",   true },   // Ljava/lang/Character;
        { "retBoxedInt",    true },   // Ljava/lang/Integer;
        { "retBoxedLong",   true },   // Ljava/lang/Long;
        { "retBoxedFloat",  true },   // Ljava/lang/Float;
        { "retBoxedDouble", true },   // Ljava/lang/Double;
        { "retBoxedVoid",   true },   // Ljava/lang/Void; — reference, NOT 'V'
    };

    constexpr expectation k_static_boxed[]{
        { "sRetBoxedBool",   true },
        { "sRetBoxedInt",    true },
        { "sRetBoxedLong",   true },
        { "sRetBoxedDouble", true },
        { "sRetBoxedChar",   true },
        { "sRetBoxedVoid",   true },   // Ljava/lang/Void; — reference, NOT 'V'
    };

    // USER-defined reference returns (a nested concrete class and the interface
    // it implements, plus an array of the user type and a 5-D primitive array).
    // Every descriptor is 'L...;' / '[...', so is_reference() is TRUE — the
    // verdict does NOT depend on the type being a JDK type, nor on array depth.
    constexpr expectation k_instance_user_refs[]{
        { "retBox",        true },   // Lvmhook/fixtures/IsReference$Box;
        { "retTagIface",   true },   // Lvmhook/fixtures/IsReference$Tag;
        { "retBoxArray",   true },   // [Lvmhook/fixtures/IsReference$Box;
        { "retInt5DArray", true },   // [[[[[I
    };

    constexpr expectation k_static_user_refs[]{
        { "sRetBox",      true },   // Lvmhook/fixtures/IsReference$Box;
        { "sRetTagIface", true },   // Lvmhook/fixtures/IsReference$Tag;
    };

    // NESTED-reference array returns: '[' + a 'L...;' element (interface / boxed /
    // boxed-Void / user / deep multi-dim-of-reference).  Every descriptor LEADS
    // with '[' so is_reference() is TRUE — the gap the earlier array sweeps left
    // (they covered primitive-element [Z..[D, Object[], String[], and one [[L).
    constexpr expectation k_instance_nested_ref_arrays[]{
        { "retInterfaceArray", true },   // [Ljava/lang/CharSequence;
        { "retBoxedIntArray",  true },   // [Ljava/lang/Integer;
        { "retBoxedVoidArray", true },   // [Ljava/lang/Void;
        { "retBox2DArray",     true },   // [[Lvmhook/fixtures/IsReference$Box;
        { "retString3DArray",  true },   // [[[Ljava/lang/String;
    };

    constexpr expectation k_static_nested_ref_arrays[]{
        { "sRetInterfaceArray", true },   // [Ljava/lang/CharSequence;
        { "sRetBoxedIntArray",  true },   // [Ljava/lang/Integer;
        { "sRetBox2DArray",     true },   // [[Lvmhook/fixtures/IsReference$Box;
        { "sRetString3DArray",  true },   // [[[Ljava/lang/String;
    };

    // Resolved-proxy EXACT-descriptor pins for the array / multi-dim returns.
    // signature() must round-trip the canonical JVM descriptor the resolution
    // reported — a stronger invariant than truth+oracle alone (it pins the
    // precise '[' depth and element char, version-stable across JDK 8..26).
    struct sig_pin
    {
        const char* name;
        const char* sig;
    };
    constexpr sig_pin k_instance_sig_pins[]{
        { "retBoolArray",     "()[Z" },
        { "retByteArray",     "()[B" },
        { "retShortArray",    "()[S" },
        { "retCharArray",     "()[C" },
        { "retIntArray",      "()[I" },
        { "retLongArray",     "()[J" },
        { "retFloatArray",    "()[F" },
        { "retDoubleArray",   "()[D" },
        { "retInt2DArray",    "()[[I" },
        { "retByte3DArray",   "()[[[B" },
        { "retInt5DArray",    "()[[[[[I" },
        { "retObjectArray",      "()[Ljava/lang/Object;" },
        { "retStringArray",      "()[Ljava/lang/String;" },
        { "retString2DArray",    "()[[Ljava/lang/String;" },
        { "retString3DArray",    "()[[[Ljava/lang/String;" },
        { "retInterfaceArray",   "()[Ljava/lang/CharSequence;" },
        { "retBoxedIntArray",    "()[Ljava/lang/Integer;" },
        { "retBoxedVoidArray",   "()[Ljava/lang/Void;" },
        { "retBox2DArray",       "()[[Lvmhook/fixtures/IsReference$Box;" },
        { "retInterface",        "()Ljava/lang/CharSequence;" },
        { "retList",             "()Ljava/util/List;" },
        { "retBox",              "()Lvmhook/fixtures/IsReference$Box;" },
        { "retTagIface",         "()Lvmhook/fixtures/IsReference$Tag;" },
        { "retString",        "()Ljava/lang/String;" },
        { "retObject",        "()Ljava/lang/Object;" },
        { "retBoxedInt",      "()Ljava/lang/Integer;" },
        { "retBoxedVoid",     "()Ljava/lang/Void;" },
        { "retVoid",          "()V" },
        { "retInt",           "()I" },
        { "retLong",          "()J" },
        { "retDouble",        "()D" },
        { "retBool",          "()Z" },
    };

    // PARAM-LIST RED HERRING: the parameter list carries 'L' / '[' but the
    // RETURN is a primitive or void.  is_reference() must verdict ONLY on the
    // char after ')', so every one of these is FALSE.  This is the case that
    // catches a parser that scanned the whole descriptor.
    constexpr expectation k_instance_param_red_herrings[]{
        { "takesString",      false },   // (Ljava/lang/String;)I
        { "takesIntArray",    false },   // ([I)I
        { "takesObjectArray", false },   // ([Ljava/lang/Object;)V
        { "takesMixed",       false },   // (Ljava/lang/String;[IJ)Z
    };

    // Run the three standard cross-checks (truth, oracle-agreement, oracle ==
    // expectation) for one resolved proxy.  Shared by every instance-path table
    // so a new return KIND is one table row, not a copied block.
    auto check_proxy(vmhook_test::context& ctx,
                     const std::string&    prefix,
                     const expectation&    e,
                     const vmhook::method_proxy& mp) -> void
    {
        const std::string sig{ mp.signature() };
        const bool        is_ref{ mp.is_reference() };

        ctx.check(prefix + "_is_reference_" + e.name, is_ref == e.expect_reference);
        ctx.check(prefix + "_is_reference_matches_signature_" + e.name,
                  is_ref == oracle_is_reference(sig));
        ctx.check(prefix + "_signature_oracle_expected_" + e.name,
                  oracle_is_reference(sig) == e.expect_reference);

        // Predicate invariant (HARD): reference / void / primitive are mutually
        // exclusive AND exhaustive for EVERY resolved descriptor.  is_reference()
        // is the reference leg; the other two are derived purely from the
        // descriptor.  Exactly one must be true.
        const bool is_void_ret{ oracle_is_void(sig) };
        const bool is_prim_ret{ oracle_is_primitive(sig) };
        ctx.check(prefix + "_trichotomy_exactly_one_" + e.name,
                  (is_ref ? 1 : 0) + (is_void_ret ? 1 : 0) + (is_prim_ret ? 1 : 0) == 1);
        // is_reference() and "void" can never both hold.
        ctx.check(prefix + "_reference_and_void_disjoint_" + e.name,
                  !(is_ref && is_void_ret));
        // is_reference() and "primitive" can never both hold.
        ctx.check(prefix + "_reference_and_primitive_disjoint_" + e.name,
                  !(is_ref && is_prim_ret));

        if (is_ref != e.expect_reference || is_ref != oracle_is_reference(sig))
        {
            const char rc{ return_char(sig) };
            ctx.record(std::string{ "[INFO] " } + prefix + " " + e.name + " signature='" + sig
                       + "' returnChar='" + (rc ? std::string(1, rc) : std::string{ "\\0" })
                       + "' is_reference=" + (is_ref ? "true" : "false")
                       + " expected=" + (e.expect_reference ? "true" : "false"));
        }
    }
}

VMHOOK_JVM_MODULE(method_is_reference)
{
    vmhook::register_class<isref>("vmhook/fixtures/IsReference");

    // =====================================================================
    //  0. Sanity: the class resolves and a known method resolves both ways.
    // =====================================================================
    ctx.check("isref_class_registered_static_method_resolves",
              isref::static_method("sRetInt").has_value());

    const auto singleton{ isref::acquire_singleton() };
    ctx.check("isref_singleton_acquired", singleton != nullptr);
    if (singleton)
    {
        ctx.check("isref_instance_method_resolves",
                  singleton->get_method("retInt").has_value());
    }

    // =====================================================================
    //  1. INSTANCE path: every return kind, asserted via SINGLETON->get_method.
    //     For each: is_reference() == expected truth, AND is_reference() agrees
    //     with the independent signature() oracle (so it matches the JVM's
    //     descriptor, not just itself).
    // =====================================================================
    if (singleton)
    {
        for (const expectation& e : k_instance_methods)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "inst_resolves_" } + e.name, mp.has_value());
            if (!mp)
            {
                continue;
            }

            // signature() returns std::string_view; construct a std::string from
            // it explicitly (string_view has no implicit std::string conversion).
            // The MSVC copy-init-not-brace-init caveat applies to value_t (the
            // variant call()/get() result), NOT to this string_view.
            const std::string sig{ mp->signature() };
            const bool is_ref{ mp->is_reference() };

            // (a) is_reference() reports the expected truth for this return kind.
            ctx.check(std::string{ "inst_is_reference_" } + e.name,
                      is_ref == e.expect_reference);

            // (b) is_reference() agrees with the hand-parsed descriptor oracle.
            ctx.check(std::string{ "inst_is_reference_matches_signature_" } + e.name,
                      is_ref == oracle_is_reference(sig));

            // (c) the oracle in turn matches the static expectation — pins down
            //     that the descriptor the JVM reports is the one we expect.
            ctx.check(std::string{ "inst_signature_oracle_expected_" } + e.name,
                      oracle_is_reference(sig) == e.expect_reference);

            // Characterise any surprise (descriptor + return char) without
            // hiding it — the assertions above still gate the run.
            if (is_ref != e.expect_reference || is_ref != oracle_is_reference(sig))
            {
                const char rc{ return_char(sig) };
                ctx.record(std::string{ "[INFO] inst " } + e.name + " signature='" + sig
                           + "' returnChar='" + (rc ? std::string(1, rc) : std::string{ "\\0" })
                           + "' is_reference=" + (is_ref ? "true" : "false")
                           + " expected=" + (e.expect_reference ? "true" : "false"));
            }
        }
    }

    // =====================================================================
    //  2. STATIC path: the parallel static twins via static_method("name").
    //     Same three cross-checks; proves is_reference() is independent of
    //     static-ness (each twin shares the instance method's return descriptor).
    // =====================================================================
    for (const expectation& e : k_static_methods)
    {
        const auto mp{ isref::static_proxy(e.name) };
        ctx.check(std::string{ "static_resolves_" } + e.name, mp.has_value());
        if (!mp)
        {
            continue;
        }

        const std::string sig{ mp->signature() };
        const bool is_ref{ mp->is_reference() };

        ctx.check(std::string{ "static_is_reference_" } + e.name,
                  is_ref == e.expect_reference);
        ctx.check(std::string{ "static_is_reference_matches_signature_" } + e.name,
                  is_ref == oracle_is_reference(sig));
        ctx.check(std::string{ "static_signature_oracle_expected_" } + e.name,
                  oracle_is_reference(sig) == e.expect_reference);

        if (is_ref != e.expect_reference || is_ref != oracle_is_reference(sig))
        {
            const char rc{ return_char(sig) };
            ctx.record(std::string{ "[INFO] static " } + e.name + " signature='" + sig
                       + "' returnChar='" + (rc ? std::string(1, rc) : std::string{ "\\0" })
                       + "' is_reference=" + (is_ref ? "true" : "false")
                       + " expected=" + (e.expect_reference ? "true" : "false"));
        }
    }

    // =====================================================================
    //  3. INSTANCE vs STATIC parity: a String twin and a primitive twin must
    //     report identical is_reference() across the two resolution paths.
    // =====================================================================
    if (singleton)
    {
        const auto inst_str{ singleton->get_method("retString") };
        const auto stat_str{ isref::static_proxy("sRetString") };
        if (inst_str && stat_str)
        {
            ctx.check("parity_string_both_reference",
                      inst_str->is_reference() == true && stat_str->is_reference() == true);
            ctx.check("parity_string_agree",
                      inst_str->is_reference() == stat_str->is_reference());
        }

        const auto inst_int{ singleton->get_method("retInt") };
        const auto stat_int{ isref::static_proxy("sRetInt") };
        if (inst_int && stat_int)
        {
            ctx.check("parity_int_both_not_reference",
                      inst_int->is_reference() == false && stat_int->is_reference() == false);
            ctx.check("parity_int_agree",
                      inst_int->is_reference() == stat_int->is_reference());
        }
    }

    // =====================================================================
    //  4. OVERLOAD DISAMBIGUATION (the headline): the SAME name "dual" carries a
    //     primitive-return and a reference-return overload, selectable ONLY by
    //     EXACT descriptor.  is_reference() must track the RESOLVED overload, not
    //     the name.  Done on both the instance and the static path.
    // =====================================================================
    {
        // ---- instance: dual(I)I  vs  dual(String)Object ----
        if (singleton)
        {
            const auto dual_prim{ singleton->get_method("dual", "(I)I") };
            const auto dual_ref{ singleton->get_method("dual", "(Ljava/lang/String;)Ljava/lang/Object;") };

            ctx.check("inst_dual_primitive_resolves", dual_prim.has_value());
            ctx.check("inst_dual_reference_resolves", dual_ref.has_value());

            if (dual_prim)
            {
                const std::string sig{ dual_prim->signature() };
                ctx.check("inst_dual_primitive_is_reference_false", dual_prim->is_reference() == false);
                ctx.check("inst_dual_primitive_signature_I", sig == "(I)I");
                ctx.check("inst_dual_primitive_oracle_false", oracle_is_reference(sig) == false);
            }
            if (dual_ref)
            {
                const std::string sig{ dual_ref->signature() };
                ctx.check("inst_dual_reference_is_reference_true", dual_ref->is_reference() == true);
                ctx.check("inst_dual_reference_signature_object",
                          sig == "(Ljava/lang/String;)Ljava/lang/Object;");
                ctx.check("inst_dual_reference_oracle_true", oracle_is_reference(sig) == true);
            }
            // The crux: SAME name, DIFFERENT is_reference() — tracks the overload.
            if (dual_prim && dual_ref)
            {
                ctx.check("inst_dual_same_name_distinct_is_reference",
                          dual_prim->is_reference() != dual_ref->is_reference());
            }
        }

        // ---- static: sdual(I)I  vs  sdual(String)Object ----
        const auto sdual_prim{ isref::static_proxy("sdual", "(I)I") };
        const auto sdual_ref{ isref::static_proxy("sdual", "(Ljava/lang/String;)Ljava/lang/Object;") };

        ctx.check("static_sdual_primitive_resolves", sdual_prim.has_value());
        ctx.check("static_sdual_reference_resolves", sdual_ref.has_value());

        if (sdual_prim)
        {
            const std::string sig{ sdual_prim->signature() };
            ctx.check("static_sdual_primitive_is_reference_false", sdual_prim->is_reference() == false);
            ctx.check("static_sdual_primitive_oracle_false", oracle_is_reference(sig) == false);
        }
        if (sdual_ref)
        {
            const std::string sig{ sdual_ref->signature() };
            ctx.check("static_sdual_reference_is_reference_true", sdual_ref->is_reference() == true);
            ctx.check("static_sdual_reference_oracle_true", oracle_is_reference(sig) == true);
        }
        if (sdual_prim && sdual_ref)
        {
            ctx.check("static_sdual_same_name_distinct_is_reference",
                      sdual_prim->is_reference() != sdual_ref->is_reference());
        }
    }

    // =====================================================================
    //  5. Method* validity for a resolved proxy + the raw_method() deref guard.
    //     is_reference()/signature() never touch the Method*; the ONE place we
    //     read it (raw_method()) is gated with is_valid_pointer.
    // =====================================================================
    if (singleton)
    {
        const auto mp{ singleton->get_method("retObject") };
        if (mp)
        {
            vmhook::hotspot::method* const m{ mp->raw_method() };
            ctx.check("resolved_proxy_raw_method_valid",
                      m != nullptr && vmhook::hotspot::is_valid_pointer(m));
            // is_reference() is a pure-metadata read independent of the Method*.
            ctx.check("resolved_proxy_object_is_reference_true", mp->is_reference() == true);
        }
    }

    // =====================================================================
    //  6. MALFORMED / EMPTY descriptor handling — no JVM needed; constructed
    //     directly.  is_reference() must NOT deref and must report false on each
    //     ill-formed descriptor (the npos / close+1>=size guards).
    // =====================================================================
    {
        // Empty signature: find(')') == npos -> false.
        const vmhook::method_proxy empty_sig{ nullptr, nullptr, std::string{} };
        ctx.check("empty_signature_is_reference_false", empty_sig.is_reference() == false);
        ctx.check("empty_signature_raw_method_null", empty_sig.raw_method() == nullptr);

        // Open-paren only, no ')' : find(')') == npos -> false.
        const vmhook::method_proxy no_close{ nullptr, nullptr, std::string{ "(" } };
        ctx.check("no_close_paren_is_reference_false", no_close.is_reference() == false);

        // ')' is the LAST char: close+1 == size -> false (nothing after ')').
        const vmhook::method_proxy nothing_after{ nullptr, nullptr, std::string{ "()" } };
        ctx.check("nothing_after_close_is_reference_false", nothing_after.is_reference() == false);

        // Explicit void return: char after ')' is 'V' -> false.
        const vmhook::method_proxy void_ret{ nullptr, nullptr, std::string{ "()V" } };
        ctx.check("explicit_void_is_reference_false", void_ret.is_reference() == false);

        // Explicit primitive return: 'I' -> false.
        const vmhook::method_proxy int_ret{ nullptr, nullptr, std::string{ "(I)I" } };
        ctx.check("explicit_int_is_reference_false", int_ret.is_reference() == false);

        // Explicit object return: 'L' -> true (no Method* required).
        const vmhook::method_proxy obj_ret{ nullptr, nullptr,
                                            std::string{ "()Ljava/lang/Object;" } };
        ctx.check("explicit_object_is_reference_true", obj_ret.is_reference() == true);

        // Explicit array returns: '[' -> true, for BOTH primitive- and
        // reference-element arrays (is_reference() keys on '[' regardless).
        const vmhook::method_proxy intarr_ret{ nullptr, nullptr, std::string{ "()[I" } };
        ctx.check("explicit_int_array_is_reference_true", intarr_ret.is_reference() == true);

        const vmhook::method_proxy strarr_ret{ nullptr, nullptr,
                                               std::string{ "()[Ljava/lang/String;" } };
        ctx.check("explicit_string_array_is_reference_true", strarr_ret.is_reference() == true);

        // A null-Method* proxy must be safe to introspect: raw_method() is null,
        // so the is_valid_pointer guard refuses any deref (no crash).
        ctx.check("null_method_proxy_not_valid_pointer",
                  vmhook::hotspot::is_valid_pointer(obj_ret.raw_method()) == false);
    }

    // =====================================================================
    //  7. INSTANCE path: every PRIMITIVE-ELEMENT array return ([Z..[D) plus
    //     Object[].  All TRUE — arrays are references regardless of element
    //     kind.  This is the deliberate "[I is a reference" semantic swept
    //     across all eight element types, the gap section 1 left at just [I.
    // =====================================================================
    if (singleton)
    {
        for (const expectation& e : k_instance_primitive_arrays)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "primarr_resolves_" } + e.name, mp.has_value());
            if (mp)
            {
                check_proxy(ctx, "primarr", e, *mp);
            }
        }
    }

    // =====================================================================
    //  8. INSTANCE path: MULTI-dimensional arrays ([[I, [[L..;, [[[B) and
    //     reference returns that are neither String nor Object (a JDK
    //     collection and an interface).  Leading '[' / 'L' => TRUE in every
    //     case; depth and element kind never change the verdict.
    // =====================================================================
    if (singleton)
    {
        for (const expectation& e : k_instance_multidim_arrays)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "multidim_resolves_" } + e.name, mp.has_value());
            if (mp)
            {
                check_proxy(ctx, "multidim", e, *mp);
            }
        }

        for (const expectation& e : k_instance_other_refs)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "otherref_resolves_" } + e.name, mp.has_value());
            if (mp)
            {
                check_proxy(ctx, "otherref", e, *mp);
            }
        }
    }

    // =====================================================================
    //  9. PARAM-LIST RED HERRING (the new headline): a parameter list that
    //     CONTAINS 'L' / '[' but a primitive / void RETURN.  is_reference()
    //     must verdict on the char AFTER ')' only — every one of these is
    //     FALSE.  A parser that scanned the whole descriptor (or used the
    //     FIRST 'L'/'[' anywhere) would wrongly report true here.  Resolved
    //     by EXACT descriptor so the proxy carries the precise param area.
    // =====================================================================
    if (singleton)
    {
        struct red_herring
        {
            const char* name;
            const char* sig;
        };
        const red_herring herrings[]{
            { "takesString",      "(Ljava/lang/String;)I" },
            { "takesIntArray",    "([I)I" },
            { "takesObjectArray", "([Ljava/lang/Object;)V" },
            { "takesMixed",       "(Ljava/lang/String;[IJ)Z" },
        };

        // (a) resolved by EXACT descriptor — the param area is reference-heavy
        //     yet the return is primitive/void, so is_reference() is false.
        for (const red_herring& h : herrings)
        {
            const auto mp{ singleton->get_method(h.name, h.sig) };
            ctx.check(std::string{ "redherring_resolves_" } + h.name, mp.has_value());
            if (!mp)
            {
                continue;
            }
            const std::string sig{ mp->signature() };
            ctx.check(std::string{ "redherring_signature_exact_" } + h.name, sig == h.sig);
            ctx.check(std::string{ "redherring_is_reference_false_" } + h.name,
                      mp->is_reference() == false);
            ctx.check(std::string{ "redherring_oracle_false_" } + h.name,
                      oracle_is_reference(sig) == false);
        }

        // (b) also resolved by NAME ONLY — same false verdict; the param 'L'/'['
        //     does not leak in regardless of which resolution path latched the
        //     descriptor.  (takesString/takesIntArray are unique names.)
        for (const expectation& e : k_instance_param_red_herrings)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "redherring_nameonly_resolves_" } + e.name, mp.has_value());
            if (mp)
            {
                check_proxy(ctx, "redherring_nameonly", e, *mp);
            }
        }

        // (c) static red-herring twin: sTakesString(L..;)I — still false.
        const auto s_mp{ isref::static_proxy("sTakesString", "(Ljava/lang/String;)I") };
        ctx.check("static_redherring_resolves", s_mp.has_value());
        if (s_mp)
        {
            const std::string sig{ s_mp->signature() };
            ctx.check("static_redherring_is_reference_false", s_mp->is_reference() == false);
            ctx.check("static_redherring_oracle_false", oracle_is_reference(sig) == false);
        }

        // (d) the crux contrast: a reference PARAM with a primitive RETURN
        //     (takesString) vs a reference RETURN (retString) — the param 'L'
        //     of the former must NOT make it look like the latter.
        const auto inst_ref_ret{ singleton->get_method("retString") };
        const auto inst_ref_param{ singleton->get_method("takesString", "(Ljava/lang/String;)I") };
        if (inst_ref_ret && inst_ref_param)
        {
            ctx.check("redherring_param_L_not_confused_with_return_L",
                      inst_ref_ret->is_reference() == true
                          && inst_ref_param->is_reference() == false);
        }
    }

    // =====================================================================
    // 10. EXHAUSTIVE MALFORMED / EDGE descriptors — no JVM.  Sweeps every
    //     single primitive return char, deep arrays, lowercase 'l' (NOT a
    //     reference — only uppercase 'L' is), garbage after ')', a doubled
    //     ')' (the find-FIRST vs find-LAST distinction the accessor relies
    //     on), an empty param list with a reference return, and the
    //     four-arg pinned ctor (is_reference() independent of the pinned flag).
    //     Each proxy is hand-built; is_reference() must NOT deref.
    // =====================================================================
    {
        struct desc_case
        {
            const char* sig;
            bool        expect;
            const char* label;
        };
        const desc_case cases[]{
            // every single primitive return char -> false
            { "()Z", false, "void_paren_Z" },
            { "()B", false, "void_paren_B" },
            { "()S", false, "void_paren_S" },
            { "()C", false, "void_paren_C" },
            { "()I", false, "void_paren_I" },
            { "()J", false, "void_paren_J" },
            { "()F", false, "void_paren_F" },
            { "()D", false, "void_paren_D" },
            { "()V", false, "void_paren_V" },
            // reference returns -> true
            { "()Ljava/lang/Object;",        true,  "ret_Object" },
            { "()Ljava/lang/String;",        true,  "ret_String" },
            { "()Ljava/util/List;",          true,  "ret_List" },
            // arrays of every primitive element -> true (leading '[')
            { "()[Z", true,  "ret_boolArray" },
            { "()[B", true,  "ret_byteArray" },
            { "()[S", true,  "ret_shortArray" },
            { "()[C", true,  "ret_charArray" },
            { "()[I", true,  "ret_intArray" },
            { "()[J", true,  "ret_longArray" },
            { "()[F", true,  "ret_floatArray" },
            { "()[D", true,  "ret_doubleArray" },
            // multi-dim arrays -> true regardless of depth
            { "()[[I",                       true,  "ret_int2D" },
            { "()[[[B",                      true,  "ret_byte3D" },
            { "()[[Ljava/lang/String;",      true,  "ret_string2D" },
            // param area carries 'L' / '[' but primitive/void return -> false
            { "(Ljava/lang/String;)I",       false, "param_L_ret_I" },
            { "([I)I",                       false, "param_arr_ret_I" },
            { "([Ljava/lang/Object;)V",      false, "param_arr_ret_V" },
            { "(Ljava/lang/String;[IJ)Z",    false, "param_mix_ret_Z" },
            { "(Ljava/lang/Object;)J",       false, "param_L_ret_J" },
            // lowercase 'l' is NOT a reference marker — only uppercase 'L' is.
            { "()lava/lang/String;",         false, "lowercase_l_not_reference" },
            // a return char that is none of L/[/primitive (garbage) -> false
            { "()X",                         false, "garbage_X_after_paren" },
            { "()@",                         false, "garbage_at_after_paren" },
            // 'L' / '[' NOT in the return slot but elsewhere as trailing junk
            // after a primitive return char -> the FIRST char after ')' wins.
            { "()IL",                        false, "primitive_then_L" },
            { "()V[",                        false, "void_then_bracket" },
            // empty param list, reference return (canonical no-arg getter)
            { "()Ljava/lang/Integer;",       true,  "noarg_boxed_ref" },
        };

        for (const desc_case& c : cases)
        {
            const vmhook::method_proxy mp{ nullptr, nullptr, std::string{ c.sig } };
            ctx.check(std::string{ "malformed_is_reference_" } + c.label,
                      mp.is_reference() == c.expect);
            // is_reference() must agree with the independent oracle on EVERY
            // descriptor, well-formed or not — they share the same guard logic.
            ctx.check(std::string{ "malformed_oracle_agree_" } + c.label,
                      mp.is_reference() == oracle_is_reference(c.sig));
            // signature() round-trips the exact descriptor handed to the ctor.
            const std::string round{ mp.signature() };
            ctx.check(std::string{ "malformed_signature_roundtrip_" } + c.label,
                      round == c.sig);
        }

        // Doubled ')' — the accessor uses find(')') (the FIRST ')'), so the
        // char after the FIRST ')' decides.  "()L)V" -> first ')' is at index 1,
        // next char is 'L' -> reference true.  This pins the find-FIRST contract
        // (a find-LAST parser would look after the SECOND ')' and see 'V').
        {
            const vmhook::method_proxy doubled{ nullptr, nullptr, std::string{ "()L)V" } };
            ctx.check("doubled_paren_uses_first_close_true", doubled.is_reference() == true);
            ctx.check("doubled_paren_oracle_agree",
                      doubled.is_reference() == oracle_is_reference("()L)V"));
        }
        // Mirror: "()I)L" -> after the FIRST ')' is 'I' -> false (the trailing
        // ')L' is ignored).  Distinguishes find-FIRST from find-LAST the other way.
        {
            const vmhook::method_proxy doubled2{ nullptr, nullptr, std::string{ "()I)L" } };
            ctx.check("doubled_paren_first_primitive_false", doubled2.is_reference() == false);
        }

        // Single ')' as the FIRST and only character: char after it is 'L' here.
        {
            const vmhook::method_proxy lead_close{ nullptr, nullptr, std::string{ ")Ljava/lang/Object;" } };
            ctx.check("leading_close_then_L_true", lead_close.is_reference() == true);
        }
        // ')' first char, primitive after -> false.
        {
            const vmhook::method_proxy lead_close_prim{ nullptr, nullptr, std::string{ ")I" } };
            ctx.check("leading_close_then_primitive_false", lead_close_prim.is_reference() == false);
        }

        // Four-arg PINNED ctor: is_reference() reads signature_text only and is
        // INDEPENDENT of the pinned flag.  A pinned reference descriptor is still
        // true; a pinned primitive is still false.
        {
            const vmhook::method_proxy pinned_ref{ nullptr, nullptr,
                                                   std::string{ "()Ljava/lang/Object;" }, true };
            ctx.check("pinned_reference_is_reference_true", pinned_ref.is_reference() == true);

            const vmhook::method_proxy pinned_prim{ nullptr, nullptr, std::string{ "()I" }, true };
            ctx.check("pinned_primitive_is_reference_false", pinned_prim.is_reference() == false);

            // Same descriptor, pinned vs unpinned, agrees — the flag is inert here.
            const vmhook::method_proxy unpinned_ref{ nullptr, nullptr,
                                                     std::string{ "()Ljava/lang/Object;" }, false };
            ctx.check("pinned_flag_does_not_change_is_reference",
                      pinned_ref.is_reference() == unpinned_ref.is_reference());
        }

        // Whitespace / single-char degenerate inputs that contain no ')'.
        {
            const vmhook::method_proxy just_L{ nullptr, nullptr, std::string{ "L" } };
            ctx.check("no_paren_just_L_false", just_L.is_reference() == false);

            const vmhook::method_proxy just_bracket{ nullptr, nullptr, std::string{ "[" } };
            ctx.check("no_paren_just_bracket_false", just_bracket.is_reference() == false);

            const vmhook::method_proxy space{ nullptr, nullptr, std::string{ " " } };
            ctx.check("no_paren_space_false", space.is_reference() == false);
        }

        // 'L' / '[' as the return char with NO terminating ';' — is_reference()
        // keys ONLY on the FIRST char after ')', so a missing ';' or class name
        // does not change the verdict.  (Not a real JVM descriptor, but it pins
        // that the accessor never scans past the return char.)
        {
            const vmhook::method_proxy bare_L{ nullptr, nullptr, std::string{ "()L" } };
            ctx.check("ret_bare_L_no_semicolon_true", bare_L.is_reference() == true);

            const vmhook::method_proxy bare_bracket{ nullptr, nullptr, std::string{ "()[" } };
            ctx.check("ret_bare_bracket_true", bare_bracket.is_reference() == true);

            // Boxed-Void descriptor hand-built (reference) vs primitive void.
            const vmhook::method_proxy boxed_void{ nullptr, nullptr,
                                                   std::string{ "()Ljava/lang/Void;" } };
            ctx.check("hand_boxed_void_is_reference_true", boxed_void.is_reference() == true);

            const vmhook::method_proxy prim_void{ nullptr, nullptr, std::string{ "()V" } };
            ctx.check("hand_primitive_void_is_reference_false", prim_void.is_reference() == false);

            // The crux contrast in one assertion: boxed Void and primitive void
            // share the word "void" yet disagree.
            ctx.check("boxed_void_differs_from_primitive_void",
                      boxed_void.is_reference() != prim_void.is_reference());

            // A 7-deep primitive array — leading '[' still decides -> true.
            const vmhook::method_proxy deep_arr{ nullptr, nullptr, std::string{ "()[[[[[[[I" } };
            ctx.check("hand_seven_deep_array_true", deep_arr.is_reference() == true);

            // A user-type descriptor with a '$' nested-class name -> true.
            const vmhook::method_proxy user_ref{ nullptr, nullptr,
                                                 std::string{ "()Lcom/example/Outer$Inner;" } };
            ctx.check("hand_user_nested_type_true", user_ref.is_reference() == true);
        }
    }

    // =====================================================================
    // 11. STATIC path: EVERY reference return kind (arrays, multi-dim, the
    //     collection + interface twins).  Mirrors the instance sweeps of
    //     sections 7-8 so the static_method() path is proven over the SAME
    //     '[' / 'L' descriptors, not just the 13 scalars of section 2.
    // =====================================================================
    for (const expectation& e : k_static_reference_returns)
    {
        const auto mp{ isref::static_proxy(e.name) };
        ctx.check(std::string{ "static_ref_resolves_" } + e.name, mp.has_value());
        if (mp)
        {
            check_proxy(ctx, "static_ref", e, *mp);
        }
    }

    // =====================================================================
    // 12. BOXED wrapper-type returns (instance + static).  Every boxed type
    //     is a reference; the boxed java.lang.Void is the sharp edge against
    //     the primitive void 'V'.  Same three cross-checks per method.
    // =====================================================================
    if (singleton)
    {
        for (const expectation& e : k_instance_boxed)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "boxed_inst_resolves_" } + e.name, mp.has_value());
            if (mp)
            {
                check_proxy(ctx, "boxed_inst", e, *mp);
            }
        }

        // The boxed-Void vs primitive-void headline on RESOLVED proxies: same
        // English "void", opposite is_reference() verdict.
        const auto boxed_void{ singleton->get_method("retBoxedVoid") };
        const auto prim_void{ singleton->get_method("retVoid") };
        if (boxed_void && prim_void)
        {
            ctx.check("resolved_boxed_void_is_reference_true",
                      boxed_void->is_reference() == true);
            ctx.check("resolved_primitive_void_is_reference_false",
                      prim_void->is_reference() == false);
            ctx.check("resolved_boxed_void_differs_from_primitive_void",
                      boxed_void->is_reference() != prim_void->is_reference());
        }
    }

    for (const expectation& e : k_static_boxed)
    {
        const auto mp{ isref::static_proxy(e.name) };
        ctx.check(std::string{ "boxed_static_resolves_" } + e.name, mp.has_value());
        if (mp)
        {
            check_proxy(ctx, "boxed_static", e, *mp);
        }
    }

    // =====================================================================
    // 13. USER-defined reference returns (instance + static): a nested
    //     concrete class, the interface it implements, an array of the user
    //     type, and a 5-D primitive array.  Reference verdict does NOT depend
    //     on the type being a JDK type nor on array depth.
    // =====================================================================
    if (singleton)
    {
        for (const expectation& e : k_instance_user_refs)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "userref_inst_resolves_" } + e.name, mp.has_value());
            if (mp)
            {
                check_proxy(ctx, "userref_inst", e, *mp);
            }
        }
    }
    for (const expectation& e : k_static_user_refs)
    {
        const auto mp{ isref::static_proxy(e.name) };
        ctx.check(std::string{ "userref_static_resolves_" } + e.name, mp.has_value());
        if (mp)
        {
            check_proxy(ctx, "userref_static", e, *mp);
        }
    }

    // =====================================================================
    // 14. EXACT-DESCRIPTOR PINS on resolved proxies.  signature() must
    //     round-trip the canonical JVM descriptor (precise '[' depth + element
    //     char) the resolution reported — a stronger invariant than truth+
    //     oracle, and version-stable across JDK 8..26.  is_reference() and the
    //     oracle are re-checked against the pinned text for full closure.
    // =====================================================================
    if (singleton)
    {
        for (const sig_pin& p : k_instance_sig_pins)
        {
            const auto mp{ singleton->get_method(p.name) };
            ctx.check(std::string{ "sigpin_resolves_" } + p.name, mp.has_value());
            if (!mp)
            {
                continue;
            }
            const std::string sig{ mp->signature() };
            // (a) the descriptor text is EXACTLY the canonical one expected.
            ctx.check(std::string{ "sigpin_signature_exact_" } + p.name, sig == p.sig);
            // (b) is_reference() agrees with the oracle run over the SAME text.
            ctx.check(std::string{ "sigpin_is_reference_matches_oracle_" } + p.name,
                      mp->is_reference() == oracle_is_reference(sig));
            // (c) the oracle over the EXPECTED literal matches the accessor —
            //     ties the JVM-reported text to the hand-derived classification.
            ctx.check(std::string{ "sigpin_oracle_literal_matches_accessor_" } + p.name,
                      oracle_is_reference(p.sig) == mp->is_reference());

            if (sig != p.sig)
            {
                ctx.record(std::string{ "[INFO] sigpin " } + p.name
                           + " expected='" + p.sig + "' actual='" + sig + "'");
            }
        }
    }

    // =====================================================================
    // 15. INSTANCE vs STATIC parity for the ARRAY / reference return kinds —
    //     not just the scalars section 3 covered.  Each instance returner and
    //     its static twin share the identical return descriptor, so
    //     is_reference() agrees (and is true) across the two resolution paths.
    // =====================================================================
    if (singleton)
    {
        struct twin
        {
            const char* inst;
            const char* stat;
        };
        const twin twins[]{
            { "retIntArray",    "sRetIntArray" },     // [I
            { "retStringArray", "sRetStringArray" },  // [Ljava/lang/String;
            { "retObjectArray", "sRetObjectArray" },  // [Ljava/lang/Object;
            { "retInt2DArray",  "sRetInt2DArray" },   // [[I
            { "retByte3DArray", "sRetByte3DArray" },  // [[[B
            { "retList",        "sRetList" },         // Ljava/util/List;
            { "retInterface",   "sRetInterface" },    // Ljava/lang/CharSequence;
            { "retBox",         "sRetBox" },          // user nested class
            { "retBoxedInt",    "sRetBoxedInt" },     // Ljava/lang/Integer;
            { "retBoxedVoid",   "sRetBoxedVoid" },    // Ljava/lang/Void;
            { "retInterfaceArray", "sRetInterfaceArray" }, // [Ljava/lang/CharSequence;
            { "retBoxedIntArray",  "sRetBoxedIntArray" },  // [Ljava/lang/Integer;
            { "retBox2DArray",     "sRetBox2DArray" },     // [[Lvmhook/.../$Box;
            { "retString3DArray",  "sRetString3DArray" },  // [[[Ljava/lang/String;
            { "retTagIface",       "sRetTagIface" },       // Ljava/lang/.../$Tag;
        };
        for (const twin& t : twins)
        {
            const auto inst_mp{ singleton->get_method(t.inst) };
            const auto stat_mp{ isref::static_proxy(t.stat) };
            ctx.check(std::string{ "arrparity_inst_resolves_" } + t.inst, inst_mp.has_value());
            ctx.check(std::string{ "arrparity_static_resolves_" } + t.stat, stat_mp.has_value());
            if (inst_mp && stat_mp)
            {
                ctx.check(std::string{ "arrparity_both_reference_" } + t.inst,
                          inst_mp->is_reference() == true && stat_mp->is_reference() == true);
                ctx.check(std::string{ "arrparity_agree_" } + t.inst,
                          inst_mp->is_reference() == stat_mp->is_reference());
                // The twins also share the EXACT descriptor (same return type).
                const std::string isig{ inst_mp->signature() };
                const std::string ssig{ stat_mp->signature() };
                ctx.check(std::string{ "arrparity_signatures_equal_" } + t.inst, isig == ssig);
            }
        }
    }

    // =====================================================================
    // 16. NESTED-reference array returns (instance + static): '[' + a 'L...;'
    //     element — interface[], boxed[], boxed-Void[], user 2-D, deep
    //     String[][][].  All TRUE.  The check_proxy() helper also fires the
    //     trichotomy predicate-invariant on each (reference XOR void XOR
    //     primitive), so these rows prove BOTH the array '[' verdict AND that
    //     a nested 'L' element never flips a leg of the trichotomy.
    // =====================================================================
    if (singleton)
    {
        for (const expectation& e : k_instance_nested_ref_arrays)
        {
            const auto mp{ singleton->get_method(e.name) };
            ctx.check(std::string{ "nestedarr_inst_resolves_" } + e.name, mp.has_value());
            if (mp)
            {
                check_proxy(ctx, "nestedarr_inst", e, *mp);
            }
        }
    }
    for (const expectation& e : k_static_nested_ref_arrays)
    {
        const auto mp{ isref::static_proxy(e.name) };
        ctx.check(std::string{ "nestedarr_static_resolves_" } + e.name, mp.has_value());
        if (mp)
        {
            check_proxy(ctx, "nestedarr_static", e, *mp);
        }
    }

    // =====================================================================
    // 17. DESCRIPTOR-SIDE value_t SHAPE: without ANY call(), assert — straight
    //     off signature() — the trichotomy a future call's value_t would land
    //     in (reference / void / primitive) AND the is_string() shape (return
    //     type EXACTLY java.lang.String).  These mirror value_t::is_reference()
    //     / is_void() / is_string() on the descriptor side, the call-free way to
    //     deepen "is_void()/is_string() on method returns".  Each row resolves a
    //     real method and pins all three predicate legs + the String shape.
    // =====================================================================
    if (singleton)
    {
        struct shape_case
        {
            const char* name;       // instance method on the fixture
            bool        reference;  // is_reference() truth
            bool        is_void;    // return char is 'V'
            bool        primitive;  // return char is a primitive
            bool        is_string;  // return type is EXACTLY java/lang/String
        };
        const shape_case shapes[]{
            //  name              ref    void   prim   string
            { "retString",      true,  false, false, true  },  // the lone is_string TRUE
            { "retObject",      true,  false, false, false },
            { "retList",        true,  false, false, false },
            { "retInterface",   true,  false, false, false },
            { "retBox",         true,  false, false, false },
            { "retBoxedInt",    true,  false, false, false },
            { "retBoxedVoid",   true,  false, false, false },  // boxed Void: ref, NOT void
            { "retStringArray", true,  false, false, false },  // [L..String; is NOT bare String
            { "retIntArray",    true,  false, false, false },
            { "retVoid",        false, true,  false, false },  // the lone is_void TRUE
            { "retInt",         false, false, true,  false },
            { "retBool",        false, false, true,  false },
            { "retLong",        false, false, true,  false },
            { "retDouble",      false, false, true,  false },
            { "retChar",        false, false, true,  false },
            { "takesString",    false, false, true,  false },  // (L..)I — primitive return
            { "takesObjectArray", false, true, false, false }, // ([L..)V — void return
        };

        for (const shape_case& s : shapes)
        {
            // takesString / takesObjectArray are unique names; the rest are no-arg.
            const auto mp{ singleton->get_method(s.name) };
            ctx.check(std::string{ "shape_resolves_" } + s.name, mp.has_value());
            if (!mp)
            {
                continue;
            }
            const std::string sig{ mp->signature() };
            const bool        is_ref{ mp->is_reference() };

            ctx.check(std::string{ "shape_reference_" } + s.name, is_ref == s.reference);
            ctx.check(std::string{ "shape_void_" } + s.name, oracle_is_void(sig) == s.is_void);
            ctx.check(std::string{ "shape_primitive_" } + s.name,
                      oracle_is_primitive(sig) == s.primitive);
            ctx.check(std::string{ "shape_is_string_" } + s.name,
                      oracle_returns_string(sig) == s.is_string);

            // HARD predicate invariant: exactly one of the three legs holds.
            ctx.check(std::string{ "shape_trichotomy_exactly_one_" } + s.name,
                      (is_ref ? 1 : 0) + (oracle_is_void(sig) ? 1 : 0)
                          + (oracle_is_primitive(sig) ? 1 : 0) == 1);
            // is_string() can ONLY be true when is_reference() is — a String
            // return is a reference return; the converse need not hold.
            ctx.check(std::string{ "shape_string_implies_reference_" } + s.name,
                      !oracle_returns_string(sig) || is_ref);
            // is_string() and is_void() are mutually exclusive.
            ctx.check(std::string{ "shape_string_and_void_disjoint_" } + s.name,
                      !(oracle_returns_string(sig) && oracle_is_void(sig)));
        }

        // The is_string() DISCRIMINATION crux: retString's return is exactly
        // String (is_string TRUE) while retStringArray's is '[L..String;' (a
        // reference, but is_string FALSE — the array is not a String).  Both are
        // references, yet the String-shape predicate separates them.
        const auto str_ret{ singleton->get_method("retString") };
        const auto strarr_ret{ singleton->get_method("retStringArray") };
        if (str_ret && strarr_ret)
        {
            const std::string s0{ str_ret->signature() };
            const std::string s1{ strarr_ret->signature() };
            ctx.check("shape_string_vs_stringarray_both_reference",
                      str_ret->is_reference() == true && strarr_ret->is_reference() == true);
            ctx.check("shape_string_only_first_is_string",
                      oracle_returns_string(s0) == true && oracle_returns_string(s1) == false);
        }
    }

    // =====================================================================
    // 18. RED-HERRING INVERSE on a RESOLVED method: refParamRefReturn has a
    //     reference PARAM and a reference RETURN — is_reference() must be TRUE
    //     (it reads the return slot), while takesString (reference param,
    //     primitive return) is FALSE.  Same param 'L', opposite verdict driven
    //     ENTIRELY by the return char.  Pins that is_reference() never confuses
    //     the param 'L' for the return 'L' in EITHER direction.
    // =====================================================================
    if (singleton)
    {
        const auto ref_ref{ singleton->get_method("refParamRefReturn",
                                                   "(Ljava/lang/String;)Ljava/lang/Object;") };
        const auto ref_prim{ singleton->get_method("takesString", "(Ljava/lang/String;)I") };
        ctx.check("inverse_refParamRefReturn_resolves", ref_ref.has_value());
        ctx.check("inverse_takesString_resolves", ref_prim.has_value());
        if (ref_ref)
        {
            const std::string sig{ ref_ref->signature() };
            ctx.check("inverse_refParamRefReturn_signature_exact",
                      sig == "(Ljava/lang/String;)Ljava/lang/Object;");
            ctx.check("inverse_refParamRefReturn_is_reference_true",
                      ref_ref->is_reference() == true);
            ctx.check("inverse_refParamRefReturn_oracle_true",
                      oracle_is_reference(sig) == true);
        }
        if (ref_ref && ref_prim)
        {
            // SAME param descriptor (Ljava/lang/String;), opposite is_reference().
            ctx.check("inverse_same_param_distinct_is_reference",
                      ref_ref->is_reference() != ref_prim->is_reference());
        }
    }

    // =====================================================================
    // 19. MORE hand-built MALFORMED / EDGE return chars — no JVM.  Boundary
    //     chars the section-10 sweep did not cover: ')' as the only char, ';'
    //     immediately after ')', whitespace return chars (space / tab),
    //     lowercase primitive-lookalikes (only UPPERCASE Z..D are primitives —
    //     but the accessor only keys 'L'/'[' so lowercase is simply non-ref),
    //     a '0' digit, and a leading '[' / 'L' as the FIRST char with no ')'.
    //     is_reference() must NOT deref and must report the documented verdict;
    //     the oracle must agree on every one.
    // =====================================================================
    {
        struct edge_case
        {
            const char* sig;
            bool        expect;
            const char* label;
        };
        const edge_case edges[]{
            { ")",                       false, "close_only" },          // close is last char
            { "();",                     false, "semicolon_after_close" },// ';' is not L/[
            { "() ",                     false, "space_after_close" },    // ' ' not L/[
            { "()\t",                    false, "tab_after_close" },      // tab not L/[
            { "()z",                     false, "lowercase_z_after" },    // lowercase != primitive marker, and != L
            { "()b",                     false, "lowercase_b_after" },
            { "()i",                     false, "lowercase_i_after" },
            { "()0",                     false, "digit_after_close" },
            { "()l",                     false, "lowercase_l_after" },    // only UPPER 'L' is reference
            { "()(",                     false, "open_paren_after_close" },
            { "()]",                     false, "close_bracket_after" },  // ']' not '['
            { "[",                       false, "lead_bracket_no_paren" },// no ')' -> false
            { "Ljava/lang/Object;",      false, "lead_L_no_paren" },      // no ')' -> false
            { "()L",                     true,  "bare_L_after_close" },   // 'L' wins, no ';' needed
            { "()[",                     true,  "bare_bracket_after_close" },
        };

        for (const edge_case& c : edges)
        {
            const vmhook::method_proxy mp{ nullptr, nullptr, std::string{ c.sig } };
            ctx.check(std::string{ "edge_is_reference_" } + c.label,
                      mp.is_reference() == c.expect);
            ctx.check(std::string{ "edge_oracle_agree_" } + c.label,
                      mp.is_reference() == oracle_is_reference(c.sig));
            // Trichotomy holds even for ill-formed text: exactly one leg true.
            ctx.check(std::string{ "edge_trichotomy_exactly_one_" } + c.label,
                      (mp.is_reference() ? 1 : 0) + (oracle_is_void(c.sig) ? 1 : 0)
                          + (oracle_is_primitive(c.sig) ? 1 : 0) == 1);
            // is_reference() never touches the Method* — null here, must be safe.
            ctx.check(std::string{ "edge_raw_method_null_" } + c.label,
                      mp.raw_method() == nullptr);
        }
    }

    // =====================================================================
    // 20. IDEMPOTENCY + VALUE-SEMANTIC STABILITY (hand-built, no JVM).
    //     is_reference() is a pure const read of signature_text; calling it
    //     repeatedly must return the SAME bool, and a COPY / a MOVE of the
    //     proxy must preserve both is_reference() and signature() (the member
    //     is a std::string carried by value).  name() of a null-Method* proxy
    //     is the empty string (name() returns {} when there is no Method*).
    // =====================================================================
    {
        struct idem_case
        {
            const char* sig;
            bool        expect;
            const char* label;
        };
        const idem_case idem[]{
            { "()Ljava/lang/Object;",   true,  "object" },
            { "()[I",                   true,  "intarray" },
            { "()[[Ljava/lang/String;", true,  "string2d" },
            { "()V",                    false, "void" },
            { "()I",                    false, "int" },
            { "(Ljava/lang/String;)J",  false, "param_L_ret_J" },
            { "",                       false, "empty" },
            { "(",                      false, "open_only" },
            { "()",                     false, "nothing_after" },
        };
        for (const idem_case& c : idem)
        {
            const vmhook::method_proxy mp{ nullptr, nullptr, std::string{ c.sig } };

            // (a) is_reference() is stable across repeated calls (no mutation).
            const bool first{ mp.is_reference() };
            const bool second{ mp.is_reference() };
            const bool third{ mp.is_reference() };
            ctx.check(std::string{ "idem_is_reference_repeat_stable_" } + c.label,
                      first == second && second == third);
            ctx.check(std::string{ "idem_is_reference_value_" } + c.label,
                      first == c.expect);

            // (b) signature() is stable across repeated calls (same text).
            const std::string sig_a{ mp.signature() };
            const std::string sig_b{ mp.signature() };
            ctx.check(std::string{ "idem_signature_repeat_stable_" } + c.label,
                      sig_a == sig_b);
            ctx.check(std::string{ "idem_signature_roundtrip_" } + c.label,
                      sig_a == c.sig);

            // (c) a COPY preserves is_reference() AND signature().
            const vmhook::method_proxy copy{ mp };
            ctx.check(std::string{ "idem_copy_is_reference_agrees_" } + c.label,
                      copy.is_reference() == first);
            const std::string copy_sig{ copy.signature() };
            ctx.check(std::string{ "idem_copy_signature_agrees_" } + c.label,
                      copy_sig == sig_a);

            // (d) a MOVE-constructed proxy carries the same descriptor verdict.
            //     Build a fresh source so the original `mp` stays valid for later
            //     rows; the moved-into proxy must report identically.
            vmhook::method_proxy move_src{ nullptr, nullptr, std::string{ c.sig } };
            const vmhook::method_proxy moved{ std::move(move_src) };
            ctx.check(std::string{ "idem_move_is_reference_agrees_" } + c.label,
                      moved.is_reference() == c.expect);
            const std::string moved_sig{ moved.signature() };
            ctx.check(std::string{ "idem_move_signature_agrees_" } + c.label,
                      moved_sig == c.sig);

            // (e) name() of a null-Method* proxy is empty (no Method* to read).
            ctx.check(std::string{ "idem_null_method_name_empty_" } + c.label,
                      mp.name().empty());
            ctx.check(std::string{ "idem_null_method_raw_null_" } + c.label,
                      mp.raw_method() == nullptr);
        }
    }

    // =====================================================================
    // 21. SELF-DESCRIPTION CONSISTENCY: is_reference() agrees with the oracle
    //     run over the LIBRARY's OWN signature() return (a std::string built
    //     from the returned string_view), not just the literal we constructed.
    //     This closes the loop: the accessor and the descriptor it exposes
    //     classify identically.  Also pins signature().size() == constructed
    //     length (no truncation / no extra bytes for embedded characters).
    // =====================================================================
    {
        const char* sigs[]{
            "()Ljava/lang/Object;",
            "()Ljava/lang/String;",
            "()[Ljava/lang/Integer;",
            "()[[[I",
            "()V",
            "()Z",
            "()B",
            "()S",
            "()C",
            "()I",
            "()J",
            "()F",
            "()D",
            "(Ljava/lang/String;[IJ)Z",
            "([Ljava/lang/Object;)V",
            "()Lvmhook/fixtures/IsReference$Box;",
        };
        for (const char* s : sigs)
        {
            const std::string built{ s };
            const vmhook::method_proxy mp{ nullptr, nullptr, std::string{ s } };
            const std::string viewed{ mp.signature() };

            // signature() returns exactly what we constructed, byte-for-byte.
            ctx.check(std::string{ "selfdesc_signature_size_" } + s,
                      viewed.size() == built.size());
            ctx.check(std::string{ "selfdesc_signature_equal_" } + s,
                      viewed == built);

            // is_reference() classifies the SAME way the oracle classifies the
            // descriptor the library hands back via signature().
            ctx.check(std::string{ "selfdesc_accessor_matches_own_signature_" } + s,
                      mp.is_reference() == oracle_is_reference(viewed));

            // And the trichotomy over the library-returned descriptor: exactly one.
            ctx.check(std::string{ "selfdesc_trichotomy_over_own_signature_" } + s,
                      (mp.is_reference() ? 1 : 0) + (oracle_is_void(viewed) ? 1 : 0)
                          + (oracle_is_primitive(viewed) ? 1 : 0) == 1);
        }
    }

    // =====================================================================
    // 22. EXHAUSTIVE RETURN-CHAR DOMAIN SWEEP (every byte 1..127 after ')').
    //     The single char following ')' is the WHOLE input domain of the
    //     accessor's decision.  For EVERY printable/control byte c in [1,127]
    //     build "()" + c and assert is_reference() is TRUE iff c is 'L' or '['
    //     (and primitive iff c is one of Z B S C I J F D, with the two sets
    //     disjoint).  This is the "every possible input" coverage for the
    //     classification, programmatic so no byte is skipped.  Byte 0 (NUL) is
    //     handled separately in section 23 (it cannot go in a const char*).
    // =====================================================================
    {
        for (int ci = 1; ci <= 127; ++ci)
        {
            const char        c{ static_cast<char>(ci) };
            std::string       sig{ "()" };
            sig.push_back(c);
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };

            const bool expect_ref{ c == 'L' || c == '[' };
            const bool is_ref{ mp.is_reference() };

            // Build a stable, ASCII-only label from the byte value (NOT the raw
            // char, which could be a control byte) so check names are printable.
            const std::string label{ std::to_string(ci) };

            ctx.check(std::string{ "domain_is_reference_byte_" } + label,
                      is_ref == expect_ref);
            ctx.check(std::string{ "domain_oracle_agree_byte_" } + label,
                      is_ref == oracle_is_reference(sig));

            // The primitive leg is exactly the eight JVM primitive descriptor
            // chars; reference and primitive legs are disjoint over the domain.
            const bool expect_prim{ c == 'Z' || c == 'B' || c == 'S' || c == 'C'
                                    || c == 'I' || c == 'J' || c == 'F' || c == 'D' };
            ctx.check(std::string{ "domain_primitive_byte_" } + label,
                      oracle_is_primitive(sig) == expect_prim);
            ctx.check(std::string{ "domain_ref_prim_disjoint_byte_" } + label,
                      !(expect_ref && expect_prim));

            // Trichotomy over the whole single-char domain: exactly one leg.
            ctx.check(std::string{ "domain_trichotomy_byte_" } + label,
                      (is_ref ? 1 : 0) + (oracle_is_void(sig) ? 1 : 0)
                          + (oracle_is_primitive(sig) ? 1 : 0) == 1);
        }
    }

    // =====================================================================
    // 23. EMBEDDED-NUL descriptors (built with explicit \0 escapes + length-
    //     aware std::string -- NEVER a raw NUL byte in this source).  A NUL
    //     buried in signature_text must not break find(')') or the return-char
    //     read.  The accessor keys ONLY on the FIRST char after the FIRST ')',
    //     so a NUL AFTER that char is irrelevant, and a ')' before a NUL still
    //     resolves the return char.  Each case is certain from the source
    //     (std::string::find / operator[] are length-based, not C-string based).
    // =====================================================================
    {
        // "()L\0junk" -- return char is 'L' (the NUL is AFTER it) -> reference.
        {
            std::string sig{ "()L" };
            sig.push_back('\0');
            sig += "junk";
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("nul_after_L_is_reference_true", mp.is_reference() == true);
            ctx.check("nul_after_L_oracle_agree",
                      mp.is_reference() == oracle_is_reference(sig));
            // signature() preserves the full length INCLUDING the embedded NUL.
            const std::string viewed{ mp.signature() };
            ctx.check("nul_after_L_signature_length_preserved",
                      viewed.size() == sig.size());
        }
        // "()V\0L" -- return char is 'V' (NUL then 'L' are after it) -> NOT ref.
        // A naive C-string parse would stop at the NUL but still see 'V' first;
        // either way the FIRST char after ')' is 'V', so the verdict is false.
        {
            std::string sig{ "()V" };
            sig.push_back('\0');
            sig.push_back('L');
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("nul_then_L_after_V_is_reference_false", mp.is_reference() == false);
            ctx.check("nul_then_L_after_V_oracle_agree",
                      mp.is_reference() == oracle_is_reference(sig));
        }
        // "()\0L" -- the FIRST char after ')' is the NUL itself (neither 'L' nor
        // '[' nor a primitive) -> false.  This pins that a NUL return char is
        // simply "not a reference", exactly like any other non-L/[ byte.
        {
            std::string sig{ "()" };
            sig.push_back('\0');
            sig.push_back('L');
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("nul_as_return_char_is_reference_false", mp.is_reference() == false);
            ctx.check("nul_as_return_char_oracle_agree",
                      mp.is_reference() == oracle_is_reference(sig));
            ctx.check("nul_as_return_char_signature_length_preserved",
                      std::string{ mp.signature() }.size() == sig.size());
        }
        // NUL BEFORE the ')' in the param area -- find(')') skips past it and the
        // return char ('I') still decides -> false (a reference param NUL noise
        // does not leak into the return verdict).
        {
            std::string sig{ "(" };
            sig.push_back('\0');
            sig += ")I";
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("nul_in_param_ret_I_is_reference_false", mp.is_reference() == false);
            ctx.check("nul_in_param_ret_I_oracle_agree",
                      mp.is_reference() == oracle_is_reference(sig));
        }
        // NUL before ')' with a reference RETURN -- still true (param noise inert).
        {
            std::string sig{ "(" };
            sig.push_back('\0');
            sig += ")Ljava/lang/Object;";
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("nul_in_param_ret_L_is_reference_true", mp.is_reference() == true);
            ctx.check("nul_in_param_ret_L_oracle_agree",
                      mp.is_reference() == oracle_is_reference(sig));
        }
        // A lone NUL descriptor: no ')' at all -> false (find == npos).
        {
            std::string sig;
            sig.push_back('\0');
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("lone_nul_no_paren_is_reference_false", mp.is_reference() == false);
            ctx.check("lone_nul_signature_length_one",
                      std::string{ mp.signature() }.size() == 1);
        }
    }

    // =====================================================================
    // 24. EXTREME ARRAY-DEPTH + LONG-NAME BOUNDARIES (hand-built, no JVM).
    //     The accessor keys on the FIRST char after ')', so a leading '[' makes
    //     it a reference at ANY depth and for ANY (even absurdly long) element
    //     name.  Sweep depths {1,2,3,16,32,64,255}; the verdict must stay TRUE.
    //     Also a very long 'L...;' class name (1000 chars) -> TRUE, and the
    //     degenerate "()" + '[' * N with NO element (just brackets) -> TRUE.
    // =====================================================================
    {
        const int depths[]{ 1, 2, 3, 16, 32, 64, 255 };
        for (int d : depths)
        {
            const std::string label{ std::to_string(d) };

            // "()" + '['*d + "I"  -- primitive-element array of depth d.
            {
                std::string sig{ "()" };
                sig.append(static_cast<std::size_t>(d), '[');
                sig.push_back('I');
                const vmhook::method_proxy mp{ nullptr, nullptr, sig };
                ctx.check(std::string{ "deep_prim_array_is_reference_true_d" } + label,
                          mp.is_reference() == true);
                ctx.check(std::string{ "deep_prim_array_oracle_agree_d" } + label,
                          mp.is_reference() == oracle_is_reference(sig));
                ctx.check(std::string{ "deep_prim_array_signature_size_d" } + label,
                          std::string{ mp.signature() }.size() == sig.size());
            }
            // "()" + '['*d  -- brackets only, NO element char after them.  The
            // FIRST char after ')' is still '[', so reference TRUE regardless of
            // there being no element (accessor never scans past the return char).
            {
                std::string sig{ "()" };
                sig.append(static_cast<std::size_t>(d), '[');
                const vmhook::method_proxy mp{ nullptr, nullptr, sig };
                ctx.check(std::string{ "brackets_only_is_reference_true_d" } + label,
                          mp.is_reference() == true);
            }
        }

        // Very long reference class name: "()L" + 1000*'a' + ";" -> TRUE.
        {
            std::string sig{ "()L" };
            sig.append(static_cast<std::size_t>(1000), 'a');
            sig.push_back(';');
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("long_class_name_is_reference_true", mp.is_reference() == true);
            ctx.check("long_class_name_oracle_agree",
                      mp.is_reference() == oracle_is_reference(sig));
            ctx.check("long_class_name_signature_size",
                      std::string{ mp.signature() }.size() == sig.size());
        }

        // Very long PARAM area, primitive RETURN: "(" + 1000*'I' + ")J" -> FALSE.
        // find(')') must locate the ')' after the long param run and read 'J'.
        {
            std::string sig{ "(" };
            sig.append(static_cast<std::size_t>(1000), 'I');
            sig += ")J";
            const vmhook::method_proxy mp{ nullptr, nullptr, sig };
            ctx.check("long_param_ret_J_is_reference_false", mp.is_reference() == false);
            ctx.check("long_param_ret_J_oracle_agree",
                      mp.is_reference() == oracle_is_reference(sig));
        }
    }

    // =====================================================================
    // 25. RESOLVED-PROXY IDEMPOTENCY + COPY (live JVM, no call()).  The same
    //     stability invariants of section 20, but on PROXIES THE JVM RESOLVED:
    //     repeated is_reference() agrees, a copy agrees, signature() is stable,
    //     and name() is NON-empty for a resolved proxy (it has a real Method*),
    //     in contrast to the empty name() of the hand-built proxies.
    // =====================================================================
    if (singleton)
    {
        struct resolved_case
        {
            const char* name;
            bool        expect;
        };
        const resolved_case rc[]{
            { "retObject", true  },
            { "retString", true  },
            { "retIntArray", true },
            { "retInt",    false },
            { "retVoid",   false },
        };
        for (const resolved_case& r : rc)
        {
            const auto mp{ singleton->get_method(r.name) };
            ctx.check(std::string{ "resolved_idem_resolves_" } + r.name, mp.has_value());
            if (!mp)
            {
                continue;
            }

            const bool first{ mp->is_reference() };
            const bool second{ mp->is_reference() };
            ctx.check(std::string{ "resolved_idem_repeat_stable_" } + r.name,
                      first == second);
            ctx.check(std::string{ "resolved_idem_value_" } + r.name, first == r.expect);

            const std::string sig_a{ mp->signature() };
            const std::string sig_b{ mp->signature() };
            ctx.check(std::string{ "resolved_idem_signature_stable_" } + r.name,
                      sig_a == sig_b);

            // Copy preserves is_reference() and signature().
            const vmhook::method_proxy copy{ *mp };
            ctx.check(std::string{ "resolved_copy_is_reference_agrees_" } + r.name,
                      copy.is_reference() == first);
            ctx.check(std::string{ "resolved_copy_signature_agrees_" } + r.name,
                      std::string{ copy.signature() } == sig_a);

            // A resolved proxy has a real Method*, so name() is NON-empty and
            // equals the requested method name -- the contrast against the
            // empty name() of hand-built null-Method* proxies (section 20e).
            ctx.check(std::string{ "resolved_name_nonempty_" } + r.name,
                      !mp->name().empty());
            ctx.check(std::string{ "resolved_name_matches_" } + r.name,
                      mp->name() == r.name);

            // is_reference() stays correct independent of the Method* identity:
            // a valid Method* and a true/false verdict coexist with no deref.
            vmhook::hotspot::method* const m{ mp->raw_method() };
            ctx.check(std::string{ "resolved_raw_method_valid_" } + r.name,
                      m != nullptr && vmhook::hotspot::is_valid_pointer(m));
        }
    }
}
