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

#include <cstddef>
#include <cstdio>
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

    return failures == 0 ? 0 : 1;
}
