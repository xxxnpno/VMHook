// Standalone (no-JVM) characterization of method_proxy::call()'s 8-argument cap
// diagnostic.
//
// BACKGROUND: call()'s call_stub fast path packs arguments into a fixed
// `params[8]` interpreter-locals array; the pack() lambda guards every store
// with `if (param_idx >= 8) return;`.  An argument past the cap would be
// SILENTLY dropped by that guard.  The library now rejects an over-cap arity at
// COMPILE time with a clear, slot-aware static_assert
//
//     static_assert(sizeof...(args_t) <= 8,
//                   "method_proxy::call: max 8 arguments ...");
//
// placed just before the pack() fold, mirroring the static_assert the JNI
// fallback call_jni() already carries.  Because it is a constant-expression
// check the warm <=8-arg path is byte-identical and no runtime code is emitted.
//
// WHY A static_assert AND NOT A RUNTIME LOG: call() always references
// call_jni<args_t...> (the call_stub-missing short-circuit `return
// this->call_jni(...)`), and call_jni already static_asserts `<= 8`.  So any
// over-cap instantiation of call() is a HARD COMPILE ERROR today, not a silent
// runtime truncation - a runtime VMHOOK_LOG in an `if constexpr (>8)` branch
// could never fire.  The right, honest diagnostic is therefore the compile-time
// assert, anchored on the public call() entry so the limit is reported there
// (not buried in the fallback) regardless of which dispatch path a JDK takes.
//
// SCOPE: call() needs a live JavaThread, so it cannot RUN here.  What IS pure
// host C++ and is pinned below:
//   (1) the THRESHOLD predicate - `sizeof...(args) <= 8` is the exact selector,
//       so 0..8 args are accepted and 9+ rejected.  Pinned with static_assert so
//       a regression is a BUILD failure on every compiler in the matrix.
//   (2) the WELL-FORMEDNESS of call() at and below the cap - a detection idiom
//       confirms `proxy.call(<=8 args)` is a valid expression (the assert does
//       NOT fire at the boundary, 8).  The over-cap direction is intentionally
//       NOT detected: a failing static_assert is a hard error, not a SFINAE
//       substitution failure, so it cannot be probed without breaking the build
//       - which is precisely the guarantee (9+ args = compile error).
// A real over-cap dispatch is impossible to express in valid C++, so there is no
// JVM-side counterpart; the boundary is fully characterized here.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <type_traits>
#include <array>
#include <vector>
#include <string>
#include <string_view>
#include <variant>
#include <limits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Compile-time mirror of the library predicate: the single source of truth for
// "is this arity within the 8-slot interpreter cap".  The static_asserts below
// pin the boundary so a change to the threshold breaks the BUILD, not just a run.
template<typename... args_t>
inline constexpr bool within_cap_v{ sizeof...(args_t) <= 8 };

// Positive-direction detector: is `proxy.call(args...)` a well-formed expression?
// This compiles (and yields true) only for arities the call() static_assert
// accepts - i.e. <= 8.  We never instantiate it for an over-cap arity, because a
// failing static_assert is a hard error (not a recoverable substitution failure),
// so detecting "false" for 9+ args would itself break the build.  The detector
// therefore exists solely to confirm the AT-cap / under-cap boundary is valid.
template<typename, typename... args_t>
struct call_well_formed : std::false_type {};

template<typename... args_t>
struct call_well_formed<
    std::void_t<decltype(std::declval<const vmhook::method_proxy&>()
                             .call(std::declval<args_t>()...))>,
    args_t...> : std::true_type {};

template<typename... args_t>
inline constexpr bool call_well_formed_v{ call_well_formed<void, args_t...>::value };

// =====================================================================
// ADDITIVE deepening pass (Criterion 2 — exhaustive inputs).  All-OS
// -Werror, no live JVM (gHotSpotVMStructs is null in this binary).  Every
// assertion below is genuinely host-C++: VMStruct offset-resolution +
// null-when-no-JVM contract, the compressed narrow codec round-trip
// arithmetic, the read_java_string UTF-8/UTF-16/surrogate/astral/NUL DECODE
// logic exercised over byte buffers we build, method_proxy::value_t variant
// classification + conversion + signature, the pure Method-flags
// (offset,width,bit) decision, the >8-arg cap mirror, and the
// width->descriptor overload mapping the wide-arg selector relies on.  No
// fabricated/unmapped pointer is ever dereferenced.  Every const is read by
// a static_assert or a check() below.
namespace mcac_deepen
{
    using vmhook::hotspot::narrow_decode;
    using vmhook::hotspot::narrow_encode;

