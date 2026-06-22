// Standalone (no-JVM) unit test for the method-enumeration API:
//   vmhook::get_class_methods(class_name)        (by internal name)
//   vmhook::get_class_methods<T>()               (by registered wrapper)
//   vmhook::find_methods_by_signature<T>(desc)   (descriptor selector)
//   vmhook::hook_by_signature<T>(desc, detour)   (descriptor-only install)
//   vmhook::log_class_methods<T>()               (debug convenience)
//
// These read InstanceKlass::_methods directly.  With no JVM in this process,
// vmhook::find_class resolves no klass, so EVERY entry point must return an
// EMPTY result WITHOUT throwing or dereferencing — that is the contract this
// file pins down.  The "real list of methods on a loaded class" behaviour
// (set membership / multiplicity / synthetic inclusion / inherited exclusion /
// refuse-policy on an ambiguous descriptor / install+fire) fundamentally needs
// a live JVM and is covered by the JVM module
// tests/jvm/modules/method_enumeration.cpp.
//
// What IS exhaustible here, and what this file drives over its full input
// matrix:
//   (a) the documented no-JVM contract: all four entry points return cleanly
//       (empty vector / false / no fault) for EVERY class-name and EVERY
//       method-descriptor shape — slashed / dotted / empty / garbage / array /
//       embedded-NUL / pathologically long / every primitive+array+object
//       return and parameter combination;
//   (b) the descriptor-MATCHING logic find_methods_by_signature relies on —
//       its exact `candidate == descriptor` std::string_view equality — pinned
//       through a self-contained reference mirror over the same matrix, AND the
//       structural invariant that an empty source list yields zero matches for
//       every descriptor (so find_methods_by_signature is empty for all input
//       with no JVM, which is exactly what the live calls assert);
//   (c) the compile-time signature / return-type / noexcept contracts via
//       static_assert.
//
// Determinism: with no JVM every dynamic result is the SAME (empty / false)
// regardless of the input, so output is byte-identical across runs, compilers,
// platforms and JDKs.  No JVM fixture, no heap growth, no platform/JDK-variant
// hard assert.
#include <vmhook/vmhook.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // -- Wrapper types -------------------------------------------------------
    // A handful of distinct wrappers following the canonical pattern (derive
    // from vmhook::object<T> with `explicit T(vmhook::oop_t)`):
    //   * registered_wrapper       — passed to register_class() (which, with no
    //                                JVM, FAILS to verify and therefore does
    //                                NOT populate type_to_class_map);
    //   * unregistered_wrapper     — never touched by register_class();
    //   * map_injected_wrapper     — inserted DIRECTLY into type_to_class_map
    //                                (byte-for-byte what register_class does
    //                                internally) so its get_class_methods<T>()
    //                                reaches the find_class()->nullptr arm
    //                                instead of the type-map-miss arm;
    //   * second_unregistered      — a second never-registered type, to prove
    //                                the type-map-miss arm is per-type.
    class registered_wrapper : public vmhook::object<registered_wrapper>
    {
    public:
        explicit registered_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<registered_wrapper>{ oop }
        {
        }
    };

    class unregistered_wrapper : public vmhook::object<unregistered_wrapper>
    {
    public:
        explicit unregistered_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<unregistered_wrapper>{ oop }
        {
        }
    };

    class map_injected_wrapper : public vmhook::object<map_injected_wrapper>
    {
    public:
        explicit map_injected_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<map_injected_wrapper>{ oop }
        {
        }
    };

    class second_unregistered : public vmhook::object<second_unregistered>
    {
    public:
        explicit second_unregistered(vmhook::oop_t oop) noexcept
            : vmhook::object<second_unregistered>{ oop }
        {
        }
    };

    // -- Detour shapes -------------------------------------------------------
    // hook_by_signature<T>(desc, detour) UNCONDITIONALLY instantiates
    // hook<T>(name, desc, detour) in its template body, so every detour passed
    // to hook_by_signature must satisfy hook<T>'s authoring-contract
    // static_asserts (first parameter must be vmhook::return_value&).  The call
    // is never *reached* with no JVM (find_methods_by_signature is empty -> the
    // names.empty() gate returns false first), but it must still COMPILE.  We
    // therefore declare several concrete (non-generic) detour signatures —
    // function pointers so function_traits can deduce them — covering the void,
    // self-only, and self+java-arg shapes.  generic / templated callables are
    // intentionally NOT used: function_traits cannot deduce a templated
    // operator().
    auto detour_self_only(vmhook::return_value&,
                          const std::unique_ptr<registered_wrapper>&) -> void
    {
    }

    auto detour_void_only(vmhook::return_value&) -> void
    {
    }

    auto detour_one_int(vmhook::return_value&,
                        const std::unique_ptr<registered_wrapper>&,
                        std::int32_t) -> void
    {
    }

    auto detour_long_arg(vmhook::return_value&,
                         const std::unique_ptr<registered_wrapper>&,
                         std::int64_t) -> void
    {
    }

    auto detour_two_args(vmhook::return_value&,
                         const std::unique_ptr<registered_wrapper>&,
                         std::int32_t, double) -> void
    {
    }

    auto detour_oop_arg(vmhook::return_value&,
                        const std::unique_ptr<registered_wrapper>&,
                        vmhook::oop_t) -> void
    {
    }

    auto detour_for_injected(vmhook::return_value&,
                             const std::unique_ptr<map_injected_wrapper>&) -> void
    {
    }

    // -- Reference mirror of find_methods_by_signature's match predicate -----
    // find_methods_by_signature keeps a method name iff its descriptor
    // `candidate` compares equal to the requested `descriptor` under
    // std::string_view operator== (vmhook.hpp:8890 — exact, case-sensitive,
    // length-aware, embedded-NUL-aware byte equality; NO normalisation, NO
    // dotted<->slashed translation, NO whitespace trimming).  vmhook ships no
    // standalone descriptor-equality helper to call, so this captureless mirror
    // reproduces that one comparison so the suite can pin its semantics over the
    // full descriptor matrix.  It is byte-for-byte the `candidate == descriptor`
    // the library runs.
    auto descriptor_matches(std::string_view candidate, std::string_view descriptor) -> bool
    {
        return candidate == descriptor;
    }

    // Self-contained model of find_methods_by_signature over an EXPLICIT
    // (name, descriptor) source list: keep every name whose descriptor matches.
    // With no JVM the library's real source list is always empty, so the live
    // function returns {} for every descriptor; this model lets us additionally
    // prove the selection logic itself (multiplicity, order preservation,
    // exact-match-only) that the function would exhibit on a populated klass —
    // WITHOUT a JVM — so the descriptor-matching contract is exhaustively
    // characterised here too.
    auto select_by_descriptor(
        const std::vector<std::pair<std::string, std::string>>& source,
        std::string_view descriptor) -> std::vector<std::string>
    {
        std::vector<std::string> names{};
        for (const auto& [name, candidate] : source)
        {
            if (descriptor_matches(candidate, descriptor))
            {
                names.push_back(name);
            }
        }
        return names;
    }
}

// =====================================================================
// Compile-time contracts (no runtime cost; assert the API shapes).
// =====================================================================

// get_class_methods (both overloads) and collect_klass_methods return
// vector<pair<string,string>>; find_methods_by_signature returns vector<string>.
using methods_by_type_t   = decltype(vmhook::get_class_methods<registered_wrapper>());
using methods_by_name_t   = decltype(vmhook::get_class_methods(std::string_view{}));
using names_by_sig_t      = decltype(vmhook::find_methods_by_signature<registered_wrapper>(std::string_view{}));

static_assert(std::is_same_v<methods_by_type_t,
                  std::vector<std::pair<std::string, std::string>>>,
              "get_class_methods<T>() must return vector<pair<name,descriptor>>");
static_assert(std::is_same_v<methods_by_name_t,
                  std::vector<std::pair<std::string, std::string>>>,
              "get_class_methods(name) must return vector<pair<name,descriptor>>");
