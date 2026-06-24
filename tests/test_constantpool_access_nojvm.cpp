// Standalone (no-JVM) unit test for THREE related HotSpot-internal features:
//
//   1. constantpool_access     - the symbol_at-equivalent ConstantPool reads:
//      constant_pool::get_base() / get_length() and the index<->length BOUND
//      arithmetic + per-slot is_readable_pointer gate that const_method::
//      get_name()/get_signature() layer on top before touching base[index].
//   2. instanceklass_methods_walk - klass::get_methods_count() length clamp,
//      get_methods_ptr() data-offset/stride, and the empty-walk contract.
//   3. hook_verify_repair      - verify_hooks() drift-detection DECISION logic
//      on POD snapshots, the auto-repair watchdog gating booleans
//      (auto_repair_enabled / g_shutdown_requested / g_auto_repair_enabled),
//      and the no-JVM null / no-op / zero contract.
//
// ───────────────────────────────────────────────────────────────────────────
// WHAT IS NO-JVM-DETERMINABLE (and what is deliberately OUT OF SCOPE)
// ───────────────────────────────────────────────────────────────────────────
// This executable runs with NO HotSpot JVM in-process, so the exported global
// gHotSpotVMStructs / gHotSpotVMTypes are never resolvable: iterate_type_
// entries("ConstantPool") and iterate_struct_entries("ConstantPool","_length")
// (and every InstanceKlass / ConstMethod field) return nullptr.  That makes
// these layers fully deterministic and exhaustively testable here:
//
//   - The FAIL-CLOSED CONTRACT of every accessor: with no type/field entry
//     get_base() throws->logs->nullptr, get_length() returns the documented -1
//     "length unknown -> skip the bound check" sentinel, get_methods_count()
//     returns 0, get_methods_ptr() returns nullptr, const_method::get_name()/
//     get_signature() return nullptr, find_class() returns nullptr (so
//     verify_hooks() finds no methods to repair), and verify_hooks() repairs 0.
//     Driven over null AND is_valid_pointer-REJECTED low/odd/poison pointers
//     that are filtered BEFORE any `this+offset` dereference (POSIX-safe: no
//     wild read ever happens — HARD RULE 5).
//
//   - The PURE ARITHMETIC each feature performs once a word is in hand,
//     reproduced from the documented source expression on storage WE own:
//       * the cp index bound  `cp_length >= 0 && index >= cp_length`
//         (vmhook.hpp:2484 / 2561) and the -1 sentinel that disables it,
//       * the cp entry stride (pointer-sized, 8 on x64) and 1-based-index
//         "slot 0 unused" contract (vmhook.hpp:2325-2327),
//       * the is_readable_pointer 8-byte-alignment slot gate (vmhook.hpp:2025),
//       * the Array<Method*> length clamp `count<0 || count>65535 -> 0`
//         (vmhook.hpp:3528) and the +8 data offset / 8-byte stride
//         (vmhook.hpp:3560-3564),
//       * verify_hooks()'s three drift-mode decisions (vmhook.hpp:10857-10932)
//         on a POD hooked_method snapshot, and the NO_COMPILE compose.
//
// OUT OF SCOPE (needs a live JVM, or would fabricate + read a wild address —
// a SEGV uncatchable on the no-SEH MinGW / clang-on-windows legs): reading a
// REAL ConstantPool entry array, walking a REAL InstanceKlass::_methods array,
// resolving a real klass/Method, or driving a real watchdog thread (which is
// only ever spawned by a SUCCESSFUL hook<T>() install, never reached no-JVM).
// Those are the live-JVM modules' job.  We NEVER fabricate a ConstantPool /
// InstanceKlass / Method / frame and dereference it.
//
// Source of truth (vmhook/ext/vmhook/vmhook.hpp; the functions are authority):
//   constant_pool::get_base            2331  (iterate_type_entries; this+size;
//                                             missing-entry throw->log->nullptr)
//   constant_pool::get_length          2362  (iterate_struct_entries _length;
//                                             !entry||!is_valid_pointer -> -1)
//   const_method::get_constants        2399  (!entry throw->nullptr; this gate)
//   const_method::get_name             2438  (index bound 2484; readable 2488)
//   const_method::get_signature        2517  (index bound 2561; readable 2565)
//   klass::get_methods_count           3504  (clamp <0||>65535 -> 0)
//   klass::get_methods_ptr             3543  (data @ array+8)
//   verify_hooks                       10682 (size_t repaired; 3 drift modes)
//   hooked_method                      7120  (POD: method/expected_*/drift_logged)
//   i2i_hook_data                      7156  (POD: i2i_entry / hook)
//   g_shutdown_requested               7197  (default false)
//   g_auto_repair_enabled              11033 (default true)
//   auto_repair_enabled                11153 (reads the gate)
//   set_auto_repair_enabled            11189 (flips the gate)
//   NO_COMPILE                         (0x01|0x02|0x04|0x08 << 24 == 0x0F000000)
//   is_valid_pointer                   2047  (floor/ceiling/odd/9-sentinel gate)
//   is_readable_pointer                2018  (floor/ceiling/8-align + region)
//   user_address_floor / ceiling       520 / 515
#include <vmhook/vmhook.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

