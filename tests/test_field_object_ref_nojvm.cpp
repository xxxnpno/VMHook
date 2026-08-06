// Standalone unit test: field_object_ref — OBJECT-REFERENCE field access via
// `field_proxy::get()` decoded into `std::unique_ptr<wrapper>` and `void*`.
//
// WAVE-30 LEDGER GAPS this file closes (no-JVM):
//   * cold-state object-ref field accessors on NULL return a null wrapper
//     (`unique_ptr<W>` empty, `void*` nullptr) — never fabricate.
//   * noexcept characterized end-to-end: `field_proxy::get()` AND the
//     `value_t -> unique_ptr<W>` / `value_t -> void*` conversion operators.
//   * static_asserts on the return type of `field_proxy::get()` (value_t) and
//     on convertibility into `std::unique_ptr<W>` / `void*` (the two object-ref
//     decoder targets the cast_for_variant switch produces).
//   * null parent + null child safe-default: a field_proxy built over nullptr
//     with an 'L...;' descriptor yields a null wrapper; built with an empty,
//     '[L...;', or primitive descriptor yields a null wrapper too (FLAW B + the
//     non-uint32 alternative branches all return nullptr by contract).
//
// OUT OF SCOPE (live-JVM module owns these):
//   * non-null ref -> usable wrapper that reads int/String/nested-ref fields
//     and dispatches a method;
//   * self-ref decode identity, shared-ref aliasing,
//   * `re-encode(decode(x)) == x` round-trip on a real compressed OOP;
//   * `operator void*` agreeing with `field_oop()` on a real slot.
//
// The three documented flaws (A: no wrapper-klass match check, B: no signature-
// shape check on unique_ptr targets, C: no signature guard on
// get_compressed_oop) are FIXED in this branch; we re-pin their post-fix
// observable contract here so a regression would surface as a no-JVM failure.

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

// ---------------------------------------------------------------------------
// CONTRACT CHANGE: a std::unique_ptr handed out by vmhook is NEVER null.  The
// POINTER is always valid; the OBJECT inside it is absent when the Java
// reference was null or could not be decoded.  Every "is null" assertion below
// therefore now means "the wrapper arrived, and it holds no instance".
// ---------------------------------------------------------------------------
namespace
{
    template<typename wrapper_t>
    auto is_empty_wrapper(const std::unique_ptr<wrapper_t>& handle) noexcept
        -> bool
    {
        return handle != nullptr
            && handle->vmhook::object_base::get_instance() == nullptr;
    }
}


static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// A minimal wrapper type — same pattern as wp_base in test_wrapper_pattern.
// This is the element_type for std::unique_ptr<ref_object>, the very target
// the cast_for_variant 'L' arm fabricates.
// ---------------------------------------------------------------------------
class ref_object : public vmhook::object<ref_object>
{
public:
    explicit ref_object(vmhook::oop_t oop) noexcept
        : vmhook::object<ref_object>{ oop }
    {
    }
};

static_assert(std::is_base_of_v<vmhook::object_base, ref_object>);

// ---------------------------------------------------------------------------
// SECTION 1 — noexcept + return-type static_asserts on the field-ref read path.
// ---------------------------------------------------------------------------

static_assert(noexcept(std::declval<const vmhook::field_proxy&>().get()),
              "field_proxy::get() must be noexcept");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy&>().get()),
                             vmhook::field_proxy::value_t>,
              "field_proxy::get() must return value_t");

// The two object-ref decoder targets of value_t — both must be reachable
// through the constrained operator target_type() and BOTH must be noexcept.
static_assert(noexcept(static_cast<std::unique_ptr<ref_object>>(
                  std::declval<const vmhook::field_proxy::value_t&>())),
              "value_t -> unique_ptr<wrapper> conversion must be noexcept");
static_assert(noexcept(static_cast<void*>(
                  std::declval<const vmhook::field_proxy::value_t&>())),
              "value_t -> void* conversion must be noexcept");

// Compile-time convertibility — pin both targets are present in the
// value_t_convertible_target_v allowlist (a future tightening that drops
// either silently would break user code).
static_assert(std::is_convertible_v<vmhook::field_proxy::value_t,
                                    std::unique_ptr<ref_object>>,
              "value_t must be convertible to unique_ptr<wrapper>");
static_assert(std::is_convertible_v<vmhook::field_proxy::value_t, void*>,
              "value_t must be convertible to void*");

// ---------------------------------------------------------------------------
// SECTION 2 — null parent + 'L...;' descriptor: the cast_for_variant 'L' arm
// runs (signature.front() == 'L'), the source alternative for an empty proxy
// is the int32_t{0} default (not the uint32 OOP arm), so the unique_ptr arm
// short-circuits to nullptr in the source_type != uint32_t else-branch.
// ---------------------------------------------------------------------------

