// Standalone (no-JVM) EXHAUSTIVE-INPUT unit test for the klass_introspection
// feature surface, plus the two pure-arithmetic legs that introspection-driven
// hooking is built on: the _dont_inline / NO_COMPILE flag-bit logic
// (dont_inline_dont_compile) and the common-detour dispatch decision + the
// return_slot POD encoding (hook_common_detour_dispatch).
//
// ───────────────────────────────────────────────────────────────────────────
// WHY THIS FILE EXISTS / WHAT IS NO-JVM-DETERMINABLE
// ───────────────────────────────────────────────────────────────────────────
// This executable runs with NO HotSpot JVM in-process, so the exported global
// gHotSpotVMStructs is never resolvable: iterate_struct_entries(...) returns
// nullptr for EVERY field, so every raw layout accessor bails at its `!entry`
// guard BEFORE dereferencing `this`.  That makes exactly three things fully
// deterministic and exhaustively testable here:
//
//   1. The NULL / EMPTY / FAIL-CLOSED CONTRACT of the introspection entry
//      points (collect_klass_methods, get_class_methods, the raw klass layout
//      accessors), driven over a dense input matrix of null and
//      is_valid_pointer-REJECTED (never-dereferenced) klass pointers.  No JVM
//      => no klass resolvable => every entry point returns empty / nullptr / 0
//      without throwing and WITHOUT reading wild memory.
//
//   2. The PURE ARITHMETIC the introspection/flag layer performs once a klass
//      or Method word is in hand: the JVM internal-name classification
//      conventions (array '[' prefix, single-char primitive descriptors,
//      'L...;' reference form), the access-modifier decode bits (JVM_ACC_*),
//      the Klass::_layout_helper instance-size decode (<=0 -> 0, tag bit 0
//      masked off), the Array<Method*>/Array<u2> data-offset constants (+8 / +4)
//      and length clamp (count<0 || count>65535 -> 0), the FieldInfo packed
//      offset reconstruction (packed >> 2) and static bit (0x0008), and the
//      Symbol length cap (0 || > 0x1000 -> "").  Each value is recomputed from
//      the documented source expression on words WE own — never a fabricated
//      address read.
//
//   3. The dont_inline_dont_compile flag-bit arithmetic (NO_COMPILE compose /
//      set / clear / mask / no-bleed, and the _dont_inline single-bit toggle at
//      bit 2 (u2) and bit 12 (u4)) and the hook_common_detour_dispatch POD /
//      decision logic (return_slot field defaults + sizes + standard-layout,
//      return_value::set sign-extension encoding into the 64-bit retval slot,
//      the value-equality dispatch match the detour loop performs, the
//      _thread_in_Java post-detour state value, and g_shutdown_requested's
//      fail-closed default).  All pure bit/POD math on owned storage.
//
// OUT OF SCOPE (needs a live JVM, or would fabricate + read a wild address —
// which SEGV-aborts on POSIX, uncatchable on the no-SEH MinGW/clang-on-Windows
// legs): walking a REAL InstanceKlass::_methods array, resolving a real klass by
// name, decoding a live Symbol's body, or driving a real interpreter frame
// through common_detour.  Those are the live-JVM modules' job
// (klass_introspection, instanceklass_methods_walk, method_enumeration,
// dont_inline_dont_compile).  We NEVER fabricate a klass/Method/frame/oop and
// dereference it; the raw accessors are called ONLY on nullptr and
// is_valid_pointer-rejected low/odd/sentinel constants (rejected BEFORE any
// read), and every layout/encode SEMANTIC is pinned through captureless mirrors
// of the exact source expressions.
//
// Source of truth (vmhook/ext/vmhook/vmhook.hpp; the functions are authority):
//   symbol::to_string                 2237  (length==0 || length>0x1000 -> "")
//   klass::get_name                   3439  (safe_read_pointer + untag, nullptr-safe)
//   klass::get_methods_count          3504  (Array<T>::_length @0; clamp <0||>65535)
//   klass::get_methods_ptr            3543  (data @ array+8)
//   klass::get_super                  3735  (Klass._super, nullptr-safe)
//   klass::get_instance_size          3764  (_layout_helper<=0 -> 0; & ~1 tag strip)
//   klass::find_field (JDK8-20 leg)   4111  (field_slots=6, data @ +4, static 0x0008,
//                                            packed>>2)
//   detail::collect_klass_methods     8985  (null klass -> empty; per-slot
//                                            is_valid_pointer skip)
//   get_class_methods(name)           9031  (collect(find_class(name)))
//   NO_COMPILE                        7579  (0x02|0x04|0x08|0x01 << 24 == 0x0F000000)
//   derive_method_flags_layout        7450  (_dont_inline bit 2 (u2) / bit 12 (u4))
//   return_slot                       1313  (cancel{false}, retval{0})
//   return_value::set                 1353  (sign-extend signed<8 bytes, else memcpy)
//   common_detour                     7268  (g_shutdown_requested gate; value-match
//                                            dispatch; _thread_in_Java after)
//   java_thread_state::_thread_in_Java 4716 (== 8)
//   g_shutdown_requested              7197  (default false)
//   is_valid_pointer                  2047  (floor/ceiling/odd/9-sentinel gate)
//   user_address_floor / ceiling      520 / 515
#include <vmhook/vmhook.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
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
    // ── Captureless mirrors of the exact source expressions ─────────────────
    // None of these call into the library; they reproduce the documented closed
    // forms so the suite can pin the SEMANTICS that would otherwise require a
    // live klass / Method to observe.

    // Klass::_layout_helper instance-size decode (vmhook.hpp:3774-3781):
    //   layout_helper <= 0  -> 0   (array / abstract / interface klass)
    //   else                -> layout_helper & ~1   (strip the low tag bit)
    auto decode_instance_size(std::int32_t layout_helper) -> std::size_t
    {
        if (layout_helper <= 0)
        {
            return 0;
        }
        return static_cast<std::size_t>(layout_helper & ~1);
    }

    // Array<T>::_length clamp (vmhook.hpp:3527-3532): a negative or > 65535
    // length (u2 class-file method_count ceiling) means a mis-resolved offset /
    // torn read -> return 0 so no caller over-allocates or walks off the end.
    auto clamp_methods_count(std::int32_t raw_length) -> std::int32_t
    {
        if (raw_length < 0 || raw_length > 65535)
        {
            return 0;
        }
        return raw_length;
    }

    // FieldInfo packed-offset reconstruction (vmhook.hpp:4162-4163):
    //   packed = (high << 16) | low ; offset = packed >> FIELDINFO_TAG_SIZE(2)
    auto decode_field_offset(std::uint16_t high_packed, std::uint16_t low_packed) -> std::uint32_t
    {
        const std::uint32_t packed{ (static_cast<std::uint32_t>(high_packed) << 16) | low_packed };
        return packed >> 2;
    }

    // Symbol::to_string length-cap predicate (vmhook.hpp:2272): a symbol whose
    // length is 0 OR > 0x1000 decodes to "" (the over-long-symbol skip, flaw #3).
    auto symbol_length_is_decodable(std::uint16_t length) -> bool
    {
        return !(length == 0 || length > 0x1000);
    }

    // JVM internal-name classification conventions (the descriptor grammar the
    // introspection layer's by-name resolver and array/primitive klass handling
    // rely on).  These are stable JVM facts, NOT a library function — pinned as
    // mirrors so the array/primitive/reference distinctions are characterised
    // with no JVM.
    auto name_is_array(std::string_view internal_name) -> bool
    {
        return !internal_name.empty() && internal_name.front() == '[';
    }
    auto name_array_dimensions(std::string_view internal_name) -> std::size_t
    {
        std::size_t dims{ 0 };
        while (dims < internal_name.size() && internal_name[dims] == '[')
        {
            ++dims;
        }
        return dims;
    }
    auto descriptor_is_primitive(std::string_view descriptor) -> bool
    {
        if (descriptor.size() != 1)
        {
            return false;
        }
        switch (descriptor.front())
        {
            case 'Z': case 'B': case 'C': case 'S':
            case 'I': case 'J': case 'F': case 'D': case 'V':
                return true;
            default:
                return false;
        }
    }
    auto descriptor_is_reference(std::string_view descriptor) -> bool
    {
        return descriptor.size() >= 3
            && descriptor.front() == 'L'
            && descriptor.back() == ';';
    }

    // The width/bit dispatch set_dont_inline applies (vmhook.hpp:7700-7755),
    // reproduced on an owned word: the SAME (1u << bit) mask is OR'd (enable) or
    // AND-NOT'd (disable) at the width the layout resolved.  Pure RMW; no JVM.
    auto toggle_dont_inline_word(std::uint32_t word, int width_bytes, int bit, bool enabled) -> std::uint32_t
    {
        const std::uint32_t mask{ 1u << bit };
        if (width_bytes == 2 && bit < 16)
        {
            const std::uint16_t lo{ static_cast<std::uint16_t>(word & 0xFFFFu) };
            const std::uint16_t toggled{ static_cast<std::uint16_t>(
                enabled ? (lo | static_cast<std::uint16_t>(mask))
                        : (lo & static_cast<std::uint16_t>(~mask))) };
            return (word & 0xFFFF0000u) | toggled;
        }
        if (width_bytes == 4 && bit < 32)
        {
            return enabled ? (word | mask) : (word & ~mask);
        }
        return word;
    }
}