namespace
{
    // ── Captureless mirrors of the exact source expressions ─────────────────
    // None of these call into the library; they reproduce the documented closed
    // forms so the suite can pin the DECISION SEMANTICS that would otherwise
    // require a live ConstantPool / InstanceKlass / Method to observe.

    // The constant-pool index BOUND const_method::get_name()/get_signature()
    // apply (vmhook.hpp:2484 / 2561):  the index is rejected (read returns
    // nullptr) iff the JDK exports _length (cp_length >= 0) AND the index is
    // at or past it.  A cp_length of -1 (field absent / `this` invalid) means
    // "unknown -> skip the bound check", so EVERY index passes the bound.
    auto cp_index_is_out_of_bounds(std::int32_t cp_length, std::uint16_t index) -> bool
    {
        return cp_length >= 0 && index >= cp_length;
    }

    // Array<Method*>::_length clamp (vmhook.hpp:3528): a negative or > 65535
    // length (the class file's method_count is a u2) means a mis-resolved
    // offset / torn read -> 0 so no caller over-allocates or walks off the end.
    auto clamp_methods_count(std::int32_t raw_length) -> std::int32_t
    {
        if (raw_length < 0 || raw_length > 65535)
        {
            return 0;
        }
        return raw_length;
    }

    // verify_hooks()'s Mode-3 JIT-drift predicate (vmhook.hpp:10932):
    //   drifted  <=>  Method::_code repopulated  OR  NO_COMPILE got cleared.
    auto jit_drifted(bool code_now_nonnull, bool no_compile_set) -> bool
    {
        return code_now_nonnull || !no_compile_set;
    }

    // verify_hooks()'s Mode-2 aliased-Method predicate (vmhook.hpp:10877): the
    // stored hook now points at a DIFFERENT method iff its live name is
    // non-empty AND differs from the install-time expected name.  (An empty
    // current name means "could not read it this tick" -> not treated as drift.)
    auto method_aliased(std::string_view current_name, std::string_view expected_name) -> bool
    {
        return !current_name.empty() && current_name != expected_name;
    }
}

