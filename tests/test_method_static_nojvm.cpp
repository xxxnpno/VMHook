// Standalone (no-JVM) unit test for the NAME-ONLY static_method() entry
// point — `vmhook::object<T>::static_method("name")` — which forwards to
// `object_base::get_method(type_index, name)` (the static resolution path).
//
// In a process without a live JVM `resolve_klass()` returns nullptr, so the
// call must short-circuit to std::nullopt cleanly: no throw, no faulting
// deref of the (null) klass, regardless of the name shape thrown at it.
// This pins the cold-state contract complementary to
// test_method_explicit_signature_nojvm.cpp (which covers the (name,sig)
// overload).
//
// Determinism: every dynamic result is std::nullopt, every static_assert is
// a pure compile-time check.  Byte-identical across compilers / platforms /
// JDKs.  No JVM fixture, no platform-variant assertion.
#include <vmhook/vmhook.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    class static_wrapper : public vmhook::object<static_wrapper>
    {
    public:
        explicit static_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<static_wrapper>{ oop }
        {
        }
    };

    class other_wrapper : public vmhook::object<other_wrapper>
    {
    public:
        explicit other_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<other_wrapper>{ oop }
        {
        }
    };
} // namespace

// =====================================================================
// Compile-time contracts.
// =====================================================================

// (1) Name-only static_method returns std::optional<method_proxy>.
using sm_ret_t = decltype(static_wrapper::static_method(std::string_view{}));
static_assert(std::is_same_v<sm_ret_t, std::optional<vmhook::method_proxy>>,
              "static_method(name) must return optional<method_proxy>");

// (2) Same return type across distinct wrapper types.
using sm_other_ret_t = decltype(other_wrapper::static_method(std::string_view{}));
static_assert(std::is_same_v<sm_ret_t, sm_other_ret_t>,
              "static_method return type is uniform across wrapper types");

// (3) The optional<method_proxy> destructor must be noexcept so that
//     stack-unwinding through callers is well-formed.
static_assert(std::is_nothrow_destructible_v<std::optional<vmhook::method_proxy>>,
              "optional<method_proxy> destructor must be noexcept");

// (4) Underlying object_base::get_method(type_index, name) also returns
//     optional<method_proxy> (this is what static_method forwards to).
using base_static_ret_t = decltype(vmhook::object_base::get_method(
    std::type_index{ typeid(static_wrapper) }, std::string_view{}));
static_assert(std::is_same_v<base_static_ret_t,
                             std::optional<vmhook::method_proxy>>,
              "object_base::get_method(type_index,name) must return optional<method_proxy>");

// (5) The name-only and (name,sig) overloads are DISTINCT functions but
//     return the SAME type.
using sm_sig_ret_t = decltype(static_wrapper::static_method(
    std::string_view{}, std::string_view{}));
static_assert(std::is_same_v<sm_ret_t, sm_sig_ret_t>,
              "name-only and (name,sig) static_method overloads return the same type");

// (6) string_view literal call-sites compile (the common user path).
static_assert(std::is_constructible_v<std::string_view, const char*>,
              "string_view from C-string must be supported (literal call sites)");

// (7) method_proxy::is_static() return type — bool.  This is the user-
//     visible discriminator the live module relies on; pin it here so any
//     future change to its signature trips this TU at compile time.
using is_static_ret_t = decltype(std::declval<vmhook::method_proxy const&>()
                                     .is_static());
static_assert(std::is_same_v<is_static_ret_t, bool>,
              "method_proxy::is_static() must return bool");

// (8) method_proxy::get_compressed_oop() — receiver OOP of a static proxy
//     is documented to be 0.  Just pin the return type is integral so the
//     value comparison `== 0` is well-typed.
using compressed_oop_ret_t = decltype(std::declval<vmhook::method_proxy const&>()
                                          .get_compressed_oop());
static_assert(std::is_integral_v<compressed_oop_ret_t>,
              "method_proxy::get_compressed_oop() must return an integral type");

