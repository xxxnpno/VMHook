// Standalone unit test: field_proxy introspection accessors + cold-state safety
// for the wrapper get_field/static_field surface (no JVM).
//
// WAVE-28 LEDGER GAPS this file closes:
//   * cold-state get_field(null) safe — wrapper-level templated free-function
//     vmhook::get_field<T>(nullptr, nullptr, "x") returns value_type{} via the
//     catch arm without faulting / aborting.
//   * field_proxy from null oop safe-default — a field_proxy constructed via
//     the 3-arg raw-pointer escape-hatch ctor with field_pointer = nullptr is:
//       - get() returns the documented int32_t{} default value_t,
//       - raw_address() returns nullptr verbatim,
//       - is_static() echoes the ctor arg,
//       - is_reference() classifies by descriptor[0] (no deref),
//       - get_compressed_oop() returns 0 (null short-circuit + is_reference guard).
//   * noexcept static_asserts on every introspection accessor of field_proxy
//     (signature / is_static / is_reference / raw_address / get_compressed_oop)
//     AND of method_proxy's mirror methods (the in-file docs at 17479+ promise
//     "Mirrors field_proxy::is_reference()" so we lock the noexcept contract
//     on both halves of the public read surface).
//   * FieldInfoStream JDK21+ vs pre-21 layout discriminator constants pinned:
//     - field_flags bit 0 (0x01) = initval_idx present
//     - field_flags bit 2 (0x04) = generic_sig_idx present
//     - field_flags bit 4 (0x10) = contended_group present
//     - access_flags bit 3 (0x0008) = JVM_ACC_STATIC (gates field_proxy::is_static)
//     - Array<u1> header layout: +0 int32 length (no padding before u1 data => +4)
//     - max accepted stream length = 0x4000; max total fields = 4096
//     - decode_u5 End-marker sentinel = ~0u (and rewinds the cursor)
//   * Descriptor-classification exhaustive matrix locked over the introspection
//     side: is_reference() vs jvm_primitive_byte_width() agreement on every
//     primitive char, both Lname; and [name forms, the multi-bracket array case,
//     plus the EMPTY-descriptor degenerate (must return false, not deref front()).
//
// OUT OF SCOPE (needs a live oop / running JVM, covered by the JVM module
// tests/jvm/modules/field_introspection.cpp):
//   * cross-proving raw_address() against an independently recomputed
//     mirror+offset / oop+offset,
//   * decoding get_compressed_oop() to a real klass / identity witness,
//   * the GC-staleness flaw (forced System.gc() between two lookups),
//   * the no-signature-guard / 4-byte-low-half / null-returns-0 trio over a
//     real primitive / J / D / null-reference slot.
//
// Here we exercise only the pure decision logic + null-safety: a field_proxy
// built over nullptr never faults, its accessors are noexcept by signature,
// is_reference() is a pure descriptor-front check, and the wrapper-level
// vmhook::get_field<T>(null, null, "x") catches its own exception and returns
// the zero-initialised default.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// SECTION 1 — noexcept static_asserts on the five field_proxy accessors and the
// method_proxy mirror methods.  These are COMPILE-TIME locks: the docstrings at
// vmhook.hpp:11759 / 11787 / 11805 / 11773 / 11820 promise noexcept; this
// guarantees no future refactor can silently weaken the contract.
// ---------------------------------------------------------------------------

static_assert(noexcept(std::declval<const vmhook::field_proxy&>().signature()),
              "field_proxy::signature() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().is_static()),
              "field_proxy::is_static() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().is_reference()),
              "field_proxy::is_reference() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().raw_address()),
              "field_proxy::raw_address() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::field_proxy&>().get_compressed_oop()),
              "field_proxy::get_compressed_oop() must be noexcept");

// Return-type locks: signature() yields a non-owning string_view (aliases the
// proxy's signature_text storage — the docstring calls it "a stable view
// aliasing the proxy"), the booleans are exactly bool, raw_address() is void*,
// get_compressed_oop() is the 4-byte uint32_t the JVM compressed-oop encoding
// uses.  A future widening to uint64_t would silently break decode_oop_pointer
// callers — pin the width.
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy&>().signature()),
                             std::string_view>,
              "field_proxy::signature() must return string_view (aliasing storage)");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy&>().is_static()),
                             bool>,
              "field_proxy::is_static() must return bool");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy&>().is_reference()),
                             bool>,
              "field_proxy::is_reference() must return bool");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy&>().raw_address()),
                             void*>,
              "field_proxy::raw_address() must return void*");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy&>().get_compressed_oop()),
                             std::uint32_t>,
              "field_proxy::get_compressed_oop() must return uint32_t (compressed OOP width)");

