// Standalone unit test: field_static — COLD-STATE behaviour of the portable
// `object<T>::static_field("name")` accessor and of static-flavoured
// `field_proxy` instances built directly over null pointers.
//
// WAVE-30 LEDGER GAPS this file closes (no-JVM):
//   * cold-state static_field on an UNREGISTERED wrapper resolves the klass to
//     nullptr and returns std::nullopt (no crash, no allocation),
//   * static_field is a static member function (callable without an instance) —
//     pinned via decltype + static_assert,
//   * static_field signature returns std::optional<field_proxy> — pinned,
//   * cold-state static_method on an unregistered wrapper also returns nullopt
//     (sibling factory; same resolve_klass null arm),
//   * a static-flavoured field_proxy built with (nullptr, sig, true) returns
//     the safe-default value_t on get() for EVERY primitive descriptor
//     (Z/B/C/S/I/J/F/D) AND for "Ljava/lang/String;" and "[I",
//   * the static-flavoured proxy's is_reference() classification is correct
//     for L/[ vs primitives,
//   * field_proxy::get() is noexcept (static_assert),
//   * 32-iter idempotence — repeating the cold-state static_field call never
//     poisons internal state nor leaks an std::optional.
//
// Complementary to sibling no-JVM files:
//   * test_field_introspection_nojvm.cpp covers read-accessor noexcept +
//     descriptor classification matrix,
//   * test_field_null_safety_nojvm.cpp covers INSTANCE-side null/empty/NUL
//     name handling at the wrapper find_field entry,
//   * THIS file covers the STATIC-side factory + static-proxy safe defaults.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{

int g_failures{ 0 };

auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++g_failures; }
}

// Local wrapper subclassing object<T>.  NOT registered via register_class<>(),
// so resolve_klass(typeid(unregistered_wrapper)) returns nullptr and the
// static_field / static_method factories take the early-out null arm.
struct unregistered_wrapper : public vmhook::object<unregistered_wrapper>
{
    using vmhook::object<unregistered_wrapper>::object;
};

// ---------------------------------------------------------------------------
// SECTION 1 — signature locks on `static_field` and `static_method`.
// These are STATIC member functions of object<derived>.  Pin via static_assert
// so a refactor that converts them to non-static (which would silently break
// every "MyClass::static_field(...)" call site) trips at compile time.
// ---------------------------------------------------------------------------

using static_field_fn_t  = std::optional<vmhook::field_proxy>(*)(std::string_view);
using static_method1_fn_t = std::optional<vmhook::method_proxy>(*)(std::string_view);
using static_method2_fn_t = std::optional<vmhook::method_proxy>(*)(std::string_view, std::string_view);

static_assert(std::is_same_v<decltype(&unregistered_wrapper::static_field),
                             static_field_fn_t>,
              "object<T>::static_field must be a static member taking "
              "std::string_view and returning optional<field_proxy>");

// static_method has two overloads; we cannot use decltype(&...) on an
// overloaded name, so resolve each via a static_cast to the target function
// pointer type — a compile error here means the overload no longer exists
// with the documented signature.
// A failed conversion-init of these constexpr function pointers means the
// matching overload no longer exists with the documented signature.
constexpr static_method1_fn_t  kStaticMethod1{ &unregistered_wrapper::static_method };
constexpr static_method2_fn_t  kStaticMethod2{ &unregistered_wrapper::static_method };
static_assert(std::is_same_v<decltype(kStaticMethod1), const static_method1_fn_t>,
              "static_method(string_view) overload signature lock");
static_assert(std::is_same_v<decltype(kStaticMethod2), const static_method2_fn_t>,
              "static_method(string_view, string_view) overload signature lock");

// field_proxy::get() is documented noexcept; static_field's whole safety story
// rests on this.  Pin it.
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().get()),
              "field_proxy::get() must be noexcept (static-field cold-path "
              "safety contract)");

// The 5-arg static-aware ctor must be noexcept-constructible from
// (nullptr, "", true, nullptr, 0).
static_assert(std::is_nothrow_constructible_v<vmhook::field_proxy,
                                              void*, std::string, bool,
                                              vmhook::hotspot::klass*, std::size_t>,
              "field_proxy 5-arg static-aware ctor must be noexcept");

// ---------------------------------------------------------------------------
// SECTION 2 — cold-state static_field returns nullopt on an unregistered
// wrapper.  resolve_klass(typeid(T)) is nullptr → the documented early-out
// at vmhook.hpp:18202-18209 takes over and returns std::nullopt without
// touching any HotSpot internals.  Verify across multiple name shapes.
// ---------------------------------------------------------------------------

auto section_cold_static_field_returns_nullopt() -> void
{
    {
        const auto proxy{ unregistered_wrapper::static_field("anyName") };
        check("cold static_field('anyName') == nullopt",
              !proxy.has_value());
    }
    {
        const auto proxy{ unregistered_wrapper::static_field("") };
        check("cold static_field('') == nullopt",
              !proxy.has_value());
    }
    {
        // Pointer-only-NUL string_view (data == nullptr, size == 0).
        constexpr std::string_view null_view{};
        const auto proxy{ unregistered_wrapper::static_field(null_view) };
        check("cold static_field(null_view) == nullopt (no crash)",
              !proxy.has_value());
    }
    {
        // Long name — bounded by string_view, no truncation logic in the
        // resolve path, just bails on null klass.
        const std::string long_name(2048, 'x');
        const auto proxy{ unregistered_wrapper::static_field(long_name) };
        check("cold static_field(2KiB-name) == nullopt",
              !proxy.has_value());
    }
}