int main()
{
    // --------------------------------------------------------------
    // Cold-state contract: every call returns nullopt safely.
    // No JVM => resolve_klass() returns nullptr, the superclass walk
    // never executes, no method is matched, no signature std::string
    // is allocated.
    // --------------------------------------------------------------

    // (A) Plain ascii name.
    {
        const auto r = static_wrapper::static_method(std::string_view{ "snap" });
        check("static_method ascii name nullopt", !r.has_value());
    }

    // (B) Empty name.
    {
        const auto r = static_wrapper::static_method(std::string_view{});
        check("static_method empty name nullopt", !r.has_value());
    }

    // (C) Instance-style method name — cold path doesn't care; the open
    //     library flaw (no JVM_ACC_STATIC filter) is invisible here, since
    //     the klass walk never runs.  This still pins that the cold-state
    //     contract is uniform across name shapes.
    {
        const auto r = static_wrapper::static_method(
            std::string_view{ "iGetSeed" });
        check("static_method instance-shape name nullopt", !r.has_value());
    }

    // (D) Constructor name.
    {
        const auto r = static_wrapper::static_method(std::string_view{ "<init>" });
        check("static_method <init> nullopt", !r.has_value());
    }

    // (E) Class-init name (the only "real" static name in the JVM).
    {
        const auto r = static_wrapper::static_method(
            std::string_view{ "<clinit>" });
        check("static_method <clinit> nullopt", !r.has_value());
    }

    // (F) Embedded NUL in name.
    {
        const char buf[] = { 'a', '\0', 'b' };
        const auto r = static_wrapper::static_method(
            std::string_view{ buf, sizeof(buf) });
        check("static_method embedded NUL nullopt", !r.has_value());
    }

    // (G) Very long name.
    {
        std::string name(8000, 'x');
        const auto r = static_wrapper::static_method(std::string_view{ name });
        check("static_method pathologically long name nullopt", !r.has_value());
    }

    // (H) Different (also unregistered) wrapper type — distinct type_index
    //     path, same cold-state nullopt.
    {
        const auto r = other_wrapper::static_method(std::string_view{ "x" });
        check("static_method on other wrapper nullopt", !r.has_value());
    }

    // (I) Underlying object_base::get_method(type_index, name) direct call.
    {
        const auto r = vmhook::object_base::get_method(
            std::type_index{ typeid(static_wrapper) },
            std::string_view{ "x" });
        check("object_base::get_method(type_index,name) nullopt",
              !r.has_value());
    }

    // (J) object_base on the SECOND wrapper through type_index.
    {
        const auto r = vmhook::object_base::get_method(
            std::type_index{ typeid(other_wrapper) },
            std::string_view{ "y" });
        check("object_base::get_method other wrapper nullopt",
              !r.has_value());
    }

    // --------------------------------------------------------------
    // No-throw contract: a refactor that moves allocation earlier
    // (e.g. building the candidate name std::string before the
    // resolve_klass null check) would surface here.  Wrap and assert
    // safe return.
    // --------------------------------------------------------------
    {
        bool returned_safely{ false };
        bool caught{ false };
        try
        {
            const auto r = static_wrapper::static_method(
                std::string_view{ "anything" });
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
    // The cold-state proxy contract: even if a future build accidentally
    // produced a proxy (it shouldn't), the surface is still uniform.
    // Here we just confirm we did NOT get one (the documented behaviour).
    // --------------------------------------------------------------
    {
        const auto r = static_wrapper::static_method(std::string_view{ "go" });
        check("static_method returns optional with no value",
              !r.has_value());
        // has_value()==false implies operator-> is UB; do NOT deref.
    }

    // --------------------------------------------------------------
    // Null-OOP receiver: building an instance with a null mirror oop
    // must not crash; instance-side wrappers never participate in the
    // *static* resolution path, but constructing one with a null oop
    // and IMMEDIATELY calling the static accessor must be safe — the
    // type_index keyed lookup ignores the instance entirely.
    // --------------------------------------------------------------
    {
        const static_wrapper null_instance{
            reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0)) };
        (void)null_instance; // touch the construction, then ignore
        const auto r = static_wrapper::static_method(std::string_view{ "any" });
        check("static_method safe after null-oop instance construction",
              !r.has_value());
    }

    // --------------------------------------------------------------
    // Stress matrix.  Names of every interesting shape; every call
    // must return nullopt for both static_method() and the underlying
    // object_base::get_method() entry.
    // --------------------------------------------------------------
    {
        const std::array<std::string_view, 10> names{
            "", "a", "snap", "go", "<init>", "<clinit>",
            "veryLongStaticNameThatNoMethodHas",
            std::string_view{ "with\0nul", 8 },
            "$Lambda$1",
            "iGetSeed" // instance-shape; cold-state still nullopt
        };
        int total{ 0 };
        int nullopt_count{ 0 };
        for (const auto& n : names)
        {
            ++total;
            if (!static_wrapper::static_method(n).has_value()) { ++nullopt_count; }
            if (!other_wrapper::static_method(n).has_value())  { ++nullopt_count; }
            if (!vmhook::object_base::get_method(
                     std::type_index{ typeid(static_wrapper) }, n)
                     .has_value())
            {
                ++nullopt_count;
            }
        }
        check("name matrix: every call returns nullopt",
              nullopt_count == total * 3);
        check("name matrix: full coverage exercised", total == 10);
    }

    if (failures == 0)
    {
        std::printf("[OK] test_method_static_nojvm: all checks passed\n");
        return 0;
    }
    std::printf("[FAIL] test_method_static_nojvm: %d failure(s)\n", failures);
    return 1;
}