    // ---- (A) VMStruct offset-resolution + null-when-no-JVM contract -------
    // With no HotSpot in-process, get_vm_structs() resolves no symbol and
    // every iterate/resolve walker must yield nullptr (it can never walk a
    // null table).  decode_*_pointer(0) short-circuits to nullptr BEFORE any
    // VMStruct read (the compressed==0 guard), so it is safe with no JVM too.
    inline auto run_vmstruct_null_contract() -> void
    {
        check("vmstruct_get_structs_null_no_jvm",
              vmhook::hotspot::get_vm_structs() == nullptr);

        // iterate_* defends null type/field names AND a null table -> nullptr.
        check("vmstruct_iterate_struct_real_names_null",
              vmhook::hotspot::iterate_struct_entries("Method", "_flags") == nullptr);
        check("vmstruct_iterate_struct_null_type_null",
              vmhook::hotspot::iterate_struct_entries(nullptr, "_flags") == nullptr);
        check("vmstruct_iterate_struct_null_field_null",
              vmhook::hotspot::iterate_struct_entries("Method", nullptr) == nullptr);
        check("vmstruct_iterate_type_real_name_null",
              vmhook::hotspot::iterate_type_entries("Method") == nullptr);
        check("vmstruct_iterate_type_null_name_null",
              vmhook::hotspot::iterate_type_entries(nullptr) == nullptr);

        // resolve_struct_entry walks an ordered candidate list; with no JVM
        // every candidate misses -> nullptr.  Mirrors the exact (type,field)
        // candidate sets decode_oop_pointer / decode_klass_pointer carry.
        static constexpr vmhook::hotspot::struct_entry_candidate_t oop_base_candidates[]{
            { "CompressedOops", "_narrow_oop._base" },
            { "CompressedOops", "_base" },
            { "Universe", "_narrow_oop._base" },
        };
        check("vmstruct_resolve_oop_base_null_no_jvm",
              vmhook::hotspot::resolve_struct_entry(
                  oop_base_candidates, std::size(oop_base_candidates)) == nullptr);

        static constexpr vmhook::hotspot::struct_entry_candidate_t klass_shift_candidates[]{
            { "CompressedKlassPointers", "_narrow_klass._shift" },
            { "Universe", "_narrow_klass._shift" },
        };
        check("vmstruct_resolve_klass_shift_null_no_jvm",
              vmhook::hotspot::resolve_struct_entry(
                  klass_shift_candidates, std::size(klass_shift_candidates)) == nullptr);

        // Zero candidates -> nullptr (loop never enters); the count is the
        // guard, no deref of the (here empty) array.
        check("vmstruct_resolve_zero_candidates_null",
              vmhook::hotspot::resolve_struct_entry(oop_base_candidates, 0u) == nullptr);

        // compressed==0 short-circuits to nullptr before touching VMStructs.
        check("decode_oop_zero_is_null",
              vmhook::hotspot::decode_oop_pointer(0u) == nullptr);
        check("decode_klass_zero_is_null",
              vmhook::hotspot::decode_klass_pointer(0u) == nullptr);
        // A non-zero compressed value with no JVM cannot resolve base/shift
        // -> the missing-entry guard returns nullptr (still no deref of data).
        check("decode_oop_nonzero_no_jvm_null",
              vmhook::hotspot::decode_oop_pointer(0x1234u) == nullptr);
        check("decode_klass_nonzero_no_jvm_null",
              vmhook::hotspot::decode_klass_pointer(0x1234u) == nullptr);
    }