static auto section_null_proxy_L_descriptor() -> void
{
    vmhook::field_proxy proxy{ nullptr,
                               std::string{ "Lref/object;" },
                               /*is_static=*/false };

    // The proxy reports the descriptor it was built with.
    check("null-proxy 'L...;' signature() round-trips",
          proxy.signature() == "Lref/object;");
    check("null-proxy 'L...;' is_reference() == true",
          proxy.is_reference() == true);
    check("null-proxy 'L...;' raw_address() == nullptr",
          proxy.raw_address() == nullptr);

    // get() with read_pointer == nullptr returns value_t{ int32_t{0}, sig }.
    // The cast_for_variant int32_t arm of unique_ptr returns nullptr.
    const auto value{ proxy.get() };
    std::unique_ptr<ref_object> wrapper = value;
    check("null-proxy 'L...;' -> unique_ptr<ref_object> holds no instance",
          is_empty_wrapper(wrapper));

    // void* conversion from the int32 source alternative also yields nullptr.
    void* const raw = value;
    check("null-proxy 'L...;' -> void* is nullptr",
          raw == nullptr);

    // get_compressed_oop on a null read_pointer yields 0 (flaw-C post-fix).
    check("null-proxy 'L...;' get_compressed_oop() == 0",
          proxy.get_compressed_oop() == 0u);
}

// ---------------------------------------------------------------------------
// SECTION 3 — null parent + '[L...;' array descriptor.  is_reference() is
// true (front '['), but the cast_for_variant unique_ptr arm explicitly rejects
// any non-'L' front (FLAW B fix) and returns nullptr.  Even if the source
// alternative HAD been the uint32 OOP arm, the wrapper would not have been
// fabricated.
// ---------------------------------------------------------------------------

static auto section_null_proxy_array_descriptor() -> void
{
    vmhook::field_proxy proxy{ nullptr,
                               std::string{ "[Lref/object;" },
                               /*is_static=*/false };

    check("null-proxy '[L...;' is_reference() == true",
          proxy.is_reference() == true);

    const auto value{ proxy.get() };
    std::unique_ptr<ref_object> wrapper = value;
    check("null-proxy '[L...;' -> unique_ptr<ref_object> holds no instance (FLAW B)",
          is_empty_wrapper(wrapper));

    // Multi-bracket too.
    vmhook::field_proxy multi{ nullptr,
                               std::string{ "[[Lref/object;" },
                               /*is_static=*/false };
    std::unique_ptr<ref_object> mw = multi.get();
    check("null-proxy '[[L...;' -> unique_ptr<ref_object> holds no instance",
          is_empty_wrapper(mw));
}

// ---------------------------------------------------------------------------
// SECTION 4 — null parent + primitive descriptor.  is_reference() is false
// (the 'I' / 'J' / 'D' fronts) — the unique_ptr arm STILL must yield nullptr
// because the source alternative is the primitive type, not uint32_t, so the
// else-arm of `is_unique_ptr_v` returns nullptr regardless of the descriptor.
// ---------------------------------------------------------------------------

static auto section_null_proxy_primitive_descriptor() -> void
{
    const char* const prims[]{ "I", "J", "Z", "B", "S", "F", "D", "C" };
    for (const char* sig : prims)
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ sig }, false };
        check("null-proxy primitive is_reference() == false",
              proxy.is_reference() == false);
        check("null-proxy primitive get_compressed_oop() == 0 (FLAW C)",
              proxy.get_compressed_oop() == 0u);
        const auto value{ proxy.get() };
        std::unique_ptr<ref_object> w = value;
        check("null-proxy primitive -> unique_ptr<ref_object> holds no instance",
              is_empty_wrapper(w));
    }
}

// ---------------------------------------------------------------------------
// SECTION 5 — null parent + EMPTY descriptor (the degenerate proxy).  Flaw B
// guard checks `signature.empty()` and returns nullptr; the empty descriptor
// drives the int32 default arm of get() so void* also yields nullptr.
// ---------------------------------------------------------------------------

static auto section_null_proxy_empty_descriptor() -> void
{
    vmhook::field_proxy proxy{ nullptr, std::string{}, /*is_static=*/false };
    check("null-proxy empty-sig is_reference() == false (no front to peek)",
          proxy.is_reference() == false);
    check("null-proxy empty-sig get_compressed_oop() == 0",
          proxy.get_compressed_oop() == 0u);
    const auto value{ proxy.get() };
    std::unique_ptr<ref_object> w = value;
    check("null-proxy empty-sig -> unique_ptr<ref_object> holds no instance",
          is_empty_wrapper(w));
    void* raw = value;
    check("null-proxy empty-sig -> void* is nullptr",
          raw == nullptr);
}

// ---------------------------------------------------------------------------
// SECTION 6 — 32-iter idempotency: repeating the null-proxy 'L...;' get()
// must yield an instance-less wrapper EVERY iteration.  No cached fabrication, no
// state drift, no descriptor mutation.
// ---------------------------------------------------------------------------

static auto section_idempotent_null_ref() -> void
{
    vmhook::field_proxy proxy{ nullptr, std::string{ "Ljava/lang/Object;" }, false };
    bool all_null{ true };
    for (int i{ 0 }; i < 32; ++i)
    {
        std::unique_ptr<ref_object> w = proxy.get();
        if (!is_empty_wrapper(w)) { all_null = false; }
    }
    check("32 iterations of null-proxy 'L...;' get() all instance-less", all_null);
}

int main()
{
    std::printf("field_object_ref no-JVM unit test\n");
    section_null_proxy_L_descriptor();
    section_null_proxy_array_descriptor();
    section_null_proxy_primitive_descriptor();
    section_null_proxy_empty_descriptor();
    section_idempotent_null_ref();
    if (failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", failures);
    return 1;
}
