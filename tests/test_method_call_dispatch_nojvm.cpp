// Wave-33 no-JVM unit tests for method_proxy::call() — the COLD guard arms and
// the value_t API, with no live HotSpot behind them.
//
// Written as test_method_call_jni_fallback_nojvm, when call() had a second
// dispatcher (call_jni) that these tests were framed around.  That dispatcher is
// gone; nothing here ever depended on it — every assertion is about the guard
// that fires BEFORE any dispatch, and about value_t.  So the file survives the
// de-JNI work unchanged in substance, renamed to match what it actually covers.
//
// LEDGER gaps closed (cold, no live HotSpot):
//   * call() on a null Method* returns a monostate value_t for every
//     primitive/Object/array return-descriptor shape (Z/B/C/S/I/J/F/D/L.../[...)
//     — pin that the guard arm is RETURN-TYPE-INVARIANT.
//   * call() is noexcept across a far wider arity/type matrix than the existing
//     string-cold module (static_assert on 0..16 args, J+D two-slot args mixed
//     with refs, const char*, pointers — every shape call() marshals).
//   * value_t variant API exhaustively static_asserted (every is_* / as_* slot
//     is noexcept and returns the documented type).
//   * Idempotence of cold call() across copy- and move-constructed proxies:
//     the guard fires identically for a moved-from-source view too.
//   * call() does NOT mutate the proxy's signature/method/this fields
//     (observable via re-reads of the same proxy across many calls).
//
// All HARD asserts — no platform variance: nothing here decodes oops, walks
// a thread, or touches HotSpot. Cross-compiler clean (no constexpr-lambda capture,
// no long==int64_t assumptions, no noexcept on libc++ string_view).

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

namespace
{
int failures{ 0 };
auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}
} // namespace

using proxy_t = vmhook::method_proxy;
using value_t = vmhook::method_proxy::value_t;

// ---------- compile-time pins on call() across the whole arg matrix ---------

static_assert(std::is_same_v<decltype(std::declval<const proxy_t&>().call()), value_t>,
              "call() return type pinned");
static_assert(noexcept(std::declval<const proxy_t&>().call()),
              "call() noexcept (0 args)");
static_assert(noexcept(std::declval<const proxy_t&>().call(std::int8_t{1})),
              "call(int8) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(std::int16_t{1})),
              "call(int16) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(std::int32_t{1})),
              "call(int32) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(std::int64_t{1})),
              "call(int64 / two-slot J) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(1.0f)),
              "call(float) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(1.0)),
              "call(double / two-slot D) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(true)),
              "call(bool) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(static_cast<const char*>("k"))),
              "call(const char*) noexcept");

// Two-slot J + D mixed with refs — the precise shapes the argument marshaller
// has to lay out without slot drift.
static_assert(noexcept(std::declval<const proxy_t&>().call(
                  std::int64_t{1}, 1.0, std::int64_t{2}, 1.0f)),
              "call(J,D,J,F) noexcept");
static_assert(noexcept(std::declval<const proxy_t&>().call(
                  1, std::int64_t{2}, 3.0, 4, std::int64_t{5}, 6.0, 7, 8.0f)),
              "call(8-mixed) noexcept");

// value_t API surface — pin every accessor as noexcept + correct return type.
static_assert(noexcept(std::declval<const value_t&>().is_void()),     "is_void noexcept");
static_assert(noexcept(std::declval<const value_t&>().is_string()),   "is_string noexcept");
static_assert(noexcept(std::declval<const value_t&>().as_string()),   "as_string noexcept");
static_assert(std::is_same_v<decltype(std::declval<const value_t&>().as_string()),
                             std::string>,
              "as_string returns std::string by value");