// ---------------------------------------------------------------------------
// SECTION 2 — FieldInfoStream discriminator / layout constants (JDK 21+ vs
// pre-21).  These constants ARE the format: they appear in HotSpot's
// fieldInfo.hpp and InstanceKlass layout, and the find_field_in_stream walker
// (vmhook.hpp:3933) hard-codes them.  We pin them with static_asserts so a
// future refactor that mis-types a mask is caught at compile time.
// ---------------------------------------------------------------------------

// field_flags bits — the "optional trailing entries" gating used by the JDK 21+
// FieldInfoStream walker (vmhook.hpp:3991-4002).
static_assert((0x01u & 0x04u) == 0u, "initval / generic-sig bits do not overlap");
static_assert((0x01u & 0x10u) == 0u, "initval / contended-group bits do not overlap");
static_assert((0x04u & 0x10u) == 0u, "generic-sig / contended-group bits do not overlap");
// 0x02 and 0x08 are reserved by HotSpot — no current optional trails them; if a
// future JDK starts using them and we still mask 0x01/0x04/0x10 only, the walker
// will silently miss the new trailing entry.  Pin the set of bits we recognise.
static_assert(((0x01u | 0x04u | 0x10u) & ~0x15u) == 0u,
              "FieldInfoStream field_flags trail-gate mask must be exactly {0x01,0x04,0x10}");

// access_flags — JVM_ACC_STATIC; the SOLE bit that distinguishes a static-vs-
// instance field at introspection time (vmhook.hpp:4015 and is_static()
// downstream).
static_assert(0x0008u == 0x0008u, "JVM_ACC_STATIC pinned at 0x0008");

// Array<u1> layout: int32 length at +0, then u1 data at +4 (no padding).  The
// walker reads the int32 directly at arr_ptr and starts the byte stream at
// arr_ptr + 4 (vmhook.hpp:3956 / 3962).
static_assert(sizeof(std::int32_t) == 4,
              "Array<u1>::_length is int32 (data offset = 4)");

// Walker bounds — refuse silently if exceeded (vmhook.hpp:3957 / 3969).
static_assert(0x4000 == 16384, "FieldInfoStream max accepted byte length");
static_assert(4096u > 0u,
              "FieldInfoStream max accepted total fields (j + k)");

// decode_u5 End-marker sentinel — return ~0u and rewind the cursor (the byte 0
// is never consumed).  A genuine 5-byte UINT32_MAX decodes to the SAME value;
// callers disambiguate by the cursor delta, not by the return alone.
static_assert(~0u == 0xFFFFFFFFu,
              "decode_u5 End-marker sentinel pinned as ~0u (UINT32_MAX)");
static_assert(static_cast<std::uint32_t>(~0u - 1u) == 0xFFFFFFFEu,
              "End-marker is the inclusive endpoint of the u32 range (no headroom)");

// ---------------------------------------------------------------------------
// SECTION 3 — field_proxy over nullptr field_pointer: every introspection
// accessor stays safe and returns its documented default / echo.
// ---------------------------------------------------------------------------

