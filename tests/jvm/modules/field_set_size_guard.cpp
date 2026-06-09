// field_set_size_guard JVM test module — area: fields.
//
// THE size/type-guard + anti-clobber authority for field_proxy::set()
// (vmhook.hpp ~11956-12091, audit/findings/field_proxy_set_size_guard.md).
//
// field_proxy::set() writes a C++ value into a field's raw storage.  Its only
// runtime safety net is a SIZE guard inside the trivially-copyable branch:
//   * the field's JVM width comes from jvm_primitive_byte_width(sig)
//     (Z/B=1, S/C=2, I/F=4, J/D=8; 0 for reference/array/unknown), and
//   * if that width is non-zero and != sizeof(value), the write is REFUSED
//     (no memcpy) with a diagnostic — so a too-WIDE C++ value can never spill
//     past a narrow slot into the adjacent field, and a too-NARROW value can
//     never leave the high bytes of a wide slot stale.
//   * a symmetric guard at the top refuses string / vector / unique_ptr writes
//     into a primitive field (those would otherwise reinterpret the field's
//     bytes as a compressed OOP).
//   * a "C" + 1-byte-value shortcut widens a C++ char to the full 2-byte Java
//     char before writing.
//
// What this module proves on a LIVE JVM (every JDK x MSVC/Clang/GCC), via the
// modular harness API only (register_class / static_field / get_field / set /
// run_probe) — distinct from every sibling field module:
//
//   (1) CORRECT-WIDTH ROUND-TRIP for EVERY primitive width (Z B C S I J F D)
//       plus a reference field: each native write lands and is read back both
//       natively AND through the JVM's own getfield/getstatic (the mode-1
//       "seen*" snapshot) and Java getters (static_method calls).
//
//   (2) GUARD REJECTION: a too-WIDE write (set(int64) into "I"/"B"/"S"), a
//       too-NARROW write (set(int32) into "J"/"D"), and a NON-PRIMITIVE write
//       (set(std::string) / set(std::vector<int>) / set(unique_ptr<wrapper>)
//       into a primitive) are ALL refused — the field is byte-for-byte
//       unchanged, proven natively and Java-side.
//
//   (3) ANTI-CLOBBER (the headline): each clobber target is declared between
//       two same-width sentinels (Before/clob/After).  The module reads
//       raw_address() of all three, proves they are CONTIGUOUS in the object
//       layout, then shows that a too-wide (refused) write to the middle leaves
//       BOTH sentinels intact — i.e. the guard is what prevents an 8-byte write
//       into a 4-byte slot from smashing the neighbour.  If a given JDK lays the
//       trio non-adjacently, the strong assertion degrades to an [INFO] note and
//       the per-field "unchanged" assertions still run (never a spurious FAIL,
//       never a JVM crash).
//
//   (4) "C" WIDENING writes EXACTLY two bytes (full Java char, high byte zero),
//       not one and not three — pinned by char sentinels on both sides.
//
//   (5) TYPE-CONFUSION CHARACTERISATION (a guard LIMITATION, not a bug this test
//       can fix): set(float) into "I" and set(int32) into "F" are SAME width, so
//       the size guard passes and the IEEE-754 / two's-complement bit pattern is
//       reinterpreted verbatim.  The module asserts the ACTUAL bytes that land
//       and records an [INFO] so a future signature-aware type check is flagged.
//
//   (6) REFERENCE-vs-PRIMITIVE: a unique_ptr<wrapper> write into a reference
//       field round-trips (identity flips, null nulls), while the same
//       unique_ptr into a primitive field is refused (point 2).
//
//   (7) NULL field_pointer set() is a safe no-op (no crash, nothing written).
//
// Harness shape mirrors field_static: register_class, a `mode` selector with a
// `done` reset on the rising edge of go, a dense ctx.check() battery.  Every
// accessor is a STATIC wrapper method reaching the field via static_field /
// static_method (the GCC-portable path; deducing-this get_field overloads are
// non-viable from a static context).  ALL value_t extraction is COPY-INIT (=)
// to stay MSVC-unambiguous.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.FieldSetGuard.
    //
    // Every accessor is a STATIC method routed through static_field / get_field
    // on an explicit instance — never the deducing-this get_field from a static
    // context (which would not compile on GCC).
    class fsg : public vmhook::object<fsg>
    {
    public:
        explicit fsg(vmhook::oop_t instance) noexcept
            : vmhook::object<fsg>{ instance }
        {
        }

        // ── handshake + scenario selector (all via static_field) ──────────
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // ── resolve helper ────────────────────────────────────────────────
        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // ── generic typed SET via a static field (proves set lands per width) ─
        template<typename value_type>
        static auto set_static(const char* name, const value_type& v) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(v);
            return true;
        }

        // ── typed static GETs ──────────────────────────────────────────────
        static auto get_bool(const char* name) -> bool          { return static_field(name)->get(); }
        static auto get_i8(const char* name) -> std::int8_t      { return static_field(name)->get(); }
        static auto get_i16(const char* name) -> std::int16_t    { return static_field(name)->get(); }
        static auto get_i32(const char* name) -> std::int32_t    { return static_field(name)->get(); }
        static auto get_i64(const char* name) -> std::int64_t    { return static_field(name)->get(); }
        static auto get_u16(const char* name) -> std::uint16_t   { return static_field(name)->get(); }
        static auto get_float(const char* name) -> float         { return static_field(name)->get(); }
        static auto get_double(const char* name) -> double       { return static_field(name)->get(); }
        static auto get_string(const char* name) -> std::string  { return static_field(name)->get(); }

        // ── acquire a published instance wrapper (instance / refA / refB / refSlot) ─
        static auto acquire(const char* name) -> std::unique_ptr<fsg> { return static_field(name)->get(); }

        // ── set an object-reference static field to a wrapper (or null) ───
        static auto set_ref(const char* name, const std::unique_ptr<fsg>& target) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return false; }
            proxy->set(target);
            return true;
        }

        // ── Java getters via static_method (visible-to-bytecode proof) ────
        static auto call_get_i32(const char* method) -> std::int32_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return -1; }
            const std::int32_t v = m->call();
            return v;
        }
        static auto call_get_i64(const char* method) -> std::int64_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return -1; }
            const std::int64_t v = m->call();
            return v;
        }
        static auto call_get_bool(const char* method) -> bool
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return false; }
            const bool v = m->call();
            return v;
        }

        // ── instance tag (distinguish the published references) ───────────
        auto tag() const -> std::int32_t
        {
            const std::int32_t v = get_field("tag")->get();
            return v;
        }

        // ── a guard-target instance field proxy by name (for adjacency reads) ─
        auto field(const char* name) const -> std::optional<vmhook::field_proxy>
        {
            return get_field(name);
        }
    };

    // value_t variant-alternative indices (must match field_proxy::value_t order).
    constexpr std::size_t kIdxBool   = 0;
    constexpr std::size_t kIdxI8     = 1;
    constexpr std::size_t kIdxI16    = 2;
    constexpr std::size_t kIdxI32    = 3;
    constexpr std::size_t kIdxI64    = 4;
    constexpr std::size_t kIdxFloat  = 5;
    constexpr std::size_t kIdxDouble = 6;
    constexpr std::size_t kIdxU16    = 7;

    auto float_bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{};
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    auto double_bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{};
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }
    auto bits_to_float(std::uint32_t b) noexcept -> float
    {
        float f{};
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }
    auto bits_to_double(std::uint64_t b) noexcept -> double
    {
        double d{};
        std::memcpy(&d, &b, sizeof(d));
        return d;
    }

    // Known bit patterns the native side writes (so Java's raw-bits snapshot can
    // be matched exactly).
    constexpr std::uint32_t k_okF_bits{ 0x3FC00000u };             // 1.5f
    constexpr std::uint64_t k_okD_bits{ 0x400921FB54442D18ULL };   // Math.PI
    constexpr std::uint32_t k_conf_float_bits{ 0x3FC00000u };      // 1.5f, written into "I"
    constexpr std::uint32_t k_conf_int_as_float_bits{ 0x40490FDBu };// float-from-bits, into "F"
    constexpr std::uint64_t k_conf_double_bits{ 0x400921FB54442D18ULL }; // PI, written into "J"
    constexpr std::uint64_t k_conf_long_as_double_bits{ 0x3FF0000000000000ULL }; // 1.0, into "D"

    // Drive one probe cycle for `mode`: clears latched `done` and programs the
    // selector on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    fsg::set_done(false);
                    fsg::set_mode(mode);
                }
                fsg::set_go(value);
            },
            []() { return fsg::get_done(); });
    }

    // Read the raw field address of an instance proxy as a uintptr_t (0 if absent).
    auto addr_of(const std::optional<vmhook::field_proxy>& p) -> std::uintptr_t
    {
        if (!p) { return 0; }
        return reinterpret_cast<std::uintptr_t>(p->raw_address());
    }
}

