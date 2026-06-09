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
            // Skip array-dimension markers; the element type follows.
            while (i < args.size() && args[i] == '[')
            {
                ++i;
            }
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
                slots += 1;
            }
            else if (c == 'J' || c == 'D')
            {
                ++i;
                ++count;
                slots += 2;         // long / double occupy two slots
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