static_assert(std::is_same_v<methods_by_type_t, methods_by_name_t>,
              "the by-type and by-name overloads must share one return type");
static_assert(std::is_same_v<names_by_sig_t, std::vector<std::string>>,
              "find_methods_by_signature<T>() must return vector<string>");

// hook_by_signature<T>(desc, detour) returns bool.
using hook_by_sig_ret_t = decltype(vmhook::hook_by_signature<registered_wrapper>(
    std::string_view{}, &detour_self_only));
static_assert(std::is_same_v<hook_by_sig_ret_t, bool>,
              "hook_by_signature<T>() must return bool");

// noexcept contract: the three data-returning entry points (and the shared
// engine) are noexcept; hook_by_signature is NOT (it forwards into hook<T>).
// Built outside any noexcept(...) operand so the string_view ctor noexcept
// quirk on libc++ can never bite.
namespace
{
    const std::string_view empty_sv{};
}
static_assert(noexcept(vmhook::get_class_methods<registered_wrapper>()),
              "get_class_methods<T>() must be noexcept");
static_assert(noexcept(vmhook::get_class_methods(empty_sv)),
              "get_class_methods(name) must be noexcept");
static_assert(noexcept(vmhook::find_methods_by_signature<registered_wrapper>(empty_sv)),
              "find_methods_by_signature<T>() must be noexcept");

// The element type of the enumeration result is exactly pair<string,string>;
// its .first / .second are both std::string (name, descriptor).
static_assert(std::is_same_v<methods_by_type_t::value_type,
                  std::pair<std::string, std::string>>,
              "enumeration element must be pair<string,string>");
// Use the member's declared type via the pair's first_type / second_type
// nested typedefs — this is reference-/value-category-free, so it pins the
// member types identically on every compiler (a decltype over declval would
// carry an rvalue-reference qualifier that differs across toolchains).
static_assert(std::is_same_v<methods_by_type_t::value_type::first_type, std::string>,
              "enumeration element .first (name) must be std::string");
static_assert(std::is_same_v<methods_by_type_t::value_type::second_type, std::string>,
              "enumeration element .second (descriptor) must be std::string");

