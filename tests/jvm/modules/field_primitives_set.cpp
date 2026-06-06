// field_primitives_set JVM test module — area: fields.
//
// THE SET mirror of field_primitives_get.  Exhaustively exercises
// field_proxy::set() (vmhook.hpp ~12051-12194) for EVERY JVM primitive
// descriptor (Z B S C I J F D) at regular AND boundary values, written by the
// NATIVE side through the public wrapper API -- static_field("name")->set(v)
// and an instance wrapper's get_field("name")->set(v) -- and then proven THREE
// independent ways:
//
//   (1) IMMEDIATE NATIVE RE-READ: get() reads the value back bit-exact and the
//       value_t variant holds the CORRECT alternative (proving set wrote the
//       right number of bytes into the right slot).
//   (2) JAVA-OBSERVED SNAPSHOT (mode 1): the probe copies every field into its
//       seen* witness via genuine getstatic/getfield + putstatic; the native
//       side reads the witnesses back and asserts the JVM ITSELF observed the
//       native write.  F/D captured as RAW bits (floatToRawIntBits) so a NaN
//       payload / signaling-NaN bit is preserved verbatim.
//   (3) JAVA-EVALUATED COMPARE (mode 2) + GETTERS: Java bytecode compares each
//       live field against the native-programmed expected value (eq[] booleans)
//       AND returns each field through a getter (static_method->call()).
//
// Float/double are checked BIT-EXACT (memcpy type-pun, NOT std::bit_cast) so
// +/-0.0, +/-Inf, canonical/signaling/payload NaN, denormal and MIN_NORMAL all
// survive set()->get() and set()->Java-getfield unchanged.
//
// Division of labour vs siblings (zero overlap):
//   * field_primitives_get owns the GET decode paths.
//   * field_set_size_guard owns the SIZE/TYPE guard, the spatial anti-clobber
//     proof (raw_address adjacency), wrong-kind / non-primitive rejection, and
//     null-pointer no-op for ALL signature kinds.  This module re-pins only the
//     few guard/no-op facts that bound its OWN value-matrix writes (so a guard
//     regression that silenced this module's writes is caught here too), and
//     otherwise focuses entirely on the VALUE matrix that no sibling covers.
//
// Flaws this module pins (see audit/findings/field_proxy_set_*.md and the
// agent-def "Flaws I found"):
//   * set() is a raw memcpy with a SIZE guard but NO TYPE guard: a same-width
//     wrong-kind value reinterprets bits.  Within the value matrix we only ever
//     pass the width-matched primitive, so this module's writes are well-typed;
//     the wrong-kind characterisation is owned by field_set_size_guard.  We pin
//     the boundary that a width-matched write is NEVER refused.
//   * The "C" + 1-byte-char shortcut widens to 2 bytes; we drive BOTH the
//     uint16 path (the value matrix) AND the char shortcut and assert the full
//     2-byte Java char lands.
//   * set(value) on a null field_pointer is a silent no-op (early return); pinned.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.FieldPrimitivesSet.
    //
    // Every accessor is a STATIC method routed through static_field / get_field
    // on an explicit instance -- never the deducing-this get_field from a static
    // context (which does not compile on GCC; see field_set_size_guard).
    class fps : public vmhook::object<fps>
    {
    public:
        explicit fps(vmhook::oop_t instance) noexcept
            : vmhook::object<fps>{ instance }
        {
        }

        // ── handshake + scenario selector (all via static_field) ──────────
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // ── generic typed SET via a static field ─────────────────────────
        template<typename value_type>
        static auto set_static(const char* name, const value_type& v) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return false; }
            proxy->set(v);
            return true;
        }

        // ── typed static GETs (copy-init extraction, MSVC-unambiguous) ────
        static auto get_bool(const char* name) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return false; }
            const bool v = proxy->get();
            return v;
        }
        static auto get_i8(const char* name) -> std::int8_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return 0; }
            const std::int8_t v = proxy->get();
            return v;
        }
        static auto get_i16(const char* name) -> std::int16_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return 0; }
            const std::int16_t v = proxy->get();
            return v;
        }
        static auto get_i32(const char* name) -> std::int32_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return -1; }
            const std::int32_t v = proxy->get();
            return v;
        }
        static auto get_i64(const char* name) -> std::int64_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return -1; }
            const std::int64_t v = proxy->get();
            return v;
        }
        static auto get_u16(const char* name) -> std::uint16_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return 0; }
            const std::uint16_t v = proxy->get();
            return v;
        }
        static auto get_float(const char* name) -> float
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return 0.0F; }
            const float v = proxy->get();
            return v;
        }
        static auto get_double(const char* name) -> double
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return 0.0; }
            const double v = proxy->get();
            return v;
        }

        // ── acquire the published instance wrapper ────────────────────────
        static auto acquire(const char* name) -> std::unique_ptr<fps>
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value()) { return nullptr; }
            std::unique_ptr<fps> ptr = proxy->get();
            return ptr;
        }

        // ── Java getters via static_method (visible-to-bytecode proof) ────
        static auto call_bool(const char* method) -> bool
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return false; }
            const bool v = m->call();
            return v;
        }
        static auto call_i8(const char* method) -> std::int8_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return 0; }
            const std::int8_t v = m->call();
            return v;
        }
        static auto call_i16(const char* method) -> std::int16_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return 0; }
            const std::int16_t v = m->call();
            return v;
        }
        static auto call_i32(const char* method) -> std::int32_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return -1; }
            const std::int32_t v = m->call();
            return v;
        }
        static auto call_i64(const char* method) -> std::int64_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value()) { return -1; }
            const std::int64_t v = m->call();
            return v;
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

    // Bit helpers (memcpy type-pun; C++17, no std::bit_cast).
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

    // Drive one probe cycle for `mode`: clears latched `done` and programs the
    // selector on the rising edge of go, then waits for done.  (Identical shape
    // to field_set_size_guard::drive so repeated probe drives are safe.)
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    fps::set_done(false);
                    fps::set_mode(mode);
                }
                fps::set_go(value);
            },
            []() { return fps::get_done(); });
    }
}