VMHOOK_JVM_MODULE(field_set_size_guard)
{
    vmhook::register_class<fsg>("vmhook/fixtures/FieldSetGuard");

    // =====================================================================
    //  0. Sanity: the class resolves and the portable static accessor works.
    // =====================================================================
    ctx.check("fsg_class_registered_static_field_resolves", fsg::resolves("okI"));
    ctx.check("fsg_static_method_resolves", fsg::static_method("getOkI").has_value());

    // =====================================================================
    //  1. CORRECT-WIDTH SET for EVERY primitive width — written BEFORE go and
    //     read back natively (the immediate C++ round-trip).  Java visibility is
    //     proven in phase 7 (seen* snapshot) and phase 8 (getters).
    // =====================================================================
    ctx.check("ok_Z_set", fsg::set_static<bool>("okZ", true));
    ctx.check("ok_B_set", fsg::set_static<std::int8_t>("okB", static_cast<std::int8_t>(0x7E))); // 126
    ctx.check("ok_C_set", fsg::set_static<std::uint16_t>("okC", 0xBEEF));
    ctx.check("ok_S_set", fsg::set_static<std::int16_t>("okS", static_cast<std::int16_t>(0x7EEF)));
    ctx.check("ok_I_set", fsg::set_static<std::int32_t>("okI", 0x0BADF00D));
    ctx.check("ok_J_set", fsg::set_static<std::int64_t>("okJ", 0x0123456789ABCDEFLL));
    ctx.check("ok_F_set", fsg::set_static<float>("okF", 1.5F));                 // 0x3FC00000
    ctx.check("ok_D_set", fsg::set_static<double>("okD", bits_to_double(k_okD_bits)));

    // Immediate native re-read round-trip + correct variant alternative.
    {
        const auto pz{ fsg::static_field("okZ") };
        if (pz) { const auto v{ pz->get() }; const bool b = v; ctx.check("ok_Z_reread_true", b == true); ctx.check("ok_Z_variant_bool", v.data.index() == kIdxBool); }
        const auto pb{ fsg::static_field("okB") };
        if (pb) { const auto v{ pb->get() }; const std::int8_t b = v; ctx.check("ok_B_reread_7E", b == static_cast<std::int8_t>(0x7E)); ctx.check("ok_B_variant_i8", v.data.index() == kIdxI8); }
        const auto pc{ fsg::static_field("okC") };
        if (pc) { const auto v{ pc->get() }; const std::uint16_t c = v; ctx.check("ok_C_reread_BEEF", c == 0xBEEF); ctx.check("ok_C_variant_u16", v.data.index() == kIdxU16); }
        const auto ps{ fsg::static_field("okS") };
        if (ps) { const auto v{ ps->get() }; const std::int16_t s = v; ctx.check("ok_S_reread_7EEF", s == static_cast<std::int16_t>(0x7EEF)); ctx.check("ok_S_variant_i16", v.data.index() == kIdxI16); }
        const auto pi{ fsg::static_field("okI") };
        if (pi) { const auto v{ pi->get() }; const std::int32_t i = v; ctx.check("ok_I_reread_0BADF00D", i == 0x0BADF00D); ctx.check("ok_I_variant_i32", v.data.index() == kIdxI32); }
        const auto pj{ fsg::static_field("okJ") };
        if (pj) { const auto v{ pj->get() }; const std::int64_t j = v; ctx.check("ok_J_reread_full", j == 0x0123456789ABCDEFLL); ctx.check("ok_J_variant_i64", v.data.index() == kIdxI64); }
        const auto pf{ fsg::static_field("okF") };
        if (pf) { const auto v{ pf->get() }; const float f = v; ctx.check("ok_F_reread_bits", float_bits(f) == k_okF_bits); ctx.check("ok_F_variant_float", v.data.index() == kIdxFloat); }
        const auto pd{ fsg::static_field("okD") };
        if (pd) { const auto v{ pd->get() }; const double d = v; ctx.check("ok_D_reread_bits", double_bits(d) == k_okD_bits); ctx.check("ok_D_variant_double", v.data.index() == kIdxDouble); }
    }

    // A read-only String field decodes correctly (reference-vs-primitive control).
    ctx.check("ok_str_reads_guard", fsg::get_string("okStr") == "guard");

    // =====================================================================
    //  2. SIZE GUARD — too-WIDE writes REFUSED (field unchanged).
    //     set(int64) into "I"/"B"/"S" and the like must NOT memcpy: an unguarded
    //     8-byte (or 4-byte) write into a narrower slot would corrupt the field
    //     AND spill into the adjacent one.
    // =====================================================================
    ctx.check("gWideI_initial", fsg::get_i32("gWideI") == 0x11223344);
    fsg::set_static<std::int64_t>("gWideI", std::int64_t{ 0x7766554433221100LL }); // 8B -> 4B field
    ctx.check("gWideI_too_wide_refused", fsg::get_i32("gWideI") == 0x11223344);

    ctx.check("gWideB_initial", fsg::get_i8("gWideB") == static_cast<std::int8_t>(0x5A));
    fsg::set_static<std::int64_t>("gWideB", std::int64_t{ 0x7766554433221100LL }); // 8B -> 1B field
    ctx.check("gWideB_too_wide_refused", fsg::get_i8("gWideB") == static_cast<std::int8_t>(0x5A));
    // also int32 (4B) into the byte (1B) field is refused.
    fsg::set_static<std::int32_t>("gWideB", std::int32_t{ 0x09ABCDEF });
    ctx.check("gWideB_int_into_byte_refused", fsg::get_i8("gWideB") == static_cast<std::int8_t>(0x5A));

    ctx.check("gWideS_initial", fsg::get_i16("gWideS") == 0x1234);
    fsg::set_static<std::int32_t>("gWideS", std::int32_t{ 0x09ABCDEF }); // 4B -> 2B field
    ctx.check("gWideS_too_wide_refused", fsg::get_i16("gWideS") == 0x1234);

    // =====================================================================
    //  3. SIZE GUARD — too-NARROW writes REFUSED (field unchanged).
    //     set(int32) into "J"/"D" must NOT memcpy 4 bytes into an 8-byte slot
    //     (which would leave the high 4 bytes stale).
    // =====================================================================
    ctx.check("gNarrowJ_initial", fsg::get_i64("gNarrowJ") == 0x1122334455667788LL);
    fsg::set_static<std::int32_t>("gNarrowJ", std::int32_t{ 0x09ABCDEF }); // 4B -> 8B field
    ctx.check("gNarrowJ_too_narrow_refused", fsg::get_i64("gNarrowJ") == 0x1122334455667788LL);

    ctx.check("gNarrowD_initial_bits", double_bits(fsg::get_double("gNarrowD")) == 0x3FF0000000000000ULL);
    fsg::set_static<std::int32_t>("gNarrowD", std::int32_t{ 0x09ABCDEF }); // 4B -> 8B field
    ctx.check("gNarrowD_too_narrow_refused", double_bits(fsg::get_double("gNarrowD")) == 0x3FF0000000000000ULL);
    // float (4B) into the double (8B) field is also too narrow -> refused.
    fsg::set_static<float>("gNarrowD", 2.5F);
    ctx.check("gNarrowD_float_into_double_refused", double_bits(fsg::get_double("gNarrowD")) == 0x3FF0000000000000ULL);

    // =====================================================================
    //  4. NON-PRIMITIVE into PRIMITIVE — REFUSED (the symmetric guard).
    //     set(std::string) / set(std::vector<int>) / set(unique_ptr<wrapper>)
    //     into a primitive field must NOT reinterpret the field bytes as a
    //     compressed OOP — the write is refused, the field is unchanged.
    // =====================================================================
    ctx.check("gStrI_initial", fsg::get_i32("gStrI") == 0x0BADBEEF);
    {
        const auto p{ fsg::static_field("gStrI") };
        if (p) { p->set(std::string{ "99999" }); }       // std::string into "I"
        ctx.check("gStrI_string_refused", fsg::get_i32("gStrI") == 0x0BADBEEF);
        if (p) { p->set(std::string_view{ "abc" }); }    // string_view into "I"
        ctx.check("gStrI_string_view_refused", fsg::get_i32("gStrI") == 0x0BADBEEF);
        if (p) { p->set("literal"); }                    // const char* into "I"
        ctx.check("gStrI_cstr_refused", fsg::get_i32("gStrI") == 0x0BADBEEF);
    }

    ctx.check("gVecB_initial", fsg::get_i8("gVecB") == static_cast<std::int8_t>(0x33));
    {
        const auto p{ fsg::static_field("gVecB") };
        if (p) { const std::vector<int> v{ 1, 2, 3 }; p->set(v); } // std::vector<int> into "B"
        ctx.check("gVecB_vector_refused", fsg::get_i8("gVecB") == static_cast<std::int8_t>(0x33));
    }

    ctx.check("gRefI_initial", fsg::get_i32("gRefI") == 0x600DC0DE);
    {
        const auto refA{ fsg::acquire("refA") };   // a live wrapper to pass as unique_ptr
        ctx.check("gRefI_helper_refA_acquired", refA != nullptr);
        const auto p{ fsg::static_field("gRefI") };
        if (p) { p->set(refA); }                   // unique_ptr<wrapper> into "I"
        ctx.check("gRefI_unique_ptr_refused", fsg::get_i32("gRefI") == 0x600DC0DE);
        // an EMPTY unique_ptr into "I" must also be refused (not write a 0 OOP).
        if (p) { const std::unique_ptr<fsg> empty{}; p->set(empty); }
        ctx.check("gRefI_empty_unique_ptr_refused", fsg::get_i32("gRefI") == 0x600DC0DE);
    }

    // =====================================================================
    //  5. "C" 1-byte -> 2-byte WIDENING shortcut.  A C++ char (1 byte) into a
    //     "C" field (2 bytes) must land the FULL 2-byte Java char (0x00NN),
    //     never a half-written value, never sign-extended.
    // =====================================================================
    ctx.check("gCharByte_initial", fsg::get_u16("gCharByte") == 0x0000);
    fsg::set_static<char>("gCharByte", 'Z');                 // 0x5A
    ctx.check("gCharByte_widened_to_005A", fsg::get_u16("gCharByte") == 0x005A);
    fsg::set_static<char>("gCharByte", static_cast<char>(0xE9)); // high-bit byte
    ctx.check("gCharByte_high_byte_zero_extended", fsg::get_u16("gCharByte") == 0x00E9);
    // A correctly-sized uint16 into "C" also lands intact (control for widening).
    fsg::set_static<std::uint16_t>("gCharByte", 0x20AC);     // euro sign
    ctx.check("gCharByte_uint16_intact", fsg::get_u16("gCharByte") == 0x20AC);

    // =====================================================================
    //  6. TYPE-CONFUSION CHARACTERISATION (a guard LIMITATION, not a fixable
    //     bug here).  set(value) where sizeof(value) == field width but the KIND
    //     differs passes the SIZE guard and reinterprets the bit pattern.  We
    //     assert the ACTUAL bytes that land and record [INFO].  Do NOT edit
    //     vmhook.hpp; this is the documented "size guard is not a type guard".
    // =====================================================================
    ctx.record("[INFO] field_set_size_guard: field_proxy::set's guard is a SIZE "
               "guard, not a TYPE guard. A same-width wrong-KIND write (set(float) "
               "into \"I\", set(int32) into \"F\", set(double) into \"J\", set(int64) "
               "into \"D\") PASSES the size check and reinterprets the bit pattern "
               "verbatim (vmhook.hpp ~12064-12079). Characterised below; a future "
               "signature-aware type check would reject these.");

    // set(float 1.5f) into "I" -> int holds the IEEE-754 bits 0x3FC00000.
    ctx.check("conf_I_initial", fsg::get_i32("confI") == 0);
    fsg::set_static<float>("confI", bits_to_float(k_conf_float_bits));
    ctx.check("conf_float_into_int_keeps_ieee_bits",
              static_cast<std::uint32_t>(fsg::get_i32("confI")) == k_conf_float_bits);

    // set(int32 0x40490FDB) into "F" -> float holds that pattern (~3.1415927).
    ctx.check("conf_F_initial_bits", float_bits(fsg::get_float("confF")) == 0x00000000u);
    fsg::set_static<std::int32_t>("confF", static_cast<std::int32_t>(k_conf_int_as_float_bits));
    ctx.check("conf_int_into_float_keeps_int_bits",
              float_bits(fsg::get_float("confF")) == k_conf_int_as_float_bits);

    // set(double PI) into "J" -> long holds the IEEE-754 double bits.
    ctx.check("conf_J_initial", fsg::get_i64("confJ") == 0);
    fsg::set_static<double>("confJ", bits_to_double(k_conf_double_bits));
    ctx.check("conf_double_into_long_keeps_ieee_bits",
              static_cast<std::uint64_t>(fsg::get_i64("confJ")) == k_conf_double_bits);

    // set(int64) into "D" -> double holds that pattern (here the bits of 1.0).
    ctx.check("conf_D_initial_bits", double_bits(fsg::get_double("confD")) == 0x0000000000000000ULL);
    fsg::set_static<std::int64_t>("confD", static_cast<std::int64_t>(k_conf_long_as_double_bits));
    ctx.check("conf_int64_into_double_keeps_int_bits",
              double_bits(fsg::get_double("confD")) == k_conf_long_as_double_bits);

    // =====================================================================
    //  7. ANTI-CLOBBER (the headline): write-then-read BOTH the target AND its
    //     adjacent sentinels.  Proven on a live JVM via instance fields laid out
    //     Before / clob / After.  We (a) prove contiguity with raw_address(),
    //     (b) show a too-WIDE refused write to the middle leaves BOTH sentinels
    //     untouched, and (c) show a CORRECT-width write touches ONLY the middle.
    // =====================================================================
    {
        const auto inst{ fsg::acquire("instance") };
        ctx.check("clob_instance_acquired", inst != nullptr);
        if (inst)
        {
            // ---- generic adjacency probe for one width class ----
            // width      : JVM/byte width of the trio's slots
            // before/mid/after : field names of the contiguous trio
            // write_too_wide   : performs a guarded (refused) over-wide write to mid
            // write_correct    : performs a correct-width write of `good` to mid
            // read_*           : read each slot back as a 64-bit value for compare
            auto probe_trio =
                [&](const char*                    tag_prefix,
                    std::size_t                    width,
                    const char*                    before,
                    const char*                    mid,
                    const char*                    after,
                    std::int64_t                   before_init,
                    std::int64_t                   mid_init,
                    std::int64_t                   after_init,
                    const std::function<void(const vmhook::field_proxy&)>& write_too_wide,
                    const std::function<void(const vmhook::field_proxy&)>& write_correct,
                    std::int64_t                   correct_expect,
                    const std::function<std::int64_t(const vmhook::field_proxy&)>& read_slot)
            {
                const auto pb{ inst->field(before) };
                const auto pm{ inst->field(mid) };
                const auto pa{ inst->field(after) };
                ctx.check(std::string{ tag_prefix } + "_before_resolves", pb.has_value());
                ctx.check(std::string{ tag_prefix } + "_mid_resolves",    pm.has_value());
                ctx.check(std::string{ tag_prefix } + "_after_resolves",  pa.has_value());
                if (!pb || !pm || !pa) { return; }

                // Baseline: every slot holds its declared init value.
                ctx.check(std::string{ tag_prefix } + "_before_init", read_slot(*pb) == before_init);
                ctx.check(std::string{ tag_prefix } + "_mid_init",     read_slot(*pm) == mid_init);
                ctx.check(std::string{ tag_prefix } + "_after_init",   read_slot(*pa) == after_init);

                // Contiguity: are the three slots laid out adjacently?
                const std::uintptr_t ab{ addr_of(pb) };
                const std::uintptr_t am{ addr_of(pm) };
                const std::uintptr_t aa{ addr_of(pa) };
                const bool before_mid_adj{ ab != 0 && am != 0 && am == ab + width };
                const bool mid_after_adj{ am != 0 && aa != 0 && aa == am + width };
                const bool contiguous{ before_mid_adj && mid_after_adj };

                if (contiguous)
                {
                    ctx.check(std::string{ tag_prefix } + "_trio_contiguous", true);
                }
                else
                {
                    ctx.record(std::string{ "[INFO] field_set_size_guard: " } + tag_prefix
                               + " trio is NOT contiguous on this JDK/GC layout "
                                 "(before+mid_adjacent=" + (before_mid_adj ? "1" : "0")
                               + ", mid+after_adjacent=" + (mid_after_adj ? "1" : "0")
                               + "); the strong cross-slot anti-clobber assertion is "
                                 "skipped, per-slot unchanged checks still run.");
                }

                // (b) Too-wide write to the middle is REFUSED -> mid unchanged AND,
                //     when contiguous, both neighbours are byte-for-byte intact.
                write_too_wide(*pm);
                ctx.check(std::string{ tag_prefix } + "_mid_after_overwide_unchanged",
                          read_slot(*pm) == mid_init);
                ctx.check(std::string{ tag_prefix } + "_before_after_overwide_unchanged",
                          read_slot(*pb) == before_init);
                ctx.check(std::string{ tag_prefix } + "_after_after_overwide_unchanged",
                          read_slot(*pa) == after_init);
                if (contiguous)
                {
                    // The decisive statement: a refused over-wide write into `mid`
                    // did not spill into the physically-next slot.
                    ctx.check(std::string{ tag_prefix } + "_neighbour_not_clobbered_by_overwide",
                              read_slot(*pb) == before_init && read_slot(*pa) == after_init);
                }

                // (c) CORRECT-width write touches ONLY the middle: mid takes the
                //     new value, both neighbours stay put.
                write_correct(*pm);
                ctx.check(std::string{ tag_prefix } + "_mid_correct_write_lands",
                          read_slot(*pm) == correct_expect);
                ctx.check(std::string{ tag_prefix } + "_before_after_correct_unchanged",
                          read_slot(*pb) == before_init);
                ctx.check(std::string{ tag_prefix } + "_after_after_correct_unchanged",
                          read_slot(*pa) == after_init);
            };

            // ---- byte trio (1B): set(int64) is the over-wide write ----
            probe_trio(
                "clobB", 1, "clobBBefore", "clobB", "clobBAfter",
                static_cast<std::int64_t>(static_cast<std::int8_t>(0x71)),
                static_cast<std::int64_t>(static_cast<std::int8_t>(0x11)),
                static_cast<std::int64_t>(static_cast<std::int8_t>(0x72)),
                [](const vmhook::field_proxy& p) { p.set(std::int64_t{ 0x7766554433221100LL }); },
                [](const vmhook::field_proxy& p) { p.set(static_cast<std::int8_t>(0x2D)); },
                static_cast<std::int64_t>(static_cast<std::int8_t>(0x2D)),
                [](const vmhook::field_proxy& p) -> std::int64_t { const std::int8_t v = p.get(); return v; });

            // ---- short trio (2B): set(int64) over-wide; correct is int16 ----
            probe_trio(
                "clobS", 2, "clobSBefore", "clobS", "clobSAfter",
                static_cast<std::int64_t>(static_cast<std::int16_t>(0x7AAA)),
                static_cast<std::int64_t>(static_cast<std::int16_t>(0x1111)),
                static_cast<std::int64_t>(static_cast<std::int16_t>(0x7BBB)),
                [](const vmhook::field_proxy& p) { p.set(std::int64_t{ 0x7766554433221100LL }); },
                [](const vmhook::field_proxy& p) { p.set(static_cast<std::int16_t>(0x2DEF)); },
                static_cast<std::int64_t>(static_cast<std::int16_t>(0x2DEF)),
                [](const vmhook::field_proxy& p) -> std::int64_t { const std::int16_t v = p.get(); return v; });

            // ---- int trio (4B): THE canonical case — set(int64) would smash the
            //      next 4-byte slot if unguarded; correct is int32 ----
            probe_trio(
                "clobI", 4, "clobIBefore", "clobI", "clobIAfter",
                static_cast<std::int64_t>(static_cast<std::int32_t>(0x7AAAAAAA)),
                static_cast<std::int64_t>(static_cast<std::int32_t>(0x11111111)),
                static_cast<std::int64_t>(static_cast<std::int32_t>(0x7BBBBBBB)),
                [](const vmhook::field_proxy& p) { p.set(std::int64_t{ 0x7766554433221100LL }); },
                [](const vmhook::field_proxy& p) { p.set(static_cast<std::int32_t>(0x2DEF1234)); },
                static_cast<std::int64_t>(static_cast<std::int32_t>(0x2DEF1234)),
                [](const vmhook::field_proxy& p) -> std::int64_t { const std::int32_t v = p.get(); return v; });

            // ---- long trio (8B): set(int32) is the (too-narrow) refused write;
            //      a too-narrow write must not leave the high bytes stale and must
            //      not touch neighbours; correct is int64 ----
            probe_trio(
                "clobJ", 8, "clobJBefore", "clobJ", "clobJAfter",
                static_cast<std::int64_t>(0x7AAAAAAAAAAAAAAALL),
                static_cast<std::int64_t>(0x1111111111111111LL),
                static_cast<std::int64_t>(0x7BBBBBBBBBBBBBBBLL),
                [](const vmhook::field_proxy& p) { p.set(std::int32_t{ 0x09ABCDEF }); },
                [](const vmhook::field_proxy& p) { p.set(std::int64_t{ 0x2DEF123456789ABCLL }); },
                static_cast<std::int64_t>(0x2DEF123456789ABCLL),
                [](const vmhook::field_proxy& p) -> std::int64_t { const std::int64_t v = p.get(); return v; });

            // ---- char trio (2B): the "C" widening writes EXACTLY 2 bytes — a
            //      1-byte char into the middle must not touch the neighbours, and
            //      the high byte of the middle is zero-extended ----
            {
                const auto pb{ inst->field("clobCBefore") };
                const auto pm{ inst->field("clobC") };
                const auto pa{ inst->field("clobCAfter") };
                ctx.check("clobC_trio_resolves", pb.has_value() && pm.has_value() && pa.has_value());
                if (pb && pm && pa)
                {
                    auto read_u16 = [](const vmhook::field_proxy& p) -> std::uint16_t { const std::uint16_t v = p.get(); return v; };
                    ctx.check("clobC_before_init", read_u16(*pb) == 0x7AAA);
                    ctx.check("clobC_mid_init",     read_u16(*pm) == 0x1111);
                    ctx.check("clobC_after_init",   read_u16(*pa) == 0x7BBB);

                    const std::uintptr_t ab{ addr_of(pb) };
                    const std::uintptr_t am{ addr_of(pm) };
                    const std::uintptr_t aa{ addr_of(pa) };
                    const bool contiguous{ ab != 0 && am == ab + 2 && aa == am + 2 };

                    // 1-byte char into the middle "C" slot -> widening shortcut.
                    pm->set('Z'); // 0x5A
                    ctx.check("clobC_mid_widened_to_005A", read_u16(*pm) == 0x005A);
                    ctx.check("clobC_before_unchanged_by_widening", read_u16(*pb) == 0x7AAA);
                    ctx.check("clobC_after_unchanged_by_widening",  read_u16(*pa) == 0x7BBB);
                    if (contiguous)
                    {
                        ctx.check("clobC_trio_contiguous", true);
                        ctx.check("clobC_widening_wrote_exactly_two_bytes",
                                  read_u16(*pb) == 0x7AAA && read_u16(*pa) == 0x7BBB);
                    }
                    else
                    {
                        ctx.record("[INFO] field_set_size_guard: clobC trio not contiguous "
                                   "on this layout; exact-2-byte cross-slot check skipped.");
                    }
                }
            }
        }
    }

    // =====================================================================
    //  8. REFERENCE-vs-PRIMITIVE SET.  A unique_ptr<wrapper> write into a
    //     REFERENCE field round-trips (identity flips, null nulls), proving the
    //     reference path is the RIGHT home for unique_ptr (whereas phase 4 showed
    //     the same value into a primitive is refused).
    // =====================================================================
    {
        const auto refA{ fsg::acquire("refA") };
        const auto refB{ fsg::acquire("refB") };
        ctx.check("refA_acquired", refA != nullptr);
        ctx.check("refB_acquired", refB != nullptr);
        ctx.check("refA_tag_is_A", refA != nullptr && refA->tag() == 0xA);
        ctx.check("refB_tag_is_B", refB != nullptr && refB->tag() == 0xB);

        // refSlot starts aliasing refA.
        {
            const auto r0{ fsg::acquire("refSlot") };
            ctx.check("refSlot_initially_A", r0 != nullptr && r0->tag() == 0xA);
        }
        // Rewrite refSlot = refB (compressed-OOP write into a reference slot).
        ctx.check("set_refSlot_to_B", fsg::set_ref("refSlot", refB));
        {
            const auto r1{ fsg::acquire("refSlot") };
            ctx.check("refSlot_now_B", r1 != nullptr && r1->tag() == 0xB);
        }
        // Null it via an empty unique_ptr.
        {
            const std::unique_ptr<fsg> empty{};
            ctx.check("set_refSlot_to_null", fsg::set_ref("refSlot", empty));
            const auto rN{ fsg::acquire("refSlot") };
            ctx.check("refSlot_now_null", rN == nullptr);
        }
        // Put it back to refB so the Java snapshot (phase 9) sees non-null == B.
        ctx.check("set_refSlot_back_to_B", fsg::set_ref("refSlot", refB));
    }

    // =====================================================================
    //  9. NULL field_pointer set() — a safe no-op on EVERY signature kind.
    //     Constructed directly (the only way to get a null-pointer proxy) to
    //     prove set() early-returns without dereferencing the null storage.
    // =====================================================================
    {
        const char* sigs[] = { "Z", "B", "S", "C", "I", "J", "F", "D",
                               "Ljava/lang/String;", "[I" };
        for (const char* sig : sigs)
        {
            vmhook::field_proxy np{ nullptr, sig, true };
            np.set(std::int32_t{ 0x1234 });   // correct/typical primitive into null slot
            np.set(std::int64_t{ 0x55 });      // too-wide primitive into null slot
            np.set(std::string{ "x" });        // non-primitive into null slot
        }
        // Reaching here without an access violation IS the proof: set() bailed on
        // the null field_pointer for every signature kind instead of writing.
        ctx.check("null_field_pointer_set_is_safe_noop", true);
    }

    // =====================================================================
    //  10. SET-THEN-READ-BACK THROUGH JAVA (mode 1 snapshot): the probe copies
    //      every field into its seen* witness using genuine getfield/getstatic +
    //      putstatic, so we read back exactly what the JVM observed for the native
    //      writes performed above.
    // =====================================================================
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("snapshot_probe_completed", done);

        if (done)
        {
            // ---- correct-width writes, as seen by Java ----
            ctx.check("java_seen_okZ_true", fsg::get_bool("seenOkZ") == true);
            ctx.check("java_seen_okB_7E",   fsg::get_i8("seenOkB") == static_cast<std::int8_t>(0x7E));
            ctx.check("java_seen_okC_BEEF", fsg::get_u16("seenOkC") == 0xBEEF);
            ctx.check("java_seen_okS_7EEF", fsg::get_i16("seenOkS") == static_cast<std::int16_t>(0x7EEF));
            ctx.check("java_seen_okI_F00D", fsg::get_i32("seenOkI") == 0x0BADF00D);
            ctx.check("java_seen_okJ_full", fsg::get_i64("seenOkJ") == 0x0123456789ABCDEFLL);
            ctx.check("java_seen_okF_bits", static_cast<std::uint32_t>(fsg::get_i32("seenOkFBits")) == k_okF_bits);
            ctx.check("java_seen_okD_bits", static_cast<std::uint64_t>(fsg::get_i64("seenOkDBits")) == k_okD_bits);

            // ---- type-confusion writes, as seen by Java (bit-exact) ----
            ctx.check("java_seen_confI_ieee_bits", static_cast<std::uint32_t>(fsg::get_i32("seenConfIBits")) == k_conf_float_bits);
            ctx.check("java_seen_confF_int_bits",  static_cast<std::uint32_t>(fsg::get_i32("seenConfFBits")) == k_conf_int_as_float_bits);
            ctx.check("java_seen_confJ_ieee_bits", static_cast<std::uint64_t>(fsg::get_i64("seenConfJBits")) == k_conf_double_bits);
            ctx.check("java_seen_confD_int_bits",  static_cast<std::uint64_t>(fsg::get_i64("seenConfDBits")) == k_conf_long_as_double_bits);

            // ---- guard targets UNCHANGED, as seen by Java ----
            ctx.check("java_seen_gWideI_unchanged", fsg::get_i32("seenGWideI") == 0x11223344);
            ctx.check("java_seen_gWideB_unchanged", fsg::get_i8("seenGWideB") == static_cast<std::int8_t>(0x5A));
            ctx.check("java_seen_gWideS_unchanged", fsg::get_i16("seenGWideS") == 0x1234);
            ctx.check("java_seen_gNarrowJ_unchanged", fsg::get_i64("seenGNarrowJ") == 0x1122334455667788LL);
            ctx.check("java_seen_gNarrowD_unchanged", static_cast<std::uint64_t>(fsg::get_i64("seenGNarrowDBits")) == 0x3FF0000000000000ULL);
            ctx.check("java_seen_gStrI_unchanged", fsg::get_i32("seenGStrI") == 0x0BADBEEF);
            ctx.check("java_seen_gVecB_unchanged", fsg::get_i8("seenGVecB") == static_cast<std::int8_t>(0x33));
            ctx.check("java_seen_gRefI_unchanged", fsg::get_i32("seenGRefI") == 0x600DC0DE);
            // gCharByte: last native write was the euro sign 0x20AC.
            ctx.check("java_seen_gCharByte_euro", fsg::get_u16("seenGCharByte") == 0x20AC);

            // ---- adjacency sentinels, as seen by Java ----
            //  Targets hold their LAST correct-width write (phase 7 step c);
            //  every Before/After sentinel must still hold its declared init.
            ctx.check("java_seen_clobBBefore_intact", fsg::get_i8("seenClobBBefore") == static_cast<std::int8_t>(0x71));
            ctx.check("java_seen_clobB_correct",       fsg::get_i8("seenClobB") == static_cast<std::int8_t>(0x2D));
            ctx.check("java_seen_clobBAfter_intact",   fsg::get_i8("seenClobBAfter") == static_cast<std::int8_t>(0x72));

            ctx.check("java_seen_clobSBefore_intact", fsg::get_i16("seenClobSBefore") == static_cast<std::int16_t>(0x7AAA));
            ctx.check("java_seen_clobS_correct",       fsg::get_i16("seenClobS") == static_cast<std::int16_t>(0x2DEF));
            ctx.check("java_seen_clobSAfter_intact",   fsg::get_i16("seenClobSAfter") == static_cast<std::int16_t>(0x7BBB));

            ctx.check("java_seen_clobIBefore_intact", fsg::get_i32("seenClobIBefore") == static_cast<std::int32_t>(0x7AAAAAAA));
            ctx.check("java_seen_clobI_correct",       fsg::get_i32("seenClobI") == static_cast<std::int32_t>(0x2DEF1234));
            ctx.check("java_seen_clobIAfter_intact",   fsg::get_i32("seenClobIAfter") == static_cast<std::int32_t>(0x7BBBBBBB));

            ctx.check("java_seen_clobJBefore_intact", fsg::get_i64("seenClobJBefore") == static_cast<std::int64_t>(0x7AAAAAAAAAAAAAAALL));
            ctx.check("java_seen_clobJ_correct",       fsg::get_i64("seenClobJ") == static_cast<std::int64_t>(0x2DEF123456789ABCLL));
            ctx.check("java_seen_clobJAfter_intact",   fsg::get_i64("seenClobJAfter") == static_cast<std::int64_t>(0x7BBBBBBBBBBBBBBBLL));

            ctx.check("java_seen_clobCBefore_intact", fsg::get_u16("seenClobCBefore") == 0x7AAA);
            ctx.check("java_seen_clobC_widened",       fsg::get_u16("seenClobC") == 0x005A);
            ctx.check("java_seen_clobCAfter_intact",   fsg::get_u16("seenClobCAfter") == 0x7BBB);

            // ---- reference identity, as seen by Java ----
            ctx.check("java_seen_refSlot_is_B", fsg::get_bool("seenRefSlotIsB") == true);
            ctx.check("java_seen_refSlot_not_A", fsg::get_bool("seenRefSlotIsA") == false);
            ctx.check("java_seen_refSlot_not_null", fsg::get_bool("seenRefSlotIsNull") == false);
            ctx.check("java_seen_refSlot_tag_B", fsg::get_i32("seenRefSlotTag") == 0xB);
        }
    }

    // =====================================================================
    //  11. SET-THEN-READ-BACK THROUGH JAVA GETTERS (static_method portability).
    //      Java's own bytecode reads each field and returns it, proving the
    //      writes are visible to executing Java code, not just to a memory peek.
    // =====================================================================
    {
        ctx.check("java_getter_okZ_true", fsg::call_get_bool("getOkZ") == true);
        ctx.check("java_getter_okI_F00D", fsg::call_get_i32("getOkI") == 0x0BADF00D);
        ctx.check("java_getter_okJ_full", fsg::call_get_i64("getOkJ") == 0x0123456789ABCDEFLL);
        ctx.check("java_getter_okC_BEEF", fsg::call_get_i32("getOkC") == 0xBEEF); // char widened unsigned
        ctx.check("java_getter_gWideI_unchanged", fsg::call_get_i32("getGWideI") == 0x11223344);
        ctx.check("java_getter_gNarrowJ_unchanged", fsg::call_get_i64("getGNarrowJ") == 0x1122334455667788LL);
        ctx.check("java_getter_gStrI_unchanged", fsg::call_get_i32("getGStrI") == 0x0BADBEEF);
        ctx.check("java_getter_gRefI_unchanged", fsg::call_get_i32("getGRefI") == 0x600DC0DE);
        ctx.check("java_getter_gCharByte_euro", fsg::call_get_i32("getGCharByte") == 0x20AC);
        ctx.check("java_getter_refSlotIsB", fsg::call_get_bool("refSlotIsB") == true);
        ctx.check("java_getter_refSlotIsNull_false", fsg::call_get_bool("refSlotIsNull") == false);
        ctx.check("java_getter_refSlotTag_B", fsg::call_get_i32("getRefSlotTag") == 0xB);
    }
}