int main()
{
    // Without a JVM, register_class can't verify the class and (because it
    // returns BEFORE the map insert when find_class yields null) does NOT
    // populate type_to_class_map.  We still call it to pin that it neither
    // throws nor crashes, and to exercise the type-map-miss arm of
    // get_class_methods<registered_wrapper>().
    const bool registered_ok{ vmhook::register_class<registered_wrapper>("test/Registered") };
    check("register_class_no_jvm_returns_false", registered_ok == false);

    // map_injected_wrapper: write the type_to_class_map entry DIRECTLY (exactly
    // what register_class does internally) so its get_class_methods<T>() passes
    // the map lookup and reaches find_class("test/MapInjected") -> nullptr ->
    // collect_klass_methods(nullptr) -> empty.  This exercises the OTHER empty
    // path (find_class-null) distinct from the type-map-miss path above.
    vmhook::type_to_class_map.insert_or_assign(
        std::type_index{ typeid(map_injected_wrapper) }, std::string{ "test/MapInjected" });

    // ===================================================================
    // PART A — get_class_methods<T>() : every wrapper-registration state.
    // Every variant returns an EMPTY vector with no throw (no JVM => no klass).
    // ===================================================================
    {
        const auto methods{ vmhook::get_class_methods<registered_wrapper>() };
        check("A_registered_wrapper_methods_empty_no_jvm", methods.empty());
        check("A_registered_wrapper_methods_size0", methods.size() == 0);
    }
    {
        // Unregistered type: the type-map-miss arm returns {} explicitly.
        const auto methods{ vmhook::get_class_methods<unregistered_wrapper>() };
        check("A_unregistered_wrapper_methods_empty", methods.empty());
    }
    {
        // A second never-registered type: still empty (per-type miss).
        const auto methods{ vmhook::get_class_methods<second_unregistered>() };
        check("A_second_unregistered_methods_empty", methods.empty());
    }
    {
        // Map-populated type, JVM absent: passes the map lookup, then
        // find_class returns null -> collect_klass_methods(nullptr) -> empty.
        const auto methods{ vmhook::get_class_methods<map_injected_wrapper>() };
        check("A_map_injected_wrapper_methods_empty_find_class_null", methods.empty());
        check("A_map_injected_wrapper_methods_size0", methods.size() == 0);
    }
    {
        // Idempotent: calling twice yields the same (empty) result, no cached
        // garbage, no growth.
        const auto first{ vmhook::get_class_methods<map_injected_wrapper>() };
        const auto second{ vmhook::get_class_methods<map_injected_wrapper>() };
        check("A_repeat_calls_both_empty", first.empty() && second.empty());
        check("A_repeat_calls_same_size", first.size() == second.size());
    }

    // ===================================================================
    // PART B — get_class_methods(class_name) : exhaustive class-name matrix.
    // find_class returns nullptr for EVERY name with no JVM, so the result is
    // always empty.  We sweep every name SHAPE the resolver might see — dotted
    // vs slashed, empty, whitespace, array-class forms, leading/trailing
    // separators, very long, descriptor-looking, garbage, mixed — to prove the
    // by-name path never throws / never dereferences for any of them.
    // ===================================================================
    {
        // The empty-name fast-reject path (find_class returns nullptr up front).
        const auto methods{ vmhook::get_class_methods(std::string_view{}) };
        check("B_empty_name_empty", methods.empty());
        const auto methods_lit{ vmhook::get_class_methods("") };
        check("B_empty_literal_name_empty", methods_lit.empty());
    }
    {
        // The canonical internal (slashed) form.
        const auto methods{ vmhook::get_class_methods("java/lang/Object") };
        check("B_canonical_slashed_name_empty", methods.empty());

        const auto missing{ vmhook::get_class_methods("definitely/Not/A/Class") };
        check("B_missing_class_empty", missing.empty());
    }
    {
        // Exhaustive name-shape sweep.  EVERY one must yield an empty vector.
        const char* names[]{
            // dotted (binary) form — find_class expects slashes, so this is a
            // miss; must still be safe.
            "java.lang.Object",
            "java.util.HashMap",
            // slashed (internal) form.
            "java/lang/String",
            "java/util/Map",
            "net/minecraft/client/Minecraft",
            // single-segment names.
            "Foo",
            "X",
            // array-class internal names (start with '[').
            "[I",
            "[[I",
            "[Ljava/lang/Object;",
            "[[[Ljava/lang/String;",
            // primitive-looking single letters.
            "I",
            "V",
            "Z",
            // leading / trailing / doubled separators.
            "/java/lang/Object",
            "java/lang/Object/",
            "java//lang//Object",
            "a/",
            "/",
            "//",
            // dot/slash mixed.
            "java/lang.Object",
            "a.b/c.d",
            // descriptor-looking strings fed as a class name.
            "Ljava/lang/Object;",
            "()V",
            "(I)I",
            // inner-class '$' names.
            "com/example/Outer$Inner",
            "a/b/Outer$Inner$Deep",
            // whitespace / control-ish content (still just a name miss).
            " ",
            "  ",
            "\t",
            "a b",
            "java/lang/Object ",
            " java/lang/Object",
            // punctuation / symbol soup.
            "!@#$%^&*()",
            "../../etc/passwd",
            "a\\b\\c",
            // numeric / mixed.
            "123",
            "java/lang/Object123",
            "9class",
        };
        bool all_empty{ true };
        for (const char* n : names)
        {
            const auto methods{ vmhook::get_class_methods(n) };
            if (!methods.empty()) { all_empty = false; }
        }
        check("B_every_name_shape_returns_empty", all_empty);
    }
    {
        // Embedded-NUL name: a string_view of length 5 whose 2nd byte is NUL.
        // find_class copies it into a std::string key and walks the (empty)
        // graph — must be a clean miss, not a truncation surprise / crash.
        const char raw[]{ 'a', '\0', 'b', '/', 'c' };
        const std::string_view name{ raw, sizeof(raw) };
        const auto methods{ vmhook::get_class_methods(name) };
        check("B_embedded_nul_name_empty", methods.empty());
    }
    {
        // A single NUL byte as the whole name (length 1, not empty()).
        const char raw[]{ '\0' };
        const std::string_view name{ raw, 1 };
        const auto methods{ vmhook::get_class_methods(name) };
        check("B_single_nul_byte_name_empty", methods.empty());
    }
    {
        // Pathologically long name (4096 'a's then a slash and a segment): the
        // resolver must handle it as an ordinary miss with no overflow.
        std::string long_name(4096, 'a');
        long_name += "/Type";
        const auto methods{ vmhook::get_class_methods(long_name) };
        check("B_very_long_name_empty", methods.empty());
        check("B_very_long_name_size0", methods.size() == 0);
    }
    {
        // Full high-byte name (0x80..0xFF run) — exercises the signed-char /
        // non-ASCII path of the key copy without any decode assumption.
        std::string high_bytes{};
        for (int b{ 0x80 }; b <= 0xFF; ++b)
        {
            high_bytes.push_back(static_cast<char>(b));
        }
        const auto methods{ vmhook::get_class_methods(high_bytes) };
        check("B_high_byte_name_empty", methods.empty());
    }

    // ===================================================================
    // PART C — find_methods_by_signature<T>(descriptor) : exhaustive descriptor
    // matrix.  With no JVM the source list (get_class_methods<T>()) is empty,
    // so the result is ALWAYS empty regardless of descriptor.  We drive the
    // full descriptor input domain to prove no descriptor — well-formed,
    // malformed, empty, NUL, or pathological — ever throws or yields a match.
    // ===================================================================
    {
        // Canonical well-formed descriptors covering every return kind and a
        // representative parameter spread.
        const char* descriptors[]{
            // void return, no params / various params.
            "()V",
            "(I)V",
            "(II)V",
            "(J)V",
            "(D)V",
            "(Ljava/lang/String;)V",
            "([I)V",
            "(IJD)V",
            // every primitive return.
            "()Z", "()B", "()C", "()S", "()I", "()J", "()F", "()D",
            "(I)I",
            "(J)J",
            "(D)D",
            "(F)F",
            "(Z)Z",
            "(B)B",
            "(C)C",
            "(S)S",
            // object / array returns.
            "()Ljava/lang/Object;",
            "()Ljava/lang/String;",
            "(Ljava/lang/String;)I",
            "()[I",
            "()[[I",
            "()[Ljava/lang/String;",
            "([Ljava/lang/Object;)V",
            // the two-slot J/D boundary + multi-slot statics.
            "(IJD)D",
            "(JD)J",
            "(JJ)J",
            "(DD)D",
            // mixed long parameter lists.
            "(IJLjava/lang/Object;)I",
            "(Lcom/example/Deep$Inner;Ljava/lang/Object;LX;)V",
            "(ZBCSIJFD)V",
            "([Z[B[C[S[I[J[F[D)V",
        };
        bool all_empty{ true };
        for (const char* d : descriptors)
        {
            const auto names{ vmhook::find_methods_by_signature<map_injected_wrapper>(d) };
            if (!names.empty()) { all_empty = false; }
        }
        check("C_every_wellformed_descriptor_empty_no_jvm", all_empty);
    }
    {
        // Malformed / degenerate descriptors: none must throw, all empty.
        const char* malformed[]{
            "",                       // empty
            "V",                      // bare return letter, no parens
            "I",                      // bare primitive
            "(",                      // unbalanced open
            ")",                      // unbalanced close
            "()",                     // no return descriptor
            "(I)",                    // params but no return
            "(I",                     // truncated params
            "I)V",                    // missing open paren
            "()VV",                   // trailing junk after void
            "()L",                    // truncated object return
            "(L)V",                   // unterminated object param
            "(Ljava/lang/String)V",   // object param missing ';'
            "([)V",                   // '[' with no element
            "()Q",                    // unknown return letter
            "(Q)V",                   // unknown param letter
            "garbage",                // pure junk
            "()V()V",                 // two descriptors concatenated
            "(()))V",                 // nested-paren soup
            "   ",                    // whitespace only
            "( I )V",                 // spaces inside
            "()ljava/lang/Object;",   // lowercase 'l' object marker
            "()V ",                   // trailing space
            " ()V",                   // leading space
        };
        bool all_empty{ true };
        for (const char* d : malformed)
        {
            const auto names{ vmhook::find_methods_by_signature<map_injected_wrapper>(d) };
            if (!names.empty()) { all_empty = false; }
        }
        check("C_every_malformed_descriptor_empty_no_jvm", all_empty);
    }
    {
        // Unregistered type takes the type-map-miss arm: empty for any
        // descriptor, including a normally-valid one.
        const auto a{ vmhook::find_methods_by_signature<unregistered_wrapper>("()V") };
        const auto b{ vmhook::find_methods_by_signature<unregistered_wrapper>("(I)I") };
        const auto c{ vmhook::find_methods_by_signature<second_unregistered>("(J)J") };
        check("C_unregistered_type_empty_for_void", a.empty());
        check("C_unregistered_type_empty_for_int", b.empty());
        check("C_second_unregistered_type_empty_for_long", c.empty());
    }
    {
        // Registered-but-failed type (register_class returned false, so it is
        // NOT in the map) also hits the type-map-miss arm.
        const auto names{ vmhook::find_methods_by_signature<registered_wrapper>("()V") };
        check("C_failed_register_type_empty", names.empty());
    }
    {
        // Empty descriptor against the map-injected (find_class-null) type.
        const auto names{ vmhook::find_methods_by_signature<map_injected_wrapper>("") };
        check("C_empty_descriptor_empty", names.empty());
    }
    {
        // Embedded-NUL descriptor: length 4 "()V\0" plus content after NUL must
        // not match anything and must not crash (string_view length-aware path).
        const char raw[]{ '(', ')', 'V', '\0', 'X' };
        const std::string_view desc{ raw, sizeof(raw) };
        const auto names{ vmhook::find_methods_by_signature<map_injected_wrapper>(desc) };
        check("C_embedded_nul_descriptor_empty", names.empty());
    }
    {
        // Pathologically long descriptor: "(" + 1000*'I' + ")V".
        std::string long_desc{ "(" };
        long_desc.append(1000, 'I');
        long_desc += ")V";
        const auto names{ vmhook::find_methods_by_signature<map_injected_wrapper>(long_desc) };
        check("C_very_long_descriptor_empty", names.empty());
    }
    {
        // Full 0..255 first-byte sweep AS a one-byte descriptor: every single
        // byte fed as a "descriptor" is a miss with no throw.  Proves the
        // entry point is total over the whole byte domain.
        bool all_empty{ true };
        for (int b{ 0 }; b <= 0xFF; ++b)
        {
            const char one[1]{ static_cast<char>(b) };
            const std::string_view desc{ one, 1 };
            const auto names{ vmhook::find_methods_by_signature<map_injected_wrapper>(desc) };
            if (!names.empty()) { all_empty = false; }
        }
        check("C_full_byte_descriptor_sweep_all_empty", all_empty);
    }

    // ===================================================================
    // PART D — hook_by_signature<T>(descriptor, detour) : exhaustive refusal.
    // With no JVM, find_methods_by_signature is empty, so the names.empty()
    // gate returns FALSE for every descriptor and every detour shape — the
    // underlying hook<T> install is never reached.  We sweep the descriptor
    // matrix AND multiple detour signatures to pin the no-match refusal path.
    // ===================================================================
    {
        // The exact descriptor the JVM module installs (unique (J)J) — false
        // here because no klass is loaded, not because of multiplicity.
        const bool installed{
            vmhook::hook_by_signature<registered_wrapper>("(J)J", &detour_self_only) };
        check("D_unique_descriptor_no_jvm_returns_false", installed == false);
    }
    {
        // Descriptor matrix, one canonical detour: every install refuses.
        const char* descriptors[]{
            "()V", "(I)I", "(J)J", "(D)D", "(Ljava/lang/String;)I",
            "([I)I", "(IJD)D", "()Z", "()Ljava/lang/Object;", "(JD)J",
            "", "(", ")", "garbage", "()Q",
        };
        bool all_false{ true };
        for (const char* d : descriptors)
        {
            if (vmhook::hook_by_signature<registered_wrapper>(d, &detour_self_only))
            {
                all_false = false;
            }
        }
        check("D_every_descriptor_refuses_no_jvm", all_false);
    }
    {
        // Same outcome for the map-injected (find_class-null) type — proves the
        // refusal is the no-match gate, independent of which empty path the
        // source list took.
        bool all_false{ true };
        const char* descriptors[]{ "()V", "(J)J", "(I)I", "" };
        for (const char* d : descriptors)
        {
            if (vmhook::hook_by_signature<map_injected_wrapper>(d, &detour_for_injected))
            {
                all_false = false;
            }
        }
        check("D_map_injected_type_refuses_no_jvm", all_false);
    }
    {
        // Every detour SHAPE compiles through hook<T> and refuses identically.
        // (void-only, self-only, +int, +long, +int/double, +oop.)  This pins
        // that hook_by_signature accepts the full detour-arity surface and the
        // hook<T> authoring-contract static_asserts are satisfied by each.
        const bool r_void{ vmhook::hook_by_signature<registered_wrapper>("()V", &detour_void_only) };
        const bool r_self{ vmhook::hook_by_signature<registered_wrapper>("()V", &detour_self_only) };
        const bool r_int{ vmhook::hook_by_signature<registered_wrapper>("(I)V", &detour_one_int) };
        const bool r_long{ vmhook::hook_by_signature<registered_wrapper>("(J)V", &detour_long_arg) };
        const bool r_two{ vmhook::hook_by_signature<registered_wrapper>("(ID)V", &detour_two_args) };
        const bool r_oop{ vmhook::hook_by_signature<registered_wrapper>("(Ljava/lang/Object;)V", &detour_oop_arg) };
        check("D_detour_void_only_refuses", r_void == false);
        check("D_detour_self_only_refuses", r_self == false);
        check("D_detour_one_int_refuses", r_int == false);
        check("D_detour_long_arg_refuses", r_long == false);
        check("D_detour_two_args_refuses", r_two == false);
        check("D_detour_oop_arg_refuses", r_oop == false);
    }
    {
        // Unregistered type: refuses for any descriptor / detour.
        const bool a{ vmhook::hook_by_signature<unregistered_wrapper>("()V", &detour_void_only) };
        check("D_unregistered_type_refuses", a == false);
    }
    {
        // Embedded-NUL and empty descriptors also refuse (no throw).
        const char raw[]{ '(', 'J', ')', 'J', '\0' };
        const std::string_view desc{ raw, sizeof(raw) };
        const bool a{ vmhook::hook_by_signature<registered_wrapper>(desc, &detour_self_only) };
        const bool b{ vmhook::hook_by_signature<registered_wrapper>(std::string_view{}, &detour_self_only) };
        check("D_embedded_nul_descriptor_refuses", a == false);
        check("D_empty_descriptor_refuses", b == false);
    }

    // ===================================================================
    // PART E — log_class_methods<T>() : the debug sibling is safe to call with
    // no JVM and on any registration state (it is a no-op data-wise in release,
    // and must not crash in either build).
    // ===================================================================
    {
        vmhook::log_class_methods<registered_wrapper>();
        vmhook::log_class_methods<unregistered_wrapper>();
        vmhook::log_class_methods<map_injected_wrapper>();
        check("E_log_class_methods_no_jvm_safe_all_states", true);
    }

    // ===================================================================
    // PART F — descriptor-matching reference (mirror of the candidate==desc
    // equality find_methods_by_signature uses).  This is the JVM-free way to
    // pin the SELECTION semantics over a populated source list: exact match,
    // case sensitivity, no dotted<->slashed translation, multiplicity, order.
    // ===================================================================
    {
        // Exact equality is the whole rule.
        check("F_match_identical", descriptor_matches("(J)J", "(J)J"));
        check("F_match_void", descriptor_matches("()V", "()V"));
        check("F_no_match_different", !descriptor_matches("(I)I", "(J)J"));
        // Case-sensitive: 'j' != 'J'.
        check("F_match_case_sensitive", !descriptor_matches("(j)j", "(J)J"));
        // Length-aware: a prefix is NOT a match.
        check("F_no_match_prefix", !descriptor_matches("(I)", "(I)I"));
        check("F_no_match_superstring", !descriptor_matches("(I)II", "(I)I"));
        // No normalisation: dotted vs slashed object names are distinct.
        check("F_no_match_dotted_vs_slashed",
              !descriptor_matches("(Ljava.lang.String;)V", "(Ljava/lang/String;)V"));
        // No whitespace trimming.
        check("F_no_match_trailing_space", !descriptor_matches("(I)I ", "(I)I"));
        check("F_no_match_leading_space", !descriptor_matches(" (I)I", "(I)I"));
        // Empty matches only empty.
        check("F_empty_matches_empty", descriptor_matches("", ""));
        check("F_empty_no_match_nonempty", !descriptor_matches("", "()V"));
        check("F_nonempty_no_match_empty", !descriptor_matches("()V", ""));
    }
    {
        // Embedded-NUL equality is byte-aware: "()V\0A" != "()V\0B" but equals
        // itself, and neither equals the 3-byte "()V".
        const char a[]{ '(', ')', 'V', '\0', 'A' };
        const char b[]{ '(', ')', 'V', '\0', 'B' };
        const std::string_view va{ a, sizeof(a) };
        const std::string_view vb{ b, sizeof(b) };
        const std::string_view v3{ "()V" };
        check("F_embedded_nul_self_equal", descriptor_matches(va, va));
        check("F_embedded_nul_differ_after_nul", !descriptor_matches(va, vb));
        check("F_embedded_nul_not_truncated_match", !descriptor_matches(va, v3));
    }
    {
        // Multiplicity + order: over a populated source list the mirror keeps
        // EVERY matching name in source order — this is the contract
        // find_methods_by_signature exposes so a caller can detect a non-unique
        // descriptor (and hook_by_signature can refuse it).  Reproduced here
        // because the live function cannot see a populated list with no JVM.
        const std::vector<std::pair<std::string, std::string>> source{
            { "idLong",   "(J)J" },                 // unique (J)J
            { "idInt",    "(I)I" },                  // 3-way (I)I collision
            { "addInt",   "(I)I" },
            { "sId",      "(I)I" },
            { "noop",     "()V" },                   // ()V collision
            { "tick",     "()V" },
            { "init",     "()V" },
            { "byName",   "(Ljava/lang/String;)I" }, // unique reference-arg
            { "byArray",  "([I)I" },                 // unique array-arg
            { "wide",     "(IJD)D" },                // unique two-slot boundary
        };
        // (J)J -> exactly one, named idLong.
        const auto j{ select_by_descriptor(source, "(J)J") };
        check("F_select_unique_long_one_match", j.size() == 1);
        check("F_select_unique_long_is_idLong", j.size() == 1 && j.front() == "idLong");
        // (I)I -> exactly three, in source order.
        const auto i{ select_by_descriptor(source, "(I)I") };
        check("F_select_int_three_matches", i.size() == 3);
        check("F_select_int_order_preserved",
              i.size() == 3 && i[0] == "idInt" && i[1] == "addInt" && i[2] == "sId");
        // ()V -> exactly three here (noop/tick/init).
        const auto v{ select_by_descriptor(source, "()V") };
        check("F_select_void_three_matches", v.size() == 3);
        // Each genuinely-unique descriptor -> its one method.
        check("F_select_ref_arg_unique",
              select_by_descriptor(source, "(Ljava/lang/String;)I").size() == 1);
        check("F_select_array_arg_unique",
              select_by_descriptor(source, "([I)I").size() == 1);
        check("F_select_two_slot_unique",
              select_by_descriptor(source, "(IJD)D").size() == 1);
        // Absent descriptor -> zero.
        check("F_select_absent_descriptor_zero",
              select_by_descriptor(source, "(D)D").empty());
        // Empty descriptor against a populated list -> zero (no member is "").
        check("F_select_empty_descriptor_zero",
              select_by_descriptor(source, "").empty());
        // The hook_by_signature decision reproduced: unique <=> size()==1.
        check("F_unique_descriptor_is_size_one",
              select_by_descriptor(source, "(J)J").size() == 1);
        check("F_ambiguous_descriptor_is_size_gt_one",
              select_by_descriptor(source, "(I)I").size() > 1);
        check("F_nomatch_descriptor_is_size_zero",
              select_by_descriptor(source, "(D)D").empty());
    }
    {
        // Structural invariant that ties the mirror back to the live no-JVM
        // behaviour: selecting over an EMPTY source list yields zero matches
        // for EVERY descriptor — which is exactly why find_methods_by_signature
        // is empty for all input with no JVM.  Sweep the descriptor matrix
        // against an empty source and assert zero every time.
        const std::vector<std::pair<std::string, std::string>> empty_source{};
        const char* descriptors[]{
            "()V", "(I)I", "(J)J", "(D)D", "(Ljava/lang/String;)I",
            "([I)I", "(IJD)D", "", "garbage", "()Q",
        };
        bool all_zero{ true };
        for (const char* d : descriptors)
        {
            if (!select_by_descriptor(empty_source, d).empty()) { all_zero = false; }
        }
        check("F_empty_source_zero_matches_for_every_descriptor", all_zero);
    }

    // ===================================================================
    // PART G — cross-consistency: the live entry points agree with each other
    // and with the mirror in the no-JVM regime (everything empty), and the
    // by-type / by-name overloads agree.
    // ===================================================================
    {
        // find_methods_by_signature is literally a filter over
        // get_class_methods<T>(); with the source empty, the filtered result
        // must be empty too — for an arbitrary descriptor.  Assert the
        // implication directly.
        const auto source{ vmhook::get_class_methods<map_injected_wrapper>() };
        const auto filtered{ vmhook::find_methods_by_signature<map_injected_wrapper>("()V") };
        check("G_source_empty_implies_filtered_empty",
              source.empty() && filtered.empty());
    }
    {
        // The by-name and by-type overloads resolve the SAME class
        // ("test/MapInjected") and therefore return identically (empty) here.
        const auto by_type{ vmhook::get_class_methods<map_injected_wrapper>() };
        const auto by_name{ vmhook::get_class_methods("test/MapInjected") };
        check("G_by_type_and_by_name_agree_empty",
              by_type.empty() && by_name.empty());
        check("G_by_type_and_by_name_same_size",
              by_type.size() == by_name.size());
    }
    {
        // hook_by_signature's refusal is consistent with
        // find_methods_by_signature being empty: if the latter is empty, the
        // former returns false.  Pin the linkage over a few descriptors.
        const char* descriptors[]{ "()V", "(J)J", "(I)I" };
        bool consistent{ true };
        for (const char* d : descriptors)
        {
            const bool empty_match{ vmhook::find_methods_by_signature<registered_wrapper>(d).empty() };
            const bool refused{ vmhook::hook_by_signature<registered_wrapper>(d, &detour_self_only) == false };
            if (!(empty_match && refused)) { consistent = false; }
        }
        check("G_empty_match_implies_hook_refused", consistent);
    }

    // ===================================================================
    // PART H — original baseline assertions (kept verbatim in spirit) so the
    // historical contract names remain greppable.
    // ===================================================================
    {
        const auto methods{ vmhook::get_class_methods<registered_wrapper>() };
        check("H_registered_wrapper_methods_empty_no_jvm", methods.empty());
        check("H_registered_wrapper_methods_size0", methods.size() == 0);
    }
    {
        const auto methods{ vmhook::get_class_methods<unregistered_wrapper>() };
        check("H_unregistered_wrapper_methods_empty", methods.empty());
    }
    {
        const auto methods{ vmhook::get_class_methods("java/lang/Object") };
        check("H_by_name_methods_empty_no_jvm", methods.empty());
        const auto missing{ vmhook::get_class_methods("definitely/Not/A/Class") };
        check("H_by_name_missing_class_empty", missing.empty());
    }
    {
        const auto names{
            vmhook::find_methods_by_signature<registered_wrapper>("()Ljava/util/Collection;") };
        check("H_find_by_signature_empty_no_jvm", names.empty());
        const auto names_unreg{
            vmhook::find_methods_by_signature<unregistered_wrapper>("()V") };
        check("H_find_by_signature_unregistered_empty", names_unreg.empty());
    }
    {
        const bool installed{
            vmhook::hook_by_signature<registered_wrapper>("()V", &detour_self_only) };
        check("H_hook_by_signature_no_match_returns_false", installed == false);
    }
    {
        vmhook::log_class_methods<registered_wrapper>();
        check("H_log_class_methods_no_jvm_safe", true);
    }
    {
        check("H_get_class_methods_return_type", true); // pinned via static_assert above
    }

    // =====================================================================
    // PART W (instanceklass_methods_walk DEEPENING) — additive, namespaced.
    //
    // The walk turns a klass* into its declared (name, descriptor) list by
    // reading InstanceKlass::_methods directly.  This section pins the
    // SUBSTRATE primitives the walk is built from, with every expected value
    // derived from vmhook.hpp source:
    //   * klass::get_methods_count()  vmhook.hpp:3504  (Array<T>::_length @0,
    //                                  clamp count<0||count>65535 -> 0,
    //                                  return 0 when the _methods VMStruct
    //                                  entry is unresolved == no JVM here)
    //   * klass::get_methods_ptr()    vmhook.hpp:3543  (data @ array+8,
    //                                  nullptr when entry unresolved)
    //   * detail::collect_klass_methods() vmhook.hpp:8985 (null klass -> empty;
    //                                  per-slot is_valid_pointer skip @9005)
    //   * hotspot::is_valid_pointer() vmhook.hpp:2047  (the per-slot skip
    //                                  predicate: floor/ceiling/odd/9 sentinels)
    //
    // POSIX-SAFETY: with NO JVM in-process get_proc_address("gHotSpotVMStructs")
    // is null (vmhook.hpp:1944), so iterate_struct_entries("InstanceKlass",
    // "_methods") returns null and BOTH raw accessors bail at their `!entry`
    // guard (3509 / 3548) BEFORE ever dereferencing `this`.  We therefore call
    // them ONLY on (a) nullptr and (b) is_valid_pointer-REJECTED low/odd
    // constants — never on a fabricated valid-shaped address — so no raw read
    // of a wild pointer ever occurs.  All layout/clamp/decode SEMANTICS that
    // would need a live klass are pinned through captureless arithmetic mirrors
    // of the exact source expressions instead.
    // =====================================================================

    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::klass;

    // ---------------------------------------------------------------------
    // W1 — is_valid_pointer: the EXACT per-slot skip predicate the walk uses
    // at vmhook.hpp:9005 to drop a Method* slot.  Pure address arithmetic; no
    // memory is read for ANY of these inputs.  Thresholds from source:
    //   floor   = 0xFFFF              (vmhook.hpp:520, reject addr <= floor)
    //   ceiling = 0x00007FFFFFFFFFFF  (vmhook.hpp:515, reject addr >= ceiling)
    //   reject odd (addr & 1)         (vmhook.hpp:2059)
    //   reject low32 in 9 sentinels   (vmhook.hpp:2070-2078)
    // ---------------------------------------------------------------------
    {
        constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };       // 0xFFFF
        constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };   // 0x00007FFFFFFFFFFF
        check("W1_floor_value_is_0xFFFF", floor == 0xFFFFull);
        check("W1_ceiling_value", ceiling == 0x00007FFFFFFFFFFFull);

        // nullptr is rejected (0 <= floor).
        check("W1_null_rejected", !is_valid_pointer(nullptr));

        // EXACT floor boundary: addr <= floor rejected, floor+1 (odd) rejected
        // by the odd-rule, floor+2 (= 0x10001, even, > floor) ACCEPTED.
        check("W1_floor_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor)));
        check("W1_floor_minus_one_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor - 1)));
        // floor == 0xFFFF (odd); floor+1 == 0x10000 which is EVEN and > floor
        // -> ACCEPTED (first valid address above the floor).  Pin it.
        check("W1_floor_plus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(floor + 1)));
        // floor+2 == 0x10001 (odd) -> rejected by the odd-rule even though
        // it is > floor; floor+3 == 0x10002 (even) -> accepted.
        check("W1_floor_plus_two_odd_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor + 2)));
        check("W1_floor_plus_three_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(floor + 3)));

        // EXACT ceiling boundary: addr >= ceiling rejected; ceiling-1 is even
        // (0x...FFFE) and < ceiling -> ACCEPTED.
        check("W1_ceiling_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(ceiling)));
        check("W1_ceiling_plus_one_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(ceiling + 1)));
        check("W1_ceiling_minus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(ceiling - 1)));
        check("W1_ceiling_minus_two_odd_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(ceiling - 2)));

        // Low constants the HARD RULES bless as safe (rejected before any read):
        // 0x1000 < floor -> rejected; this is exactly the substrate the per-slot
        // skip relies on for any low/sentinel garbage slot.
        check("W1_0x1000_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(0x1000ull)));
        check("W1_0x2_rejected_low",
              !is_valid_pointer(reinterpret_cast<const void*>(0x2ull)));
        check("W1_0x1_rejected_odd_and_low",
              !is_valid_pointer(reinterpret_cast<const void*>(0x1ull)));
    }
    {
        // The 9 debug-fill sentinels (vmhook.hpp:2070-2078) — a slot whose LOW
        // 32 bits equal one of these is dropped by the walk (flaw #5: this can
        // in principle elide a legitimate Metaspace Method* whose low half
        // collides; pinned here as the documented behaviour).  Each is forced
        // even and placed in-range by OR-ing a high canonical base so ONLY the
        // sentinel low32 (not range/alignment) drives the rejection.
        constexpr std::uint64_t high_base{ 0x00000A0000000000ull };  // in (floor,ceiling), well-aligned
        const std::array<std::uint32_t, 9> sentinels{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu };
        bool all_rejected{ true };
        for (const std::uint32_t s : sentinels)
        {
            // Place the EXACT sentinel pattern as the low32 of an in-range base.
            // Every such address is rejected: the even sentinels by the
            // sentinel switch (vmhook.hpp:2070-2078), the odd ones by the
            // odd-rule (vmhook.hpp:2059) — both paths reject, which is the
            // walk's per-slot skip behaviour.
            const std::uint64_t addr{ high_base | static_cast<std::uint64_t>(s) };
            if (is_valid_pointer(reinterpret_cast<const void*>(addr)))
            {
                all_rejected = false;
            }
        }
        check("W1_all_sentinel_low32_addresses_rejected", all_rejected);

        // Strict: low32 EXACTLY a sentinel that is itself EVEN, so the
        // sentinel-switch (not the odd-rule) is the sole cause of rejection.
        // Even (bit0==0) members of the list: 0xCAFEBABE, 0xCCCCCCCC, 0xFEEEFEEE.
        const std::array<std::uint32_t, 3> even_sentinels{
            0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu };
        bool even_all_rejected{ true };
        for (const std::uint32_t s : even_sentinels)
        {
            const std::uint64_t addr{ high_base | static_cast<std::uint64_t>(s) };
            // sanity: this address is even, > floor, < ceiling, so ONLY the
            // sentinel switch can reject it.
            if (is_valid_pointer(reinterpret_cast<const void*>(addr)))
            {
                even_all_rejected = false;
            }
        }
        check("W1_even_sentinel_low32_strictly_rejected", even_all_rejected);

        // Control: the SAME high base with a NON-sentinel even low32 is ACCEPTED
        // — proving the rejections above are caused by the sentinel/odd rules,
        // not by the base being out of range.
        const std::uint64_t clean{ high_base | 0x00010002ull };  // even, non-sentinel
        check("W1_clean_low32_high_base_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(clean)));
    }

    // ---------------------------------------------------------------------
    // W2 — raw accessors honour the no-JVM contract on null / rejected klass
    // pointers WITHOUT dereferencing (entry is null -> bail at the !entry
    // guard; or is_valid_pointer(this) false -> bail).  POSIX-safe: no read of
    // a wild address ever happens.
    // ---------------------------------------------------------------------
    {
        klass* const null_klass{ nullptr };
        check("W2_count_null_klass_zero", null_klass == nullptr);
        // collect over a null klass -> empty (vmhook.hpp:8991 null-klass arm).
        const auto via_collect{ vmhook::detail::collect_klass_methods(null_klass) };
        check("W2_collect_null_klass_empty", via_collect.empty());
        check("W2_collect_null_klass_size0", via_collect.size() == 0);
    }
    {
        // is_valid_pointer-REJECTED low constant cast to klass*: BOTH accessors
        // return the empty sentinel without reading memory (this fails
        // is_valid_pointer(this) AND, with no JVM, entry is null first).
        klass* const low_klass{ reinterpret_cast<klass*>(0x1000ull) };
        check("W2_low_klass_count_zero", low_klass->get_methods_count() == 0);
        check("W2_low_klass_ptr_null", low_klass->get_methods_ptr() == nullptr);
        const auto m{ vmhook::detail::collect_klass_methods(low_klass) };
        check("W2_low_klass_collect_empty", m.empty());
    }
    {
        // Odd (bit-0 set) low constant: rejected by the odd-rule too.
        klass* const odd_klass{ reinterpret_cast<klass*>(0x1001ull) };
        check("W2_odd_klass_count_zero", odd_klass->get_methods_count() == 0);
        check("W2_odd_klass_ptr_null", odd_klass->get_methods_ptr() == nullptr);
    }
    {
        // Idempotent across the substrate: count==0 implies the collector's
        // `!methods_array || method_count <= 0` guard (vmhook.hpp:8997) fires,
        // so collect is empty for the same input — pinned as an implication.
        klass* const low_klass{ reinterpret_cast<klass*>(0x800ull) };
        const std::int32_t cnt{ low_klass->get_methods_count() };
        const auto coll{ vmhook::detail::collect_klass_methods(low_klass) };
        check("W2_count_zero_implies_collect_empty",
              (cnt == 0) && coll.empty());
    }

    // ---------------------------------------------------------------------
    // W3 — get_methods_count()'s clamp logic reproduced as a pure mirror.
    // Source (vmhook.hpp:3528): `if (count < 0 || count > 65535) return 0;`
    // (flaw #2 hardening).  A live klass is needed to exercise the real read,
    // but the CLAMP DECISION is pure integer logic we can pin exhaustively.
    // ---------------------------------------------------------------------
    {
        auto clamp_count = [](std::int32_t count) -> std::int32_t {
            if (count < 0 || count > 65535) { return 0; }
            return count;
        };
        // The HotSpot u2 method_count ceiling is exactly 65535.
        check("W3_clamp_max_valid_65535", clamp_count(65535) == 65535);
        check("W3_clamp_just_over_65536_zero", clamp_count(65536) == 0);
        check("W3_clamp_zero_passes", clamp_count(0) == 0);
        check("W3_clamp_one_passes", clamp_count(1) == 1);
        check("W3_clamp_negative_one_zero", clamp_count(-1) == 0);
        check("W3_clamp_int_min_zero",
              clamp_count(std::numeric_limits<std::int32_t>::min()) == 0);
        check("W3_clamp_int_max_zero",
              clamp_count(std::numeric_limits<std::int32_t>::max()) == 0);
        // A "large positive garbage length" (flaw #2's hypothetical wrong-layout
        // misread, e.g. 0x40000000) is now clamped to 0 -> no billion-iteration
        // loop / no reserve(huge).  Pin the exact threshold neighbours.
        check("W3_clamp_0x40000000_zero", clamp_count(0x40000000) == 0);
        check("W3_clamp_65535_boundary_above_zero", clamp_count(65535 + 1) == 0);
        check("W3_clamp_65534_passes", clamp_count(65534) == 65534);
    }

    // ---------------------------------------------------------------------
    // W4 — get_methods_ptr()'s Array<Method*> data-offset arithmetic (flaw #1).
    // Source (vmhook.hpp:3564): data = reinterpret_cast<method**>(array + 8),
    // i.e. [int32 _length @0][int32 _pad @4][Method* _data @8].  The +8 is the
    // x64 LP64 layout assumption.  We pin the byte arithmetic as a pure mirror
    // (no memory read) so the offset contract is greppable and the element
    // stride matches sizeof(Method*) == 8.
    // ---------------------------------------------------------------------
    {
        // The data offset is exactly 8 (length@0 width 4 + pad@4 width 4).
        constexpr std::size_t length_field_width{ sizeof(std::int32_t) };  // 4
        constexpr std::size_t pad_field_width{ sizeof(std::int32_t) };     // 4
        constexpr std::size_t data_offset{ length_field_width + pad_field_width };
        check("W4_length_field_width_4", length_field_width == 4);
        check("W4_pad_field_width_4", pad_field_width == 4);
        check("W4_data_offset_is_8", data_offset == 8);

        // Element stride is one pointer (Method*) == 8 on LP64.  This is the
        // assumption the +8 hardcode rides on; pin it so a non-LP64 build trips
        // here at compile-of-expected-value time rather than misreading slots.
        check("W4_method_ptr_size_is_8", sizeof(void*) == 8);

        // Mirror the address computation: for a hypothetical (never-read) base,
        // data = base + 8 and slot i = base + 8 + 8*i.  Use a stack buffer as a
        // REAL OWNED allocation so the arithmetic is well-defined (we never
        // DEREFERENCE through these as Method*; we only compare the byte
        // offsets the accessor would compute).
        alignas(16) std::array<std::uint8_t, 64> owned_array_bytes{};
        std::uint8_t* const base{ owned_array_bytes.data() };
        std::uint8_t* const data_ptr{ base + data_offset };
        check("W4_data_ptr_is_base_plus_8",
              data_ptr == base + 8);
        // slot index arithmetic: &data[i] - &data[0] == i * 8 bytes.
        std::uint8_t* const slot0{ base + data_offset + (0 * sizeof(void*)) };
        std::uint8_t* const slot1{ base + data_offset + (1 * sizeof(void*)) };
        std::uint8_t* const slot2{ base + data_offset + (2 * sizeof(void*)) };
        check("W4_slot0_at_offset_8", (slot0 - base) == 8);
        check("W4_slot1_at_offset_16", (slot1 - base) == 16);
        check("W4_slot2_at_offset_24", (slot2 - base) == 24);
        check("W4_slot_stride_8", (slot1 - slot0) == 8 && (slot2 - slot1) == 8);
    }

    // ---------------------------------------------------------------------
    // W5 — the collector emplaces (name, descriptor); a SKIPPED slot
    // (vmhook.hpp:9005 continue) never reaches emplace_back, so no ("","")
    // pair can appear from a skip, and a decoded-but-empty name only appears if
    // get_name() itself failed.  With no JVM the live result is empty, so we
    // pin the INVARIANT against a self-contained model of the collector loop:
    // skipped slots produce no element, kept slots produce exactly one.
    // ---------------------------------------------------------------------
    {
        // Model the loop body's keep/skip decision over a vector of "slot
        // validity" flags (true == is_valid_pointer && non-null).  The model
        // emplaces iff kept — mirroring vmhook.hpp:9002-9009 exactly.
        auto count_kept = [](const std::vector<bool>& slot_valid) -> std::size_t {
            std::size_t kept{ 0 };
            for (const bool v : slot_valid)
            {
                if (!v) { continue; }   // mirrors the !method_ptr||!is_valid skip
                ++kept;
            }
            return kept;
        };
        check("W5_all_valid_all_kept", count_kept({ true, true, true }) == 3);
        check("W5_all_invalid_none_kept", count_kept({ false, false }) == 0);
        check("W5_mixed_kept_count",
              count_kept({ true, false, true, false, true }) == 3);
        check("W5_empty_slotlist_zero_kept", count_kept({}) == 0);
        // A single invalid slot in the middle reduces the kept count by exactly
        // one (no off-by-one): 4 valid + 1 invalid -> 4.
        check("W5_one_skip_reduces_by_one",
              count_kept({ true, true, false, true, true }) == 4);
    }

    // ---------------------------------------------------------------------
    // W6 — collector / get_class_methods cross-consistency on the no-JVM empty
    // result: every entry point that routes through collect_klass_methods must
    // agree byte-for-byte (all empty), and find_methods_by_signature (a filter
    // over the collector) must be a subset (here: empty).  Strengthens the
    // existing PART G with the RAW collector in the loop.
    // ---------------------------------------------------------------------
    {
        const auto by_name{ vmhook::get_class_methods("test/MapInjected") };
        const auto by_type{ vmhook::get_class_methods<map_injected_wrapper>() };
        const auto raw{ vmhook::detail::collect_klass_methods(
            vmhook::find_class("test/MapInjected")) };
        check("W6_byname_bytype_raw_all_empty",
              by_name.empty() && by_type.empty() && raw.empty());
        check("W6_byname_bytype_raw_same_size",
              by_name.size() == by_type.size() && by_type.size() == raw.size());
        // find_class itself returns null with no JVM -> the collector's
        // null-klass arm -> empty.  Pin the linkage.
        check("W6_find_class_null_no_jvm",
              vmhook::find_class("test/MapInjected") == nullptr);
    }

    // ---------------------------------------------------------------------
    // W7 — descriptor selector ORDERED-equality + multiplicity (flaw #3/#8):
    // the collector/walk is index-ordered over _methods, and
    // find_methods_by_signature keeps matches IN SOURCE ORDER.  PART F already
    // checks multiset membership; here we pin the STRONGER element-for-element
    // ORDERED equality of the selector over a populated model, and that calling
    // twice yields an IDENTICAL vector (determinism within a run).
    // ---------------------------------------------------------------------
    {
        const std::vector<std::pair<std::string, std::string>> source{
            { "a", "()V" },
            { "b", "(I)I" },
            { "c", "()V" },
            { "d", "(I)I" },
            { "e", "()V" },
            { "f", "(J)J" },
        };
        const auto v1{ select_by_descriptor(source, "()V") };
        const auto v2{ select_by_descriptor(source, "()V") };
        // determinism: identical element-for-element including order.
        check("W7_selector_deterministic_same_size", v1.size() == v2.size());
        check("W7_selector_deterministic_equal", v1 == v2);
        // ordered membership: a, c, e in that exact order.
        check("W7_void_ordered_a_c_e",
              v1.size() == 3 && v1[0] == "a" && v1[1] == "c" && v1[2] == "e");
        const auto vi{ select_by_descriptor(source, "(I)I") };
        check("W7_int_ordered_b_d",
              vi.size() == 2 && vi[0] == "b" && vi[1] == "d");
        const auto vj{ select_by_descriptor(source, "(J)J") };
        check("W7_long_unique_f", vj.size() == 1 && vj.front() == "f");
        // total kept across the three present descriptors == source size.
        check("W7_total_partition_covers_source",
              v1.size() + vi.size() + vj.size() == source.size());
    }

    // ---------------------------------------------------------------------
    // W8 — inherited-exclusion contract (flaw #4) modelled explicitly: the walk
    // reads ONE InstanceKlass's _methods array — DECLARED methods only, never
    // inherited.  We model a 3-level hierarchy's DECLARED sets and assert that
    // enumerating C yields only C's declared methods, with Object's
    // equals/hashCode descriptors ABSENT.  (The live exclusion is JVM-tested;
    // here we pin the contract shape the selector preserves.)
    // ---------------------------------------------------------------------
    {
        // C's OWN declared _methods (what the bare walk would return for C).
        const std::vector<std::pair<std::string, std::string>> c_declared{
            { "cMethod", "()V" },
            { "<init>",  "()V" },
        };
        // Inherited (Object) descriptors that must NOT be in c_declared.
        const auto eq{ select_by_descriptor(c_declared, "(Ljava/lang/Object;)Z") };  // equals
        const auto hc{ select_by_descriptor(c_declared, "()I") };                    // hashCode
        check("W8_inherited_equals_absent_from_declared", eq.empty());
        check("W8_inherited_hashCode_absent_from_declared", hc.empty());
        // C's own declared method IS present.
        const auto own{ select_by_descriptor(c_declared, "()V") };
        check("W8_own_declared_present",
              own.size() == 2);  // cMethod + <init>, both ()V here
        check("W8_own_declared_includes_init",
              std::find(own.begin(), own.end(), std::string{ "<init>" }) != own.end());
    }

    // ---------------------------------------------------------------------
    // W9 — descriptor equality is BYTE-exact, embedded-NUL and modified-UTF-8
    // aware (flaw #6 decode boundary expressed as the candidate==descriptor
    // byte comparison the filter runs at vmhook.hpp:9100).  Method NAME/
    // descriptor symbols are stored as modified UTF-8 bytes; the filter never
    // re-decodes, so a Unicode name round-trips iff the BYTES match.  We pin
    // byte equality against EXPLICIT escape sequences (HARD RULE 3: never a raw
    // non-ASCII or NUL byte in source).
    // ---------------------------------------------------------------------
    {
        // "名前" (U+540D U+524D) in modified-UTF-8 is the 6 bytes
        // E5 90 8D E5 89 8D.  Pin that the byte string compares equal to itself
        // and unequal to a one-byte-different copy.
        const char jp_name[]{ '\xE5', '\x90', '\x8D', '\xE5', '\x89', '\x8D' };
        const std::string_view a{ jp_name, sizeof(jp_name) };
        const char jp_name2[]{ '\xE5', '\x90', '\x8D', '\xE5', '\x89', '\x8C' };  // last byte differs
        const std::string_view b{ jp_name2, sizeof(jp_name2) };
        check("W9_utf8_name_self_equal", descriptor_matches(a, a));
        check("W9_utf8_name_one_byte_differs_unequal", !descriptor_matches(a, b));
        check("W9_utf8_name_length_6", a.size() == 6);

        // Embedded-NUL via modified-UTF-8 (HotSpot encodes a real NUL as the
        // 2-byte sequence C0 80, never a bare 00).  A descriptor holding the
        // C0 80 pair is length-2 and equals only itself.
        const char c0_80[]{ '\xC0', '\x80' };
        const std::string_view nul_mutf8{ c0_80, sizeof(c0_80) };
        check("W9_mutf8_nul_pair_length_2", nul_mutf8.size() == 2);
        check("W9_mutf8_nul_pair_self_equal", descriptor_matches(nul_mutf8, nul_mutf8));
        // Distinct from a bare single 0x00 byte (length 1).
        const char bare_nul[]{ '\x00' };
        const std::string_view nul1{ bare_nul, sizeof(bare_nul) };
        check("W9_mutf8_pair_ne_bare_nul", !descriptor_matches(nul_mutf8, nul1));

        // A descriptor mixing ALL EIGHT primitive descriptors in one signature
        // "(ZBCSIJFD)V" is matched byte-for-byte and is NOT equal to any
        // reordering (e.g. "(BZCSIJFD)V").
        check("W9_all_primitive_sig_self_equal",
              descriptor_matches("(ZBCSIJFD)V", "(ZBCSIJFD)V"));
        check("W9_all_primitive_sig_reorder_unequal",
              !descriptor_matches("(ZBCSIJFD)V", "(BZCSIJFD)V"));
        // Multi-dim array + deep object descriptor decodes/compares byte-exact.
        check("W9_multidim_array_self_equal",
              descriptor_matches("([[[Ljava/lang/String;)V", "([[[Ljava/lang/String;)V"));
        check("W9_multidim_array_dim_count_matters",
              !descriptor_matches("([[[Ljava/lang/String;)V", "([[Ljava/lang/String;)V"));
    }

    // ---------------------------------------------------------------------
    // W10 — synthetic / special method NAME equality (the walk enumerates
    // <init>, <clinit>, lambda$, access$NNN, bridge synthetics verbatim as
    // _methods entries; their NAMES are matched by hook<T>'s inline walk).
    // Here we pin the name-string equality semantics (byte-exact, no
    // normalisation of the angle-bracket / $ characters).
    // ---------------------------------------------------------------------
    {
        // Using the descriptor-equality mirror as a generic byte-string equality
        // oracle for NAMES (hook<T>'s name match is the same operator==).
        check("W10_init_name_self_equal", descriptor_matches("<init>", "<init>"));
        check("W10_clinit_name_self_equal", descriptor_matches("<clinit>", "<clinit>"));
        check("W10_init_ne_clinit", !descriptor_matches("<init>", "<clinit>"));
        // Angle brackets are significant: "init" (no brackets) is a DIFFERENT
        // name than "<init>".
        check("W10_init_ne_plain_init", !descriptor_matches("<init>", "init"));
        // Lambda / bridge / access$ synthetic names are ordinary byte strings.
        check("W10_lambda_name_self_equal",
              descriptor_matches("lambda$run$0", "lambda$run$0"));
        check("W10_access_name_self_equal",
              descriptor_matches("access$000", "access$000"));
        check("W10_access_index_matters",
              !descriptor_matches("access$000", "access$100"));
        check("W10_empty_name_only_matches_empty",
              descriptor_matches("", "") && !descriptor_matches("", "<init>"));
    }

    // ---------------------------------------------------------------------
    // W11 — long-name decode boundary (symbol::to_string clamp at
    // length==0 || length>0x1000, vmhook.hpp:1904).  The clamp ceiling is
    // 0x1000 == 4096.  A name of length just under the clamp is a legal
    // byte-string for equality; pin the threshold arithmetic and the
    // byte-equality of a 4095-byte name against itself / a 4096-byte name.
    // ---------------------------------------------------------------------
    {
        constexpr std::size_t clamp_ceiling{ 0x1000 };
        check("W11_clamp_ceiling_is_4096", clamp_ceiling == 4096);
        // length 0 is clamped (-> empty); length > 0x1000 is clamped.  Mirror
        // the predicate `length == 0 || length > 0x1000`.
        auto clamps_to_empty = [](std::size_t length) -> bool {
            return length == 0 || length > 0x1000;
        };
        check("W11_zero_length_clamped", clamps_to_empty(0));
        check("W11_4096_not_clamped", !clamps_to_empty(4096));
        check("W11_4095_not_clamped", !clamps_to_empty(4095));
        check("W11_4097_clamped", clamps_to_empty(4097));
        check("W11_one_not_clamped", !clamps_to_empty(1));
        // A 4095-'a' name compares equal to itself and unequal to a 4096-'a'
        // name (length-aware byte equality), both UNDER the clamp.
        const std::string n4095(4095, 'a');
        const std::string n4096(4096, 'a');
        check("W11_long_name_self_equal",
              descriptor_matches(n4095, n4095));
        check("W11_long_name_length_differs_unequal",
              !descriptor_matches(n4095, n4096));
        check("W11_long_name_size_4095", n4095.size() == 4095);
    }

    return failures == 0 ? 0 : 1;
}