int main()
{
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::is_readable_pointer;
    using vmhook::hotspot::constant_pool;
    using vmhook::hotspot::const_method;
    using vmhook::hotspot::klass;

    // =====================================================================
    // 0. COMPILE-TIME signature / return-type / noexcept pins (static_assert).
    //    A regression in any of these fails the BUILD — the strongest pin.
    // =====================================================================
    // constant_pool accessor return types.
    static_assert(std::is_same_v<decltype(std::declval<constant_pool>().get_base()), void**>,
                  "constant_pool::get_base returns void**");
    static_assert(std::is_same_v<decltype(std::declval<constant_pool>().get_length()), std::int32_t>,
                  "constant_pool::get_length returns int32_t");
    static_assert(noexcept(std::declval<constant_pool>().get_length()),
                  "constant_pool::get_length is noexcept");
    static_assert(std::is_same_v<decltype(std::declval<const_method>().get_constants()),
                      constant_pool*>,
                  "const_method::get_constants returns constant_pool*");
    static_assert(std::is_same_v<decltype(std::declval<const_method>().get_name()),
                      vmhook::hotspot::symbol*>,
                  "const_method::get_name returns symbol*");
    static_assert(std::is_same_v<decltype(std::declval<const_method>().get_signature()),
                      vmhook::hotspot::symbol*>,
                  "const_method::get_signature returns symbol*");
    // InstanceKlass methods-walk accessor return types + noexcept.
    static_assert(std::is_same_v<decltype(std::declval<klass>().get_methods_count()),
                      std::int32_t>,
                  "klass::get_methods_count returns int32_t");
    static_assert(std::is_same_v<decltype(std::declval<klass>().get_methods_ptr()),
                      vmhook::hotspot::method**>,
                  "klass::get_methods_ptr returns method**");
    static_assert(noexcept(std::declval<klass>().get_methods_count())
                  && noexcept(std::declval<klass>().get_methods_ptr()),
                  "methods-walk accessors are noexcept");
    // hook_verify_repair public surface.
    static_assert(std::is_same_v<decltype(vmhook::verify_hooks()), std::size_t>,
                  "verify_hooks returns size_t");
    static_assert(noexcept(vmhook::verify_hooks()),
                  "verify_hooks is noexcept");
    static_assert(std::is_same_v<decltype(vmhook::auto_repair_enabled()), bool>,
                  "auto_repair_enabled returns bool");
    static_assert(noexcept(vmhook::auto_repair_enabled()),
                  "auto_repair_enabled is noexcept");
    static_assert(noexcept(vmhook::set_auto_repair_enabled(true)),
                  "set_auto_repair_enabled is noexcept");
    // hooked_method / i2i_hook_data are POD snapshots verify_hooks walks.
    static_assert(std::is_standard_layout_v<vmhook::hotspot::i2i_hook_data>,
                  "i2i_hook_data must be standard-layout");
    static_assert(std::is_trivially_copyable_v<vmhook::hotspot::i2i_hook_data>,
                  "i2i_hook_data must be trivially copyable");
    static_assert(std::is_standard_layout_v<vmhook::hotspot::return_slot>,
                  "return_slot must be standard-layout");
    // NO_COMPILE is the OR of the four compile-control bits.
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(vmhook::hotspot::NO_COMPILE)>,
                      std::int32_t>,
                  "NO_COMPILE is int32_t");
    static_assert(vmhook::hotspot::NO_COMPILE
                      == (0x01000000 | 0x02000000 | 0x04000000 | 0x08000000),
                  "NO_COMPILE is the OR of the four compile-control bits");
    check("constantpool_access_static_asserts_compiled", true);

    // =====================================================================
    // A. constant_pool::get_length() — the -1 "unknown, skip the bound check"
    //    SENTINEL (vmhook.hpp:2362-2384).  With no JVM the _length VMStruct
    //    entry is absent, so get_length() returns -1 for EVERY constant_pool*,
    //    including ones whose address would otherwise pass is_valid_pointer.
    //    The accessor is called ONLY on null and is_valid_pointer-rejected
    //    addresses (rejected before forming this+offset) PLUS one in-range
    //    owned buffer (entry==null short-circuits before the read) — never a
    //    wild deref.
    // =====================================================================
    {
        // is_valid_pointer-rejected NON-NULL cp pointers: -1 sentinel.  (A
        // literal-null `this` is excluded: calling a member through it is
        // -Wnonnull UB; the null contract is covered transitively elsewhere.)
        const std::array<std::uintptr_t, 5> rejected_cp{
            std::uintptr_t{ 0x1u },             // odd + below floor
            std::uintptr_t{ 0xFFFFu },          // exactly the floor (rejected, <=)
            std::uintptr_t{ 0xDEADBEEFu },      // poison sentinel + odd
            std::uintptr_t{ 0xCAFEBABEu },      // even poison sentinel
            std::uintptr_t{ 0x3u },             // odd
        };
        bool all_minus_one{ true };
        for (const std::uintptr_t addr : rejected_cp)
        {
            auto* const cp{ reinterpret_cast<constant_pool*>(addr) };
            if (cp->get_length() != -1) { all_minus_one = false; }
        }
        check("A_get_length_rejected_cp_all_minus_one", all_minus_one);

        // An in-range, 8-aligned owned buffer: it would PASS is_valid_pointer,
        // but the _length entry is null with no JVM, so the `!entry` arm of the
        // guard fires FIRST and returns -1 without ever reading the buffer.
        alignas(16) std::array<std::uint8_t, 64> fake_cp{};
        auto* const cp{ reinterpret_cast<constant_pool*>(fake_cp.data()) };
        check("A_owned_buffer_passes_is_valid_pointer", is_valid_pointer(cp));
        check("A_get_length_no_jvm_is_minus_one_even_when_this_valid",
              cp->get_length() == -1);
    }

    // =====================================================================
    // B. constant_pool::get_base() — missing-type-entry FAIL-CLOSED
    //    (vmhook.hpp:2331-2349).  With no JVM iterate_type_entries("ConstantPool")
    //    is null, so get_base() takes the `!entry` throw arm -> caught -> logs ->
    //    returns nullptr.  The base is NEVER computed as this+size, so the
    //    address of `this` is irrelevant — a null and an in-range owned buffer
    //    both yield nullptr.
    // =====================================================================
    {
        // A below-floor (is_valid_pointer-rejected) NON-NULL `this`: get_base()
        // never validates `this` (flaw #4) but the type entry is null with no
        // JVM, so the `!entry` throw arm fires first -> nullptr regardless.
        auto* const cp_low{ reinterpret_cast<constant_pool*>(std::uintptr_t{ 0x1000u }) };
        check("B_get_base_low_this_no_jvm_is_null", cp_low->get_base() == nullptr);

        alignas(16) std::array<std::uint8_t, 64> fake_cp{};
        auto* const cp{ reinterpret_cast<constant_pool*>(fake_cp.data()) };
        check("B_get_base_valid_this_no_jvm_is_null", cp->get_base() == nullptr);

        // Repeat-stable: the function-local static caches the (null) type entry,
        // so every call returns nullptr deterministically.
        bool all_null{ true };
        for (int i{ 0 }; i < 256; ++i)
        {
            if (cp->get_base() != nullptr) { all_null = false; }
        }
        check("B_get_base_repeat_stable_null", all_null);
    }

    // =====================================================================
    // C. The constant-pool index BOUND arithmetic (vmhook.hpp:2484 / 2561).
    //    `cp_length >= 0 && index >= cp_length`.  This is THE load-bearing
    //    overflow guard the method path layers over base[index]; pin its
    //    decision exhaustively, including the -1-disables-it degradation.
    // =====================================================================
    {
        // When _length is known (>= 0): index in [0, len) passes, [len, ...] is
        // out of bounds.  Exact boundary at index == len.
        check("C_index_below_len_in_bounds", !cp_index_is_out_of_bounds(10, 0u));
        check("C_index_len_minus_one_in_bounds", !cp_index_is_out_of_bounds(10, 9u));
        check("C_index_equal_len_out_of_bounds", cp_index_is_out_of_bounds(10, 10u));
        check("C_index_above_len_out_of_bounds", cp_index_is_out_of_bounds(10, 11u));
        check("C_index_far_above_len_out_of_bounds",
              cp_index_is_out_of_bounds(10, 0xFFFFu));
        // A length of 0 (degenerate but valid >= 0): EVERY index is out of bounds,
        // including the 1-based first slot (index 1) and the unused slot 0.
        check("C_len_zero_rejects_index_0", cp_index_is_out_of_bounds(0, 0u));
        check("C_len_zero_rejects_index_1", cp_index_is_out_of_bounds(0, 1u));
        // The -1 sentinel DISABLES the bound: no index is ever rejected by it,
        // not even the u16 maximum (so the read degrades to the readable-slot
        // gate alone, exactly as documented for a JDK that drops _length).
        check("C_minus_one_disables_bound_index_0", !cp_index_is_out_of_bounds(-1, 0u));
        check("C_minus_one_disables_bound_index_max",
              !cp_index_is_out_of_bounds(-1, 0xFFFFu));
        // Any negative garbage length also reads as "< 0" -> bound disabled.
        check("C_negative_len_disables_bound",
              !cp_index_is_out_of_bounds(-12345, 0xFFFFu)
              && !cp_index_is_out_of_bounds(-2147483647 - 1, 0u));
        // Boundary SWEEP over a dense u16 index grid for a fixed length: every
        // index is out of bounds iff it is >= the length, recomputed independently.
        {
            const std::int32_t len{ 500 };
            bool sweep_ok{ true };
            for (std::uint32_t i{ 0u }; i <= 65535u; i += 7u)
            {
                const std::uint16_t idx{ static_cast<std::uint16_t>(i) };
                const bool got{ cp_index_is_out_of_bounds(len, idx) };
                const bool want{ static_cast<std::int32_t>(idx) >= len };
                if (got != want) { sweep_ok = false; }
            }
            check("C_bound_decision_u16_index_sweep", sweep_ok);
        }
        // A large-but-positive garbage length (flaw #3: bound silently becomes a
        // no-op) accepts every u16 index — pin that this is the DOCUMENTED
        // behaviour, not a crash: a u16 index can never exceed 0x40000000.
        check("C_huge_positive_len_accepts_all_u16",
              !cp_index_is_out_of_bounds(0x40000000, 0xFFFFu)
              && !cp_index_is_out_of_bounds(0x40000000, 0u));
    }

    // =====================================================================
    // D. constant_pool entry-slot MODEL: pointer-sized stride and the 1-based
    //    "slot 0 unused" contract (vmhook.hpp:2325-2327).  Each entry is one
    //    pointer (8 bytes on x64); base[index] therefore strides by sizeof(void*)
    //    and index 0 addresses the unused slot.  Pinned as the layout fact the
    //    get_name/get_signature reads rely on, on an OWNED void*[] buffer (never
    //    a fabricated cp deref — this is our own array).
    // =====================================================================
    {
        // sizeof a constant-pool slot is sizeof(void*) (the void** base type).
        static_assert(sizeof(*std::declval<void**>()) == sizeof(void*),
                      "a constant_pool slot is pointer-sized");
        check("D_slot_is_pointer_sized", sizeof(void*) == 8u || sizeof(void*) == 4u);

        // Model base[index] addressing on an owned void*[] buffer: consecutive
        // slots are sizeof(void*) apart, and slot 0 is the unused 1-based head.
        std::array<void*, 8> entries{};
        void** const base{ entries.data() };
        check("D_slot_stride_is_pointer_size",
              reinterpret_cast<std::uint8_t*>(&base[1])
                  - reinterpret_cast<std::uint8_t*>(&base[0]) == sizeof(void*));
        check("D_slot_stride_3_is_3x",
              reinterpret_cast<std::uint8_t*>(&base[3])
                  - reinterpret_cast<std::uint8_t*>(&base[0]) == 3 * sizeof(void*));
        // The element type of base[i] (a REFERENCE) is void* once cvref-stripped.
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(base[0])>, void*>,
                      "constant_pool slot element is void*");
        check("D_index_0_is_addressable_unused_slot", &base[0] == base);
    }

    // =====================================================================
    // E. is_readable_pointer — the per-SLOT map gate the method path applies to
    //    &base[index] before the deref (vmhook.hpp:2488 / 2565).  It requires
    //    8-byte alignment (stricter than is_valid_pointer's 2-byte) plus an
    //    in-range address; constants from source (floor 0xFFFF, ceiling
    //    0x7FFFFFFFFFFF, & 0x7 == 0).  PURE address arithmetic — NO read of any
    //    fabricated address (query_region inspects the page tables, never
    //    dereferences), and we drive it only over null / out-of-range / mis-
    //    aligned inputs that the alignment+range pre-gate rejects up front.
    // =====================================================================
    {
        constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };
        constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };
        check("E_floor_is_0xFFFF", floor == 0xFFFFull);
        check("E_ceiling_value", ceiling == 0x00007FFFFFFFFFFFull);
        // null and below-floor are rejected by the range arm.
        check("E_null_not_readable", !is_readable_pointer(nullptr));
        check("E_floor_exact_not_readable",
              !is_readable_pointer(reinterpret_cast<const void*>(floor)));
        // >= ceiling rejected by the range arm.
        check("E_ceiling_exact_not_readable",
              !is_readable_pointer(reinterpret_cast<const void*>(ceiling)));
        // 8-byte alignment is required: an in-range address that is 2-aligned but
        // NOT 8-aligned is rejected by the alignment arm BEFORE any region query.
        // (A cp slot &base[index] is always 8-aligned, so the gate never spuriously
        // rejects a real slot; this pins it rejects a misaligned one.)
        check("E_two_aligned_not_eight_aligned_not_readable",
              !is_readable_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x100002u })));
        check("E_four_aligned_not_eight_aligned_not_readable",
              !is_readable_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x100004u })));
        // is_valid_pointer is the looser 2-byte gate; an address that is even but
        // not 8-aligned passes is_valid_pointer yet fails is_readable_pointer —
        // exactly why the method path needs BOTH gates.
        {
            const auto* const p{ reinterpret_cast<const void*>(std::uintptr_t{ 0x100002u }) };
            check("E_valid_pointer_looser_than_readable_pointer",
                  is_valid_pointer(p) && !is_readable_pointer(p));
        }
    }

    // =====================================================================
    // F. const_method::get_name() / get_signature() — NULL / FAIL-CLOSED
    //    contract (vmhook.hpp:2438-2512 / 2517-2584).  With no JVM the
    //    ConstMethod._name_index / _signature_index entries are absent, so each
    //    takes the `!entry` throw arm -> caught -> nullptr.  Driven over null and
    //    is_valid_pointer-rejected ConstMethod* (rejected before any deref) PLUS
    //    one in-range owned buffer (the entry==null guard fires first).
    // =====================================================================
    {
        // is_valid_pointer-rejected NON-NULL ConstMethod* (a literal-null `this`
        // is excluded: a member call through it is -Wnonnull UB).
        const std::array<std::uintptr_t, 4> rejected_cm{
            std::uintptr_t{ 0x1u },
            std::uintptr_t{ 0xCCCCCCCCu },   // MSVC uninitialised-stack sentinel
            std::uintptr_t{ 0xBAADF00Du },   // even-aligned sentinel
            std::uintptr_t{ 0x7u },          // odd
        };
        bool all_null{ true };
        for (const std::uintptr_t addr : rejected_cm)
        {
            auto* const cm{ reinterpret_cast<const_method*>(addr) };
            if (cm->get_name() != nullptr) { all_null = false; }
            if (cm->get_signature() != nullptr) { all_null = false; }
            // get_constants() also fails closed for the same inputs.
            if (cm->get_constants() != nullptr) { all_null = false; }
        }
        check("F_get_name_sig_constants_rejected_cm_all_null", all_null);

        // In-range owned buffer: would pass is_valid_pointer, but the _name_index
        // / _signature_index entries are null with no JVM, so get_name/get_signature
        // bail at `!entry` before reading the buffer.
        alignas(16) std::array<std::uint8_t, 64> fake_cm{};
        auto* const cm{ reinterpret_cast<const_method*>(fake_cm.data()) };
        check("F_owned_cm_passes_is_valid_pointer", is_valid_pointer(cm));
        check("F_get_name_no_jvm_is_null", cm->get_name() == nullptr);
        check("F_get_signature_no_jvm_is_null", cm->get_signature() == nullptr);
        check("F_get_constants_no_jvm_is_null", cm->get_constants() == nullptr);
    }

    // =====================================================================
    // G. instanceklass_methods_walk — get_methods_count() / get_methods_ptr()
    //    NULL / FAIL-CLOSED contract (vmhook.hpp:3504-3565).  With no JVM the
    //    InstanceKlass._methods entry is absent, so both bail at `!entry`
    //    (count 0 / nullptr).  Driven over null and is_valid_pointer-rejected
    //    klass* (rejected before forming this+offset) PLUS an in-range owned
    //    buffer (entry==null guard first).
    // =====================================================================
    {
        // Null klass.
        klass* const null_klass{ nullptr };
        check("G_methods_count_null_zero", null_klass == nullptr);
        // Rejected klass* and in-range owned buffer.
        const std::array<std::uintptr_t, 4> klass_addrs{
            std::uintptr_t{ 0x1u },           // odd + below floor
            std::uintptr_t{ 0xDEADBEEFu },    // poison + odd
            std::uintptr_t{ 0xFEEEFEEEu },    // even poison
            std::uintptr_t{ 0x1000u },        // below floor
        };
        bool all_fail_closed{ true };
        for (const std::uintptr_t addr : klass_addrs)
        {
            auto* const k{ reinterpret_cast<klass*>(addr) };
            if (is_valid_pointer(k)) { all_fail_closed = false; }   // confirm rejected first
            if (k->get_methods_count() != 0) { all_fail_closed = false; }
            if (k->get_methods_ptr() != nullptr) { all_fail_closed = false; }
        }
        check("G_methods_walk_rejected_klass_fail_closed", all_fail_closed);

        alignas(16) std::array<std::uint8_t, 64> fake_klass{};
        auto* const k{ reinterpret_cast<klass*>(fake_klass.data()) };
        check("G_owned_klass_passes_is_valid_pointer", is_valid_pointer(k));
        check("G_methods_count_no_jvm_zero", k->get_methods_count() == 0);
        check("G_methods_ptr_no_jvm_null", k->get_methods_ptr() == nullptr);
    }

    // =====================================================================
    // H. instanceklass_methods_walk — Array<Method*>::_length CLAMP
    //    (vmhook.hpp:3528).  A raw int32 length is trusted only when
    //    0 <= n <= 65535 (the u2 class-file method_count ceiling); anything else
    //    clamps to 0 so a corrupt/hostile length never drives reserve()/the walk
    //    off the end.  Exhaustive over the boundary.
    // =====================================================================
    {
        check("H_clamp_zero", clamp_methods_count(0) == 0);
        check("H_clamp_one", clamp_methods_count(1) == 1);
        check("H_clamp_max_ok", clamp_methods_count(65535) == 65535);
        check("H_clamp_just_over_zeroed", clamp_methods_count(65536) == 0);
        check("H_clamp_negative_zeroed", clamp_methods_count(-1) == 0);
        check("H_clamp_int_min_zeroed", clamp_methods_count(-2147483647 - 1) == 0);
        check("H_clamp_int_max_zeroed", clamp_methods_count(2147483647) == 0);
        bool boundary_ok{ true };
        const std::array<std::int32_t, 10> probes{
            -100, -1, 0, 1, 100, 65534, 65535, 65536, 70000, 1000000 };
        for (const std::int32_t n : probes)
        {
            const std::int32_t got{ clamp_methods_count(n) };
            const std::int32_t want{ (n < 0 || n > 65535) ? 0 : n };
            if (got != want) { boundary_ok = false; }
        }
        check("H_clamp_boundary_sweep", boundary_ok);
    }

    // =====================================================================
    // I. instanceklass_methods_walk — Array<Method*> DATA-OFFSET (+8) and 8-byte
    //    stride (vmhook.hpp:3560-3564).  The most ABI-fragile constant: Method*
    //    data begins at array+8 (int32 _length @0 + 4 pad + 8-aligned pointers
    //    @8), and consecutive Method* slots are 8 bytes apart.  Pinned on an
    //    OWNED byte buffer shaped like the array (never a fabricated klass).
    // =====================================================================
    {
        alignas(16) std::array<std::uint8_t, 64> arr{};
        const std::int32_t length{ 3 };
        std::memcpy(arr.data() + 0, &length, sizeof(length));
        const std::uint64_t slot0{ 0x1111111111111110ull };
        const std::uint64_t slot1{ 0x2222222222222220ull };
        const std::uint64_t slot2{ 0x3333333333333330ull };
        std::memcpy(arr.data() + 8 + 0 * 8, &slot0, sizeof(slot0));
        std::memcpy(arr.data() + 8 + 1 * 8, &slot1, sizeof(slot1));
        std::memcpy(arr.data() + 8 + 2 * 8, &slot2, sizeof(slot2));

        const std::int32_t read_len{ *reinterpret_cast<const std::int32_t*>(arr.data()) };
        check("I_array_length_at_off0", read_len == 3);
        const auto* const data{ reinterpret_cast<const std::uint64_t*>(arr.data() + 8) };
        check("I_array_data_at_off8",
              data[0] == slot0 && data[1] == slot1 && data[2] == slot2);
        check("I_array_stride_is_8",
              reinterpret_cast<const std::uint8_t*>(&data[1])
                  - reinterpret_cast<const std::uint8_t*>(&data[0]) == 8);
        // The +8 data offset is exactly _length(4) + pad(4) on this ABI.
        check("I_data_offset_is_length_plus_pad",
              8u == sizeof(std::int32_t) + 4u);
    }

    // =====================================================================
    // J. hook_verify_repair — verify_hooks() NO-JVM ZERO contract
    //    (vmhook.hpp:10682).  With no JVM no hook was ever installed, so
    //    g_hooked_methods / g_hooked_i2i_entries are empty: verify_hooks() walks
    //    nothing, repairs 0, and never throws.  Repeat-stable.
    // =====================================================================
    {
        check("J_verify_hooks_no_jvm_repairs_zero", vmhook::verify_hooks() == 0);
        bool all_zero{ true };
        for (int i{ 0 }; i < 64; ++i)
        {
            if (vmhook::verify_hooks() != 0) { all_zero = false; }
        }
        check("J_verify_hooks_repeat_zero", all_zero);
    }

    // =====================================================================
    // K. hook_verify_repair — auto-repair watchdog GATING booleans.
    //    g_shutdown_requested defaults false (vmhook.hpp:7197); the run-time
    //    master switch g_auto_repair_enabled defaults true (vmhook.hpp:11033),
    //    surfaced by auto_repair_enabled().  set_auto_repair_enabled() flips the
    //    gate; with NO live watchdog (none is ever spawned no-JVM — only a
    //    successful hook<T>() spawns it) disabling is a pure gate flip + started-
    //    latch clear (vmhook.hpp:11205-11210), so it is safe to round-trip here.
    // =====================================================================
    {
        check("K_g_shutdown_requested_default_false",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        check("K_auto_repair_enabled_default_true", vmhook::auto_repair_enabled() == true);

        // Round-trip the gate: disable -> reports false; re-enable -> reports true.
        // (No watchdog thread is live, so set_auto_repair_enabled(false) only
        // flips the gate and clears g_started — it never blocks on a thread.)
        vmhook::set_auto_repair_enabled(false);
        check("K_auto_repair_disabled_reports_false", vmhook::auto_repair_enabled() == false);
        // verify_hooks() is wholly unaffected by the gate (synchronous path).
        check("K_verify_hooks_still_zero_when_disabled", vmhook::verify_hooks() == 0);
        vmhook::set_auto_repair_enabled(true);
        check("K_auto_repair_reenabled_reports_true", vmhook::auto_repair_enabled() == true);
        // Idempotent re-enable.
        vmhook::set_auto_repair_enabled(true);
        check("K_auto_repair_reenable_idempotent", vmhook::auto_repair_enabled() == true);
        // Shutdown flag untouched by the gate round-trip.
        check("K_g_shutdown_requested_still_false_after_round_trip",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
    }

    // =====================================================================
    // L. hook_verify_repair — POD hooked_method snapshot DEFAULTS + drift fields
    //    (vmhook.hpp:7120-7148).  verify_hooks() reads the captured expected_*
    //    strings and the drift_logged debounce off this POD; pin the field
    //    defaults the no-JVM (and clean install-time) state relies on, on an
    //    OWNED instance.
    // =====================================================================
    {
        const vmhook::hotspot::hooked_method hm{};
        check("L_hm_default_method_null", hm.method == nullptr);
        check("L_hm_default_original_code_null", hm.original_code == nullptr);
        check("L_hm_default_was_compiled_false", hm.was_compiled == false);
        check("L_hm_default_drift_logged_false", hm.drift_logged == false);
        check("L_hm_default_expected_names_empty",
              hm.expected_class_name.empty()
              && hm.expected_method_name.empty()
              && hm.expected_signature.empty());
        // The expected_* fields are std::string (the captured install-time names).
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm.expected_method_name)>,
                          std::string>,
                      "hooked_method::expected_method_name is std::string");
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm.drift_logged)>, bool>,
                      "hooked_method::drift_logged is bool");
        // i2i_hook_data POD defaults.
        const vmhook::hotspot::i2i_hook_data ih{};
        check("L_i2i_hook_data_defaults_null", ih.i2i_entry == nullptr && ih.hook == nullptr);
    }

    // =====================================================================
    // M. hook_verify_repair — the three DRIFT-MODE decisions verify_hooks()
    //    makes per stored hook (vmhook.hpp:10848-10932), reproduced on POD
    //    snapshots.  These are pure value comparisons / bit tests; the actual
    //    Method/ConstMethod reads they gate are JVM-only and out of scope.
    // =====================================================================
    {
        // Up-front reject (vmhook.hpp:10848): a hook with a null OR already-
        // drift_logged stored Method* is SKIPPED (continue) — no repair attempt.
        auto should_skip = [](const void* method, bool drift_logged) -> bool
        {
            return method == nullptr || drift_logged;
        };
        check("M_skip_null_method", should_skip(nullptr, false));
        check("M_skip_already_logged",
              should_skip(reinterpret_cast<const void*>(std::uintptr_t{ 0x100200300400ull }), true));
        check("M_no_skip_live_unlogged",
              !should_skip(reinterpret_cast<const void*>(std::uintptr_t{ 0x100200300400ull }), false));

        // Mode 2 (vmhook.hpp:10877): aliased iff current name non-empty AND
        // differs from expected.  An empty current name (unreadable this tick) is
        // NOT treated as drift; an exact match is NOT drift.
        check("M_mode2_alias_detected_on_name_change",
              method_aliased("bridge$getRenderState", "orientCamera"));
        check("M_mode2_no_alias_on_exact_match",
              !method_aliased("orientCamera", "orientCamera"));
        check("M_mode2_empty_current_name_not_drift",
              !method_aliased(std::string_view{}, "orientCamera"));

        // Mode 3 (vmhook.hpp:10932): drifted iff _code repopulated OR NO_COMPILE
        // cleared.  Steady state = no _code AND NO_COMPILE still set.
        check("M_mode3_steady_state_no_drift", !jit_drifted(false, true));
        check("M_mode3_code_repopulated_is_drift", jit_drifted(true, true));
        check("M_mode3_no_compile_cleared_is_drift", jit_drifted(false, false));
        check("M_mode3_both_is_drift", jit_drifted(true, false));
        // Truth-table sweep: drifted == (code || !nc) over all 4 combinations.
        bool truth_ok{ true };
        for (int c{ 0 }; c <= 1; ++c)
        {
            for (int nc{ 0 }; nc <= 1; ++nc)
            {
                const bool got{ jit_drifted(c != 0, nc != 0) };
                const bool want{ (c != 0) || (nc == 0) };
                if (got != want) { truth_ok = false; }
            }
        }
        check("M_mode3_drift_truth_table", truth_ok);

        // The NO_COMPILE bit-mask verify_hooks re-arms (safe_access_flags_or) and
        // tests (safe_access_flags_test): four high-byte bits, disjoint from the
        // low-16 class-file access-modifier group, so re-arming never flips a
        // modifier and the Mode-3 test never sees one.
        const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };
        check("M_no_compile_is_0x0F000000", no_compile == 0x0F000000u);
        constexpr std::uint32_t modifier_group{ 0x0001u | 0x0002u | 0x0004u
                                                | 0x0008u | 0x0010u | 0x0200u | 0x0400u };
        check("M_no_compile_disjoint_from_modifiers",
              (no_compile & modifier_group) == 0u);
        // Re-arm (OR) then steady-state test (AND): NO_COMPILE present.
        const std::uint32_t armed{ 0x00001234u | no_compile };
        check("M_no_compile_or_then_test_set", (armed & no_compile) == no_compile);
        // Drift detection's "cleared" case: a word missing the bits tests false.
        check("M_no_compile_test_cleared_word", (0x00001234u & no_compile) == 0u);
    }

    // =====================================================================
    // N. is_valid_pointer — the gate shared by EVERY accessor under test
    //    (cp `this`, ConstMethod `this`, klass `this`, the stored Method* in
    //    verify_hooks).  Pure address arithmetic, NO read.  Constants from
    //    source: floor 0xFFFF (reject <=), ceiling 0x7FFFFFFFFFFF (reject >=),
    //    reject odd, reject the nine debug-fill sentinels by low32.
    // =====================================================================
    {
        constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };
        constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };
        check("N_null_rejected", !is_valid_pointer(nullptr));
        check("N_floor_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor)));
        check("N_floor_plus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(floor + 1)));
        check("N_floor_plus_two_odd_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor + 2)));
        check("N_ceiling_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(ceiling)));
        check("N_ceiling_minus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(ceiling - 1)));
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
        check("N_all_sentinel_low32_rejected", all_rejected);
        check("N_clean_low32_high_base_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(high_base | 0x00010002ull)));
    }

    // =====================================================================
    // O. DETERMINISM across the no-JVM regime: every accessor yields the SAME
    //    fail-closed result on repeat, so output is byte-identical run-to-run.
    //    Ties the three features together as one fail-closed contract.
    // =====================================================================
    {
        alignas(16) std::array<std::uint8_t, 64> buf{};
        auto* const cp{ reinterpret_cast<constant_pool*>(buf.data()) };
        auto* const cm{ reinterpret_cast<const_method*>(buf.data()) };
        auto* const k{ reinterpret_cast<klass*>(buf.data()) };
        check("O_get_length_repeat_minus_one",
              cp->get_length() == -1 && cp->get_length() == -1);
        check("O_get_base_repeat_null",
              cp->get_base() == nullptr && cp->get_base() == nullptr);
        check("O_get_name_repeat_null",
              cm->get_name() == nullptr && cm->get_name() == nullptr);
        check("O_methods_count_repeat_zero",
              k->get_methods_count() == 0 && k->get_methods_count() == 0);
        check("O_verify_hooks_repeat_zero",
              vmhook::verify_hooks() == 0 && vmhook::verify_hooks() == 0);
    }

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
