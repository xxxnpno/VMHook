// Standalone unit test: field_arrays_object — Object[] / String[] field accessors
// in the COLD state (no JVM attached). Covers:
//
// WAVE-32 LEDGER GAPS this file closes (no-JVM):
//   * cold-state object-array field accessor returns an EMPTY vector and is
//     crash-safe on a null parent (the documented `to_vector<W>()` entry point
//     and the implicit `vector<string>` conversion path);
//   * null parent + '[L...;' / '[[L...;' / '[Ljava/lang/String;' descriptors
//     all safe-default to an empty vector — no fabricated elements;
//   * static_asserts on the unique_ptr<W> element type of the documented
//     Object[] entry point and on convertibility of value_t to vector<string>;
//   * null element handling at the value_t level — building a value_t whose
//     compressed-OOP source alternative is 0 (the "null array reference"
//     shape) yields an empty vector and a null parent decode, even after
//     repeated reads (idempotency).
//
// OUT OF SCOPE (live-JVM `field_arrays_object` module owns these):
//   * non-null Object[] / String[] elements decoded with real tags;
//   * count cross-check against a Java length oracle;
//   * leading/trailing/mixed null shapes on a real array;
//   * field_oop / array_length / get_array_element walk on a real OOP.
//
// All assertions in this file are DETERMINISTIC on every platform: every
// public entry point is exercised over a null parent with each documented
// descriptor shape, and the contract is "no crash, empty vector, no
// fabrication".  No platform-variant cases — no [INFO] gating needed.

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A minimal wrapper type — same pattern used by the other cold tests.
class item_wrapper : public vmhook::object<item_wrapper>
{
public:
    explicit item_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<item_wrapper>{ oop }
    {
    }
};

static_assert(std::is_base_of_v<vmhook::object_base, item_wrapper>);

// ---------------------------------------------------------------------------
// SECTION 1 — static_asserts on the public Object[] / String[] surface.
// ---------------------------------------------------------------------------

// to_vector<W>() must return vector<unique_ptr<W>> — the documented Object[]
// entry point.  A regression that drops unique_ptr ownership would break user
// code.
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::field_proxy::value_t&>()
                               .template to_vector<item_wrapper>()),
                  std::vector<std::unique_ptr<item_wrapper>>>,
              "value_t::to_vector<W>() must return vector<unique_ptr<W>>");

// The element type of that vector is exactly unique_ptr<W>.
using to_vector_result_t =
    decltype(std::declval<const vmhook::field_proxy::value_t&>()
                 .template to_vector<item_wrapper>());
static_assert(std::is_same_v<typename to_vector_result_t::value_type,
                             std::unique_ptr<item_wrapper>>,
              "to_vector<W> element type must be unique_ptr<W>");

// String[] read path: value_t must be convertible to vector<string>.
static_assert(std::is_convertible_v<vmhook::field_proxy::value_t,
                                    std::vector<std::string>>,
              "value_t must be convertible to vector<string> for String[] reads");

// ---------------------------------------------------------------------------
// SECTION 2 — null parent + Object[] descriptors via the documented
// to_vector<W>() entry point.  The descriptor branch in the out-of-line
// definition keys on signature[0]=='[' && (signature[1]=='L' || '['); each
// shape MUST safe-default to an empty vector with no element fabrication.
// ---------------------------------------------------------------------------

static auto section_null_proxy_object_array() -> void
{
    const char* const sigs[]{
        "[Lvmhook/fixtures/Item;",
        "[[Lvmhook/fixtures/Item;",
        "[Ljava/lang/Object;",
        "[Ljava/lang/String;",
    };
    for (const char* sig : sigs)
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ sig }, /*is_static=*/false };
        check("null-proxy '[L...' is_reference() == true",
              proxy.is_reference() == true);
        check("null-proxy '[L...' get_compressed_oop() == 0",
              proxy.get_compressed_oop() == 0u);

        const auto value{ proxy.get() };
        const auto vec{ value.template to_vector<item_wrapper>() };
        check("null-proxy '[L...' to_vector<W>() returns empty vector",
              vec.empty());
    }
}

// ---------------------------------------------------------------------------
// SECTION 3 — null parent + String[] via the implicit vector<string>
// conversion.  read_array_value sees compressed=0, decode_array_oop returns
// nullptr, and the function short-circuits to an empty vector.
// ---------------------------------------------------------------------------