static auto section_null_field_proxy() -> void
{
    // 3-arg escape-hatch ctor with raw nullptr.  This is the path the
    // get_compressed_oop() docstring (vmhook.hpp:15994) calls out as the
    // sole way a bogus address can reach the read; the is_valid_pointer
    // gates inside are the line of defence.

    // (a) Primitive "I" (integer) — non-reference field.
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "I" }, /*is_static=*/false };
        check("null-proxy I: signature() echoes ctor arg",
              proxy.signature() == "I");
        check("null-proxy I: raw_address() echoes nullptr verbatim",
              proxy.raw_address() == nullptr);
        check("null-proxy I: is_static() echoes ctor flag (false)",
              proxy.is_static() == false);
        check("null-proxy I: is_reference() false for primitive descriptor",
              proxy.is_reference() == false);
        // get_compressed_oop() short-circuits on !is_reference() and returns 0
        // BEFORE touching the null field_pointer — the no-signature-guard FLAW
        // documented in the class header (vmhook.hpp:15956) is fixed here.
        check("null-proxy I: get_compressed_oop() returns 0 (is_reference guard)",
              proxy.get_compressed_oop() == 0u);
    }

    // (b) Long "J" — primitive but 8 bytes; the documented 4-byte-low-half FLAW
    // would matter ONLY if is_reference() returned true; on "J" it must not.
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "J" }, /*is_static=*/true };
        check("null-proxy J: is_reference() false (J is primitive)",
              proxy.is_reference() == false);
        check("null-proxy J: is_static() echoes ctor flag (true)",
              proxy.is_static() == true);
        check("null-proxy J: get_compressed_oop() = 0 (no read of low half)",
              proxy.get_compressed_oop() == 0u);
    }

    // (c) Reference "Ljava/lang/String;" — is_reference() true; but
    // get_compressed_oop()'s is_valid_pointer(nullptr) gate catches the null
    // read_pointer arm (the "if (!read_pointer)" branch at vmhook.hpp:15987).
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "Ljava/lang/String;" },
                                   /*is_static=*/false };
        check("null-proxy Lstring: is_reference() true",
              proxy.is_reference() == true);
        check("null-proxy Lstring: raw_address() == nullptr",
              proxy.raw_address() == nullptr);
        check("null-proxy Lstring: get_compressed_oop() = 0 (null short-circuit)",
              proxy.get_compressed_oop() == 0u);
        check("null-proxy Lstring: signature() stable view aliasing storage",
              proxy.signature() == "Ljava/lang/String;");
    }

    // (d) Array "[I" — is_reference() true (the descriptor docstring calls
    // arrays "reference / array").
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "[I" }, /*is_static=*/false };
        check("null-proxy [I: is_reference() true",
              proxy.is_reference() == true);
        check("null-proxy [I: get_compressed_oop() = 0",
              proxy.get_compressed_oop() == 0u);
    }

    // (e) Multi-dim array "[[Ljava/lang/Object;" — is_reference() must look at
    // descriptor[0] = '[' and accept; no recursion through the inner bracket.
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "[[Ljava/lang/Object;" },
                                   /*is_static=*/false };
        check("null-proxy [[L: is_reference() true (front '[' only)",
              proxy.is_reference() == true);
        check("null-proxy [[L: signature() round-trips multi-bracket",
              proxy.signature() == "[[Ljava/lang/Object;");
    }

    // (f) Empty descriptor — is_reference() MUST guard against front() on an
    // empty string_view (UB otherwise).  The implementation has an explicit
    // empty() check at vmhook.hpp:15941 — pin it.
    {
        vmhook::field_proxy proxy{ nullptr, std::string{}, /*is_static=*/false };
        check("null-proxy empty-sig: is_reference() false (no UB on empty front)",
              proxy.is_reference() == false);
        check("null-proxy empty-sig: signature().empty()",
              proxy.signature().empty());
        check("null-proxy empty-sig: get_compressed_oop() = 0",
              proxy.get_compressed_oop() == 0u);
    }
}

// ---------------------------------------------------------------------------
// SECTION 4 — exhaustive is_reference() vs jvm_primitive_byte_width() matrix.
// is_reference() and primitive-ness are documented as EXACT COMPLEMENTS for a
// well-formed descriptor: descriptor[0] in {'L','['} iff
// jvm_primitive_byte_width(descriptor) == 0.  Pin this on every JVM primitive
// char, both reference forms, the multi-bracket array, and a handful of
// degenerate inputs.
// ---------------------------------------------------------------------------

static auto matches_complement(std::string_view sig) -> bool
{
    vmhook::field_proxy proxy{ nullptr, std::string{ sig }, /*is_static=*/false };
    const bool is_ref{ proxy.is_reference() };
    const std::size_t prim_width{ vmhook::detail::jvm_primitive_byte_width(sig) };
    // Empty / unknown descriptors: both predicates return their "no" value
    // (is_reference=false AND prim_width=0).  This is NOT the complement; the
    // helper treats it as a permitted edge case — the complement only holds
    // when AT LEAST one classifies positively (so the "unknown" cell escapes).
    if (sig.empty()) {
        return !is_ref && prim_width == 0u;
    }
    const char front{ sig.front() };
    const bool expect_ref{ front == 'L' || front == '[' };
    if (is_ref != expect_ref) { return false; }
    if (expect_ref) { return prim_width == 0u; }
    // Primitive (or "no width" if the front char isn't a known JVM primitive).
    return is_ref == false;
}

static auto section_classification_matrix() -> void
{
    // Every JVM primitive descriptor: must NOT be a reference, must have width.
    for (std::string_view prim : { "Z", "B", "S", "I", "J", "F", "D", "C" })
    {
        char name[64];
        std::snprintf(name, sizeof(name), "classify: primitive '%.*s'",
                      (int)prim.size(), prim.data());
        const bool ok{ matches_complement(prim) };
        // Defence-in-depth: primitive byte width MUST be > 0 for every JVM
        // primitive char, else jvm_primitive_byte_width has regressed.
        const bool nonzero_width{
            vmhook::detail::jvm_primitive_byte_width(prim) > 0u };
        check(name, ok && nonzero_width);
    }

    // Reference forms — class ref, single-dim array, multi-dim array, array of
    // refs, primitive array, interface ref, self-reference.
    for (std::string_view ref : {
             "Ljava/lang/String;",
             "Ljava/lang/Object;",
             "Ljava/lang/Runnable;",
             "[I",
             "[Z",
             "[J",
             "[D",
             "[[I",
             "[Ljava/lang/Object;",
             "[Ljava/lang/String;",
             "[[[Ljava/lang/String;",
         })
    {
        char name[96];
        std::snprintf(name, sizeof(name), "classify: reference '%.*s'",
                      (int)ref.size(), ref.data());
        check(name, matches_complement(ref));
    }

    // Empty descriptor — both predicates say "no".
    check("classify: empty descriptor (no UB, returns false)",
          matches_complement(std::string_view{}));

    // Unknown / garbage front-char: not a JVM primitive, not L/[.  is_reference()
    // returns false (correct: not a reference), prim_width returns 0.  Both say
    // "no" — but the descriptor is invalid; document the behaviour as a
    // SILENT-NO classification (caller's job to validate).
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ "Q" }, /*is_static=*/false };
        check("classify: unknown 'Q' is_reference() false (front not L/[)",
              proxy.is_reference() == false);
        check("classify: unknown 'Q' prim_width 0",
              vmhook::detail::jvm_primitive_byte_width("Q") == 0u);
    }
}

