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
}