// ---------------------------------------------------------------------------
// SECTION 3 — cold-state static_method (sibling factory, same null-klass arm)
// also returns nullopt.  Both 1-arg and 2-arg overloads.
// ---------------------------------------------------------------------------

auto section_cold_static_method_returns_nullopt() -> void
{
    {
        const auto proxy{ unregistered_wrapper::static_method("anyMethod") };
        check("cold static_method('anyMethod') == nullopt",
              !proxy.has_value());
    }
    {
        const auto proxy{ unregistered_wrapper::static_method("anyMethod", "()V") };
        check("cold static_method('anyMethod', '()V') == nullopt",
              !proxy.has_value());
    }
}

// ---------------------------------------------------------------------------
// SECTION 4 — static-flavoured field_proxy built with (nullptr, sig, true)
// returns the safe-default value_t on get() for EVERY JVM type descriptor.
// The `is_static=true` flag must NOT change the null-pointer safe-default
// behaviour: get()'s mirror_klass arm is skipped (null mirror_klass), then
// the !read_pointer guard hits and returns value_t{ int32_t{}, sig }.
// ---------------------------------------------------------------------------

auto section_null_static_proxy_safe_defaults() -> void
{
    auto verify_prim = [&](const char* sig, const char* label) {
        vmhook::field_proxy proxy{ nullptr, std::string{ sig }, /*is_static=*/true };
        const auto v{ proxy.get() };
        const int as_int{ v };
        check(label, as_int == 0);
        check((std::string{ label } + " sig round-trip").c_str(),
              v.signature == sig);
    };

    verify_prim("Z", "null static-proxy Z (boolean) -> 0");
    verify_prim("B", "null static-proxy B (byte) -> 0");
    verify_prim("C", "null static-proxy C (char) -> 0");
    verify_prim("S", "null static-proxy S (short) -> 0");
    verify_prim("I", "null static-proxy I (int) -> 0");
    verify_prim("J", "null static-proxy J (long) -> 0");
    verify_prim("F", "null static-proxy F (float) -> 0");
    verify_prim("D", "null static-proxy D (double) -> 0");

    // Reference descriptors: get() still returns value_t default; is_reference
    // must report true (descriptor classification, no deref needed).
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "Ljava/lang/String;" },
                                   /*is_static=*/true };
        const auto v{ proxy.get() };
        const int as_int{ v };
        check("null static-proxy Ljava/lang/String; get() -> default 0",
              as_int == 0);
        check("null static-proxy Ljava/lang/String; is_reference() == true",
              proxy.is_reference() == true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "[I" },
                                   /*is_static=*/true };
        const auto v{ proxy.get() };
        const int as_int{ v };
        check("null static-proxy [I get() -> default 0",
              as_int == 0);
        check("null static-proxy [I is_reference() == true",
              proxy.is_reference() == true);
    }

    // Primitives classify as NON-reference even on the null static-proxy.
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "I" },
                                   /*is_static=*/true };
        check("null static-proxy I is_reference() == false",
              proxy.is_reference() == false);
    }
}

// ---------------------------------------------------------------------------
// SECTION 5 — 5-arg static-aware ctor with (nullptr, sig, true, nullptr, 0).
// This is the EXACT shape used by object_base::get_field when forging a
// static field_proxy.  Validate that an explicit `mirror_klass == nullptr`
// keeps get() on the safe-default path (the mirror-resolution arm at
// vmhook.hpp:15590-15597 is skipped when mirror_klass is null).
// ---------------------------------------------------------------------------

auto section_five_arg_static_ctor_safe_defaults() -> void
{
    vmhook::field_proxy proxy{
        /*field_pointer=*/nullptr,
        /*sig=*/std::string{ "I" },
        /*is_static=*/true,
        /*mirror_klass=*/nullptr,
        /*field_offset=*/0u
    };
    const auto v{ proxy.get() };
    const int as_int{ v };
    check("5-arg ctor (null,'I',true,null,0) get() -> 0", as_int == 0);
    check("5-arg ctor raw_address() echoes null",
          proxy.raw_address() == nullptr);
    check("5-arg ctor is_reference() false on 'I'",
          proxy.is_reference() == false);

    // A non-zero offset with a null mirror still safe: read_pointer stays at
    // field_pointer (null) because mirror_klass is null; safe default hits.
    vmhook::field_proxy proxy_off{
        nullptr, std::string{ "J" }, true, nullptr, /*field_offset=*/64u
    };
    const auto v2{ proxy_off.get() };
    const int as_int2{ v2 };
    check("5-arg ctor (null,'J',true,null,64) get() -> 0 (offset ignored)",
          as_int2 == 0);
}

// ---------------------------------------------------------------------------
// SECTION 6 — 32-iter idempotence on the cold static_field miss path.  No
// internal cache should poison and no allocation should leak; the result
// must remain nullopt across the full loop.
// ---------------------------------------------------------------------------

auto section_idempotent_cold_miss() -> void
{
    bool all_nullopt{ true };
    for (int i{ 0 }; i < 32; ++i)
    {
        const auto proxy{ unregistered_wrapper::static_field("never_resolved") };
        if (proxy.has_value()) { all_nullopt = false; break; }
    }
    check("32x cold static_field miss stays nullopt (no cache poison)",
          all_nullopt);
}

} // anonymous namespace

int main()
{
    std::printf("field_static no-JVM unit test\n");
    section_cold_static_field_returns_nullopt();
    section_cold_static_method_returns_nullopt();
    section_null_static_proxy_safe_defaults();
    section_five_arg_static_ctor_safe_defaults();
    section_idempotent_cold_miss();
    if (g_failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", g_failures);
    return 1;
}