VMHOOK_JVM_MODULE(field_primitives_set)
{
    vmhook::register_class<fps>("vmhook/fixtures/FieldPrimitivesSet");

    // =====================================================================
    //  0. Sanity: the class resolves and the portable static accessor works.
    // =====================================================================
    ctx.check("fps_class_registered_static_field_resolves", fps::resolves("sI"));
    ctx.check("fps_static_method_resolves", fps::static_method("getSI").has_value());

    const auto inst{ fps::acquire("instance") };
    ctx.check("fps_instance_wrapper_obtained", inst != nullptr);

    // =====================================================================
    //  1. STATIC boolean ("Z") -- set every value, re-read native, assert
    //     variant alternative is bool and the byte is canonical (0/1).
    // =====================================================================
    {
        // helper: set a Z field then native re-read it.
        auto set_chk_Z = [&](const char* tag, bool value)
        {
            const auto p{ fps::static_field("sZ") };
            ctx.check(std::string{ "Z_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(value);
            const auto v{ p->get() };
            ctx.check(std::string{ "Z_variant_bool_" } + tag, v.data.index() == kIdxBool);
            const bool got = v;
            ctx.check(std::string{ "Z_reread_" } + tag, got == value);
            // Canonical: widened to int is exactly 0 or 1.
            const int as_int = v;
            ctx.check(std::string{ "Z_canonical_" } + tag, as_int == (value ? 1 : 0));
        };
        set_chk_Z("false", false);
        set_chk_Z("true",  true);
        // Leave sZ = true as the final value the snapshot/getter will observe.
    }

    // =====================================================================
    //  2. STATIC byte ("B") -- 0,1,-1,min,max + sign-boundary 0x7F/0x80/0xFF.
    // =====================================================================
    {
        auto set_chk_B = [&](const char* tag, std::int8_t value)
        {
            const auto p{ fps::static_field("sB") };
            ctx.check(std::string{ "B_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(value);
            const auto v{ p->get() };
            ctx.check(std::string{ "B_variant_i8_" } + tag, v.data.index() == kIdxI8);
            const std::int8_t got = v;
            ctx.check(std::string{ "B_reread_" } + tag, got == value);
            const int widened = v;
            ctx.check(std::string{ "B_sign_extends_" } + tag, widened == static_cast<int>(value));
        };
        set_chk_B("zero",  0);
        set_chk_B("one",   1);
        set_chk_B("negone", -1);
        set_chk_B("min",   std::numeric_limits<std::int8_t>::min()); // -128
        set_chk_B("max",   std::numeric_limits<std::int8_t>::max()); //  127
        set_chk_B("0x7F",  static_cast<std::int8_t>(0x7F));          //  127
        set_chk_B("0x80",  static_cast<std::int8_t>(0x80));          // -128
        set_chk_B("0xFF",  static_cast<std::int8_t>(0xFF));          //   -1
        set_chk_B("0xAB",  static_cast<std::int8_t>(0xAB));          //  -85
        // Final B value for snapshot/getter == 0xAB (-85).
    }

    // =====================================================================
    //  3. STATIC short ("S") -- 0,1,-1,min,max + 0x7FFF/0x8000 boundary.
    // =====================================================================
    {
        auto set_chk_S = [&](const char* tag, std::int16_t value)
        {
            const auto p{ fps::static_field("sS") };
            ctx.check(std::string{ "S_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(value);
            const auto v{ p->get() };
            ctx.check(std::string{ "S_variant_i16_" } + tag, v.data.index() == kIdxI16);
            const std::int16_t got = v;
            ctx.check(std::string{ "S_reread_" } + tag, got == value);
            const int widened = v;
            ctx.check(std::string{ "S_sign_extends_" } + tag, widened == static_cast<int>(value));
        };
        set_chk_S("zero",   0);
        set_chk_S("one",    1);
        set_chk_S("negone", -1);
        set_chk_S("min",    std::numeric_limits<std::int16_t>::min()); // -32768
        set_chk_S("max",    std::numeric_limits<std::int16_t>::max()); //  32767
        set_chk_S("0x7FFF", static_cast<std::int16_t>(0x7FFF));        //  32767
        set_chk_S("0x8000", static_cast<std::int16_t>(0x8000));        // -32768
        set_chk_S("0xBEEF", static_cast<std::int16_t>(0xBEEF));        //  -16657
        // Final S value for snapshot/getter == 0xBEEF (-16657).
    }

    // =====================================================================
    //  4. STATIC int ("I") -- 0,1,-1,min,max + 0x80000000/0x7FFFFFFF.
    // =====================================================================
    {
        auto set_chk_I = [&](const char* tag, std::int32_t value)
        {
            const auto p{ fps::static_field("sI") };
            ctx.check(std::string{ "I_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(value);
            const auto v{ p->get() };
            ctx.check(std::string{ "I_variant_i32_" } + tag, v.data.index() == kIdxI32);
            const std::int32_t got = v;
            ctx.check(std::string{ "I_reread_" } + tag, got == value);
            const std::int64_t widened = v;
            ctx.check(std::string{ "I_sign_extends_long_" } + tag, widened == static_cast<std::int64_t>(value));
        };
        set_chk_I("zero",       0);
        set_chk_I("one",        1);
        set_chk_I("negone",     -1);
        set_chk_I("min",        std::numeric_limits<std::int32_t>::min());
        set_chk_I("max",        std::numeric_limits<std::int32_t>::max());
        set_chk_I("0x7FFFFFFF", static_cast<std::int32_t>(0x7FFFFFFF));
        set_chk_I("0x80000000", static_cast<std::int32_t>(0x80000000));
        set_chk_I("deadbeef",   static_cast<std::int32_t>(0xDEADBEEF));
        // Final I value for snapshot/getter == 0xDEADBEEF.
    }

    // =====================================================================
    //  5. STATIC long ("J") -- 0,1,-1,min,max + 64-bit boundary patterns.
    // =====================================================================
    {
        auto set_chk_J = [&](const char* tag, std::int64_t value)
        {
            const auto p{ fps::static_field("sJ") };
            ctx.check(std::string{ "J_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(value);
            const auto v{ p->get() };
            ctx.check(std::string{ "J_variant_i64_" } + tag, v.data.index() == kIdxI64);
            const std::int64_t got = v;
            ctx.check(std::string{ "J_reread_" } + tag, got == value);
        };
        set_chk_J("zero",   0);
        set_chk_J("one",    1);
        set_chk_J("negone", -1);
        set_chk_J("min",    std::numeric_limits<std::int64_t>::min());
        set_chk_J("max",    std::numeric_limits<std::int64_t>::max());
        set_chk_J("0x7FFFFFFFFFFFFFFF", 0x7FFFFFFFFFFFFFFFLL);
        set_chk_J("0x8000000000000000", static_cast<std::int64_t>(0x8000000000000000ULL));
        set_chk_J("highbits",           static_cast<std::int64_t>(0x00000000FFFFFFFFULL)); // 4294967295
        set_chk_J("deadbeef",           static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));
        // Final J value for snapshot/getter == 0xDEADBEEFCAFEBABE.
    }

    // =====================================================================
    //  6. STATIC char ("C") -- UTF-16 code units across the whole range, via
    //     the width-matched uint16 path.  0x0020 0x41 0x00E9 0x4E2D 0xD800 0xFFFF.
    // =====================================================================
    {
        auto set_chk_C = [&](const char* tag, std::uint16_t value)
        {
            const auto p{ fps::static_field("sC") };
            ctx.check(std::string{ "C_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(value);
            const auto v{ p->get() };
            ctx.check(std::string{ "C_variant_u16_" } + tag, v.data.index() == kIdxU16);
            const std::uint16_t got = v;
            ctx.check(std::string{ "C_reread_" } + tag, got == value);
            // char never sign-promotes negative: widened to int is 0..65535.
            const int widened = v;
            ctx.check(std::string{ "C_widens_unsigned_" } + tag, widened == static_cast<int>(value));
        };
        set_chk_C("space",   0x0020);
        set_chk_C("A",       0x0041);
        set_chk_C("highbit", 0x00E9); // 'e-acute'
        set_chk_C("bmp",     0x4E2D); // CJK
        set_chk_C("minsurr", 0xD800); // first high surrogate
        set_chk_C("hisurr",  0xD83D);
        set_chk_C("losurr",  0xDE00);
        set_chk_C("maxsurr", 0xDFFF);
        set_chk_C("max",     0xFFFF); // Character.MAX_VALUE
        // Final C value (so far) == 0xFFFF.  The char-shortcut sub-test below
        // then drives a 1-byte char and ALSO restores 0xFFFF for the snapshot.
    }

    // The "C" + 1-byte-char widening shortcut (vmhook.hpp ~12148-12153): a C++
    // `char` (1 byte) into a "C" field (2 bytes) must land the FULL 2-byte Java
    // char with the high byte zero-extended, never a single byte, never sign
    // extended.  (field_set_size_guard proves the spatial 2-byte width; here we
    // prove the VALUE on the value-matrix field.)
    {
        const auto p{ fps::static_field("sC") };
        if (p)
        {
            p->set('Z');                                   // 0x5A
            ctx.check("C_char_shortcut_005A", fps::get_u16("sC") == 0x005A);
            p->set(static_cast<char>(0xE9));               // high-bit byte
            ctx.check("C_char_shortcut_high_byte_zero", fps::get_u16("sC") == 0x00E9);
            // Restore 0xFFFF so the snapshot/getter phases observe Character.MAX.
            p->set(static_cast<std::uint16_t>(0xFFFF));
            ctx.check("C_restore_FFFF", fps::get_u16("sC") == 0xFFFF);
        }
    }

    // =====================================================================
    //  7. STATIC float ("F") -- BIT-EXACT writes of every special value via
    //     the width-matched float path.  Each: set bits, native re-read bits
    //     must match exactly (NaN payload, sNaN bit, +/-0, +/-Inf, denormal,
    //     MIN_NORMAL).
    // =====================================================================
    {
        auto set_chk_F = [&](const char* tag, std::uint32_t bits)
        {
            const auto p{ fps::static_field("sF") };
            ctx.check(std::string{ "F_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(bits_to_float(bits));
            const auto v{ p->get() };
            ctx.check(std::string{ "F_variant_float_" } + tag, v.data.index() == kIdxFloat);
            const float got = v;
            ctx.check(std::string{ "F_bits_exact_" } + tag, float_bits(got) == bits);
        };
        set_chk_F("poszero", 0x00000000);
        set_chk_F("negzero", 0x80000000);
        set_chk_F("one",     0x3F800000);
        set_chk_F("negone",  0xBF800000);
        set_chk_F("min",     0x00000001); // Float.MIN_VALUE (denormal)
        set_chk_F("max",     0x7F7FFFFF);
        set_chk_F("minnorm", 0x00800000); // Float.MIN_NORMAL
        set_chk_F("posinf",  0x7F800000);
        set_chk_F("neginf",  0xFF800000);
        set_chk_F("nan",     0x7FC00000); // canonical qNaN
        set_chk_F("snan",    0x7F800001); // signaling NaN
        set_chk_F("nanpay",  0x7FA55555); // qNaN with payload
        set_chk_F("denorm",  0x00000001);
        // Final F value for snapshot/getter == canonical NaN bits (set below).
        if (const auto p{ fps::static_field("sF") })
        {
            p->set(bits_to_float(0x7FC00000));
        }
    }

    // =====================================================================
    //  8. STATIC double ("D") -- BIT-EXACT writes of every special value.
    // =====================================================================
    {
        auto set_chk_D = [&](const char* tag, std::uint64_t bits)
        {
            const auto p{ fps::static_field("sD") };
            ctx.check(std::string{ "D_resolves_" } + tag, p.has_value());
            if (!p) { return; }
            p->set(bits_to_double(bits));
            const auto v{ p->get() };
            ctx.check(std::string{ "D_variant_double_" } + tag, v.data.index() == kIdxDouble);
            const double got = v;
            ctx.check(std::string{ "D_bits_exact_" } + tag, double_bits(got) == bits);
        };
        set_chk_D("poszero", 0x0000000000000000ULL);
        set_chk_D("negzero", 0x8000000000000000ULL);
        set_chk_D("one",     0x3FF0000000000000ULL);
        set_chk_D("negone",  0xBFF0000000000000ULL);
        set_chk_D("min",     0x0000000000000001ULL); // Double.MIN_VALUE
        set_chk_D("max",     0x7FEFFFFFFFFFFFFFULL);
        set_chk_D("minnorm", 0x0010000000000000ULL); // Double.MIN_NORMAL
        set_chk_D("posinf",  0x7FF0000000000000ULL);
        set_chk_D("neginf",  0xFFF0000000000000ULL);
        set_chk_D("nan",     0x7FF8000000000000ULL); // canonical qNaN
        set_chk_D("snan",    0x7FF0000000000001ULL); // signaling NaN
        set_chk_D("nanpay",  0x7FFAAAAAAAAAAAAAULL); // qNaN with payload
        set_chk_D("denorm",  0x0000000000000001ULL);
        // Final D value for snapshot/getter == canonical NaN bits (set below).
        if (const auto p{ fps::static_field("sD") })
        {
            p->set(bits_to_double(0x7FF8000000000000ULL));
        }
    }

    // =====================================================================
    //  9. INSTANCE writes -- the get_field(...)->set(...) dispatch path for
    //     every primitive.  Proves set() ignores the static/instance flag and
    //     writes the correct slot via instance dispatch.  Each: set then native
    //     re-read bit-exact + correct variant alternative.
    // =====================================================================
    if (inst)
    {
        // boolean
        {
            auto p{ inst->get_field("iZ") };
            ctx.check("inst_Z_resolves", p.has_value());
            if (p)
            {
                p->set(true);
                const auto v{ p->get() };
                ctx.check("inst_Z_variant_bool", v.data.index() == kIdxBool);
                const bool b = v; ctx.check("inst_Z_reread_true", b == true);
            }
        }
        // byte -> 0xFE (-2)
        {
            auto p{ inst->get_field("iB") };
            ctx.check("inst_B_resolves", p.has_value());
            if (p)
            {
                p->set(static_cast<std::int8_t>(0xFE));
                const auto v{ p->get() };
                ctx.check("inst_B_variant_i8", v.data.index() == kIdxI8);
                const std::int8_t b = v; ctx.check("inst_B_reread_FE", b == static_cast<std::int8_t>(0xFE));
                const int w = v; ctx.check("inst_B_sign_extends_neg2", w == -2);
            }
        }
        // short -> 0xCAFE
        {
            auto p{ inst->get_field("iS") };
            ctx.check("inst_S_resolves", p.has_value());
            if (p)
            {
                p->set(static_cast<std::int16_t>(0xCAFE));
                const auto v{ p->get() };
                ctx.check("inst_S_variant_i16", v.data.index() == kIdxI16);
                const std::int16_t s = v; ctx.check("inst_S_reread_CAFE", s == static_cast<std::int16_t>(0xCAFE));
            }
        }
        // int -> 0x0BADF00D
        {
            auto p{ inst->get_field("iI") };
            ctx.check("inst_I_resolves", p.has_value());
            if (p)
            {
                p->set(static_cast<std::int32_t>(0x0BADF00D));
                const auto v{ p->get() };
                ctx.check("inst_I_variant_i32", v.data.index() == kIdxI32);
                const std::int32_t i = v; ctx.check("inst_I_reread_0BADF00D", i == 0x0BADF00D);
            }
        }
        // long -> 0x0123456789ABCDEF
        {
            auto p{ inst->get_field("iJ") };
            ctx.check("inst_J_resolves", p.has_value());
            if (p)
            {
                p->set(static_cast<std::int64_t>(0x0123456789ABCDEFLL));
                const auto v{ p->get() };
                ctx.check("inst_J_variant_i64", v.data.index() == kIdxI64);
                const std::int64_t l = v; ctx.check("inst_J_reread_full", l == 0x0123456789ABCDEFLL);
            }
        }
        // char -> 0x20AC (euro)
        {
            auto p{ inst->get_field("iC") };
            ctx.check("inst_C_resolves", p.has_value());
            if (p)
            {
                p->set(static_cast<std::uint16_t>(0x20AC));
                const auto v{ p->get() };
                ctx.check("inst_C_variant_u16", v.data.index() == kIdxU16);
                const std::uint16_t c = v; ctx.check("inst_C_reread_20AC", c == 0x20AC);
            }
        }
        // float -> bit pattern 0xC0490FDB (-pi)
        {
            auto p{ inst->get_field("iF") };
            ctx.check("inst_F_resolves", p.has_value());
            if (p)
            {
                p->set(bits_to_float(0xC0490FDB));
                const auto v{ p->get() };
                ctx.check("inst_F_variant_float", v.data.index() == kIdxFloat);
                const float f = v; ctx.check("inst_F_bits_exact", float_bits(f) == 0xC0490FDB);
            }
        }
        // double -> pi bits 0x400921FB54442D18
        {
            auto p{ inst->get_field("iD") };
            ctx.check("inst_D_resolves", p.has_value());
            if (p)
            {
                p->set(bits_to_double(0x400921FB54442D18ULL));
                const auto v{ p->get() };
                ctx.check("inst_D_variant_double", v.data.index() == kIdxDouble);
                const double d = v; ctx.check("inst_D_bits_exact", double_bits(d) == 0x400921FB54442D18ULL);
            }
        }
    }

    // =====================================================================
    //  10. REPEATABILITY / LAST-WRITE-WINS.  set() is a pure store with no
    //      latch; writing the same field twice leaves the SECOND value, and a
    //      write after a write fully overwrites (no OR/accumulate).
    // =====================================================================
    {
        const auto p{ fps::static_field("sI") };
        if (p)
        {
            p->set(std::int32_t{ 0x11112222 });
            ctx.check("repeat_I_first", fps::get_i32("sI") == 0x11112222);
            p->set(std::int32_t{ 0x33334444 });
            ctx.check("repeat_I_last_write_wins", fps::get_i32("sI") == 0x33334444);
            // Idempotent: same value twice yields the same value.
            p->set(std::int32_t{ 0x33334444 });
            ctx.check("repeat_I_idempotent", fps::get_i32("sI") == 0x33334444);
            // Restore the deadbeef final value for the snapshot/getter phases.
            p->set(static_cast<std::int32_t>(0xDEADBEEF));
            ctx.check("repeat_I_restored_deadbeef", fps::get_i32("sI") == static_cast<std::int32_t>(0xDEADBEEF));
        }
        // Last-write-wins across DIFFERENT bit kinds on a float field: write a
        // finite value, then NaN; the NaN must fully replace it.
        const auto pf{ fps::static_field("sF") };
        if (pf)
        {
            pf->set(1.5F);
            ctx.check("repeat_F_finite", float_bits(fps::get_float("sF")) == 0x3FC00000);
            pf->set(bits_to_float(0x7FC00000));
            ctx.check("repeat_F_nan_overwrites", float_bits(fps::get_float("sF")) == 0x7FC00000);
        }
    }

    // =====================================================================
    //  11. ANTI-CLOBBER via the value-matrix fields.  Writing the middle of a
    //      Before/Mid/After trio leaves both neighbours unchanged.  (The strong
    //      raw_address adjacency proof is owned by field_set_size_guard; here we
    //      pin that a CORRECT-width write of our matrix does not disturb the
    //      sentinels -- native re-read AND, in phase 13, Java-observed.)
    // =====================================================================
    if (inst)
    {
        // int trio
        {
            auto pb{ inst->get_field("clobBefore") };
            auto pm{ inst->get_field("clobMid") };
            auto pa{ inst->get_field("clobAfter") };
            ctx.check("clob_int_trio_resolves", pb.has_value() && pm.has_value() && pa.has_value());
            if (pb && pm && pa)
            {
                ctx.check("clob_int_before_init", static_cast<std::int32_t>(pb->get()) == 0x11111111);
                ctx.check("clob_int_mid_init",     static_cast<std::int32_t>(pm->get()) == 0x22222222);
                ctx.check("clob_int_after_init",   static_cast<std::int32_t>(pa->get()) == 0x33333333);
                pm->set(std::int32_t{ 0x2DEF1234 });
                ctx.check("clob_int_mid_written",  static_cast<std::int32_t>(pm->get()) == 0x2DEF1234);
                ctx.check("clob_int_before_intact", static_cast<std::int32_t>(pb->get()) == 0x11111111);
                ctx.check("clob_int_after_intact",  static_cast<std::int32_t>(pa->get()) == 0x33333333);
            }
        }
        // long trio
        {
            auto pb{ inst->get_field("clobBeforeJ") };
            auto pm{ inst->get_field("clobMidJ") };
            auto pa{ inst->get_field("clobAfterJ") };
            ctx.check("clob_long_trio_resolves", pb.has_value() && pm.has_value() && pa.has_value());
            if (pb && pm && pa)
            {
                ctx.check("clob_long_before_init", static_cast<std::int64_t>(pb->get()) == 0x1111111111111111LL);
                ctx.check("clob_long_mid_init",     static_cast<std::int64_t>(pm->get()) == 0x2222222222222222LL);
                ctx.check("clob_long_after_init",   static_cast<std::int64_t>(pa->get()) == 0x3333333333333333LL);
                pm->set(std::int64_t{ 0x2DEF123456789ABCLL });
                ctx.check("clob_long_mid_written",  static_cast<std::int64_t>(pm->get()) == 0x2DEF123456789ABCLL);
                ctx.check("clob_long_before_intact", static_cast<std::int64_t>(pb->get()) == 0x1111111111111111LL);
                ctx.check("clob_long_after_intact",  static_cast<std::int64_t>(pa->get()) == 0x3333333333333333LL);
            }
        }
    }

    // =====================================================================
    //  12. NULL field_pointer set() is a safe no-op (early return, no write,
    //      no crash) for every primitive width.  Constructed directly (the only
    //      way to obtain a null-pointer proxy).  Reaching the assertion without
    //      an access violation IS the proof.  (field_set_size_guard covers the
    //      reference/array signature kinds; here we bound the primitive widths
    //      that THIS module writes.)
    // =====================================================================
    {
        const char* prim_sigs[] = { "Z", "B", "S", "C", "I", "J", "F", "D" };
        for (const char* sig : prim_sigs)
        {
            vmhook::field_proxy np{ nullptr, sig, true };
            np.set(true);
            np.set(static_cast<std::int8_t>(1));
            np.set(static_cast<std::int16_t>(1));
            np.set(static_cast<std::uint16_t>(1));
            np.set(std::int32_t{ 1 });
            np.set(std::int64_t{ 1 });
            np.set(1.0F);
            np.set(1.0);
            np.set('Z'); // char shortcut on a null "C" must also be safe
        }
        ctx.check("null_field_pointer_set_is_safe_noop", true);
    }

    // =====================================================================
    //  13. JAVA-OBSERVED SNAPSHOT (mode 1).  Drive the probe: run() copies
    //      every field into its seen* witness through genuine getstatic/getfield
    //      + putstatic.  Then read the witnesses back natively and assert the
    //      JVM ITSELF observed the EXACT native-written value.  F/D via RAW bits.
    //
    //      The final values written above are:
    //        sZ=true sB=0xAB(-85) sS=0xBEEF sC=0xFFFF sI=0xDEADBEEF
    //        sJ=0xDEADBEEFCAFEBABE sF=canonicalNaN sD=canonicalNaN
    //        iZ=true iB=0xFE iS=0xCAFE iC=0x20AC iI=0x0BADF00D
    //        iJ=0x0123456789ABCDEF iF=0xC0490FDB iD=pi
    // =====================================================================
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("snapshot_probe_completed", done);

        if (done)
        {
            // ---- static slots, as observed by Java ----
            ctx.check("java_seen_sZ_true", fps::get_bool("seenSZ") == true);
            ctx.check("java_seen_sB_AB",   fps::get_i8("seenSB") == static_cast<std::int8_t>(0xAB));
            ctx.check("java_seen_sS_BEEF", fps::get_i16("seenSS") == static_cast<std::int16_t>(0xBEEF));
            ctx.check("java_seen_sC_FFFF", fps::get_u16("seenSC") == 0xFFFF);
            ctx.check("java_seen_sI_deadbeef", fps::get_i32("seenSI") == static_cast<std::int32_t>(0xDEADBEEF));
            ctx.check("java_seen_sJ_deadbeef", static_cast<std::uint64_t>(fps::get_i64("seenSJ")) == 0xDEADBEEFCAFEBABEULL);
            ctx.check("java_seen_sF_nan_bits", static_cast<std::uint32_t>(fps::get_i32("seenSFBits")) == 0x7FC00000u);
            ctx.check("java_seen_sD_nan_bits", static_cast<std::uint64_t>(fps::get_i64("seenSDBits")) == 0x7FF8000000000000ULL);

            // ---- instance slots, as observed by Java ----
            ctx.check("java_seen_iZ_true", fps::get_bool("seenIZ") == true);
            ctx.check("java_seen_iB_FE",   fps::get_i8("seenIB") == static_cast<std::int8_t>(0xFE));
            ctx.check("java_seen_iS_CAFE", fps::get_i16("seenIS") == static_cast<std::int16_t>(0xCAFE));
            ctx.check("java_seen_iC_20AC", fps::get_u16("seenIC") == 0x20AC);
            ctx.check("java_seen_iI_0BADF00D", fps::get_i32("seenII") == 0x0BADF00D);
            ctx.check("java_seen_iJ_full", fps::get_i64("seenIJ") == 0x0123456789ABCDEFLL);
            ctx.check("java_seen_iF_bits", static_cast<std::uint32_t>(fps::get_i32("seenIFBits")) == 0xC0490FDBu);
            ctx.check("java_seen_iD_pi_bits", static_cast<std::uint64_t>(fps::get_i64("seenIDBits")) == 0x400921FB54442D18ULL);

            // ---- anti-clobber, as observed by Java ----
            ctx.check("java_seen_clob_before_intact", fps::get_i32("seenClobBefore") == 0x11111111);
            ctx.check("java_seen_clob_mid_written",   fps::get_i32("seenClobMid") == 0x2DEF1234);
            ctx.check("java_seen_clob_after_intact",  fps::get_i32("seenClobAfter") == 0x33333333);
            ctx.check("java_seen_clobJ_before_intact", fps::get_i64("seenClobBeforeJ") == 0x1111111111111111LL);
            ctx.check("java_seen_clobJ_mid_written",   fps::get_i64("seenClobMidJ") == 0x2DEF123456789ABCLL);
            ctx.check("java_seen_clobJ_after_intact",  fps::get_i64("seenClobAfterJ") == 0x3333333333333333LL);
        }
    }

    // =====================================================================
    //  14. FLOAT/DOUBLE VALUE-CLASS PREDICATES through Java bytecode.  Set a
    //      special value natively, then have Java evaluate isNaN/isInfinite/
    //      signbit on the live field.  These are UNIVERSAL invariants (they do
    //      not depend on raw-bit canonicalisation), so they are hard checks.
    //      Phase 13 already asserted the raw NaN/Inf BITS survived; here we
    //      confirm the VALUE CLASS the JVM assigns matches.
    // =====================================================================
    {
        // sF currently holds canonical NaN, sD canonical NaN -> assert NaN class.
        ctx.check("java_pred_sF_isNaN", fps::call_bool("sFIsNaN") == true);
        ctx.check("java_pred_sD_isNaN", fps::call_bool("sDIsNaN") == true);

        // +Inf
        if (const auto p{ fps::static_field("sF") }) { p->set(bits_to_float(0x7F800000)); }
        if (const auto p{ fps::static_field("sD") }) { p->set(bits_to_double(0x7FF0000000000000ULL)); }
        ctx.check("java_pred_sF_posinf", fps::call_bool("sFIsPosInf") == true);
        ctx.check("java_pred_sD_posinf", fps::call_bool("sDIsPosInf") == true);

        // -Inf
        if (const auto p{ fps::static_field("sF") }) { p->set(bits_to_float(0xFF800000)); }
        if (const auto p{ fps::static_field("sD") }) { p->set(bits_to_double(0xFFF0000000000000ULL)); }
        ctx.check("java_pred_sF_neginf", fps::call_bool("sFIsNegInf") == true);
        ctx.check("java_pred_sD_neginf", fps::call_bool("sDIsNegInf") == true);

        // -0.0 : value class is "zero with sign bit set".
        if (const auto p{ fps::static_field("sF") }) { p->set(bits_to_float(0x80000000)); }
        if (const auto p{ fps::static_field("sD") }) { p->set(bits_to_double(0x8000000000000000ULL)); }
        ctx.check("java_pred_sF_negzero", fps::call_bool("sFIsNegZero") == true);
        ctx.check("java_pred_sD_negzero", fps::call_bool("sDIsNegZero") == true);

        // Restore canonical NaN so any later observation is well-defined.
        if (const auto p{ fps::static_field("sF") }) { p->set(bits_to_float(0x7FC00000)); }
        if (const auto p{ fps::static_field("sD") }) { p->set(bits_to_double(0x7FF8000000000000ULL)); }
    }

    // =====================================================================
    //  15. JAVA-EVALUATED EXACT COMPARE (mode 2).  Program the expected value
    //      of EVERY field into the fixture's exp* slots (themselves written via
    //      set()), drive the probe so Java's compareAll() compares each live
    //      field against expected and records eq[i], then read eq[] back and
    //      assert every element is true.  A second, Java-side confirmation that
    //      the field equals the native target -- and a meta-proof that set()
    //      works for the exp* programming too.
    // =====================================================================
    {
        // Program expected = current final values.
        fps::set_static<bool>("expSZ", true);
        fps::set_static<std::int8_t>("expSB", static_cast<std::int8_t>(0xAB));
        fps::set_static<std::int16_t>("expSS", static_cast<std::int16_t>(0xBEEF));
        fps::set_static<std::uint16_t>("expSC", 0xFFFF);
        fps::set_static<std::int32_t>("expSI", static_cast<std::int32_t>(0xDEADBEEF));
        fps::set_static<std::int64_t>("expSJ", static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));
        fps::set_static<std::int32_t>("expSFBits", static_cast<std::int32_t>(0x7FC00000u));
        fps::set_static<std::int64_t>("expSDBits", static_cast<std::int64_t>(0x7FF8000000000000ULL));

        fps::set_static<bool>("expIZ", true);
        fps::set_static<std::int8_t>("expIB", static_cast<std::int8_t>(0xFE));
        fps::set_static<std::int16_t>("expIS", static_cast<std::int16_t>(0xCAFE));
        fps::set_static<std::uint16_t>("expIC", 0x20AC);
        fps::set_static<std::int32_t>("expII", 0x0BADF00D);
        fps::set_static<std::int64_t>("expIJ", 0x0123456789ABCDEFLL);
        fps::set_static<std::int32_t>("expIFBits", static_cast<std::int32_t>(0xC0490FDBu));
        fps::set_static<std::int64_t>("expIDBits", static_cast<std::int64_t>(0x400921FB54442D18ULL));

        const bool done{ drive(ctx, 2) };
        ctx.check("compare_probe_completed", done);

        if (done)
        {
            // eq[] is a boolean[] ("[Z") static field; the canonical primitive-
            // array read is the implicit conversion of get() into std::vector<T>
            // (see field_arrays_primitive).  Decode it and assert EVERY documented
            // element is true -- a Java-evaluated, native-visible confirmation that
            // each live field equals the native-programmed expected value (F/D via
            // raw-bits equality in compareAll()).
            const auto eq_field{ fps::static_field("eq") };
            ctx.check("eq_field_resolves", eq_field.has_value());
            ctx.check("eq_field_is_reference", eq_field.has_value() && eq_field->is_reference());
            if (eq_field)
            {
                const std::vector<bool> eq = eq_field->get();
                ctx.check("eq_array_length_18", eq.size() == 18);
                if (eq.size() == 18)
                {
                    // Named per-index assertions (indices mirror the fixture's EQ_*).
                    ctx.check("eq_sZ", eq[0]);
                    ctx.check("eq_sB", eq[1]);
                    ctx.check("eq_sS", eq[2]);
                    ctx.check("eq_sC", eq[3]);
                    ctx.check("eq_sI", eq[4]);
                    ctx.check("eq_sJ", eq[5]);
                    ctx.check("eq_sF_rawbits", eq[6]);
                    ctx.check("eq_sD_rawbits", eq[7]);
                    ctx.check("eq_iZ", eq[8]);
                    ctx.check("eq_iB", eq[9]);
                    ctx.check("eq_iS", eq[10]);
                    ctx.check("eq_iC", eq[11]);
                    ctx.check("eq_iI", eq[12]);
                    ctx.check("eq_iJ", eq[13]);
                    ctx.check("eq_iF_rawbits", eq[14]);
                    ctx.check("eq_iD_rawbits", eq[15]);
                    ctx.check("eq_clob_int_neighbours_intact", eq[16]);
                    ctx.check("eq_clob_long_neighbours_intact", eq[17]);
                    // Aggregate: ALL eighteen Java-evaluated comparisons passed.
                    bool all_eq{ true };
                    for (const bool b : eq) { all_eq = all_eq && b; }
                    ctx.check("eq_all_eighteen_true", all_eq);
                }
            }
        }
    }

    // =====================================================================
    //  16. JAVA GETTER READBACK (static_method portability).  Java's own
    //      bytecode reads each field and returns it, proving the writes are
    //      visible to executing Java code, not just to a memory peek.  char
    //      getters return unsigned int; F/D getters return raw bits.
    // =====================================================================
    {
        ctx.check("java_getter_sZ_true", fps::call_bool("getSZ") == true);
        ctx.check("java_getter_sB_AB",   fps::call_i8("getSB") == static_cast<std::int8_t>(0xAB));
        ctx.check("java_getter_sS_BEEF", fps::call_i16("getSS") == static_cast<std::int16_t>(0xBEEF));
        ctx.check("java_getter_sC_FFFF", fps::call_i32("getSC") == 0xFFFF); // char widened unsigned
        ctx.check("java_getter_sI_deadbeef", fps::call_i32("getSI") == static_cast<std::int32_t>(0xDEADBEEF));
        ctx.check("java_getter_sJ_deadbeef", static_cast<std::uint64_t>(fps::call_i64("getSJ")) == 0xDEADBEEFCAFEBABEULL);
        ctx.check("java_getter_sF_nan_bits", static_cast<std::uint32_t>(fps::call_i32("getSFBits")) == 0x7FC00000u);
        ctx.check("java_getter_sD_nan_bits", static_cast<std::uint64_t>(fps::call_i64("getSDBits")) == 0x7FF8000000000000ULL);

        ctx.check("java_getter_iZ_true", fps::call_bool("getIZ") == true);
        ctx.check("java_getter_iB_FE",   fps::call_i8("getIB") == static_cast<std::int8_t>(0xFE));
        ctx.check("java_getter_iS_CAFE", fps::call_i16("getIS") == static_cast<std::int16_t>(0xCAFE));
        ctx.check("java_getter_iC_20AC", fps::call_i32("getIC") == 0x20AC);
        ctx.check("java_getter_iI_0BADF00D", fps::call_i32("getII") == 0x0BADF00D);
        ctx.check("java_getter_iJ_full", fps::call_i64("getIJ") == 0x0123456789ABCDEFLL);
        ctx.check("java_getter_iF_bits", static_cast<std::uint32_t>(fps::call_i32("getIFBits")) == 0xC0490FDBu);
        ctx.check("java_getter_iD_pi_bits", static_cast<std::uint64_t>(fps::call_i64("getIDBits")) == 0x400921FB54442D18ULL);

        // anti-clobber neighbours visible to Java getters.
        ctx.check("java_getter_clob_before_intact", fps::call_i32("getClobBefore") == 0x11111111);
        ctx.check("java_getter_clob_mid_written",   fps::call_i32("getClobMid") == 0x2DEF1234);
        ctx.check("java_getter_clob_after_intact",  fps::call_i32("getClobAfter") == 0x33333333);
        ctx.check("java_getter_clobJ_before_intact", fps::call_i64("getClobBeforeJ") == 0x1111111111111111LL);
        ctx.check("java_getter_clobJ_mid_written",   fps::call_i64("getClobMidJ") == 0x2DEF123456789ABCLL);
        ctx.check("java_getter_clobJ_after_intact",  fps::call_i64("getClobAfterJ") == 0x3333333333333333LL);
    }

    // =====================================================================
    //  17. WIDTH-MATCHED WRITE IS NEVER REFUSED (guard lower bound).  A guard
    //      regression that started rejecting correctly-sized writes would
    //      silently break every setter; pin that each width-matched write
    //      actually changed the field away from a fresh sentinel.
    // =====================================================================
    {
        // Write a fresh, distinct value into each static slot and confirm it took
        // (i.e. the size guard did NOT refuse a correctly-typed write).
        if (const auto p{ fps::static_field("sB") }) { p->set(static_cast<std::int8_t>(0x12));  ctx.check("guard_ok_B_took", fps::get_i8("sB") == 0x12); }
        if (const auto p{ fps::static_field("sS") }) { p->set(static_cast<std::int16_t>(0x1234)); ctx.check("guard_ok_S_took", fps::get_i16("sS") == 0x1234); }
        if (const auto p{ fps::static_field("sC") }) { p->set(static_cast<std::uint16_t>(0x1234)); ctx.check("guard_ok_C_took", fps::get_u16("sC") == 0x1234); }
        if (const auto p{ fps::static_field("sI") }) { p->set(std::int32_t{ 0x12345678 }); ctx.check("guard_ok_I_took", fps::get_i32("sI") == 0x12345678); }
        if (const auto p{ fps::static_field("sJ") }) { p->set(std::int64_t{ 0x1234567812345678LL }); ctx.check("guard_ok_J_took", fps::get_i64("sJ") == 0x1234567812345678LL); }
        if (const auto p{ fps::static_field("sF") }) { p->set(2.5F); ctx.check("guard_ok_F_took", float_bits(fps::get_float("sF")) == 0x40200000u); }
        if (const auto p{ fps::static_field("sD") }) { p->set(2.5); ctx.check("guard_ok_D_took", double_bits(fps::get_double("sD")) == 0x4004000000000000ULL); }
        if (const auto p{ fps::static_field("sZ") }) { p->set(false); ctx.check("guard_ok_Z_took", fps::get_bool("sZ") == false); }
    }
}