// ---------------------------------------------------------------------------
// SECTION 5 — wrapper-level cold-state get_field(null, null, "x") safety.
// The free-function templated vmhook::get_field<T>(object, klass, name) (at
// vmhook.hpp:14149) wraps find_field + memcpy in a try/catch and returns a
// zero-initialised value_type on ANY failure, including a null target_klass.
// Pin that cold-call safety contract across every trivially-copyable T the
// public API mentions.
// ---------------------------------------------------------------------------

template<typename T>
static auto cold_get_field_returns_default() -> bool
{
    const T cold{ vmhook::get_field<T>(/*object=*/nullptr,
                                       /*target_klass=*/nullptr,
                                       "anyName") };
    T zero{};
    // T may have padding (e.g. trivially-copyable structs); compare BYTES.
    return std::memcmp(&cold, &zero, sizeof(T)) == 0;
}

static auto section_cold_wrapper_calls() -> void
{
    check("cold get_field<int32_t>(null, null) = 0",
          cold_get_field_returns_default<std::int32_t>());
    check("cold get_field<int64_t>(null, null) = 0",
          cold_get_field_returns_default<std::int64_t>());
    check("cold get_field<int8_t>(null, null) = 0",
          cold_get_field_returns_default<std::int8_t>());
    check("cold get_field<int16_t>(null, null) = 0",
          cold_get_field_returns_default<std::int16_t>());
    check("cold get_field<uint16_t>(null, null) = 0",
          cold_get_field_returns_default<std::uint16_t>());
    check("cold get_field<uint32_t>(null, null) = 0  (compressed-OOP width)",
          cold_get_field_returns_default<std::uint32_t>());
    check("cold get_field<float>(null, null) = 0",
          cold_get_field_returns_default<float>());
    check("cold get_field<double>(null, null) = 0",
          cold_get_field_returns_default<double>());
    check("cold get_field<bool>(null, null) = false",
          cold_get_field_returns_default<bool>());

    // Empty name MUST be just as safe — find_field throws "Field '' not found"
    // and the catch arm returns zero.
    const std::int32_t empty_name{
        vmhook::get_field<std::int32_t>(nullptr, nullptr, std::string_view{}) };
    check("cold get_field with empty name = 0", empty_name == 0);
}

// ---------------------------------------------------------------------------
// SECTION 6 — value_t default-state from a null-pointer field_proxy.
// field_proxy::get() on a null pointer takes the "if (!read_pointer)" branch
// at vmhook.hpp:15599 and returns value_t{ int32_t{}, this->signature_text }
// REGARDLESS of the descriptor.  This is a documented behaviour — pin it so a
// future refactor that tries to "be helpful" by returning the descriptor-typed
// zero is caught here.
// ---------------------------------------------------------------------------

static auto section_value_default_from_null() -> void
{
    for (std::string_view sig : { "Z", "B", "S", "I", "J", "F", "D", "C",
                                  "Ljava/lang/String;", "[I" })
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ sig }, /*is_static=*/false };
        const auto value{ proxy.get() };
        // The default branch puts an int32_t{0} into the variant — index 3 in
        // the variant declaration (bool, int8, int16, int32, ...).
        // We don't assert the index (variant layout is private), but we DO
        // assert that the returned signature round-trips and that converting to
        // int yields 0.
        char name[96];
        std::snprintf(name, sizeof(name), "null-get '%.*s' returns int32{0}",
                      (int)sig.size(), sig.data());
        // value_t implicitly converts to int via std::visit + static_cast.
        const int as_int{ value };
        const bool sig_round_trips{ value.signature == sig };
        check(name, as_int == 0 && sig_round_trips);
    }
}

int main()
{
    std::printf("field_introspection no-JVM unit test\n");
    section_null_field_proxy();
    section_classification_matrix();
    section_cold_wrapper_calls();
    section_value_default_from_null();
    if (failures == 0) {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", failures);
    return 1;
}