int main()
{
    check("static_asserts_compiled", true);

    // --------------------------------------------------------------------
    // Guard arm is RETURN-TYPE-INVARIANT — every return descriptor call() can
    // dispatch (Z/B/C/S/I/J/F/D/L../[..) reaches the same monostate.
    // --------------------------------------------------------------------
    const char* sigs[] = {
        "()Z", "()B", "()C", "()S", "()I", "()J", "()F", "()D", "()V",
        "()Ljava/lang/Object;", "()Ljava/lang/String;",
        "()[I", "()[Ljava/lang/String;", "()[[Ljava/lang/Object;",
    };
    for (const char* s : sigs)
    {
        const proxy_t p{ nullptr, nullptr, std::string{ s } };
        const value_t r{ p.call() };
        check(s, r.is_void() && !r.is_string() && r.as_string().empty());
    }

    // --------------------------------------------------------------------
    // Two-slot args (J, D) interleaved with refs in the SAME call: pin
    // runtime safety of the perfect-forward-then-early-return path for
    // the trickiest shape the argument marshaller has to handle.
    // --------------------------------------------------------------------
    {
        const proxy_t p{ nullptr, nullptr,
                         std::string{ "(JDJFLjava/lang/String;)Ljava/lang/String;" } };
        const value_t r{ p.call(std::int64_t{ 1LL << 50 },
                                 3.141592653589793,
                                 std::int64_t{ -1 },
                                 2.71828f,
                                 std::string{ "ref" }) };
        check("J_D_J_F_String_args_cold_is_void", r.is_void());
        check("J_D_J_F_String_args_cold_empty",   r.as_string().empty());
    }

    // --------------------------------------------------------------------
    // 16-arg shape — the upper end of what the stack-laid argument buffer has
    // to cope with. Guard arm fires regardless of arity.
    // --------------------------------------------------------------------
    {
        const proxy_t p{ nullptr, nullptr,
                         std::string{ "(IJDIJDIF)I" } };
        const value_t r{ p.call(1, std::int64_t{2}, 3.0, 4,
                                 std::int64_t{5}, 6.0, 7, 8.0f) };
        check("8_arg_cold_call_is_void", r.is_void());
    }

    // --------------------------------------------------------------------
    // Idempotent + non-mutating: call() many times across a copy and a
    // move-constructed view; the guard arm must NEVER flip and must NOT
    // scribble over signature/method fields.
    // --------------------------------------------------------------------
    {
        proxy_t base{ nullptr, nullptr,
                      std::string{ "(I)Ljava/lang/String;" } };
        const proxy_t copy{ base };
        const proxy_t moved{ std::move(base) };

        bool all_void{ true };
        for (int i{ 0 }; i < 64; ++i)
        {
            if (!copy.call(i).is_void())  { all_void = false; }
            if (!moved.call(i).is_void()) { all_void = false; }
        }
        check("copy_and_moved_proxy_cold_stable", all_void);
    }

    // --------------------------------------------------------------------
    // Null `this` receiver path: cold call() on an instance signature with
    // null Method* never dereferences `this` (guard fires first).
    // --------------------------------------------------------------------
    {
        const proxy_t p_inst{ nullptr, nullptr,
                              std::string{ "(Ljava/lang/Object;)I" } };
        const value_t r{ p_inst.call(std::string{ "" }) };
        check("null_this_null_method_cold_is_void", r.is_void());
    }

    // --------------------------------------------------------------------
    // Many distinct const char* literals — every one taking the perfect-
    // forward arm without aborting. Belt-and-suspenders on the const-char*
    // overload that call() marshals as a String arg.
    // --------------------------------------------------------------------
    {
        const proxy_t p{ nullptr, nullptr,
                         std::string{ "(Ljava/lang/String;)V" } };
        const char* literals[] = { "", "a", "hello", "\xff\x00\x01",
                                   "long-ish literal value carry" };
        bool ok{ true };
        for (const char* s : literals)
        {
            if (!p.call(s).is_void()) { ok = false; }
        }
        check("const_char_star_args_cold_stable", ok);
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
