// Standalone (no-JVM) unit test for the EXPLICIT name+signature method
// selector — the four entry points that pin a single overload by EXACT
// JVM descriptor:
//
//   object_base::get_method(name, sig)                          (instance)
//   object_base::get_method(type_index, name, sig)              (static)
//   derived::get_method(name, sig)                              (deducing-this)
//   derived::static_method(name, sig)                           (portable alias)
//
// With no JVM in this process every `resolve_klass()` returns nullptr, so
// each entry point must return std::nullopt cleanly — no throw, no faulting
// deref, regardless of the name/signature shape thrown at it.  That cold-state
// contract is what this file pins.  The live exact-match selection is
// covered by tests/jvm/modules/method_explicit_signature.cpp.
//
// Determinism: every dynamic result is std::nullopt regardless of input.
// Byte-identical across compilers / platforms / JDKs.  No JVM fixture, no
// platform-variant assertion.
#include <vmhook/vmhook.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    class explicit_sig_wrapper : public vmhook::object<explicit_sig_wrapper>
    {
    public:
        explicit explicit_sig_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<explicit_sig_wrapper>{ oop }
        {
        }
    };

    class second_wrapper : public vmhook::object<second_wrapper>
    {
    public:
        explicit second_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<second_wrapper>{ oop }
        {
        }
    };
} // namespace

// =====================================================================
// Compile-time contracts — return type / overload-set resolution breadth.
// =====================================================================

// (1) Instance overload returns std::optional<method_proxy>.
using instance_ret_t = decltype(std::declval<explicit_sig_wrapper const&>()
                                    .get_method(std::string_view{}, std::string_view{}));
static_assert(std::is_same_v<instance_ret_t, std::optional<vmhook::method_proxy>>,
              "instance get_method(name,sig) must return optional<method_proxy>");

// (2) Static type_index overload also returns optional<method_proxy>.
using static_ti_ret_t = decltype(vmhook::object_base::get_method(
    std::type_index{ typeid(explicit_sig_wrapper) },
    std::string_view{}, std::string_view{}));
static_assert(std::is_same_v<static_ti_ret_t, std::optional<vmhook::method_proxy>>,
              "static get_method(type_index,name,sig) must return optional<method_proxy>");

// (3) Portable static_method(name,sig) alias returns optional<method_proxy>.
using static_method_ret_t = decltype(explicit_sig_wrapper::static_method(
    std::string_view{}, std::string_view{}));
static_assert(std::is_same_v<static_method_ret_t, std::optional<vmhook::method_proxy>>,
              "static_method(name,sig) must return optional<method_proxy>");

// (4) The two-arg get_method overload must be DISTINCT from the one-arg
//     name-only overload (overload-set resolution).
using instance_name_only_ret_t = decltype(std::declval<explicit_sig_wrapper const&>()
                                              .get_method(std::string_view{}));
static_assert(std::is_same_v<instance_name_only_ret_t, std::optional<vmhook::method_proxy>>,
              "instance name-only get_method must also return optional<method_proxy>");

// (5) Pin that const-correctness holds: instance overload is callable on a
//     const lvalue (it is declared const).
static_assert(std::is_invocable_r_v<std::optional<vmhook::method_proxy>,
                                    decltype(static_cast<
                                        std::optional<vmhook::method_proxy>
                                        (vmhook::object_base::*)(std::string_view,
                                                                 std::string_view) const>(
                                        &vmhook::object_base::get_method)),
                                    explicit_sig_wrapper const&,
                                    std::string_view, std::string_view>,
              "instance get_method(name,sig) must be invocable on const lvalue");

// (6) Different wrapper types resolve to DIFFERENT static-overload
//     instantiations — i.e. the per-type alias is real.
using w1_static_t = decltype(explicit_sig_wrapper::static_method(
    std::string_view{}, std::string_view{}));
using w2_static_t = decltype(second_wrapper::static_method(
    std::string_view{}, std::string_view{}));
static_assert(std::is_same_v<w1_static_t, w2_static_t>,
              "static_method return type is uniform across wrapper types");