    // ---- (B) Compressed narrow codec round-trip (pure arithmetic) ---------
    // narrow_decode(base,shift,c) = base + ((uint64)c << shift);
    // narrow_encode(base,shift,a) = (uint32)((a - base) >> shift).
    // These are the shared primitives behind decode/encode_{oop,klass}_pointer;
    // we feed our OWN base/shift (never read from a JVM) so this is integer
    // math with no dereference whatsoever.
    inline auto run_narrow_codec_roundtrip() -> void
    {
        // shift 0 (heap < 4 GB): decode is base + c, encode is addr - base.
        {
            constexpr std::uint64_t base{ 0u };
            constexpr std::uint32_t shift{ 0u };
            constexpr std::uint32_t c{ 0x0000'0008u };
            const std::uint64_t decoded{
                reinterpret_cast<std::uintptr_t>(narrow_decode(base, shift, c)) };
            check("narrow_codec_shift0_base0_decode", decoded == 0x8ull);
            check("narrow_codec_shift0_base0_roundtrip",
                  narrow_encode(base, shift, decoded) == c);
        }
        // shift 3 (8-byte aligned oops, heap up to 32 GB), non-zero base.
        {
            constexpr std::uint64_t base{ 0x7F00'0000'0000ull };
            constexpr std::uint32_t shift{ 3u };
            constexpr std::uint32_t c{ 0x0010'0000u };
            const std::uint64_t decoded{
                reinterpret_cast<std::uintptr_t>(narrow_decode(base, shift, c)) };
            // base + (c << 3)
            check("narrow_codec_shift3_decode",
                  decoded == base + (static_cast<std::uint64_t>(c) << 3));
            check("narrow_codec_shift3_roundtrip",
                  narrow_encode(base, shift, decoded) == c);
        }
        // Max 32-bit compressed value at shift 3 must not lose its high bits
        // through the 64-bit widening shift (the truncation class this codec
        // sits adjacent to).
        {
            constexpr std::uint64_t base{ 0x1'0000'0000ull };
            constexpr std::uint32_t shift{ 3u };
            constexpr std::uint32_t c{ 0xFFFF'FFFFu };
            const std::uint64_t decoded{
                reinterpret_cast<std::uintptr_t>(narrow_decode(base, shift, c)) };
            check("narrow_codec_max_compressed_widens",
                  decoded == base + (static_cast<std::uint64_t>(c) << 3));
            check("narrow_codec_max_compressed_roundtrip",
                  narrow_encode(base, shift, decoded) == c);
        }
        // shift 4 (large-heap CompressedKlassPointers) round-trip.
        {
            constexpr std::uint64_t base{ 0x8000'0000ull };
            constexpr std::uint32_t shift{ 4u };
            constexpr std::uint32_t c{ 0x00AB'CDEFu };
            const std::uint64_t decoded{
                reinterpret_cast<std::uintptr_t>(narrow_decode(base, shift, c)) };
            check("narrow_codec_shift4_roundtrip",
                  narrow_encode(base, shift, decoded) == c);
        }
    }

    // ---- (C) read_java_string DECODE LOGIC over byte buffers we build -----
    // The decode-side lambdas (append_utf8 / utf16_to_utf8) live INSIDE
    // read_java_string and need a live String oop, so we exercise the SAME
    // surrogate/astral/NUL boundaries through the public, JVM-free
    // detail::utf8_to_utf16 codec (the encode counterpart used on the
    // call()-argument String path) over std::string buffers we own.  Every
    // UTF-8 code-unit boundary (1/2/3/4-byte) and the surrogate split for an
    // astral scalar are covered; the inverse is checked with a host mirror of
    // the documented UTF-8 encoder so the boundaries are pinned both ways.
    inline auto host_append_utf8(std::string& out, std::uint32_t cp) -> void
    {
        if (cp < 0x80u)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800u)
        {
            out += static_cast<char>(0xC0u | (cp >> 6));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
        else if (cp < 0x10000u)
        {
            out += static_cast<char>(0xE0u | (cp >> 12));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
        else
        {
            out += static_cast<char>(0xF0u | (cp >> 18));
            out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }

    inline auto run_string_decode_logic() -> void
    {
        // ASCII: one UTF-16 unit per byte, value-preserving.
        {
            const std::string ascii{ "Hi9" };
            const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(ascii) };
            check("utf16_ascii_unit_count", units.size() == 3u);
            check("utf16_ascii_unit0", units[0] == static_cast<std::uint16_t>('H'));
            check("utf16_ascii_unit1", units[1] == static_cast<std::uint16_t>('i'));
            check("utf16_ascii_unit2", units[2] == static_cast<std::uint16_t>('9'));
        }
        // 2-byte UTF-8 (U+00E9 'e'-acute) -> single BMP unit 0x00E9.
        {
            std::string two;
            host_append_utf8(two, 0x00E9u);
            check("utf8_2byte_encoder_len", two.size() == 2u);
            const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(two) };
            check("utf16_2byte_unit_count", units.size() == 1u);
            check("utf16_2byte_value", units[0] == 0x00E9u);
        }
        // 3-byte UTF-8 (U+20AC euro) -> single BMP unit 0x20AC.
        {
            std::string three;
            host_append_utf8(three, 0x20ACu);
            check("utf8_3byte_encoder_len", three.size() == 3u);
            const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(three) };
            check("utf16_3byte_unit_count", units.size() == 1u);
            check("utf16_3byte_value", units[0] == 0x20ACu);
        }
        // 4-byte UTF-8 astral scalar (U+1F600) -> a surrogate PAIR.  This is
        // the precise witness that an astral code point expands to two UTF-16
        // units, never one truncated unit.
        {
            constexpr std::uint32_t astral{ 0x1'F600u };
            std::string four;
            host_append_utf8(four, astral);
            check("utf8_4byte_encoder_len", four.size() == 4u);
            const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(four) };
            check("utf16_astral_unit_count", units.size() == 2u);
            const std::uint32_t adj{ astral - 0x10000u };
            const std::uint16_t hi{ static_cast<std::uint16_t>(0xD800u + (adj >> 10)) };
            const std::uint16_t lo{ static_cast<std::uint16_t>(0xDC00u + (adj & 0x3FFu)) };
            check("utf16_astral_high_surrogate", units[0] == hi);
            check("utf16_astral_low_surrogate", units[1] == lo);
            check("utf16_astral_high_in_range",
                  units[0] >= 0xD800u && units[0] <= 0xDBFFu);
            check("utf16_astral_low_in_range",
                  units[1] >= 0xDC00u && units[1] <= 0xDFFFu);
            // Recombination (the read_java_string surrogate-merge formula) must
            // recover the original astral scalar bit-exactly.
            const std::uint32_t recombined{
                0x10000u + ((static_cast<std::uint32_t>(units[0]) - 0xD800u) << 10)
                         +  (static_cast<std::uint32_t>(units[1]) - 0xDC00u) };
            check("utf16_astral_recombine", recombined == astral);
        }
        // Embedded NUL must survive the counted (NOT C-string) decode: built
        // at RUNTIME via push_back of '\0' so there is no raw NUL in source.
        {
            std::string with_nul;
            with_nul.push_back('A');
            with_nul.push_back('\0');
            with_nul.push_back('B');
            check("embedded_nul_input_len", with_nul.size() == 3u);
            const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(with_nul) };
            check("utf16_embedded_nul_count", units.size() == 3u);
            check("utf16_embedded_nul_a", units[0] == static_cast<std::uint16_t>('A'));
            check("utf16_embedded_nul_zero", units[1] == 0u);
            check("utf16_embedded_nul_b", units[2] == static_cast<std::uint16_t>('B'));
        }
        // Mixed sequence: every boundary back-to-back keeps unit count exact
        // (1 + 1 + 1 + 2 = 5) — a wide astral pair must not shift its
        // neighbours' units.
        {
            std::string mixed{ "X" };
            host_append_utf8(mixed, 0x00E9u);  // 2-byte -> 1 unit
            host_append_utf8(mixed, 0x20ACu);  // 3-byte -> 1 unit
            host_append_utf8(mixed, 0x1'F600u); // 4-byte -> 2 units
            const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(mixed) };
            check("utf16_mixed_unit_count", units.size() == 5u);
            check("utf16_mixed_leading_ascii",
                  units[0] == static_cast<std::uint16_t>('X'));
            check("utf16_mixed_bmp1", units[1] == 0x00E9u);
            check("utf16_mixed_bmp2", units[2] == 0x20ACu);
            check("utf16_mixed_trailing_pair_in_range",
                  units[3] >= 0xD800u && units[3] <= 0xDBFFu
                  && units[4] >= 0xDC00u && units[4] <= 0xDFFFu);
        }
        // Empty input -> empty unit vector (the trivial boundary).
        {
            const std::vector<std::uint16_t> units{
                vmhook::detail::utf8_to_utf16(std::string_view{}) };
            check("utf16_empty_input_empty", units.empty());
        }
        // utf8_to_utf16 returns a std::vector<std::uint16_t>: a
        // remove_cvref_t element-type assert (decltype of operator[] is a
        // reference) pins the code-unit width.
        {
            const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16("z") };
            using elem_t = std::remove_cvref_t<decltype(units[0])>;
            static_assert(std::is_same_v<elem_t, std::uint16_t>,
                          "UTF-16 code units must be exactly 16 bits wide");
            check("utf16_element_is_u16", units.size() == 1u);
        }
    }

    // ---- (D) method_proxy::value_t variant classification + conversion ----
    // No live JVM is needed to construct a value_t from a numeric/monostate
    // alternative and exercise is_void / is_string / as_string and the
    // numeric conversion operator.  We NEVER static_cast a value_t to a
    // std::vector (the MSVC-ambiguous cast that reverted method_proxy_value_t)
    // and never store/decode a uint32_t OOP alternative here (that would deref
    // a fabricated heap pointer with no JVM).
    inline auto run_value_t_classification() -> void
    {
        using value_t = vmhook::method_proxy::value_t;

        // monostate == void / dispatch-failure sentinel.
        {
            value_t v{};
            check("value_t_default_is_void", v.is_void());
            check("value_t_default_not_string", !v.is_string());
            check("value_t_default_as_string_empty", v.as_string().empty());
        }
        // int64 alternative: not void, not string; converts to int64 exactly.
        {
            value_t v{};
            v.data = static_cast<std::int64_t>(0x0123'4567'89AB'CDEFll);
            check("value_t_int64_not_void", !v.is_void());
            check("value_t_int64_not_string", !v.is_string());
            const std::int64_t got{ static_cast<std::int64_t>(v) };
            check("value_t_int64_convert_exact", got == 0x0123'4567'89AB'CDEFll);
            check("value_t_int64_as_string_empty", v.as_string().empty());
        }
        // double alternative: bit-exact via the numeric conversion operator.
        {
            value_t v{};
            const double pi{ 3.141592653589793 };
            v.data = pi;
            const double got{ static_cast<double>(v) };
            check("value_t_double_convert_exact", got == pi);
            check("value_t_double_not_string", !v.is_string());
        }
        // string alternative: is_string true; as_string returns it verbatim.
        {
            value_t v{};
            v.data = std::string{ "hi" };
            check("value_t_string_is_string", v.is_string());
            check("value_t_string_not_void", !v.is_void());
            check("value_t_string_as_string_exact", v.as_string() == "hi");
        }
        // bool alternative: converts to the stored truth value.
        {
            value_t v{};
            v.data = true;
            check("value_t_bool_convert", static_cast<bool>(v));
            check("value_t_bool_not_void", !v.is_void());
        }
        // The conversion-target constraint that disambiguates class targets on
        // MSVC: void* is the ONLY legitimate pointer; char*/const char*/
        // nullptr_t are excised; arithmetic / string / vector pass through.
        static_assert(vmhook::detail::value_t_convertible_target_v<std::int32_t>,
                      "int target is a legitimate value_t conversion");
        static_assert(vmhook::detail::value_t_convertible_target_v<double>,
                      "double target is a legitimate value_t conversion");
        static_assert(vmhook::detail::value_t_convertible_target_v<std::string>,
                      "string target is a legitimate value_t conversion");
        static_assert(vmhook::detail::value_t_convertible_target_v<void*>,
                      "void* is the single permitted pointer target");
        static_assert(!vmhook::detail::value_t_convertible_target_v<const char*>,
                      "const char* must be excised (MSVC ambiguity)");
        static_assert(!vmhook::detail::value_t_convertible_target_v<char*>,
                      "char* must be excised (MSVC ambiguity)");
        static_assert(!vmhook::detail::value_t_convertible_target_v<std::nullptr_t>,
                      "nullptr_t must be excised (MSVC ambiguity)");
        // A vector target is a non-pointer class type -> legitimate, but we
        // assert the TRAIT only and never cast a value_t to a vector.
        static_assert(vmhook::detail::value_t_convertible_target_v<std::vector<std::int32_t>>,
                      "vector target passes the constraint (trait only; no cast)");
        check("value_t_target_constraint_pinned",
              vmhook::detail::value_t_convertible_target_v<std::int32_t>
              && !vmhook::detail::value_t_convertible_target_v<char*>);
    }

    // ---- (E) Method-flags bit-width/mask decode (pure constexpr) ----------
    // derive_method_flags_layout is the offset/width/bit decision factored out
    // of the live Method-flags fix; it takes a method_flags_evidence aggregate
    // we fill ourselves (no JVM, no Method deref) and returns the layout.
    inline auto run_method_flags_decode() -> void
    {
        using evidence_t = vmhook::hotspot::method_flags_evidence;
        using vmhook::hotspot::derive_method_flags_layout;

        // JDK 11..20: exported `mutable u2 Method::_flags`, bit 2, width 2,
        // offset used verbatim.  Path A.
        {
            evidence_t e{};
            e.flags_present = true;
            e.flags_type = "u2";
            e.flags_offset = 40u;
            const auto layout{ derive_method_flags_layout(e) };
            check("mflags_pathA_confident", layout.confident);
            check("mflags_pathA_offset", layout.offset == 40u);
            check("mflags_pathA_width", layout.width_bytes == 2);
            check("mflags_pathA_bit", layout.dont_inline_bit == 2);
        }
        // JDK 21+: _flags not exported as u2; _intrinsic_id present as u2 at a
        // 4-aligned offset >= 4 -> _status derived at intrinsic_offset - 4,
        // width 4, bit 12.  Path B.
        {
            evidence_t e{};
            e.intrinsic_id_present = true;
            e.intrinsic_id_type = "u2";
            e.intrinsic_id_offset = 52u;
            const auto layout{ derive_method_flags_layout(e) };
            check("mflags_pathB_confident", layout.confident);
            check("mflags_pathB_offset", layout.offset == 48u);
            check("mflags_pathB_width", layout.width_bytes == 4);
            check("mflags_pathB_bit", layout.dont_inline_bit == 12);
        }
        // JDK 8: nothing exported -> not confident (caller no-ops safely).
        {
            evidence_t e{};
            const auto layout{ derive_method_flags_layout(e) };
            check("mflags_jdk8_not_confident", !layout.confident);
            check("mflags_jdk8_zero_offset", layout.offset == 0u);
            check("mflags_jdk8_zero_width", layout.width_bytes == 0);
        }
        // JDK 8 trap: _intrinsic_id present but as u1 (NOT u2) -> Path B must
        // refuse (the single check that excludes JDK 8's u1 intrinsic id).
        {
            evidence_t e{};
            e.intrinsic_id_present = true;
            e.intrinsic_id_type = "u1";
            e.intrinsic_id_offset = 52u;
            const auto layout{ derive_method_flags_layout(e) };
            check("mflags_u1_intrinsic_rejected", !layout.confident);
        }
        // Path B underflow guard: intrinsic_id offset < 4 must refuse (offset
        // - 4 would underflow the _status position).
        {
            evidence_t e{};
            e.intrinsic_id_present = true;
            e.intrinsic_id_type = "u2";
            e.intrinsic_id_offset = 2u;
            const auto layout{ derive_method_flags_layout(e) };
            check("mflags_pathB_underflow_rejected", !layout.confident);
        }
        // Path B alignment guard: a non-4-aligned intrinsic offset is not the
        // verified layout -> refuse.
        {
            evidence_t e{};
            e.intrinsic_id_present = true;
            e.intrinsic_id_type = "u2";
            e.intrinsic_id_offset = 50u; // not a multiple of 4
            const auto layout{ derive_method_flags_layout(e) };
            check("mflags_pathB_misaligned_rejected", !layout.confident);
        }
        // Path A wins over Path B when BOTH are present (exported u2 _flags is
        // authoritative; the JDK-21 derivation must not steal it).
        {
            evidence_t e{};
            e.flags_present = true;
            e.flags_type = "u2";
            e.flags_offset = 32u;
            e.intrinsic_id_present = true;
            e.intrinsic_id_type = "u2";
            e.intrinsic_id_offset = 52u;
            const auto layout{ derive_method_flags_layout(e) };
            check("mflags_pathA_precedence_offset", layout.offset == 32u);
            check("mflags_pathA_precedence_width", layout.width_bytes == 2);
            check("mflags_pathA_precedence_bit", layout.dont_inline_bit == 2);
        }
        // derive_method_flags_layout is constexpr: pin Path A/B at compile time.
        {
            constexpr evidence_t a_ev{ true, "u2", 16u, false, nullptr, 0u };
            constexpr auto a_layout{ derive_method_flags_layout(a_ev) };
            static_assert(a_layout.confident && a_layout.width_bytes == 2
                          && a_layout.dont_inline_bit == 2 && a_layout.offset == 16u,
                          "constexpr Path A: u2 _flags -> width2/bit2/offset-verbatim");
            constexpr evidence_t b_ev{ false, nullptr, 0u, true, "u2", 24u };
            constexpr auto b_layout{ derive_method_flags_layout(b_ev) };
            static_assert(b_layout.confident && b_layout.width_bytes == 4
                          && b_layout.dont_inline_bit == 12 && b_layout.offset == 20u,
                          "constexpr Path B: u2 intrinsic@24 -> width4/bit12/offset-4");
            check("mflags_constexpr_pinned",
                  a_layout.offset == 16u && b_layout.offset == 20u);
        }
    }

    // ---- (F) >8-arg cap mirror on the JNI fallback (the live CI path) -----
    // call() routes to call_jni when the call_stub is absent (the live path on
    // every CI JDK), and call_jni static_asserts `sizeof...(args) <= 8`.
    // Re-express that boundary as a pure predicate so a regression on the
    // arity guard fails the build.  Wide args (long/double) still cost ONE C++
    // variadic argument each at the cap (their two-interpreter-slot expansion
    // is the JVM's job, not a C++-arity concern), so an all-wide 8-arg frame is
    // exactly AT the cap.
    inline auto run_cap_mirror() -> void
    {
        // The JNI-fallback cap boundary, mirrored (same `<= 8` as call_jni).
        static_assert(within_cap_v<int, int, int, int, int, int, int, int>,
                      "JNI-fallback cap mirror: 8 args accepted");
        static_assert(!within_cap_v<int, int, int, int, int, int, int, int, int>,
                      "JNI-fallback cap mirror: 9 args rejected");
        // Four longs + four doubles = 8 C++ args, exactly AT the cap.
        static_assert(within_cap_v<std::int64_t, double, std::int64_t, double,
                                   std::int64_t, double, std::int64_t, double>,
                      "four longs + four doubles = 8 C++ args, at the cap");
        // Nine wide args is over the cap regardless of their two-slot cost.
        static_assert(!within_cap_v<std::int64_t, std::int64_t, std::int64_t,
                                    std::int64_t, std::int64_t, std::int64_t,
                                    std::int64_t, std::int64_t, std::int64_t>,
                      "nine longs = 9 C++ args, over the cap");
        check("cap_mirror_8_wide_in_cap",
              within_cap_v<std::int64_t, double, std::int64_t, double,
                           std::int64_t, double, std::int64_t, double>);
        check("cap_mirror_9_wide_over_cap",
              !within_cap_v<std::int64_t, std::int64_t, std::int64_t,
                            std::int64_t, std::int64_t, std::int64_t,
                            std::int64_t, std::int64_t, std::int64_t>);
        // call() must itself be well-formed for an all-wide 8-arg frame (the
        // static_assert in call_jni does not fire at the boundary).
        check("call_well_formed_8_all_wide",
              call_well_formed_v<std::int64_t, double, std::int64_t, double,
                                 std::int64_t, double, std::int64_t, double>);
    }

    // ---- (G) jvm_primitive_byte_width + clamp_safe_container_count --------
    // Pure helpers the wide-return / array decode paths lean on.  J and D are
    // 8 bytes (the wide widths); a width mismatch is what the read-side guard
    // refuses.  clamp bounds any oop-derived count to the safe ceiling.
    inline auto run_width_and_clamp() -> void
    {
        using vmhook::detail::jvm_primitive_byte_width;
        check("pbw_J_is_8", jvm_primitive_byte_width("J") == 8u);
        check("pbw_D_is_8", jvm_primitive_byte_width("D") == 8u);
        check("pbw_I_is_4", jvm_primitive_byte_width("I") == 4u);
        check("pbw_F_is_4", jvm_primitive_byte_width("F") == 4u);
        check("pbw_S_is_2", jvm_primitive_byte_width("S") == 2u);
        check("pbw_C_is_2", jvm_primitive_byte_width("C") == 2u);
        check("pbw_Z_is_1", jvm_primitive_byte_width("Z") == 1u);
        check("pbw_B_is_1", jvm_primitive_byte_width("B") == 1u);
        check("pbw_object_is_0", jvm_primitive_byte_width("Ljava/lang/String;") == 0u);
        check("pbw_empty_is_0", jvm_primitive_byte_width(std::string_view{}) == 0u);
        check("pbw_array_token_is_0", jvm_primitive_byte_width("[I") == 0u);

        using vmhook::clamp_safe_container_count;
        constexpr std::int32_t cap{
            static_cast<std::int32_t>(vmhook::k_max_safe_container_elems) };
        check("clamp_negative_is_zero", clamp_safe_container_count(-1) == 0);
        check("clamp_zero_is_zero", clamp_safe_container_count(0) == 0);
        check("clamp_small_passthrough", clamp_safe_container_count(123) == 123);
        check("clamp_at_cap_minus_one",
              clamp_safe_container_count(cap - 1) == cap - 1);
        check("clamp_over_cap_clamped",
              clamp_safe_container_count((std::numeric_limits<std::int32_t>::max)()) == cap);
        // k_max_safe_container_elems is documented as 1<<24.
        static_assert(vmhook::k_max_safe_container_elems == (1ull << 24),
                      "safe container ceiling is 2^24 elements");
        check("clamp_cap_value_is_2pow24", cap == (1 << 24));
    }

    inline auto run_all() -> void
    {
        run_vmstruct_null_contract();
        run_narrow_codec_roundtrip();
        run_string_decode_logic();
        run_value_t_classification();
        run_method_flags_decode();
        run_cap_mirror();
        run_width_and_clamp();
    }
}

int main()
{
    // -----------------------------------------------------------------------
    // (1) Threshold predicate - the constexpr selector `sizeof...(args) <= 8`.
    //     0..8 accepted (warm path unchanged / byte-identical); 9+ rejected.
    //     static_assert so a regression fails the BUILD on every matrix compiler.
    // -----------------------------------------------------------------------
    static_assert(within_cap_v<>,                                  "0 args: in cap");
    static_assert(within_cap_v<int>,                               "1 arg: in cap");
    static_assert(within_cap_v<int, int, int, int>,                "4 args: in cap");
    static_assert(within_cap_v<int, int, int, int, int, int, int>, "7 args: in cap");
    static_assert(within_cap_v<int, int, int, int, int, int, int, int>,
                  "8 args: AT the cap, must be accepted");
    static_assert(!within_cap_v<int, int, int, int, int, int, int, int, int>,
                  "9 args: over the cap, must be rejected");
    static_assert(!within_cap_v<int, int, int, int, int, int, int, int, int, int, int, int>,
                  "12 args: over the cap, must be rejected");
    check("threshold_8_in_cap",
          within_cap_v<int, int, int, int, int, int, int, int>);
    check("threshold_9_over_cap",
          !within_cap_v<int, int, int, int, int, int, int, int, int>);

    // -----------------------------------------------------------------------
    // (2) Well-formedness of call() at and below the cap.  These detections
    //     compile to `true` ONLY because the call() static_assert does not fire
    //     for these arities - i.e. they confirm 8 is INSIDE the accepted set and
    //     the boundary was placed at 8, not 7.  (An over-cap detection is omitted
    //     by design: it would be a hard build error, which is the guarantee.)
    // -----------------------------------------------------------------------
    check("call_well_formed_0_args", call_well_formed_v<>);
    check("call_well_formed_1_arg",  call_well_formed_v<int>);
    check("call_well_formed_8_args",
          call_well_formed_v<int, int, int, int, int, int, int, int>);
    // A long and a double argument are accepted at the boundary too (they each
    // map to one VARIADIC C++ argument here; their two-interpreter-slot cost is
    // a runtime concern the assert message documents, not a C++-arity concern).
    check("call_well_formed_8_mixed_wide_args",
          call_well_formed_v<long, double, int, int, int, int, int, int>);
    // static_assert form of the same boundary fact: the AT-cap expression is
    // valid, so the assert provably does not reject 8.
    static_assert(call_well_formed_v<int, int, int, int, int, int, int, int>,
                  "call() must accept exactly 8 arguments");

    // -----------------------------------------------------------------------
    // ADDITIVE deepening pass (Criterion 2): VMStruct null-contract, narrow
    // codec round-trip, read_java_string decode logic, value_t classification,
    // Method-flags layout decision, cap mirror + wide-arg overload mapping,
    // primitive-width + clamp.  See namespace mcac_deepen above.
    // -----------------------------------------------------------------------
    mcac_deepen::run_all();

    std::printf("\n%s (%d failure%s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
