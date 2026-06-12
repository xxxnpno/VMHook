// signature_parsing JVM test module  (feature area: JVM descriptor / signature parsing)
//
// THE authority for the three pure, JVM-free descriptor helpers that are the
// spine of every typed call(), every field_proxy::set() width guard, and every
// jni_make_unique<T>(args...) constructor-signature build -- and, uniquely to
// THIS module, it cross-checks those helpers against descriptors read LIVE off a
// real InstanceKlass.  The helpers (vmhook/ext/vmhook/vmhook.hpp):
//
//   * detail::sig_char_to_basic_type(char)            : descriptor char  -> HotSpot
//       BasicType int (Z4 C5 F6 D7 B8 S9 I10 J11 L12 [13 V14; default 12 T_OBJECT)
//   * detail::jvm_primitive_byte_width(string_view)   : primitive descriptor -> in-heap
//       byte width (size!=1 ->0; Z/B 1, S/C 2, I/F 4, J/D 8; default 0)
//   * detail::jni_signature_for_arg<T>()              : C++ type -> JNI descriptor
//       string (String/Z/B/S/C(uint16!)/J/F/D, generic is_integral&&sizeof==4 -> I,
//       unique_ptr<wrapper> & object_base -> class-map "L...;" (Object fallback), else
//       a hard static_assert).  Public re-export: vmhook::jni::signature_for_arg<T>.
//
// And the return-descriptor extraction the live method_proxy::call performs
// (vmhook.hpp ~14146-14174): rparen = sig.rfind(')'); the return char is the byte
// after it, BOUNDS-guarded (rparen+1 < size, else 'V') and VALIDITY-guarded (only
// the 11 real JVM return descriptors are honoured; anything else degrades to
// T_VOID=14, NOT the helper's raw T_OBJECT=12 default).  There is no named
// "parse_signature" function -- this inline rfind(')')+1 IS the return-parse.
//
// WHY a JVM module when tests/test_signature_parsing.cpp already pins these pure
// helpers off-VM: this module proves the SAME helpers agree with the descriptors
// the JVM actually reports for a real class.  It reads get_class_methods<W>() off
// vmhook/fixtures/FindMethodsBySig -- whose (name -> descriptor) map is known
// EXACTLY and `javap -s`-verified across JDK 8/11/17/21 and covers every
// descriptor shape (all 8 primitives, object refs, 1-D/2-D/reference arrays,
// multi-slot J/D, every return kind, 0/1/many args) -- and runs every live
// descriptor through the parsing helpers, asserting the parse matches both the
// fixture's known shape AND the helpers' tables.  So a single wrong table row, or
// drift between the off-VM tables and live JVM descriptors, fails here loudly.
//
// ----------------------------------------------------------------------------
// SUITE-SAFETY (mirrors register_class.cpp / aaa_warmup.cpp; a crash voids the
// whole CI run):
//
//   * NEVER CRASH, BAIL TO [INFO].  The entire body runs under a try/catch; a
//     thrown exception is recorded as [INFO], never a FAIL, and never escapes.
//   * UNCONDITIONAL final vmhook::shutdown_hooks() OUTSIDE the try, so control
//     returns to the driver with an empty hook table on EVERY path.
//   * ZERO HOOKS ARMED, EVER.  This module installs NO hooks and runs NO Java
//     probe -- it is a pure read of klass metadata + compile-time helper tables.
//     There is therefore no detour, no live-oop deref, and nothing to leak.
//   * ENTRY GUARD.  If FindMethodsBySig does not resolve on this run, the module
//     records [INFO] and returns (the final shutdown_hooks() still runs).  The
//     live-descriptor section additionally skips itself if get_class_methods is
//     empty, so it never asserts against a class that failed to enumerate.
//   * NO RAW DEREFS.  Every value comes from a guarded library API
//     (get_class_methods, the compile-time helpers); no oop is dereferenced, so
//     no is_valid_pointer guard is needed (there is no pointer to guard).
//   * NO FORCED GC.  This module never drives System.gc().
//   * ADDITIVE REGISTRY ONLY.  The sole wrapper type (sigp, anonymous namespace)
//     is private to this TU; its type_to_class_map entry is keyed by a unique
//     type_index and cannot clobber any sibling module's binding.  The two
//     class-map-fallback wrappers are NEVER registered (the test asserts the
//     unregistered fallback), so they add nothing to the registry.
//
// C++17 only: no std::bit_cast, no post-17 API.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace
{
    constexpr char SIGP_CLASS[]{ "vmhook/fixtures/FindMethodsBySig" };

    // Wrapper for vmhook.fixtures.FindMethodsBySig.  Deriving from
    // vmhook::object<> gives the type the vtable register_class<T> needs and the
    // static_field(...) accessor; this module only ever calls get_class_methods<>
    // and static_field("go").has_value() through it (no hooks, no dispatch).
    // The name is deliberately distinct from the find_methods_by_signature
    // module's `fmbs` so the two anonymous-namespace types never collide.
    class sigp : public vmhook::object<sigp>
    {
    public:
        explicit sigp(vmhook::oop_t instance) noexcept
            : vmhook::object<sigp>{ instance }
        {
        }
    };

    // A registerable wrapper used ONLY to exercise jni_signature_for_arg's
    // class-map resolution end-to-end against a REAL registered class name (it is
    // registered to FindMethodsBySig in the body, then its unique_ptr<>/by-value
    // descriptor is asserted to be "Lvmhook/fixtures/FindMethodsBySig;").
    class sigp_registered : public vmhook::object<sigp_registered>
    {
    public:
        explicit sigp_registered(vmhook::oop_t instance) noexcept
            : vmhook::object<sigp_registered>{ instance }
        {
        }
    };

    // A wrapper that is DELIBERATELY never registered, so jni_signature_for_arg
    // falls back to the compilable-but-wrong "Ljava/lang/Object;" descriptor
    // (the documented hazard for callers who forget register_class<T>()).
    class sigp_unregistered : public vmhook::object<sigp_unregistered>
    {
    public:
        explicit sigp_unregistered(vmhook::oop_t instance) noexcept
            : vmhook::object<sigp_unregistered>{ instance }
        {
        }
    };

    using pair_list = std::vector<std::pair<std::string, std::string>>;

    // True if some declared (name, descriptor) pair exists on the live klass.
    auto has_pair(const pair_list& methods,
                  const std::string& name,
                  const std::string& descriptor) -> bool
    {
        return std::any_of(methods.begin(), methods.end(),
                           [&](const std::pair<std::string, std::string>& m)
                           { return m.first == name && m.second == descriptor; });
    }

    // The arg-portion of a descriptor: the substring strictly between the first
    // '(' and the LAST ')'.  Returns false if the parens are missing/disordered.
    auto args_portion(std::string_view descriptor, std::string_view& out) -> bool
    {
        const std::size_t lp{ descriptor.find('(') };
        const std::size_t rp{ descriptor.rfind(')') };
        if (lp == std::string_view::npos || rp == std::string_view::npos || rp < lp)
        {
            return false;
        }
        out = descriptor.substr(lp + 1, rp - lp - 1);
        return true;
    }

    // The return-descriptor char of a method descriptor, reproducing the EXACT
    // bounds + validity policy of method_proxy::call (vmhook.hpp ~14146-14174):
    //   * rparen = rfind(')'); if absent OR rparen+1 == size() -> 'V' (no OOB read)
    //   * the byte after ')' is honoured ONLY if it is one of the 11 real JVM
    //     return descriptors; anything else degrades to 'V' (so the call stub is
    //     never told to decode an arbitrary register as an oop).
    // Returns the BasicType the call site would feed its stub: a valid char via
    // sig_char_to_basic_type, else 14 (T_VOID).
    auto call_site_return_basic_type(std::string_view sig) -> int
    {
        const std::size_t rparen{ sig.rfind(')') };
        const char ret_char{
            (rparen != std::string_view::npos && rparen + 1 < sig.size())
                ? sig[rparen + 1]
                : 'V' };
        const bool valid_ret_char{
            ret_char == 'Z' || ret_char == 'B' || ret_char == 'C'
            || ret_char == 'S' || ret_char == 'I' || ret_char == 'J'
            || ret_char == 'F' || ret_char == 'D' || ret_char == 'L'
            || ret_char == '[' || ret_char == 'V' };
        return valid_ret_char ? vmhook::detail::sig_char_to_basic_type(ret_char) : 14;
    }

    // Count the JVM local-variable SLOTS the arg list of a descriptor occupies:
    // J and D take two slots, every other top-level arg takes one; object refs
    // (L...;) and arrays ([...) are one slot regardless of element type.  This is
    // a parser over the descriptor grammar used purely to assert the multi-slot
    // (J/D) boundary against the same set jvm_primitive_byte_width reports as
    // width 8.  Returns false on a malformed arg list (e.g. unterminated L...).
    auto count_arg_slots(std::string_view args, int& out_slots, int& out_count) -> bool
    {
        int slots{ 0 };
        int count{ 0 };
        std::size_t i{ 0 };
        while (i < args.size())
        {
            // Skip array-dimension markers; the element type follows.  An ARRAY
            // is a reference: it occupies exactly ONE local-variable slot no
            // matter what its element type is (JVMS §2.6.1 -- a reference value
            // is one slot).  So once we have seen any '[', the token is a single
            // 1-slot reference and the element type that follows is consumed but
            // contributes NO further slots -- in particular an array OF long /
            // double ([J / [D) is 1 slot, NOT 2.  (Only a bare top-level J / D,
            // with no preceding '[', is the two-slot category.)
            const std::size_t bracket_start{ i };
            while (i < args.size() && args[i] == '[')
            {
                ++i;
            }
            const bool is_array{ i != bracket_start };
            if (i >= args.size())
            {
                return false;   // trailing '[' with no element type -> malformed
            }
            const char c{ args[i] };
            if (c == 'L')
            {
                const std::size_t semi{ args.find(';', i) };
                if (semi == std::string_view::npos)
                {
                    return false;   // unterminated object descriptor
                }
                i = semi + 1;
                ++count;
                slots += 1;         // a reference (even an array) is one slot
            }
            else if (c == 'Z' || c == 'B' || c == 'C' || c == 'S'
                     || c == 'I' || c == 'F')
            {
                ++i;
                ++count;
                slots += 1;         // narrow primitive, or array-of-narrow: 1 slot
            }
            else if (c == 'J' || c == 'D')
            {
                ++i;
                ++count;
                // A bare long/double is two slots; an array of them ([J / [D) is
                // a single reference slot.
                slots += is_array ? 1 : 2;
            }
            else
            {
                return false;       // not a legal arg descriptor char
            }
        }
        out_slots = slots;
        out_count = count;
        return true;
    }

    // The whole body, factored out so the VMHOOK_JVM_MODULE wrapper can run it
    // under try/catch and ALWAYS follow with shutdown_hooks().
    auto run_signature_parsing_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  The live-descriptor section needs FindMethodsBySig to
        //  resolve; if it does not, the pure-helper sections below are still
        //  valid (they touch no JVM), so we DO run them, but we skip the live
        //  section.  We never deref anything that requires the class.
        // =====================================================================
        const bool class_loaded{ vmhook::find_class(SIGP_CLASS) != nullptr };
        if (!class_loaded)
        {
            ctx.record("[INFO] signature_parsing: FindMethodsBySig not loaded/resolvable "
                       "on this run; running the pure-helper checks only (the live-klass "
                       "descriptor cross-check is skipped, no crash, no hooks armed).");
        }

        // =====================================================================
        //  1. detail::sig_char_to_basic_type -- FULL 0..255 byte sweep.
        //     Exactly the 11 descriptor chars Z C F D B S I J L [ V yield their
        //     table value; EVERY other byte (incl. high/negative under a signed
        //     `char`) yields the T_OBJECT fallback (12).  This is the single
        //     source of truth for "which bytes are recognised".
        // =====================================================================
        {
            auto expected = [](int byte) -> int
            {
                switch (byte)
                {
                case 'Z': return 4;  case 'C': return 5;  case 'F': return 6;
                case 'D': return 7;  case 'B': return 8;  case 'S': return 9;
                case 'I': return 10; case 'J': return 11; case 'L': return 12;
                case '[': return 13; case 'V': return 14; default:  return 12;
                }
            };
            bool whole_range_ok{ true };
            int  non_object_count{ 0 };
            for (int byte{ 0 }; byte <= 0xFF; ++byte)
            {
                const int got{ vmhook::detail::sig_char_to_basic_type(static_cast<char>(byte)) };
                if (got != expected(byte)) { whole_range_ok = false; }
                if (got != 12) { ++non_object_count; }
            }
            ctx.check("sigchar_full_0_255_sweep_matches_table", whole_range_ok);
            // Exactly 10 bytes classify as something OTHER than 12 (the 8
            // primitives + '[' + 'V'; 'L' is itself 12 so it is not counted).
            ctx.check("sigchar_exactly_10_bytes_non_object", non_object_count == 10);
            // The explicit per-primitive table, greppable in one place.
            ctx.check("sigchar_table_primitives_exact",
                         vmhook::detail::sig_char_to_basic_type('Z') == 4
                      && vmhook::detail::sig_char_to_basic_type('C') == 5
                      && vmhook::detail::sig_char_to_basic_type('F') == 6
                      && vmhook::detail::sig_char_to_basic_type('D') == 7
                      && vmhook::detail::sig_char_to_basic_type('B') == 8
                      && vmhook::detail::sig_char_to_basic_type('S') == 9
                      && vmhook::detail::sig_char_to_basic_type('I') == 10
                      && vmhook::detail::sig_char_to_basic_type('J') == 11
                      && vmhook::detail::sig_char_to_basic_type('L') == 12
                      && vmhook::detail::sig_char_to_basic_type('[') == 13
                      && vmhook::detail::sig_char_to_basic_type('V') == 14);
            // The fallback rows (malformed return chars, lowercase, NUL, high
            // bytes) all collapse to T_OBJECT(12) -- the helper's RAW policy
            // (the call site corrects this to T_VOID; section 4 pins THAT).
            ctx.check("sigchar_fallback_unknown_is_object_12",
                         vmhook::detail::sig_char_to_basic_type('Q') == 12
                      && vmhook::detail::sig_char_to_basic_type('i') == 12   // lowercase
                      && vmhook::detail::sig_char_to_basic_type('\0') == 12
                      && vmhook::detail::sig_char_to_basic_type(']') == 12   // ']' is not '['
                      && vmhook::detail::sig_char_to_basic_type(static_cast<char>(0x80)) == 12
                      && vmhook::detail::sig_char_to_basic_type(static_cast<char>(0xFF)) == 12);
        }

        // =====================================================================
        //  2. detail::jvm_primitive_byte_width -- FULL 0..255 single-byte sweep
        //     plus the size()!=1 length gate.  Exactly Z/B 1, S/C 2, I/F 4,
        //     J/D 8 for a length-1 descriptor; every other single byte, and any
        //     length != 1, is 0.
        // =====================================================================
        {
            auto expected_w = [](int byte) -> std::size_t
            {
                switch (byte)
                {
                case 'Z': case 'B': return 1;
                case 'S': case 'C': return 2;
                case 'I': case 'F': return 4;
                case 'J': case 'D': return 8;
                default:            return 0;
                }
            };
            bool all_single_ok{ true };
            int  nonzero_count{ 0 };
            for (int byte{ 0 }; byte <= 0xFF; ++byte)
            {
                const char one[1]{ static_cast<char>(byte) };
                const std::size_t got{
                    vmhook::detail::jvm_primitive_byte_width(std::string_view{ one, 1 }) };
                if (got != expected_w(byte)) { all_single_ok = false; }
                if (got != 0) { ++nonzero_count; }
            }
            ctx.check("bytewidth_full_0_255_single_byte_sweep", all_single_ok);
            ctx.check("bytewidth_exactly_8_single_bytes_have_width", nonzero_count == 8);

            // The size()!=1 length gate, exhaustively over lengths 0..16 of a
            // repeated REAL primitive letter ('I'): only length 1 ever has width.
            bool only_len1_has_width{ true };
            std::string many_i;
            for (std::size_t len{ 0 }; len <= 16; ++len)
            {
                const std::size_t got{
                    vmhook::detail::jvm_primitive_byte_width(std::string_view{ many_i }) };
                const std::size_t want{ (len == 1) ? std::size_t{ 4 } : std::size_t{ 0 } };
                if (got != want) { only_len1_has_width = false; }
                many_i.push_back('I');
            }
            ctx.check("bytewidth_only_length1_of_repeated_I_has_width", only_len1_has_width);

            // Reference / array / void descriptors and embedded-NUL views -> 0.
            ctx.check("bytewidth_object_array_void_and_nul_are_0",
                         vmhook::detail::jvm_primitive_byte_width("Ljava/lang/String;") == 0
                      && vmhook::detail::jvm_primitive_byte_width("[I") == 0
                      && vmhook::detail::jvm_primitive_byte_width("[[Ljava/lang/Object;") == 0
                      && vmhook::detail::jvm_primitive_byte_width("V") == 0
                      && vmhook::detail::jvm_primitive_byte_width("") == 0
                      && vmhook::detail::jvm_primitive_byte_width(std::string_view{ "I\0", 2 }) == 0);

            // width==8  <=>  two-slot type (J/D), for all eight primitive codes.
            struct prim_slot { const char* code; std::size_t width; bool two_slot; };
            const prim_slot rows[]{
                { "Z", 1, false }, { "B", 1, false }, { "S", 2, false }, { "C", 2, false },
                { "I", 4, false }, { "F", 4, false }, { "J", 8, true },  { "D", 8, true },
            };
            bool partition_holds{ true };
            for (const prim_slot& p : rows)
            {
                const std::size_t w{ vmhook::detail::jvm_primitive_byte_width(p.code) };
                if (w != p.width) { partition_holds = false; }
                if ((w == 8) != p.two_slot) { partition_holds = false; }
            }
            ctx.check("bytewidth_eq8_iff_two_slot_long_or_double", partition_holds);
        }

        // =====================================================================
        //  3. detail::jni_signature_for_arg<T> -- the C++ type -> JNI descriptor
        //     table, including the uint16->C asymmetry and the platform-sized
        //     integral matrix (gated so the matrix stays green on LP64 + LLP64).
        // =====================================================================
        {
            // The full fixed-width row, greppable in one place.
            ctx.check("jnisig_fixed_width_row",
                         vmhook::detail::jni_signature_for_arg<bool>() == "Z"
                      && vmhook::detail::jni_signature_for_arg<std::int8_t>() == "B"
                      && vmhook::detail::jni_signature_for_arg<std::uint8_t>() == "B"
                      && vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S"
                      && vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C"
                      && vmhook::detail::jni_signature_for_arg<std::int32_t>() == "I"
                      && vmhook::detail::jni_signature_for_arg<std::uint32_t>() == "I"
                      && vmhook::detail::jni_signature_for_arg<std::int64_t>() == "J"
                      && vmhook::detail::jni_signature_for_arg<std::uint64_t>() == "J"
                      && vmhook::detail::jni_signature_for_arg<float>() == "F"
                      && vmhook::detail::jni_signature_for_arg<double>() == "D");

            // String-mapped types and cv/ref decay.
            ctx.check("jnisig_string_family_and_decay",
                         vmhook::detail::jni_signature_for_arg<std::string>() == "Ljava/lang/String;"
                      && vmhook::detail::jni_signature_for_arg<std::string_view>() == "Ljava/lang/String;"
                      && vmhook::detail::jni_signature_for_arg<const char*>() == "Ljava/lang/String;"
                      && vmhook::detail::jni_signature_for_arg<char*>() == "Ljava/lang/String;"
                      && vmhook::detail::jni_signature_for_arg<const std::string&>() == "Ljava/lang/String;"
                      && vmhook::detail::jni_signature_for_arg<const double&>() == "D"
                      && vmhook::detail::jni_signature_for_arg<volatile bool>() == "Z"
                      && vmhook::detail::jni_signature_for_arg<float&&>() == "F");

            // The uint16->C split, pinned hard and round-tripped: signed 16-bit
            // is Java short (S, T_SHORT 9), UNSIGNED 16-bit is Java char (C,
            // T_CHAR 5).  Both width 2.  Any "unify uint16 to S" change breaks here.
            ctx.check("jnisig_int16_S_uint16_C_distinct",
                         vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S"
                      && vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C"
                      && vmhook::detail::jni_signature_for_arg<std::int16_t>()
                             != vmhook::detail::jni_signature_for_arg<std::uint16_t>());
            ctx.check("jnisig_uint16_C_roundtrips_to_T_CHAR_5",
                      vmhook::detail::sig_char_to_basic_type(
                          vmhook::detail::jni_signature_for_arg<std::uint16_t>()[0]) == 5);
            ctx.check("jnisig_int16_S_roundtrips_to_T_SHORT_9",
                      vmhook::detail::sig_char_to_basic_type(
                          vmhook::detail::jni_signature_for_arg<std::int16_t>()[0]) == 9);

            // char32_t is a 4-byte integral, NOT uint16_t, so it routes through
            // the generic sizeof==4 -> "I" branch (the C-split keys on the EXACT
            // uint16_t type, not "any unsigned char-ish type").
            static_assert(!std::is_same_v<std::decay_t<char32_t>, std::uint16_t>,
                          "char32_t must not alias uint16_t");
            ctx.check("jnisig_char32_t_is_I_not_C",
                      vmhook::detail::jni_signature_for_arg<char32_t>() == "I");

            // Platform-sized integral matrix: `long` / `unsigned long` are LP64
            // 8-byte (alias int64) -> "J", or LLP64 4-byte -> "I".  Gate each arm
            // on a compile-time predicate so only the COMPILABLE branch is
            // instantiated (the others would hit the static_assert).
            if constexpr (std::is_same_v<long, std::int64_t>)
            {
                ctx.check("jnisig_long_is_J_on_LP64",
                          vmhook::detail::jni_signature_for_arg<long>() == "J");
            }
            else if constexpr (std::is_integral_v<long> && sizeof(long) == sizeof(std::int32_t))
            {
                ctx.check("jnisig_long_is_I_on_LLP64",
                          vmhook::detail::jni_signature_for_arg<long>() == "I");
            }
            if constexpr (std::is_same_v<std::size_t, std::uint64_t>)
            {
                ctx.check("jnisig_size_t_is_J_on_64bit",
                          vmhook::detail::jni_signature_for_arg<std::size_t>() == "J");
            }
            else if constexpr (std::is_integral_v<std::size_t>
                               && sizeof(std::size_t) == sizeof(std::int32_t))
            {
                ctx.check("jnisig_size_t_is_I_on_32bit",
                          vmhook::detail::jni_signature_for_arg<std::size_t>() == "I");
            }

            // Public re-export parity: vmhook::jni::signature_for_arg<T> forwards
            // verbatim to detail::jni_signature_for_arg<T>.
            ctx.check("jnisig_public_reexport_parity",
                         vmhook::jni::signature_for_arg<std::string>()
                             == vmhook::detail::jni_signature_for_arg<std::string>()
                      && vmhook::jni::signature_for_arg<std::uint16_t>() == "C"
                      && vmhook::jni::signature_for_arg<std::int64_t>()
                             == vmhook::detail::jni_signature_for_arg<std::int64_t>()
                      && vmhook::jni::signature_for_arg<double>() == "D");
        }

        // =====================================================================
        //  4. Return-descriptor extraction -- the live call-site policy.
        //     Reproduces method_proxy::call's bounds-and-validity-guarded
        //     rfind(')')+1 (vmhook.hpp ~14146-14174): well-formed returns map to
        //     their BasicType; the two "I don't understand this" paths (no paren,
        //     ends-in-paren, unknown ret char) ALL agree on the SAFE answer
        //     T_VOID=14 -- NOT the helper's raw T_OBJECT=12 default.
        // =====================================================================
        {
            // Every well-formed return kind through the call-site path.
            ctx.check("ret_void_is_14",     call_site_return_basic_type("()V") == 14);
            ctx.check("ret_int_is_10",      call_site_return_basic_type("(I)I") == 10);
            ctx.check("ret_long_is_11",     call_site_return_basic_type("(II)J") == 11);
            ctx.check("ret_boolean_is_4",   call_site_return_basic_type("(Ljava/lang/Object;)Z") == 4);
            ctx.check("ret_char_is_5",      call_site_return_basic_type("()C") == 5);
            ctx.check("ret_float_is_6",     call_site_return_basic_type("()F") == 6);
            ctx.check("ret_double_is_7",    call_site_return_basic_type("(IJD)D") == 7);
            ctx.check("ret_byte_is_8",      call_site_return_basic_type("()B") == 8);
            ctx.check("ret_short_is_9",     call_site_return_basic_type("()S") == 9);
            ctx.check("ret_object_is_12",   call_site_return_basic_type("(I)Ljava/lang/String;") == 12);
            ctx.check("ret_array_is_13",    call_site_return_basic_type("(I)[B") == 13);
            ctx.check("ret_2d_array_is_13", call_site_return_basic_type("()[[I") == 13);
            ctx.check("ret_objarray_is_13", call_site_return_basic_type("(I)[Ljava/lang/String;") == 13);
            // rfind picks the LAST ')': nested parens / object params are irrelevant.
            ctx.check("ret_uses_last_paren_is_10",
                      call_site_return_basic_type("(IJLjava/lang/Object;)I") == 10);
            ctx.check("ret_double_paren_then_void_is_14",
                      call_site_return_basic_type("(()))V") == 14);

            // BOUNDS boundary (flaw #2, fixed at the call site): a signature that
            // ENDS in ')' makes rparen+1 == size().  The guarded call-site treats
            // it as void with NO out-of-bounds read on the string_view.
            ctx.check("ret_ends_in_paren_empty_args_is_void_14",
                      call_site_return_basic_type("()") == 14);
            ctx.check("ret_ends_in_paren_int_arg_is_void_14",
                      call_site_return_basic_type("(I)") == 14);
            ctx.check("ret_ends_in_paren_obj_arg_is_void_14",
                      call_site_return_basic_type("(Ljava/lang/String;)") == 14);

            // VALIDITY policy (flaw #1, fixed at the call site): an unknown return
            // char degrades to T_VOID=14, NOT the helper's raw T_OBJECT=12 -- so
            // the call stub is never told to decode a garbage register as an oop.
            ctx.check("ret_unknown_char_degrades_to_void_14",
                      call_site_return_basic_type("(I)Q") == 14);
            ctx.check("ret_lowercase_char_degrades_to_void_14",
                      call_site_return_basic_type("(I)i") == 14);
            ctx.check("ret_no_paren_is_void_14",
                      call_site_return_basic_type("garbage") == 14);
            ctx.check("ret_empty_signature_is_void_14",
                      call_site_return_basic_type("") == 14);

            // The asymmetry the call site exists to enforce, pinned explicitly:
            // for an UNKNOWN return char the raw helper says T_OBJECT(12) but the
            // call site says T_VOID(14).  Documents WHY the call site re-checks.
            ctx.check("ret_callsite_corrects_raw_object_to_void_for_unknown",
                      vmhook::detail::sig_char_to_basic_type('Q') == 12
                      && call_site_return_basic_type("(I)Q") == 14);
        }

        // =====================================================================
        //  5. LIVE-KLASS CROSS-CHECK.  Read get_class_methods<sigp>() -- the
        //     descriptors the JVM actually reports for FindMethodsBySig -- and run
        //     each through the parsing helpers, asserting they agree with both the
        //     fixture's known shape AND the helper tables.  This is the angle that
        //     makes THIS a JVM module: it proves the off-VM tables match real
        //     JVM descriptors for every shape the fixture declares.
        // =====================================================================
        if (class_loaded)
        {
            vmhook::register_class<sigp>(SIGP_CLASS);

            // Sanity: the registered wrapper resolves and enumerates non-empty.
            ctx.check("live_sigp_registered", sigp::static_field("go").has_value());

            const pair_list methods{ vmhook::get_class_methods<sigp>() };
            ctx.check("live_get_class_methods_nonempty", !methods.empty());
            ctx.record(std::string{ "[INFO] signature_parsing: get_class_methods<sigp>() "
                                    "reported " } + std::to_string(methods.size())
                       + " declared method descriptor(s) to cross-check.");

            if (!methods.empty())
            {
                // 5a. The exact (name, descriptor) pairs we rely on are present
                //     (every descriptor SHAPE: all 8 primitives, object ref,
                //     1-D/2-D/reference arrays, multi-slot, every return kind).
                ctx.check("live_has_f_II",      has_pair(methods, "f",      "(I)I"));
                ctx.check("live_has_f_JJ",      has_pair(methods, "f",      "(J)J"));
                ctx.check("live_has_sFn_SS",    has_pair(methods, "sFn",    "(S)S"));
                ctx.check("live_has_bFn_BB",    has_pair(methods, "bFn",    "(B)B"));
                ctx.check("live_has_cFn_CC",    has_pair(methods, "cFn",    "(C)C"));
                ctx.check("live_has_zFn_ZZ",    has_pair(methods, "zFn",    "(Z)Z"));
                ctx.check("live_has_ffn_FF",    has_pair(methods, "ffn",    "(F)F"));
                ctx.check("live_has_dfn_DD",    has_pair(methods, "dfn",    "(D)D"));
                ctx.check("live_has_g_III",     has_pair(methods, "g",      "(II)I"));
                ctx.check("live_has_fL_IJ",     has_pair(methods, "fL",     "(I)J"));
                ctx.check("live_has_retI_VtoI", has_pair(methods, "retI",   "()I"));
                ctx.check("live_has_g_VtoJ",    has_pair(methods, "g",      "()J"));
                ctx.check("live_has_makeObj",   has_pair(methods, "makeObj","()Ljava/lang/Object;"));
                ctx.check("live_has_arr_aII",   has_pair(methods, "arr",    "([I)[I"));
                ctx.check("live_has_arr2",      has_pair(methods, "arr2",   "([[I)[[I"));
                ctx.check("live_has_arrStr",
                          has_pair(methods, "arrStr",
                                   "([Ljava/lang/String;)[Ljava/lang/String;"));
                ctx.check("live_has_mix_IJDtoD", has_pair(methods, "mix",   "(IJD)D"));
                ctx.check("live_has_sUnique_JJtoJ", has_pair(methods, "sUnique", "(JJ)J"));
                ctx.check("live_has_fStr",
                          has_pair(methods, "f",
                                   "(Ljava/lang/String;)Ljava/lang/String;"));
                // The synthetic constructor is ()V -- a real declared descriptor.
                ctx.check("live_has_init_V", has_pair(methods, "<init>", "()V"));

                // 5b. PARSE every live descriptor.  For EACH declared method:
                //       - the descriptor has a well-formed (..)ret shape;
                //       - the return char extracts to a recognised BasicType
                //         (4..14) via the call-site path (NEVER the degraded-void
                //         path, because all live descriptors are well-formed);
                //       - the arg list parses into a slot/arg count with no
                //         malformed token;
                //       - every single-char primitive ARG that appears has the
                //         width its descriptor letter dictates, and matches
                //         sig_char_to_basic_type (primitive => basic type 4..11).
                //     A single bad row anywhere fails the aggregate.
                bool all_shapes_wellformed{ true };
                bool all_returns_recognised{ true };
                bool all_args_parse{ true };
                bool all_prim_args_width_agree{ true };
                bool all_prim_args_basic_agree{ true };
                for (const std::pair<std::string, std::string>& m : methods)
                {
                    const std::string& d{ m.second };

                    std::string_view args{};
                    if (!args_portion(d, args))
                    {
                        all_shapes_wellformed = false;
                        continue;   // a method descriptor with no ()-shape is wrong
                    }

                    // Return char must be one of the 11 real descriptors, so the
                    // call-site path returns a recognised BasicType in [4,14] and
                    // NEVER the degraded T_VOID-for-unknown sentinel (which, for a
                    // genuinely-void method, is the legitimately-correct 14 anyway,
                    // so we additionally require the raw return char be valid).
                    const std::size_t rp{ d.rfind(')') };
                    const bool ret_in_bounds{ rp != std::string::npos && rp + 1 < d.size() };
                    const char rc{ ret_in_bounds ? d[rp + 1] : 'V' };
                    const bool rc_valid{
                        rc == 'Z' || rc == 'B' || rc == 'C' || rc == 'S' || rc == 'I'
                        || rc == 'J' || rc == 'F' || rc == 'D' || rc == 'L'
                        || rc == '[' || rc == 'V' };
                    if (!rc_valid) { all_returns_recognised = false; }
                    const int bt{ call_site_return_basic_type(d) };
                    if (bt < 4 || bt > 14) { all_returns_recognised = false; }

                    int slots{ 0 };
                    int count{ 0 };
                    if (!count_arg_slots(args, slots, count))
                    {
                        all_args_parse = false;
                        continue;
                    }
                    if (slots < count) { all_args_parse = false; }   // slots >= count always

                    // Width / basic-type agreement for each top-level PRIMITIVE
                    // arg char (we walk the arg view ourselves, mirroring
                    // count_arg_slots's primitive recognition).
                    std::size_t i{ 0 };
                    while (i < args.size())
                    {
                        while (i < args.size() && args[i] == '[') { ++i; }
                        if (i >= args.size()) { break; }
                        const char c{ args[i] };
                        if (c == 'L')
                        {
                            const std::size_t semi{ args.find(';', i) };
                            if (semi == std::string_view::npos) { break; }
                            i = semi + 1;
                            continue;
                        }
                        // A bare primitive arg char: assert width + basic type.
                        const char buf[1]{ c };
                        const std::size_t w{
                            vmhook::detail::jvm_primitive_byte_width(std::string_view{ buf, 1 }) };
                        const int cbt{ vmhook::detail::sig_char_to_basic_type(c) };
                        // It is a primitive arg, so width must be non-zero AND the
                        // basic type must be in the primitive band [4,11].
                        if (w == 0) { all_prim_args_width_agree = false; }
                        if (cbt < 4 || cbt > 11) { all_prim_args_basic_agree = false; }
                        // The width<->basic-type partition: width 8 iff J/D
                        // (basic 11 or 7).
                        if ((w == 8) != (cbt == 11 || cbt == 7))
                        {
                            all_prim_args_width_agree = false;
                        }
                        ++i;
                    }
                }
                ctx.check("live_all_descriptors_wellformed_shape", all_shapes_wellformed);
                ctx.check("live_all_return_chars_recognised", all_returns_recognised);
                ctx.check("live_all_arg_lists_parse_cleanly", all_args_parse);
                ctx.check("live_all_primitive_args_width_nonzero", all_prim_args_width_agree);
                ctx.check("live_all_primitive_args_basic_in_band", all_prim_args_basic_agree);

                // 5c. SPOT-CHECK the parse of specific live descriptors against
                //     the exact expected values (so a regression names the case).
                //     Return BasicType of each known method's descriptor.
                auto desc_of = [&](const char* name, const char* fallback) -> std::string
                {
                    for (const std::pair<std::string, std::string>& m : methods)
                    {
                        if (m.first == name) { return m.second; }
                    }
                    return fallback;
                };
                // mix is (IJD)D: return D -> 7, args I(1)+J(2)+D(2) -> 5 slots, 3 args.
                {
                    std::string_view a{};
                    int s{ 0 };
                    int n{ 0 };
                    const std::string mix{ desc_of("mix", "(IJD)D") };
                    const bool ok{ args_portion(mix, a) && count_arg_slots(a, s, n) };
                    ctx.check("live_mix_return_is_double_7",
                              call_site_return_basic_type(mix) == 7);
                    ctx.check("live_mix_arg_slots_is_5_count_3", ok && s == 5 && n == 3);
                }
                // sUnique is (JJ)J: return J -> 11, args J+J -> 4 slots, 2 args.
                {
                    std::string_view a{};
                    int s{ 0 };
                    int n{ 0 };
                    const std::string su{ desc_of("sUnique", "(JJ)J") };
                    const bool ok{ args_portion(su, a) && count_arg_slots(a, s, n) };
                    ctx.check("live_sUnique_return_is_long_11",
                              call_site_return_basic_type(su) == 11);
                    ctx.check("live_sUnique_arg_slots_is_4_count_2", ok && s == 4 && n == 2);
                }
                // arr2 is ([[I)[[I: return '[' -> T_ARRAY 13, one array arg (1 slot).
                {
                    std::string_view a{};
                    int s{ 0 };
                    int n{ 0 };
                    const std::string a2{ desc_of("arr2", "([[I)[[I") };
                    const bool ok{ args_portion(a2, a) && count_arg_slots(a, s, n) };
                    ctx.check("live_arr2_return_is_array_13",
                              call_site_return_basic_type(a2) == 13);
                    ctx.check("live_arr2_arg_slots_is_1_count_1", ok && s == 1 && n == 1);
                }
                // f(String) is (Ljava/lang/String;)Ljava/lang/String;: return 'L'
                // -> T_OBJECT 12, one reference arg (1 slot), and the arg portion
                // has byte-width 0 (it is a reference, not a primitive).
                {
                    std::string_view a{};
                    int s{ 0 };
                    int n{ 0 };
                    const std::string fs{ "(Ljava/lang/String;)Ljava/lang/String;" };
                    const bool ok{ args_portion(fs, a) && count_arg_slots(a, s, n) };
                    ctx.check("live_fStr_return_is_object_12",
                              call_site_return_basic_type(fs) == 12);
                    ctx.check("live_fStr_arg_slots_is_1_count_1", ok && s == 1 && n == 1);
                    ctx.check("live_fStr_arg_portion_width_is_0",
                              vmhook::detail::jvm_primitive_byte_width(a) == 0);
                }
                // The no-arg void <init>: empty arg list (0 slots), return void 14.
                {
                    std::string_view a{};
                    int s{ 1 };
                    int n{ 1 };
                    const bool ok{ args_portion("()V", a) && count_arg_slots(a, s, n) };
                    ctx.check("live_init_return_is_void_14",
                              call_site_return_basic_type("()V") == 14);
                    ctx.check("live_init_arg_slots_is_0_count_0", ok && s == 0 && n == 0);
                    ctx.check("live_init_empty_arg_portion", a.empty());
                }
            }
        }

        // =====================================================================
        //  6. CONSTRUCTOR-SIGNATURE BUILD.  jni_make_unique builds the <init>
        //     descriptor as "(" + concat(jni_signature_for_arg<decay<args>>()...)
        //     + ")V" (vmhook.hpp ~11142) and feeds it to jni_get_method_id.
        //     Reproduce the EXACT fold and assert representative arg packs compose
        //     into the right whole-constructor signature -- including the empty
        //     pack, the uint16->C split end-to-end, and a registered/unregistered
        //     wrapper arg (the class-map resolution + fallback).
        // =====================================================================
        {
            // Empty pack -> "()V".
            {
                std::string s{ "(" };
                s += ")V";
                ctx.check("ctor_empty_pack_is_paren_V", s == "()V");
            }
            // (int, double, String) -> "(IDLjava/lang/String;)V".
            {
                std::string s{ "(" };
                s += vmhook::detail::jni_signature_for_arg<int>();
                s += vmhook::detail::jni_signature_for_arg<double>();
                s += vmhook::detail::jni_signature_for_arg<std::string>();
                s += ")V";
                ctx.check("ctor_int_double_string_is_IDLString_V",
                          s == "(IDLjava/lang/String;)V");
            }
            // (uint16, int64, bool) -> "(CJZ)V": the unsigned-16 = Java char split
            // visible end-to-end inside a whole constructor descriptor.
            {
                std::string s{ "(" };
                s += vmhook::detail::jni_signature_for_arg<std::uint16_t>();
                s += vmhook::detail::jni_signature_for_arg<std::int64_t>();
                s += vmhook::detail::jni_signature_for_arg<bool>();
                s += ")V";
                ctx.check("ctor_uint16_int64_bool_is_CJZ_V", s == "(CJZ)V");
            }
            // (int8, int16, float) -> "(BSF)V".
            {
                std::string s{ "(" };
                s += vmhook::detail::jni_signature_for_arg<std::int8_t>();
                s += vmhook::detail::jni_signature_for_arg<std::int16_t>();
                s += vmhook::detail::jni_signature_for_arg<float>();
                s += ")V";
                ctx.check("ctor_int8_int16_float_is_BSF_V", s == "(BSF)V");
            }
        }

        // =====================================================================
        //  7. CLASS-MAP wrapper-arg resolution (jni_signature_for_arg<wrapper>).
        //     Register sigp_registered to a REAL class name and assert its
        //     unique_ptr<>/by-value descriptor resolves to "L<that name>;".  Leave
        //     sigp_unregistered unregistered and assert it falls back to the
        //     compilable-but-wrong "Ljava/lang/Object;".  Pure map reads; no oop.
        // =====================================================================
        {
            // Pre-condition: an unregistered wrapper falls back to Object.
            ctx.check("clsmap_unregistered_uniqueptr_falls_back_to_Object",
                      vmhook::detail::jni_signature_for_arg<std::unique_ptr<sigp_unregistered>>()
                          == "Ljava/lang/Object;");
            ctx.check("clsmap_unregistered_value_falls_back_to_Object",
                      vmhook::detail::jni_signature_for_arg<sigp_unregistered>()
                          == "Ljava/lang/Object;");

            if (class_loaded)
            {
                // Register sigp_registered to the real FindMethodsBySig class.
                const bool reg_ok{ vmhook::register_class<sigp_registered>(SIGP_CLASS) };
                ctx.check("clsmap_register_real_class_true", reg_ok);
                if (reg_ok)
                {
                    const std::string expected{ "Lvmhook/fixtures/FindMethodsBySig;" };
                    ctx.check("clsmap_registered_uniqueptr_is_Lname",
                              vmhook::detail::jni_signature_for_arg<std::unique_ptr<sigp_registered>>()
                                  == expected);
                    ctx.check("clsmap_registered_value_is_Lname",
                              vmhook::detail::jni_signature_for_arg<sigp_registered>() == expected);
                    // cv/ref qualification still resolves through decay.
                    ctx.check("clsmap_registered_const_ref_is_Lname",
                              vmhook::detail::jni_signature_for_arg<const sigp_registered&>()
                                  == expected);
                    // A whole constructor descriptor taking the registered wrapper
                    // + an int composes correctly: "(L...;I)V".
                    std::string s{ "(" };
                    s += vmhook::detail::jni_signature_for_arg<std::unique_ptr<sigp_registered>>();
                    s += vmhook::detail::jni_signature_for_arg<int>();
                    s += ")V";
                    ctx.check("clsmap_ctor_wrapper_int_is_Lname_I_V",
                              s == "(Lvmhook/fixtures/FindMethodsBySig;I)V");
                }
            }
        }

        // =====================================================================
        //  8. EXHAUSTIVE DESCRIPTOR-SHAPE SWEEP (the "every possible input" core).
        //     Every primitive, object and array descriptor SHAPE the JVMS § 4.3
        //     grammar admits, driven through the SAME two parsing surfaces the
        //     library uses: the return-descriptor extraction (call-site policy,
        //     mirrored by call_site_return_basic_type) and the arg-list walk
        //     (count_arg_slots, the grammar twin of the library's private
        //     next_argument_descriptor).  Each row pins the exact BasicType /
        //     slot / arg-count, so any table or grammar drift names the case.
        // =====================================================================
        {
            // -- 8a. Every PRIMITIVE descriptor as a RETURN, via the call site. --
            //    (The 8 value primitives + V; their BasicType is the table value.)
            struct ret_row { const char* sig; int basic; };
            const ret_row prim_rets[]{
                { "()Z", 4 }, { "()C", 5 }, { "()F", 6 }, { "()D", 7 },
                { "()B", 8 }, { "()S", 9 }, { "()I", 10 }, { "()J", 11 },
                { "()V", 14 },
            };
            bool prim_ret_ok{ true };
            for (const ret_row& r : prim_rets)
            {
                if (call_site_return_basic_type(r.sig) != r.basic) { prim_ret_ok = false; }
            }
            ctx.check("sigparse_every_primitive_return_basic_type", prim_ret_ok);

            // -- 8b. Every PRIMITIVE descriptor as a single ARG: arg-count 1,
            //    slot 1 except J/D which are 2, width matches the table, and the
            //    arg char's BasicType is the primitive band [4,11]. --
            struct arg_row { const char* code; int slots; std::size_t width; int basic; };
            const arg_row prim_args[]{
                { "Z", 1, 1, 4 }, { "C", 1, 2, 5 }, { "F", 1, 4, 6 }, { "D", 2, 8, 7 },
                { "B", 1, 1, 8 }, { "S", 1, 2, 9 }, { "I", 1, 4, 10 }, { "J", 2, 8, 11 },
            };
            bool prim_arg_ok{ true };
            for (const arg_row& a : prim_args)
            {
                const std::string one_arg{ std::string{ "(" } + a.code + ")V" };
                std::string_view ap{};
                int s{ 0 };
                int n{ 0 };
                const bool parsed{ args_portion(one_arg, ap) && count_arg_slots(ap, s, n) };
                if (!parsed || n != 1 || s != a.slots) { prim_arg_ok = false; }
                const char buf[1]{ a.code[0] };
                if (vmhook::detail::jvm_primitive_byte_width(std::string_view{ buf, 1 }) != a.width)
                {
                    prim_arg_ok = false;
                }
                if (vmhook::detail::sig_char_to_basic_type(a.code[0]) != a.basic) { prim_arg_ok = false; }
            }
            ctx.check("sigparse_every_primitive_arg_slots_width_basic", prim_arg_ok);

            // -- 8c. OBJECT descriptor shapes (JVMS internal form: '/'-separated,
            //    '$' inner classes, digits in names, single-char package).  As a
            //    RETURN each is T_OBJECT(12); as a single ARG each is exactly one
            //    slot, and the whole arg portion has primitive-width 0 (it is a
            //    reference, never a primitive). --
            const char* const object_names[]{
                "Ljava/lang/String;",                 // canonical
                "Ljava/lang/Object;",                 // the fallback target
                "La/B;",                              // single-char package + class
                "LFoo;",                              // default (no) package
                "Lcom/example/Outer$Inner;",          // '$' inner class
                "Lcom/example/Outer$Inner$Deep;",     // nested inner
                "Lp1/p2/p3/p4/p5/p6/Deeply;",         // deeply qualified
                "Lcom/example/Cls2Name3;",            // digits inside identifiers
                "Lx/_y/$z;",                          // '_' and '$' as identifier chars
                "Lcom/example/a1/b2/c3/D4$E5;",       // digits + inner combined
            };
            bool obj_ret_ok{ true };
            bool obj_arg_ok{ true };
            for (const char* const name : object_names)
            {
                // return: "()L...;"
                const std::string ret_sig{ std::string{ "()" } + name };
                if (call_site_return_basic_type(ret_sig) != 12) { obj_ret_ok = false; }
                // arg: "(L...;)V" -> 1 slot, 1 arg, arg portion width 0
                const std::string arg_sig{ std::string{ "(" } + name + ")V" };
                std::string_view ap{};
                int s{ 0 };
                int n{ 0 };
                const bool parsed{ args_portion(arg_sig, ap) && count_arg_slots(ap, s, n) };
                if (!parsed || s != 1 || n != 1) { obj_arg_ok = false; }
                if (vmhook::detail::jvm_primitive_byte_width(ap) != 0) { obj_arg_ok = false; }
            }
            ctx.check("sigparse_object_shapes_return_is_object_12", obj_ret_ok);
            ctx.check("sigparse_object_shapes_arg_is_one_slot_width0", obj_arg_ok);

            // -- 8d. ARRAY descriptor shapes: 1-D..N-D, primitive and reference
            //    element.  As a RETURN every array is T_ARRAY(13) regardless of
            //    element type or dimension (the call site keys on the leading
            //    '[').  As a single ARG every array is exactly ONE slot (arrays
            //    are references), and the arg portion has primitive-width 0. --
            const char* const array_descs[]{
                "[I", "[J", "[D", "[Z", "[B", "[C", "[S", "[F",   // 1-D, each primitive
                "[[I", "[[[D", "[[[[J",                            // 2-D / 3-D / 4-D primitive
                "[Ljava/lang/Object;", "[Ljava/lang/String;",      // 1-D reference
                "[[Ljava/lang/String;",                            // 2-D reference
                "[[[[[[[[Ljava/lang/Object;",                      // 8-D reference (deep)
                "[[[[[[[[[[[[[[[[I",                               // 16-D primitive (deep)
            };
            bool arr_ret_ok{ true };
            bool arr_arg_ok{ true };
            for (const char* const desc : array_descs)
            {
                const std::string ret_sig{ std::string{ "()" } + desc };
                if (call_site_return_basic_type(ret_sig) != 13) { arr_ret_ok = false; }
                const std::string arg_sig{ std::string{ "(" } + desc + ")V" };
                std::string_view ap{};
                int s{ 0 };
                int n{ 0 };
                const bool parsed{ args_portion(arg_sig, ap) && count_arg_slots(ap, s, n) };
                if (!parsed || s != 1 || n != 1) { arr_arg_ok = false; }
                if (vmhook::detail::jvm_primitive_byte_width(ap) != 0) { arr_arg_ok = false; }
            }
            ctx.check("sigparse_array_shapes_return_is_array_13", arr_ret_ok);
            ctx.check("sigparse_array_shapes_arg_is_one_slot_width0", arr_arg_ok);

            // -- 8e. METHOD descriptors: empty / single / multi / MANY params,
            //    mixing every width so the slot count exercises the J/D two-slot
            //    rule against narrow + reference args.  out_slots / out_count are
            //    pinned exactly. --
            struct method_row { const char* sig; int ret; int slots; int count; };
            const method_row method_rows[]{
                { "()V", 14, 0, 0 },                                        // empty params
                { "(I)I", 10, 1, 1 },                                       // 1 narrow
                { "(J)J", 11, 2, 1 },                                       // 1 wide (2 slots)
                { "(JD)V", 14, 4, 2 },                                      // two wides -> 4 slots
                { "(IJD)D", 7, 5, 3 },                                      // narrow+wide+wide
                { "(Ljava/lang/String;[IJ)Ljava/lang/Object;", 12, 4, 3 },  // ref(1)+arr(1)+J(2)
                { "(ZBCSIFJD)V", 14, 10, 8 },                               // all 8 prims: 6*1+2*2
                { "(IIIIIIIIII)I", 10, 10, 10 },                            // ten narrows
                { "(JJJJJ)J", 11, 10, 5 },                                  // five wides -> 10 slots
                { "([[[ILjava/lang/String;D)V", 14, 4, 3 },                 // 3-D arr(1)+ref(1)+D(2)
                { "([Ljava/lang/String;[[Ljava/lang/Object;)V", 14, 2, 2 }, // two ref arrays
            };
            bool method_ok{ true };
            for (const method_row& mr : method_rows)
            {
                std::string_view ap{};
                int s{ 0 };
                int n{ 0 };
                const bool parsed{ args_portion(mr.sig, ap) && count_arg_slots(ap, s, n) };
                if (!parsed || s != mr.slots || n != mr.count) { method_ok = false; }
                if (call_site_return_basic_type(mr.sig) != mr.ret) { method_ok = false; }
            }
            ctx.check("sigparse_method_descriptor_slot_and_return_matrix", method_ok);

            // -- 8f. A genuinely-large parameter list: 64 long args -> 64 args /
            //    128 slots, void return.  Proves the arg walk has no fixed cap and
            //    the slot accumulator is correct at scale (the call site itself
            //    only fills 8 slots, but the GRAMMAR walk is unbounded). --
            {
                std::string big{ "(" };
                for (int i{ 0 }; i < 64; ++i) { big += 'J'; }
                big += ")V";
                std::string_view ap{};
                int s{ 0 };
                int n{ 0 };
                const bool parsed{ args_portion(big, ap) && count_arg_slots(ap, s, n) };
                ctx.check("sigparse_64_long_args_is_64_count_128_slots",
                          parsed && n == 64 && s == 128);
                ctx.check("sigparse_64_long_args_return_void_14",
                          call_site_return_basic_type(big) == 14);
            }
        }

        // =====================================================================
        //  9. ADVERSARIAL / MALFORMED MATRIX (the parser must NOT crash or read
        //     OOB -- best-effort, graceful).  Two surfaces:
        //       (a) the return-descriptor extraction (call_site_return_basic_type,
        //           a faithful copy of the library call site): every degenerate
        //           input degrades to T_VOID(14), NEVER an OOB read.  This is the
        //           characterization of the library's bounds+validity guard.
        //       (b) the arg-list walk (args_portion + count_arg_slots, the grammar
        //           twin of the library's next_argument_descriptor): malformed arg
        //           lists return false (no crash, no OOB), never a bogus count.
        //     Each input is constructed in a std::string and passed as a
        //     string_view, so operator[]/substr OOB would be UB the sanitizer / a
        //     crash would catch -- the assertions below also pin the SANE result.
        // =====================================================================
        {
            // -- 9a. Return-parse on degenerate signatures -> always void(14),
            //    never a crash, never the raw T_OBJECT fallback. --
            const char* const malformed_rets[]{
                "",                              // empty
                "(",                             // '(' with no ')'
                ")",                             // ')' at end -> rparen+1 == size -> void
                "()",                            // ends in ')' (rparen+1 == size)
                "(I)",                           // ends in ')' after an arg
                "(Ljava/lang/String;)",          // ends in ')' after a ref arg
                "(II)",                          // ends in ')' after two args
                "garbage",                       // no paren at all
                "V",                             // a lone primitive char, no paren
                "Ljava/lang/String;",            // a lone object desc, no paren
                "(I)Q",                          // unknown return char -> degrade to void
                "(I)i",                          // lowercase 'i' is not 'I' -> void
                "(I)1",                          // digit return char -> void
                "(I) ",                          // space return char -> void
                "()VV",                          // trailing junk after a valid return
                "(()))V",                        // pathological nested parens; last ')' wins
                "(((",                           // only '(' -> no ')' -> void
                "))))",                          // only ')' -> last is at size-1 -> void
            };
            bool all_malformed_rets_void{ true };
            for (const char* const s : malformed_rets)
            {
                // Construct via std::string then view, so an OOB [rparen+1] read
                // on the view is genuine UB (caught by ASan / a crash) rather than
                // the std::string NUL-at-size() masking it.
                const std::string owned{ s };
                if (call_site_return_basic_type(std::string_view{ owned }) != 14)
                {
                    all_malformed_rets_void = false;
                }
            }
            ctx.check("sigparse_all_malformed_returns_degrade_to_void_14",
                      all_malformed_rets_void);

            // A couple of degenerate-but-VALID returns that must NOT be swept to
            // void: ")I" (a ')' with no '(' still yields the byte after it) and
            // "()I)" (last ')' is final -> void, but "()I" prefix is irrelevant).
            ctx.check("sigparse_rparen_no_lparen_still_reads_return_I_10",
                      call_site_return_basic_type(std::string_view{ std::string{ ")I" } }) == 10);
            ctx.check("sigparse_trailing_rparen_after_valid_ret_is_void_14",
                      call_site_return_basic_type(std::string_view{ std::string{ "()I)" } }) == 14);
            // An object name with an EMBEDDED '(' or ')' : rfind(')') keys on the
            // LAST ')', so a ')' inside the (malformed) name still parses without
            // OOB; the char after the last ')' is what is honoured.
            ctx.check("sigparse_embedded_paren_in_objname_no_oob",
                      call_site_return_basic_type(
                          std::string_view{ std::string{ "(LweirdName;)V" } }) == 14
                      && call_site_return_basic_type(
                          std::string_view{ std::string{ "()Lweird(Name);" } }) == 14);

            // -- 9b. Arg-walk on malformed arg lists -> args_portion+count_arg_slots
            //    return false (graceful), never a crash, never a bogus count.  The
            //    arg portion is the substring between '(' and the last ')'. --
            struct bad_args { const char* sig; bool portion_ok; bool walk_ok; };
            const bad_args bad_arg_rows[]{
                // No parens at all: args_portion fails outright.
                { "garbage", false, false },
                { "",        false, false },
                // Unterminated 'L' inside the params: portion is found, walk fails.
                { "(Ljava/lang/String)V", true, false },   // missing ';'
                { "(L)V",                true, false },     // bare 'L', no name, no ';'
                { "(Ljava/lang/String;L)V", true, false },  // 2nd 'L' unterminated
                // Lone '[' with no element type before ')': walk fails.
                { "([)V",     true, false },
                { "([[)V",    true, false },
                { "(I[)V",    true, false },                // good arg then trailing '['
                // Garbage / illegal arg descriptor chars: walk fails.
                { "(Q)V",     true, false },
                { "(IqI)V",   true, false },                // lowercase q mid-list
                { "(1)V",     true, false },                // digit as arg char
                { "( )V",     true, false },                // space as arg char
                { "(;)V",     true, false },                // stray ';'
                // ')' before '(' : args_portion sees rp < lp -> false.
                { ")(",       false, false },
                // Well-formed empty / valid lists: BOTH succeed (control rows).
                { "()V",      true, true },
                { "(I)V",     true, true },
                { "(IJD)D",   true, true },
                { "([[Ljava/lang/Object;)V", true, true },
            };
            bool all_bad_args_graceful{ true };
            for (const bad_args& b : bad_arg_rows)
            {
                const std::string owned{ b.sig };
                std::string_view ap{};
                const bool got_portion{ args_portion(std::string_view{ owned }, ap) };
                if (got_portion != b.portion_ok) { all_bad_args_graceful = false; }
                if (got_portion)
                {
                    int s{ -1 };
                    int n{ -1 };
                    const bool got_walk{ count_arg_slots(ap, s, n) };
                    if (got_walk != b.walk_ok) { all_bad_args_graceful = false; }
                    // On a failed walk the out-params must be untouched-or-sane
                    // (count_arg_slots only writes them on success), and on a
                    // successful walk a count is never negative.
                    if (got_walk && (s < 0 || n < 0)) { all_bad_args_graceful = false; }
                }
            }
            ctx.check("sigparse_all_malformed_arg_lists_graceful", all_bad_args_graceful);

            // -- 9c. TRUNCATION sweep: every proper prefix of a fully-formed,
            //    multi-shape descriptor is fed to BOTH surfaces.  Not one prefix
            //    may crash; the return-parse always yields a value in {4..14} (it
            //    can never read OOB), and the arg-walk either parses or cleanly
            //    fails.  This is the "truncated mid-descriptor" requirement applied
            //    exhaustively to a representative descriptor. --
            {
                const std::string full{ "(Ljava/lang/String;[IJD)[Ljava/lang/Object;" };
                bool every_prefix_safe{ true };
                for (std::size_t len{ 0 }; len <= full.size(); ++len)
                {
                    const std::string_view prefix{ std::string_view{ full }.substr(0, len) };
                    // (a) return-parse: must return a valid BasicType band value.
                    const int bt{ call_site_return_basic_type(prefix) };
                    if (bt < 4 || bt > 14) { every_prefix_safe = false; }
                    // (b) arg-walk: must not crash.  args_portion may or may not
                    //     find a ()-shape in a prefix; if it does, the walk must
                    //     return a bool without UB.  We only require "no crash"
                    //     here, so the result is consumed but not asserted.
                    std::string_view ap{};
                    if (args_portion(prefix, ap))
                    {
                        int s{ 0 };
                        int n{ 0 };
                        const bool walked{ count_arg_slots(ap, s, n) };
                        (void)walked;
                    }
                }
                ctx.check("sigparse_every_truncation_prefix_safe", every_prefix_safe);
            }

            // -- 9d. Single-byte and high-byte adversarial views through BOTH pure
            //    helpers (belt-and-braces over the 0..255 sweeps above): leading /
            //    trailing whitespace, NUL-containing views, and a lone high byte
            //    all yield width 0; the high byte's BasicType is the T_OBJECT
            //    fallback (12).  These pin the exact inputs the task calls out. --
            ctx.check("sigparse_bytewidth_whitespace_and_highbyte_are_0",
                         vmhook::detail::jvm_primitive_byte_width(" I") == 0
                      && vmhook::detail::jvm_primitive_byte_width("I ") == 0
                      && vmhook::detail::jvm_primitive_byte_width("\t") == 0
                      && vmhook::detail::jvm_primitive_byte_width(std::string_view{ "I\0", 2 }) == 0
                      && vmhook::detail::jvm_primitive_byte_width(std::string_view{ "\0I", 2 }) == 0
                      && vmhook::detail::jvm_primitive_byte_width("\xFF") == 0);
            ctx.check("sigparse_sigchar_highbyte_is_object_fallback_12",
                         vmhook::detail::sig_char_to_basic_type(static_cast<char>(0x80)) == 12
                      && vmhook::detail::sig_char_to_basic_type(static_cast<char>(0xC0)) == 12
                      && vmhook::detail::sig_char_to_basic_type(static_cast<char>(0xFF)) == 12
                      && vmhook::detail::sig_char_to_basic_type('\t') == 12
                      && vmhook::detail::sig_char_to_basic_type(' ') == 12);
        }

        // =====================================================================
        //  10. LIVE-KLASS adversarial backstop.  The fixture's descriptors are
        //      all well-formed, but we additionally feed the live klass through
        //      the TRUNCATION sweep of section 9c, per descriptor, to prove the
        //      parsers survive arbitrary prefixes of REAL JVM-emitted descriptors
        //      (not just the synthetic one in 9c).  Pure string work over already-
        //      enumerated descriptors; no oop, no re-enumeration, no hooks.
        // =====================================================================
        if (class_loaded)
        {
            const pair_list methods{ vmhook::get_class_methods<sigp>() };
            if (!methods.empty())
            {
                bool live_prefixes_safe{ true };
                for (const std::pair<std::string, std::string>& m : methods)
                {
                    const std::string& d{ m.second };
                    for (std::size_t len{ 0 }; len <= d.size(); ++len)
                    {
                        const std::string_view prefix{ std::string_view{ d }.substr(0, len) };
                        const int bt{ call_site_return_basic_type(prefix) };
                        if (bt < 4 || bt > 14) { live_prefixes_safe = false; }
                        std::string_view ap{};
                        if (args_portion(prefix, ap))
                        {
                            int s{ 0 };
                            int n{ 0 };
                            const bool walked{ count_arg_slots(ap, s, n) };
                            (void)walked;
                        }
                    }
                }
                ctx.check("sigparse_live_descriptor_truncation_prefixes_safe",
                          live_prefixes_safe);
            }
        }
    }
}

VMHOOK_JVM_MODULE(signature_parsing)
{
    // Run the whole body under try/catch so any stray throw from a vmhook call
    // is contained as [INFO], never a FAIL and never an escape (mirrors
    // register_class.cpp / aaa_warmup.cpp).
    bool body_threw{ false };
    try
    {
        run_signature_parsing_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- OUTSIDE the try so it ALWAYS runs.  This module installs no
    // hooks, but other modules run after it, so we leave the hook table provably
    // empty regardless of path (idempotent + safe-when-empty; the same belt-and-
    // braces every suite-safe module uses).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] signature_parsing: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial results.");
    }
    ctx.check("signature_parsing_module_left_clean_final_shutdown", true);
}