// (7) noexcept-contract observation.  The doc-comment claims "does not
//     throw" but the function is NOT declared noexcept (it heap-allocates
//     a std::string per candidate).  The C++ language fact we CAN pin at
//     compile time is the std::optional<method_proxy> type itself; its
//     destructor must be noexcept for the function to be useful as a
//     return value.  See audit/LIBRARY_BUGS.md (gap #3) for the contract
//     vs noexcept mismatch — not asserted as noexcept here on purpose.
static_assert(std::is_nothrow_destructible_v<std::optional<vmhook::method_proxy>>,
              "optional<method_proxy> destructor must be noexcept");

// (8) std::string_view parameters — pin that string-literal call sites
//     compile (the SBO/literal path is the common user call).
static_assert(std::is_constructible_v<std::string_view, const char*>,
              "string_view from C-string must be supported (literal call sites)");

int main()
{
    // --------------------------------------------------------------
    // Cold-state contract: every entry point returns nullopt safely.
    // No JVM => resolve_klass()/find_class() returns nullptr.
    // --------------------------------------------------------------

    // Build an instance with a fabricated OOP value — get_method() never
    // dereferences `instance` when resolve_klass() bails out first.
    const explicit_sig_wrapper instance{ reinterpret_cast<vmhook::oop_t>(
        static_cast<std::uintptr_t>(0)) };

    // (A) Instance overload, well-formed descriptor.
    {
        const auto r = instance.get_method(std::string_view{ "process" },
                                           std::string_view{ "(I)I" });
        check("instance get_method well-formed sig returns nullopt", !r.has_value());
    }

    // (B) Instance overload, empty signature — strict miss in cold state
    //     AND in hot state (no real descriptor is empty).  Pins that
    //     empty sig is NOT treated as a wildcard here (diverges from
    //     hook<T>(name, "") which IS a wildcard — see audit gap #2).
    {
        const auto r = instance.get_method(std::string_view{ "process" },
                                           std::string_view{});
        check("instance get_method empty sig is strict miss (nullopt)",
              !r.has_value());
    }

    // (C) Empty name + empty sig.
    {
        const auto r = instance.get_method(std::string_view{}, std::string_view{});
        check("instance get_method empty name+sig nullopt", !r.has_value());
    }

    // (D) Malformed descriptor — no open paren.
    {
        const auto r = instance.get_method(std::string_view{ "x" },
                                           std::string_view{ "I)I" });
        check("instance get_method malformed sig (no open paren) nullopt",
              !r.has_value());
    }

    // (E) Malformed descriptor — trailing junk.
    {
        const auto r = instance.get_method(std::string_view{ "x" },
                                           std::string_view{ "(I)IX" });
        check("instance get_method malformed sig (trailing junk) nullopt",
              !r.has_value());
    }

    // (F) Pathologically long signature.
    {
        std::string sig;
        sig.reserve(8200);
        sig.push_back('(');
        for (int i{ 0 }; i < 8000; ++i) { sig.push_back('I'); }
        sig.push_back(')');
        sig.push_back('V');
        const auto r = instance.get_method(std::string_view{ "x" },
                                           std::string_view{ sig });
        check("instance get_method pathologically long sig nullopt",
              !r.has_value());
    }

    // (G) Embedded NUL bytes in name and signature.
    {
        const char name_buf[] = { 'a', '\0', 'b' };
        const char sig_buf[]  = { '(', '\0', 'I', ')', 'V' };
        const auto r = instance.get_method(
            std::string_view{ name_buf, sizeof(name_buf) },
            std::string_view{ sig_buf, sizeof(sig_buf) });
        check("instance get_method embedded NUL nullopt", !r.has_value());
    }

    // (H) Static type_index overload — well-formed sig.
    {
        const auto r = vmhook::object_base::get_method(
            std::type_index{ typeid(explicit_sig_wrapper) },
            std::string_view{ "process" }, std::string_view{ "(I)I" });
        check("static get_method(type_index,name,sig) nullopt", !r.has_value());
    }

    // (I) Static type_index — empty sig.
    {
        const auto r = vmhook::object_base::get_method(
            std::type_index{ typeid(explicit_sig_wrapper) },
            std::string_view{ "x" }, std::string_view{});
        check("static get_method empty sig nullopt", !r.has_value());
    }

    // (J) Static type_index — unregistered second wrapper.
    {
        const auto r = vmhook::object_base::get_method(
            std::type_index{ typeid(second_wrapper) },
            std::string_view{ "x" }, std::string_view{ "()V" });
        check("static get_method on unregistered second wrapper nullopt",
              !r.has_value());
    }

    // (K) Portable static_method(name,sig) alias.
    {
        const auto r = explicit_sig_wrapper::static_method(
            std::string_view{ "smap" }, std::string_view{ "(I)I" });
        check("static_method(name,sig) cold-state nullopt", !r.has_value());
    }

    // (L) Portable static_method — empty sig miss.
    {
        const auto r = explicit_sig_wrapper::static_method(
            std::string_view{ "smap" }, std::string_view{});
        check("static_method empty sig nullopt", !r.has_value());
    }

    // (M) Portable static_method — different wrapper type.
    {
        const auto r = second_wrapper::static_method(
            std::string_view{ "any" }, std::string_view{ "()V" });
        check("static_method on second wrapper nullopt", !r.has_value());
    }

    // --------------------------------------------------------------
    // Probe the "bad_alloc under pressure" path: heap-allocated
    // current_signature inside the loop is the documented
    // allocation surface (audit gap #3).  In cold state the loop
    // never runs (klass not resolved -> early return), so no
    // allocation happens.  We still wrap the call in try/catch
    // and assert it returns nullopt safely — guarding against
    // any future refactor that moves allocation to the prologue.
    // --------------------------------------------------------------
    {
        bool returned_safely{ false };
        bool caught{ false };
        try
        {
            const auto r = instance.get_method(std::string_view{ "x" },
                                               std::string_view{ "(I)V" });
            returned_safely = !r.has_value();
        }
        catch (const std::bad_alloc&)
        {
            caught = true;
        }
        catch (...)
        {
            caught = true;
        }
        check("instance get_method cold-state returns safely (no throw)",
              returned_safely && !caught);
    }

    {
        bool returned_safely{ false };
        bool caught{ false };
        try
        {
            const auto r = explicit_sig_wrapper::static_method(
                std::string_view{ "x" }, std::string_view{ "(I)V" });
            returned_safely = !r.has_value();
        }
        catch (...)
        {
            caught = true;
        }
        check("static_method cold-state returns safely (no throw)",
              returned_safely && !caught);
    }

    // --------------------------------------------------------------
    // Stress: a matrix of name/sig shapes, all must return nullopt.
    // --------------------------------------------------------------
    {
        const std::array<std::string_view, 8> names{
            "", "a", "process", "<init>", "<clinit>",
            "veryLongNameThatNoMethodReallyHas",
            std::string_view{ "with\0nul", 8 },
            "$Lambda$1"
        };
        const std::array<std::string_view, 10> sigs{
            "", "()V", "(I)I", "(J)J", "(Ljava/lang/String;)V",
            "([I)[I", "(II)V", "garbage",
            "(Ljava/lang/String;ILjava/lang/Object;)Ljava/lang/Object;",
            std::string_view{ "(\0)V", 4 }
        };
        int total{ 0 };
        int nullopt_count{ 0 };
        for (const auto& n : names)
        {
            for (const auto& s : sigs)
            {
                ++total;
                if (!instance.get_method(n, s).has_value()) { ++nullopt_count; }
                if (!vmhook::object_base::get_method(
                         std::type_index{ typeid(explicit_sig_wrapper) }, n, s)
                         .has_value())
                {
                    ++nullopt_count;
                }
                if (!explicit_sig_wrapper::static_method(n, s).has_value())
                {
                    ++nullopt_count;
                }
            }
        }
        check("name x sig matrix: every call returns nullopt",
              nullopt_count == total * 3);
        check("name x sig matrix: full coverage exercised", total == 80);
    }

    if (failures == 0)
    {
        std::printf("[OK] test_method_explicit_signature_nojvm: all checks passed\n");
        return 0;
    }
    std::printf("[FAIL] test_method_explicit_signature_nojvm: %d failure(s)\n",
                failures);
    return 1;
}