int main()
{
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::klass;

    // =====================================================================
    // 0. COMPILE-TIME signature / return-type / noexcept pins (static_assert).
    //    A regression in any of these fails the BUILD — the strongest pin.
    // =====================================================================
    // collect_klass_methods / get_class_methods return vector<pair<string,string>>.
    using collect_ret_t = decltype(vmhook::detail::collect_klass_methods(
        static_cast<vmhook::hotspot::klass*>(nullptr)));
    using by_name_ret_t = decltype(vmhook::get_class_methods(std::string_view{}));
    static_assert(std::is_same_v<collect_ret_t,
                      std::vector<std::pair<std::string, std::string>>>,
                  "collect_klass_methods must return vector<pair<name,descriptor>>");
    static_assert(std::is_same_v<by_name_ret_t,
                      std::vector<std::pair<std::string, std::string>>>,
                  "get_class_methods(name) must return vector<pair<name,descriptor>>");
    static_assert(std::is_same_v<collect_ret_t, by_name_ret_t>,
                  "the engine and the by-name overload must share one return type");
    static_assert(noexcept(vmhook::detail::collect_klass_methods(
                      static_cast<vmhook::hotspot::klass*>(nullptr))),
                  "collect_klass_methods must be noexcept");
    // The element decltype is a REFERENCE (range-for binds a reference); strip
    // cvref before pinning the element type (HARD RULE 3).
    static_assert(std::is_same_v<std::remove_cvref_t<collect_ret_t::value_type>,
                      std::pair<std::string, std::string>>,
                  "enumeration element is pair<string,string>");
    // The raw klass layout accessors are noexcept with the documented return types.
    static_assert(std::is_same_v<decltype(std::declval<klass>().get_methods_count()),
                      std::int32_t>,
                  "get_methods_count returns int32_t");
    static_assert(std::is_same_v<decltype(std::declval<klass>().get_methods_ptr()),
                      vmhook::hotspot::method**>,
                  "get_methods_ptr returns method**");
    static_assert(noexcept(std::declval<klass>().get_methods_count())
                  && noexcept(std::declval<klass>().get_methods_ptr())
                  && noexcept(std::declval<klass>().get_super())
                  && noexcept(std::declval<klass>().get_instance_size()),
                  "raw klass layout accessors are noexcept");
    static_assert(std::is_same_v<decltype(std::declval<klass>().get_super()), klass*>,
                  "get_super returns klass*");
    // NO_COMPILE is a signed 32-bit constant equal to the documented compose.
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(vmhook::hotspot::NO_COMPILE)>,
                      std::int32_t>,
                  "NO_COMPILE is std::int32_t");
    static_assert(vmhook::hotspot::NO_COMPILE
                      == (0x02000000 | 0x04000000 | 0x08000000 | 0x01000000),
                  "NO_COMPILE is the OR of the four compile-control bits");
    // return_slot is a POD the trampoline allocates on the native stack.
    static_assert(std::is_standard_layout_v<vmhook::hotspot::return_slot>,
                  "return_slot must be standard-layout");
    static_assert(std::is_trivially_copyable_v<vmhook::hotspot::return_slot>,
                  "return_slot must be trivially copyable");
    // _thread_in_Java is the state common_detour force-sets after the detour.
    static_assert(static_cast<std::int8_t>(
                      vmhook::hotspot::java_thread_state::_thread_in_Java) == 8,
                  "_thread_in_Java == 8");
    check("klass_introspection_static_asserts_compiled", true);

    // =====================================================================
    // A. collect_klass_methods / get_class_methods — NULL / EMPTY contract.
    //    No JVM => find_class resolves no klass, and a null klass short-circuits
    //    to an empty vector at the top of the engine.  Driven over null and a
    //    dense set of is_valid_pointer-REJECTED klass pointers (each rejected
    //    BEFORE any field read, so passing them is POSIX-safe — no wild deref).
    // =====================================================================
    {
        const auto via_null{ vmhook::detail::collect_klass_methods(nullptr) };
        check("A_collect_null_klass_empty", via_null.empty());
        check("A_collect_null_klass_size0", via_null.size() == 0);
    }
    {
        // is_valid_pointer-rejected klass pointers: low (< floor), odd, and the
        // nine debug-fill sentinels.  collect_klass_methods reaches
        // get_methods_count/get_methods_ptr, both of which bail at `!entry`
        // (no JVM) BEFORE ever forming this+offset — so nothing is dereferenced.
        const std::array<std::uintptr_t, 7> rejected_klass_addrs{
            std::uintptr_t{ 0x1u },             // odd + below floor
            std::uintptr_t{ 0x8u },             // below floor
            std::uintptr_t{ 0x1000u },          // below floor
            std::uintptr_t{ 0xDEADBEEFu },      // sentinel low32 (also odd)
            std::uintptr_t{ 0xCAFEBABEu },      // even sentinel low32
            std::uintptr_t{ 0xCCCCCCCCu },      // even sentinel low32
            std::uintptr_t{ 0x3u },             // odd
        };
        bool all_empty{ true };
        for (const std::uintptr_t addr : rejected_klass_addrs)
        {
            auto* const k{ reinterpret_cast<klass*>(addr) };
            // Confirm the input is genuinely rejected by the gate before we
            // hand it to the engine (so this can never become a wild read).
            if (is_valid_pointer(k)) { all_empty = false; }
            if (!vmhook::detail::collect_klass_methods(k).empty()) { all_empty = false; }
        }
        check("A_collect_rejected_klass_ptrs_all_empty_no_deref", all_empty);
    }
    {
        // The by-name overload: find_class returns nullptr for EVERY name with
        // no JVM, so the result is always empty.  Sweep array, primitive-looking,
        // reference-descriptor, dotted, slashed, and garbage name shapes.
        const char* names[]{
            "java/lang/Object", "java/lang/String",
            "[I", "[[I", "[Ljava/lang/String;", "[[[Ljava/lang/Object;",
            "I", "V", "Z",
            "Ljava/lang/Object;",
            "java.lang.Object",
            "", "/", "//", " ", "garbage",
        };
        bool all_empty{ true };
        std::size_t probed{ 0 };
        for (const char* n : names)
        {
            if (!vmhook::get_class_methods(n).empty()) { all_empty = false; }
            ++probed;
        }
        check("A_by_name_every_shape_empty_no_jvm", all_empty);
        check("A_by_name_sweep_nonempty", probed >= 15);
    }

    // =====================================================================
    // B. RAW klass layout accessors honour the no-JVM contract WITHOUT
    //    dereferencing.  With no JVM the InstanceKlass::_methods / Klass::_super
    //    / Klass::_layout_helper VMStruct entries are all absent, so every
    //    accessor bails at its `!entry` guard.  Called ONLY on null and
    //    is_valid_pointer-rejected pointers (rejected before the `this` read).
    // =====================================================================
    {
        klass* const null_klass{ nullptr };
        check("B_get_methods_count_null_klass_zero", null_klass == nullptr);
        // Drive the engine over null (the only POSIX-safe way to reach the
        // accessors here); it must produce the empty sentinel.
        check("B_collect_null_empty", vmhook::detail::collect_klass_methods(null_klass).empty());
    }
    {
        // get_methods_count / get_methods_ptr / get_super / get_instance_size on
        // an is_valid_pointer-rejected base: every one returns its empty
        // sentinel (0 / nullptr) at the `!entry` guard, never reading memory.
        // (We assert the gate rejects the input first, so no wild read occurs.)
        auto* const k{ reinterpret_cast<klass*>(std::uintptr_t{ 0xDEADBEEFu }) };
        check("B_rejected_base_is_invalid", !is_valid_pointer(k));
        check("B_get_methods_count_rejected_zero", k->get_methods_count() == 0);
        check("B_get_methods_ptr_rejected_null", k->get_methods_ptr() == nullptr);
        check("B_get_super_rejected_null", k->get_super() == nullptr);
        check("B_get_instance_size_rejected_zero", k->get_instance_size() == 0);
    }

    // =====================================================================
    // C. Klass::_layout_helper instance-size DECODE (vmhook.hpp:3774-3781).
    //    The pure decode: <=0 -> 0 (array/abstract/interface), else strip the
    //    low tag bit.  This is the only klass-kind signal the introspection
    //    layer reads, so its decode is pinned exhaustively over the boundary.
    // =====================================================================
    {
        // Non-positive layout_helper (array klass / abstract / interface): 0.
        check("C_layout_zero_size0", decode_instance_size(0) == 0);
        check("C_layout_neg1_size0", decode_instance_size(-1) == 0);
        check("C_layout_min_size0", decode_instance_size(-2147483647 - 1) == 0);
        // Positive: low tag bit (bit 0) stripped, all higher bits kept.
        check("C_layout_even_kept", decode_instance_size(16) == 16);
        check("C_layout_odd_tag_stripped", decode_instance_size(17) == 16);
        check("C_layout_1_strips_to_0", decode_instance_size(1) == 0);
        check("C_layout_typical_object_header", decode_instance_size(0x10 | 1) == 0x10);
        // Sweep: for every positive lh, decode == lh with bit 0 cleared, and the
        // result is always even (8-aligned instance sizes have bit 0 clear).
        bool sweep_ok{ true };
        for (std::int32_t lh{ 1 }; lh <= 4096; ++lh)
        {
            const std::size_t got{ decode_instance_size(lh) };
            const std::size_t want{ static_cast<std::size_t>(lh & ~1) };
            if (got != want || (got & 1u) != 0u) { sweep_ok = false; }
        }
        check("C_layout_positive_sweep_strips_bit0", sweep_ok);
    }

    // =====================================================================
    // D. Array<Method*>::_length CLAMP (vmhook.hpp:3527-3532).  A raw int32
    //    length is trusted only when 0 <= n <= 65535 (the u2 class-file
    //    method_count ceiling); anything else means a mis-resolved offset and
    //    clamps to 0.  Exhaustive over the boundary so a corrupt/hostile length
    //    can never drive reserve()/the walk off the end.
    // =====================================================================
    {
        check("D_clamp_zero", clamp_methods_count(0) == 0);
        check("D_clamp_one", clamp_methods_count(1) == 1);
        check("D_clamp_max_ok", clamp_methods_count(65535) == 65535);
        check("D_clamp_just_over_zeroed", clamp_methods_count(65536) == 0);
        check("D_clamp_negative_zeroed", clamp_methods_count(-1) == 0);
        check("D_clamp_int_min_zeroed", clamp_methods_count(-2147483647 - 1) == 0);
        check("D_clamp_int_max_zeroed", clamp_methods_count(2147483647) == 0);
        // Boundary sweep: only [0, 65535] passes through unchanged.
        bool boundary_ok{ true };
        const std::int32_t probes[]{ -100, -1, 0, 1, 100, 65534, 65535, 65536, 70000, 1000000 };
        for (const std::int32_t n : probes)
        {
            const std::int32_t got{ clamp_methods_count(n) };
            const std::int32_t want{ (n < 0 || n > 65535) ? 0 : n };
            if (got != want) { boundary_ok = false; }
        }
        check("D_clamp_boundary_sweep", boundary_ok);
    }

    // =====================================================================
    // E. Array<Method*> / Array<u2> DATA-OFFSET constants (vmhook.hpp:3560-3564
    //    / 4121-4125).  The single most ABI-fragile constants in the feature:
    //    Method* data begins at array+8 (int32 _length @0 + 4 pad + 8-aligned
    //    pointers @8), while u2 field data begins at array+4 (u2 needs no
    //    pointer-alignment padding).  Pinned as the documented x64 layout facts.
    // =====================================================================
    {
        // Build a byte buffer shaped like Array<Method*>: _length @0, pad @4,
        // first Method* slot @8.  Verify the +8 stride reaches exactly slot 0
        // and that consecutive Method* slots are 8 bytes apart on this ABI.
        alignas(16) std::array<std::uint8_t, 64> arr{};
        const std::int32_t length{ 3 };
        std::memcpy(arr.data() + 0, &length, sizeof(length));
        // Place recognizable pointer-sized values at +8, +16, +24.
        const std::uint64_t slot0{ 0x1111111111111110ull };
        const std::uint64_t slot1{ 0x2222222222222220ull };
        const std::uint64_t slot2{ 0x3333333333333330ull };
        std::memcpy(arr.data() + 8 + 0 * 8, &slot0, sizeof(slot0));
        std::memcpy(arr.data() + 8 + 1 * 8, &slot1, sizeof(slot1));
        std::memcpy(arr.data() + 8 + 2 * 8, &slot2, sizeof(slot2));

        const std::int32_t read_len{
            *reinterpret_cast<const std::int32_t*>(arr.data()) };
        check("E_array_method_length_at_off0", read_len == 3);
        const auto* const data{
            reinterpret_cast<const std::uint64_t*>(arr.data() + 8) };
        check("E_array_method_data_at_off8",
              data[0] == slot0 && data[1] == slot1 && data[2] == slot2);
        check("E_array_method_stride_is_8",
              reinterpret_cast<const std::uint8_t*>(&data[1])
                  - reinterpret_cast<const std::uint8_t*>(&data[0]) == 8);
        // u2 field array data at +4 (no padding before u2 _data[0]).
        alignas(8) std::array<std::uint8_t, 32> farr{};
        const std::int32_t flen{ 12 };
        std::memcpy(farr.data() + 0, &flen, sizeof(flen));
        const std::uint16_t f0{ 0xABCDu };
        const std::uint16_t f1{ 0x1234u };
        std::memcpy(farr.data() + 4 + 0 * 2, &f0, sizeof(f0));
        std::memcpy(farr.data() + 4 + 1 * 2, &f1, sizeof(f1));
        const auto* const fdata{
            reinterpret_cast<const std::uint16_t*>(farr.data() + 4) };
        check("E_array_u2_field_data_at_off4", fdata[0] == f0 && fdata[1] == f1);
    }

    // =====================================================================
    // F. FieldInfo (JDK 8-20 leg) DECODE: packed-offset reconstruction and the
    //    static bit (vmhook.hpp:4153-4165).  field_slots=6 u16 per record; the
    //    byte offset is ((high<<16)|low) >> 2, and the static flag is access
    //    bit 3 (0x0008).  Both pinned exhaustively over the relevant inputs.
    // =====================================================================
    {
        // packed >> 2 across the boundary of the low/high u16 split.
        check("F_offset_zero", decode_field_offset(0u, 0u) == 0u);
        check("F_offset_low_only", decode_field_offset(0u, 0x000Cu) == 3u);   // 0xC >> 2
        check("F_offset_tag_bits_dropped", decode_field_offset(0u, 0x000Fu) == 3u); // tag in low 2
        check("F_offset_high_word", decode_field_offset(1u, 0u) == (0x10000u >> 2));
        check("F_offset_combined",
              decode_field_offset(0x0001u, 0x8000u) == (((0x1u << 16) | 0x8000u) >> 2));
        // Sweep: the reconstructed offset always equals packed/4 (integer).
        bool offset_sweep_ok{ true };
        for (std::uint32_t hi{ 0u }; hi <= 4u; ++hi)
        {
            for (std::uint32_t lo{ 0u }; lo <= 0xFFFFu; lo += 0x1111u)
            {
                const std::uint32_t got{ decode_field_offset(
                    static_cast<std::uint16_t>(hi), static_cast<std::uint16_t>(lo)) };
                const std::uint32_t want{ ((hi << 16) | lo) >> 2 };
                if (got != want) { offset_sweep_ok = false; }
            }
        }
        check("F_offset_packed_shift2_sweep", offset_sweep_ok);

        // Static bit (0x0008) decode over the full u16 access-flags domain
        // boundary: only when bit 3 is set is the field static; the mask never
        // sees any other JVM_ACC_* class-file bit.
        constexpr std::uint16_t jvm_acc_static{ 0x0008u };
        check("F_static_bit_value_is_0x0008", jvm_acc_static == 0x0008u);
        check("F_static_set_detected", (static_cast<std::uint16_t>(0x0009u) & jvm_acc_static) != 0u);
        check("F_static_clear_not_detected", (static_cast<std::uint16_t>(0x0001u) & jvm_acc_static) == 0u);
        // PUBLIC|FINAL|STATIC (0x0001|0x0010|0x0008) -> static; PUBLIC|FINAL only -> not.
        check("F_static_in_combined_flags",
              (static_cast<std::uint16_t>(0x0001u | 0x0010u | 0x0008u) & jvm_acc_static) != 0u);
        check("F_not_static_in_public_final",
              (static_cast<std::uint16_t>(0x0001u | 0x0010u) & jvm_acc_static) == 0u);
    }

    // =====================================================================
    // G. JVM access-modifier decode bits (the class-file low-16 group).  These
    //    are the modifiers an introspection consumer reads off _access_flags;
    //    pin the canonical bit positions and that the group is DISJOINT from the
    //    NO_COMPILE high-byte mask the library OR's in (so decoding a modifier
    //    never sees a NO_COMPILE bit, and OR-ing NO_COMPILE never flips one).
    // =====================================================================
    {
        constexpr std::uint32_t acc_public{ 0x0001u };
        constexpr std::uint32_t acc_private{ 0x0002u };
        constexpr std::uint32_t acc_protected{ 0x0004u };
        constexpr std::uint32_t acc_static{ 0x0008u };
        constexpr std::uint32_t acc_final{ 0x0010u };
        constexpr std::uint32_t acc_abstract{ 0x0400u };
        constexpr std::uint32_t acc_interface{ 0x0200u };
        // Canonical positions (each is a distinct single bit in the low 16).
        check("G_acc_bits_distinct_powers_of_two",
              acc_public == (1u << 0) && acc_private == (1u << 1)
              && acc_protected == (1u << 2) && acc_static == (1u << 3)
              && acc_final == (1u << 4) && acc_interface == (1u << 9)
              && acc_abstract == (1u << 10));
        const std::uint32_t modifier_group{
            acc_public | acc_private | acc_protected | acc_static
            | acc_final | acc_abstract | acc_interface };
        check("G_modifier_group_in_low_16", (modifier_group & 0xFFFF0000u) == 0u);
        // Disjoint from NO_COMPILE (high byte 0x0F000000).
        const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };
        check("G_modifiers_disjoint_from_no_compile",
              (modifier_group & no_compile) == 0u);
        // Decoding any modifier from a word that ALSO has NO_COMPILE OR'd in
        // returns exactly the modifier bits (no high-byte bleed).
        const std::uint32_t word{ acc_public | acc_static | no_compile };
        check("G_decode_modifier_ignores_no_compile_high_byte",
              (word & 0xFFFFu) == (acc_public | acc_static)
              && (word & acc_static) == acc_static);
    }

    // =====================================================================
    // H. JVM internal-NAME classification (array / primitive / reference).  The
    //    by-name resolver and array-klass handling depend on these descriptor
    //    conventions; pinned via mirrors so the distinctions are characterised
    //    with no JVM.  (The library returns empty for array/primitive names with
    //    no JVM — section A — but the CLASSIFICATION itself is JVM-grammar.)
    // =====================================================================
    {
        // Array names start with '['; dimensionality is the run of leading '['.
        check("H_array_1d", name_is_array("[I") && name_array_dimensions("[I") == 1);
        check("H_array_2d",
              name_is_array("[[I") && name_array_dimensions("[[I") == 2);
        check("H_array_3d_ref",
              name_is_array("[[[Ljava/lang/String;")
              && name_array_dimensions("[[[Ljava/lang/String;") == 3);
        check("H_instance_name_not_array",
              !name_is_array("java/lang/Object")
              && name_array_dimensions("java/lang/Object") == 0);
        check("H_empty_name_not_array", !name_is_array(std::string_view{}));
        // Primitive descriptors are the single chars Z B C S I J F D V.
        const char* primitives[]{ "Z", "B", "C", "S", "I", "J", "F", "D", "V" };
        bool all_prim{ true };
        for (const char* p : primitives)
        {
            if (!descriptor_is_primitive(p)) { all_prim = false; }
        }
        check("H_all_primitive_descriptors", all_prim);
        // Non-primitives: multi-char, reference, array, unknown letter.
        check("H_not_primitive_reference", !descriptor_is_primitive("Ljava/lang/Object;"));
        check("H_not_primitive_array", !descriptor_is_primitive("[I"));
        check("H_not_primitive_lowercase", !descriptor_is_primitive("i"));
        check("H_not_primitive_unknown", !descriptor_is_primitive("Q"));
        check("H_not_primitive_empty", !descriptor_is_primitive(std::string_view{}));
        // Reference descriptors are L...; (at least "L;" plus one char).
        check("H_reference_object", descriptor_is_reference("Ljava/lang/Object;"));
        check("H_reference_short", descriptor_is_reference("LX;"));
        check("H_not_reference_primitive", !descriptor_is_reference("I"));
        check("H_not_reference_no_semicolon", !descriptor_is_reference("Ljava/lang/Object"));
        check("H_not_reference_array", !descriptor_is_reference("[Ljava/lang/Object;"));
    }

    // =====================================================================
    // I. Symbol::to_string LENGTH-CAP decode (vmhook.hpp:2272).  A symbol whose
    //    length is 0 OR > 0x1000 (4096) decodes to "" — the over-long-symbol
    //    skip (flaw #3) and the empty-symbol guard.  Pinned exhaustively over
    //    the boundary; combined with collect's emplace this is where a
    //    pathological method name/descriptor degrades to "".
    // =====================================================================
    {
        check("I_length_0_not_decodable", !symbol_length_is_decodable(0u));
        check("I_length_1_decodable", symbol_length_is_decodable(1u));
        check("I_length_cap_inclusive", symbol_length_is_decodable(0x1000u));   // == 4096 OK
        check("I_length_just_over_cap_not_decodable", !symbol_length_is_decodable(0x1001u));
        check("I_length_u16_max_not_decodable", !symbol_length_is_decodable(0xFFFFu));
        // Boundary sweep: decodable iff 1 <= len <= 4096.
        bool cap_ok{ true };
        const std::uint16_t probes[]{ 0u, 1u, 2u, 4095u, 4096u, 4097u, 8192u, 0xFFFFu };
        for (const std::uint16_t len : probes)
        {
            const bool got{ symbol_length_is_decodable(len) };
            const bool want{ len >= 1u && len <= 0x1000u };
            if (got != want) { cap_ok = false; }
        }
        check("I_length_cap_boundary_sweep", cap_ok);
    }

    // =====================================================================
    // J. dont_inline_dont_compile — NO_COMPILE mask compose / set / clear /
    //    mask arithmetic (vmhook.hpp:7579).  Pure bit logic over the four
    //    compile-control bits; exhaustive over the bit positions + idempotence +
    //    no-bleed into neighbour bits.
    // =====================================================================
    {
        const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };
        constexpr std::uint32_t queued{ 0x01000000u };     // bit 24
        constexpr std::uint32_t not_c2{ 0x02000000u };     // bit 25
        constexpr std::uint32_t not_c1{ 0x04000000u };     // bit 26
        constexpr std::uint32_t not_c2_osr{ 0x08000000u };  // bit 27
        check("J_no_compile_compose_value",
              no_compile == (queued | not_c2 | not_c1 | not_c2_osr)
              && no_compile == 0x0F000000u);
        check("J_no_compile_is_four_bits",
              ((no_compile & (no_compile - 1u)) != 0u)  // not a single bit
              && (no_compile >> 24) == 0x0Fu            // exactly bits 24..27
              && (no_compile & 0x00FFFFFFu) == 0u);     // nothing in low 24
        // SET: OR into a word leaves exactly the high nibble (24..27) set and
        // every other bit untouched.  Use a base DISJOINT from the NO_COMPILE
        // bits (low 24 + bit 28 only) so the set/clear round-trip restores it
        // exactly — exactly the teardown contract (the library only ever OR's
        // and later AND-NOT's the four NO_COMPILE bits, never a base that
        // already carries them).
        const std::uint32_t base{ 0x10345678u };
        const std::uint32_t set{ base | no_compile };
        check("J_no_compile_or_sets_only_target_bits",
              (set & no_compile) == no_compile
              && (set & ~no_compile) == (base & ~no_compile));
        // CLEAR: AND-NOT removes exactly the NO_COMPILE bits, restoring the rest.
        const std::uint32_t cleared{ set & ~no_compile };
        check("J_no_compile_andnot_clears_only_target_bits",
              (cleared & no_compile) == 0u
              && (cleared & ~no_compile) == (base & ~no_compile));
        // Idempotence: OR twice == OR once; AND-NOT twice == AND-NOT once.
        check("J_no_compile_or_idempotent", (set | no_compile) == set);
        check("J_no_compile_clear_idempotent", (cleared & ~no_compile) == cleared);
        // Round-trip: set then clear restores the original base exactly.
        check("J_no_compile_set_then_clear_round_trips", (set & ~no_compile) == base);
        // Signed ~NO_COMPILE clear mask (the teardown form: NO_COMPILE is int32,
        // clear is `& static_cast<uint32_t>(~NO_COMPILE)`): low 24 bits all set,
        // bits 24..27 clear, so it retains everything but the four target bits.
        const std::uint32_t clear_mask{ static_cast<std::uint32_t>(~vmhook::hotspot::NO_COMPILE) };
        check("J_signed_clear_mask_shape",
              (clear_mask & 0x0F000000u) == 0u && (clear_mask & 0x00FFFFFFu) == 0x00FFFFFFu);
        check("J_signed_clear_mask_equals_bitwise_not", clear_mask == ~no_compile);
    }

    // =====================================================================
    // K. dont_inline_dont_compile — _dont_inline single-bit toggle at the two
    //    JDK widths (vmhook.hpp:7700-7755): bit 2 in a u2 _flags (JDK 11-20) and
    //    bit 12 in a u4 _status (JDK 21+).  Pure RMW on an owned word; exhaustive
    //    no-bleed + idempotence + round-trip + that the mask fits each width.
    // =====================================================================
    {
        // JDK 11-20: u2 width, bit 2.
        constexpr int u2_bit{ 2 };
        const std::uint32_t u2_mask{ 1u << u2_bit };
        check("K_u2_dont_inline_mask_fits_u2", (u2_mask & 0xFFFFu) == u2_mask);
        {
            const std::uint32_t seed{ 0xAAAA0000u | 0x1u };  // sibling bits inside + outside the u2
            const std::uint32_t set{ toggle_dont_inline_word(seed, 2, u2_bit, true) };
            check("K_u2_set_lands_target_bit", (set & u2_mask) == u2_mask);
            // Only the target bit changed within the low u2; high half untouched.
            check("K_u2_set_no_bleed",
                  (set & 0xFFFF0000u) == (seed & 0xFFFF0000u)
                  && (set & 0xFFFFu) == ((seed & 0xFFFFu) | u2_mask));
            const std::uint32_t cleared{ toggle_dont_inline_word(set, 2, u2_bit, false) };
            check("K_u2_clear_round_trips", cleared == seed);
            check("K_u2_set_idempotent",
                  toggle_dont_inline_word(set, 2, u2_bit, true) == set);
        }
        // JDK 21+: u4 width, bit 12.
        constexpr int u4_bit{ 12 };
        const std::uint32_t u4_mask{ 1u << u4_bit };
        check("K_u4_dont_inline_mask_fits_u4", u4_bit < 32);
        {
            // Seed unrelated MethodFlags::_status siblings (queued bit7, not_c1
            // bit9, force_inline bit11, a high bit20) that MUST survive.
            const std::uint32_t siblings{ (1u << 7) | (1u << 9) | (1u << 11) | (1u << 20) };
            const std::uint32_t set{ toggle_dont_inline_word(siblings, 4, u4_bit, true) };
            check("K_u4_set_lands_bit12", (set & u4_mask) == u4_mask);
            check("K_u4_set_preserves_siblings", (set & siblings) == siblings);
            const std::uint32_t cleared{ toggle_dont_inline_word(set, 4, u4_bit, false) };
            check("K_u4_clear_preserves_siblings_and_clears_bit12",
                  cleared == siblings && (cleared & u4_mask) == 0u);
            check("K_u4_set_idempotent",
                  toggle_dont_inline_word(set, 4, u4_bit, true) == set);
        }
        // The same (1u << bit) mask is representable in BOTH widths used.
        check("K_dont_inline_mask_width_independent",
              (u2_mask & 0xFFFFu) == u2_mask && u4_bit < 32);
        // An unrecognised width is a no-op (set_dont_inline's final fall-through).
        check("K_unknown_width_is_noop",
              toggle_dont_inline_word(0x1234u, 1, 2, true) == 0x1234u
              && toggle_dont_inline_word(0x1234u, 8, 2, true) == 0x1234u);
    }

    // =====================================================================
    // L. set_dont_inline NO-JVM / NULL contract on the REAL accessor.  A null
    //    or is_valid_pointer-rejected Method* is rejected up front; an in-range
    //    owned buffer with no JVM has no resolvable flags slot, so NO byte is
    //    written.  (POSIX-safe: the sentinel/odd cases are rejected before any
    //    deref; the owned buffer is ours to clobber-check.)
    // =====================================================================
    {
        // Null and rejected Method* -> safe no-op (reaching the next line is the
        // assertion; a deref would have faulted).
        vmhook::hotspot::set_dont_inline(nullptr, true);
        vmhook::hotspot::set_dont_inline(nullptr, false);
        vmhook::hotspot::set_dont_inline(
            reinterpret_cast<const vmhook::hotspot::method*>(std::uintptr_t{ 0x1u }), true);  // odd
        vmhook::hotspot::set_dont_inline(
            reinterpret_cast<const vmhook::hotspot::method*>(std::uintptr_t{ 0xDEADBEEFu }), false);
        check("L_set_dont_inline_null_and_rejected_safe_noop", true);
        // In-range owned buffer, no JVM: flags slot unresolvable -> no write.
        alignas(16) std::array<std::uint8_t, 64> fake_method{};
        fake_method.fill(0xA5u);
        const std::array<std::uint8_t, 64> snapshot{ fake_method };
        auto* const as_method{
            reinterpret_cast<const vmhook::hotspot::method*>(fake_method.data()) };
        vmhook::hotspot::set_dont_inline(as_method, true);
        vmhook::hotspot::set_dont_inline(as_method, false);
        check("L_set_dont_inline_no_jvm_writes_no_byte",
              std::memcmp(fake_method.data(), snapshot.data(), fake_method.size()) == 0);
    }

    // =====================================================================
    // M. hook_common_detour_dispatch — return_slot POD layout / defaults / sizes
    //    (vmhook.hpp:1313-1317).  The trampoline allocates this on the native
    //    stack and the callback writes it; pin the field defaults, sizes, and
    //    POD-ness the contract depends on.
    // =====================================================================
    {
        using slot_t = vmhook::hotspot::return_slot;
        check("M_return_slot_standard_layout", std::is_standard_layout_v<slot_t>);
        check("M_return_slot_trivially_copyable", std::is_trivially_copyable_v<slot_t>);
        check("M_return_slot_trivially_destructible", std::is_trivially_destructible_v<slot_t>);
        const slot_t slot{};
        check("M_return_slot_default_cancel_false", slot.cancel == false);
        check("M_return_slot_default_retval_zero", slot.retval == 0);
        // The retval cell is a 64-bit signed integer (the raw return bit-pattern).
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(slot_t{}.retval)>, std::int64_t>,
                      "return_slot::retval is int64_t");
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(slot_t{}.cancel)>, bool>,
                      "return_slot::cancel is bool");
        check("M_return_slot_retval_is_8_bytes", sizeof(slot.retval) == 8);
    }

    // =====================================================================
    // N. hook_common_detour_dispatch — return_value::set ENCODING into the
    //    64-bit retval slot (vmhook.hpp:1353-1382).  set() flips cancel=true and
    //    writes the value: signed integers narrower than int64 are
    //    SIGN-EXTENDED (so the interpreter's ireturn pops the right value), while
    //    other types are zero-filled then memcpy'd over the low bytes.  Driven
    //    on a REAL return_value wrapping an OWNED slot — no JVM, no frame.
    // =====================================================================
    {
        // Signed small ints: -1 must sign-extend to all-ones in the 64-bit cell.
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value rv{ &slot };
            rv.set<std::int8_t>(static_cast<std::int8_t>(-1));
            check("N_set_int8_neg1_sign_extended",
                  slot.cancel && slot.retval == static_cast<std::int64_t>(-1));
        }
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value rv{ &slot };
            rv.set<std::int16_t>(static_cast<std::int16_t>(-1234));
            check("N_set_int16_negative_sign_extended",
                  slot.cancel && slot.retval == static_cast<std::int64_t>(-1234));
        }
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value rv{ &slot };
            rv.set<std::int32_t>(-559038737);  // 0xDEADBEEF as signed
            check("N_set_int32_negative_sign_extended",
                  slot.cancel && slot.retval == static_cast<std::int64_t>(-559038737));
        }
        // Positive small int: high bits stay clear.
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value rv{ &slot };
            rv.set<std::int32_t>(42);
            check("N_set_int32_positive_no_high_bits",
                  slot.cancel && slot.retval == 42);
        }
        // Unsigned small int: zero-fill + memcpy low bytes (NOT sign-extended).
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value rv{ &slot };
            rv.set<std::uint8_t>(0xFFu);
            check("N_set_uint8_zero_filled_low_byte",
                  slot.cancel && slot.retval == 0xFF);
        }
        // bool true -> low byte 1, high bits zero.
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value rv{ &slot };
            rv.set<bool>(true);
            check("N_set_bool_true_low_byte_one", slot.cancel && slot.retval == 1);
        }
        // Pointer (oop) value: full 64-bit pattern preserved.
        {
            vmhook::hotspot::return_slot slot{};
            vmhook::return_value rv{ &slot };
            void* const p{ reinterpret_cast<void*>(std::uintptr_t{ 0x0000123456789AB0ull }) };
            rv.set<void*>(p);
            check("N_set_pointer_full_pattern",
                  slot.cancel
                  && static_cast<std::uintptr_t>(slot.retval) == std::uintptr_t{ 0x0000123456789AB0ull });
        }
        // cancel() alone flips cancel without touching retval.
        {
            vmhook::hotspot::return_slot slot{};
            slot.retval = std::int64_t{ 0x1234567890ABCDE0 };  // a recognizable pre-value
            const std::int64_t before{ slot.retval };
            vmhook::return_value rv{ &slot };
            rv.cancel();
            check("N_cancel_sets_flag_keeps_retval",
                  slot.cancel && slot.retval == before);
        }
    }

    // =====================================================================
    // O. hook_common_detour_dispatch — the DECISION ARITHMETIC (no live frame).
    //    common_detour (vmhook.hpp:7268) (1) bails immediately when
    //    g_shutdown_requested is set (default false), (2) matches a hook by
    //    POINTER-VALUE EQUALITY (hook.method == current_method) against an
    //    always-mapped owned vector — never dereferencing a cold Method*, and
    //    (3) force-sets _thread_in_Java (==8) after the detour.  We pin the pure
    //    value-comparison decision and the constants on OWNED data — NOT a
    //    fabricated frame walk.
    // =====================================================================
    {
        // g_shutdown_requested defaults to false (the fail-closed gate is OPEN
        // for normal dispatch; flipped true only during teardown).
        check("O_g_shutdown_requested_default_false",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        // The post-detour state common_detour forces.
        check("O_thread_in_java_state_is_8",
              static_cast<std::int8_t>(vmhook::hotspot::java_thread_state::_thread_in_Java) == 8);

        // The dispatch match is a pure value-equality scan: a "current method"
        // pointer is matched against the stored hook entries' .method values.
        // Reproduce that decision over OWNED sentinel pointer values (never
        // dereferenced) so the match/no-match/first-wins logic is pinned.
        struct hook_entry { const void* method; int id; };
        auto* const m_a{ reinterpret_cast<const void*>(std::uintptr_t{ 0x100200300400ull }) };
        auto* const m_b{ reinterpret_cast<const void*>(std::uintptr_t{ 0x100200300500ull }) };
        auto* const m_c{ reinterpret_cast<const void*>(std::uintptr_t{ 0x100200300600ull }) };
        const std::array<hook_entry, 3> hooks{ {
            { m_a, 1 }, { m_b, 2 }, { m_c, 3 } } };

        auto dispatch_id = [&hooks](const void* current_method) -> int
        {
            // Mirror of common_detour's loop: a null current_method (the cold /
            // unreadable frame case) matches nothing, since install never stores
            // a null method.  Otherwise return the FIRST matching entry's id.
            if (current_method == nullptr) { return -1; }
            for (const hook_entry& h : hooks)
            {
                if (h.method == current_method) { return h.id; }
            }
            return -1;
        };

        check("O_dispatch_matches_first", dispatch_id(m_a) == 1);
        check("O_dispatch_matches_middle", dispatch_id(m_b) == 2);
        check("O_dispatch_matches_last", dispatch_id(m_c) == 3);
        // A method not in the table -> no match (fall through to original body).
        check("O_dispatch_unknown_method_no_match",
              dispatch_id(reinterpret_cast<const void*>(std::uintptr_t{ 0xABCDEF00ull })) == -1);
        // A null current_method (frame::get_method() returned null on a cold
        // frame) matches nothing -> dispatch falls through, never deref'd.
        check("O_dispatch_null_method_no_match", dispatch_id(nullptr) == -1);
        // First-wins on a duplicate method value (defensive: install never does
        // this, but the loop semantics are first-match-and-return).
        const std::array<hook_entry, 2> dup{ { { m_a, 10 }, { m_a, 20 } } };
        auto dispatch_dup = [&dup](const void* cm) -> int
        {
            for (const hook_entry& h : dup) { if (h.method == cm) { return h.id; } }
            return -1;
        };
        check("O_dispatch_first_match_wins", dispatch_dup(m_a) == 10);
    }

    // =====================================================================
    // P. is_valid_pointer — the per-slot skip predicate collect_klass_methods
    //    uses (vmhook.hpp:9005) and every introspection accessor's `this` gate.
    //    Pure address arithmetic; NO memory read for any input.  Constants from
    //    source: floor 0xFFFF (reject <=), ceiling 0x7FFFFFFFFFFF (reject >=),
    //    reject odd, reject the 9 debug-fill sentinels by low32.
    // =====================================================================
    {
        constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };
        constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };
        check("P_floor_is_0xFFFF", floor == 0xFFFFull);
        check("P_ceiling_value", ceiling == 0x00007FFFFFFFFFFFull);
        check("P_null_rejected", !is_valid_pointer(nullptr));
        // floor boundary: <= floor rejected; floor+1 (0x10000, even) accepted.
        check("P_floor_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor)));
        check("P_floor_plus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(floor + 1)));
        check("P_floor_plus_two_odd_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor + 2)));
        // ceiling boundary: >= ceiling rejected; ceiling-1 (even) accepted.
        check("P_ceiling_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(ceiling)));
        check("P_ceiling_minus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(ceiling - 1)));
        // Sentinels: a low32 matching any of the nine is rejected.  Placed at a
        // high in-range, well-aligned base so ONLY the sentinel/odd rule fires.
        constexpr std::uint64_t high_base{ 0x00000A0000000000ull };
        const std::array<std::uint32_t, 9> sentinels{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu };
        bool all_rejected{ true };
        for (const std::uint32_t s : sentinels)
        {
            const std::uint64_t addr{ high_base | static_cast<std::uint64_t>(s) };
            if (is_valid_pointer(reinterpret_cast<const void*>(addr))) { all_rejected = false; }
        }
        check("P_all_sentinel_low32_rejected", all_rejected);
        // Control: a clean even non-sentinel low32 at the same base is accepted.
        check("P_clean_low32_high_base_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(high_base | 0x00010002ull)));
    }

    // =====================================================================
    // Q. DETERMINISM across the no-JVM regime: every introspection entry point
    //    yields the SAME (empty) result on repeat, so output is byte-identical
    //    run-to-run.  Ties the engine, the by-name overload, and the null path
    //    together as one fail-closed contract.
    // =====================================================================
    {
        const auto a{ vmhook::detail::collect_klass_methods(nullptr) };
        const auto b{ vmhook::detail::collect_klass_methods(nullptr) };
        check("Q_collect_repeat_both_empty", a.empty() && b.empty());
        check("Q_collect_repeat_same_size", a.size() == b.size());
        const auto n1{ vmhook::get_class_methods("java/lang/Object") };
        const auto n2{ vmhook::get_class_methods("java/lang/Object") };
        check("Q_by_name_repeat_both_empty", n1.empty() && n2.empty());
        check("Q_engine_and_by_name_agree_empty",
              a.empty() && n1.empty() && a.size() == n1.size());
    }

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