static auto section_null_proxy_string_array() -> void
{
    vmhook::field_proxy proxy{ nullptr,
                               std::string{ "[Ljava/lang/String;" },
                               /*is_static=*/false };

    const auto value{ proxy.get() };
    std::vector<std::string> v = value;
    check("null-proxy '[Ljava/lang/String;' -> vector<string> is empty",
          v.empty());

    // The static variant is also a valid construction shape.
    vmhook::field_proxy static_proxy{ nullptr,
                                      std::string{ "[Ljava/lang/String;" },
                                      /*is_static=*/true };
    const auto static_value{ static_proxy.get() };
    std::vector<std::string> sv = static_value;
    check("null-static-proxy '[Ljava/lang/String;' -> vector<string> empty",
          sv.empty());
}

// ---------------------------------------------------------------------------
// SECTION 4 — value_t directly carrying a NULL array reference (compressed
// OOP = 0 in the uint32 alternative).  This is the "null array reference"
// shape from the live-JVM module's PART A9 / PART B-h: both the documented
// to_vector<W>() entry point and the implicit vector<string> conversion must
// safe-default to empty.
// ---------------------------------------------------------------------------

static auto section_value_t_null_compressed_oop() -> void
{
    // The "[L..." branch in to_vector<W>(): compressed=0 → decode_oop_pointer
    // returns nullptr → empty vector, no element fabrication.
    {
        vmhook::field_proxy::value_t v{
            std::uint32_t{ 0 },
            std::string{ "[Lvmhook/fixtures/Item;" }
        };
        const auto vec = v.template to_vector<item_wrapper>();
        check("value_t{0,'[L...'} to_vector<W>() empty (null Object[])",
              vec.empty());
    }
    // The String[] arm: read_array_value sees compressed=0 → empty.
    {
        vmhook::field_proxy::value_t v{
            std::uint32_t{ 0 },
            std::string{ "[Ljava/lang/String;" }
        };
        std::vector<std::string> svec = v;
        check("value_t{0,'[Ljava/lang/String;'} -> vector<string> empty",
              svec.empty());
    }
    // The '[[L...' nested-array branch — same descriptor key, still empty.
    {
        vmhook::field_proxy::value_t v{
            std::uint32_t{ 0 },
            std::string{ "[[Lvmhook/fixtures/Item;" }
        };
        const auto vec = v.template to_vector<item_wrapper>();
        check("value_t{0,'[[L...'} to_vector<W>() empty (null Object[][])",
              vec.empty());
    }
}

// ---------------------------------------------------------------------------
// SECTION 5 — degenerate descriptor shapes (mis-typed registration).  A
// non-'[' descriptor with an int32 source alternative falls through to
// collection::to_vector; on a null OOP this still safe-defaults to empty.
// An EMPTY signature must also empty-default with no crash.
// ---------------------------------------------------------------------------

static auto section_value_t_non_array_signatures() -> void
{
    {
        vmhook::field_proxy::value_t v{
            std::int32_t{ 0 },
            std::string{ "Ljava/util/ArrayList;" }
        };
        const auto vec = v.template to_vector<item_wrapper>();
        check("value_t{0,'Ljava/util/ArrayList;'} to_vector<W>() empty",
              vec.empty());
    }
    {
        vmhook::field_proxy::value_t v{ std::int32_t{ 0 }, std::string{} };
        const auto vec = v.template to_vector<item_wrapper>();
        check("value_t empty-sig to_vector<W>() empty", vec.empty());
    }
}

// ---------------------------------------------------------------------------
// SECTION 6 — 32-iter idempotency.  Repeating both entry points over a null
// proxy must NEVER fabricate, drift, or grow the result.
// ---------------------------------------------------------------------------

static auto section_idempotent_null_array() -> void
{
    vmhook::field_proxy obj_proxy{ nullptr,
                                   std::string{ "[Lvmhook/fixtures/Item;" },
                                   false };
    vmhook::field_proxy str_proxy{ nullptr,
                                   std::string{ "[Ljava/lang/String;" },
                                   false };
    bool obj_all_empty{ true };
    bool str_all_empty{ true };
    for (int i{ 0 }; i < 32; ++i)
    {
        const auto ov = obj_proxy.get().template to_vector<item_wrapper>();
        if (!ov.empty()) { obj_all_empty = false; }
        std::vector<std::string> sv = str_proxy.get();
        if (!sv.empty()) { str_all_empty = false; }
    }
    check("32 iterations of null-proxy '[L...' to_vector<W>() all empty",
          obj_all_empty);
    check("32 iterations of null-proxy '[Ljava/lang/String;' all empty",
          str_all_empty);
}

int main()
{
    std::printf("field_arrays_object no-JVM unit test\n");
    section_null_proxy_object_array();
    section_null_proxy_string_array();
    section_value_t_null_compressed_oop();
    section_value_t_non_array_signatures();
    section_idempotent_null_array();
    if (failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", failures);
    return 1;
}
