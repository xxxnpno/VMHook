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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
        { "retObjectArray",   "()[Ljava/lang/Object;" },
        { "retStringArray",   "()[Ljava/lang/String;" },
        { "retString2DArray", "()[[Ljava/lang/String;" },
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
}
