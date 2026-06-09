// field_static JVM test module  (feature area: fields)
//
// THE static-field authority: exhaustively exercises vmhook's portable static
// field accessor static_field("name") for GET and -- the centre of gravity --
// SET, for EVERY JVM primitive (Z B C S I J F D), java.lang.String, and an
// object reference, with every write PROVEN VISIBLE TO JAVA ITSELF.
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * static_field(name)->set(v) lands the correct bytes on the java.lang.Class
//     mirror for every primitive width AND boundary value, and the JVM observes
//     the new value -- verified two independent ways:
//       (a) the fixture snapshots each field into a "seen*" witness using genuine
//           getstatic/putstatic bytecode (mode-1 probe), which the module reads
//           back, and
//       (b) the module additionally pulls each value back through a Java getter
//           via static_method("getX")->call(), so Java's own bytecode reads the
//           native-written field.
//   * static_field / static_method WORK FROM A STATIC C++ WRAPPER METHOD on every
//     compiler -- the GCC portability guarantee (the deducing-this get_field
//     overloads would not compile here; every accessor below is a static method
//     that calls static_field/static_method, never get_field).
//   * field_proxy::set's size/type guards (audit/findings/field_proxy_set_size_guard.md)
//     refuse a too-wide / mistyped write into a primitive static field, leaving
//     the JVM-visible value byte-for-byte unchanged.
//   * the "C" 1-byte->2-byte widening shortcut lands a full Java char.
//   * object-reference set via unique_ptr<wrapper> rewrites the compressed OOP so
//     Java sees the new identity, and an empty unique_ptr nulls the field.
//   * static GET decodes every primitive + boundary + a String correctly through
//     the same portable accessor, and the static get() path ignores stale init
//     constants (re-read after a runtime putstatic in mode 2).
//
// Harness shape mirrors hook_basic: register_class, a `mode` selector with a
// `done` reset on the rising edge of go, and a dense battery of ctx.check()s.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.FieldStatic.
    //
    // EVERY accessor here is a STATIC method that reaches the field through
    // static_field(...) / static_method(...).  That is the whole portability
    // point of this module: on GCC the deducing-this get_field overloads are
    // non-viable from a static context and would fail to compile, so a uniformly
    // portable wrapper must use the explicit static_field/static_method names.
    class fs : public vmhook::object<fs>
    {
    public:
        explicit fs(vmhook::oop_t instance) noexcept
            : vmhook::object<fs>{ instance }
        {
        }

        // ---- handshake + scenario selector (all via static_field) ----
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // ---- resolve helpers ----
        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // ---- generic typed SET via static_field (proves set lands per width) ----
        template<typename value_type>
        static auto set_value(const char* name, const value_type& v) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(v);
            return true;
        }

        // ---- generic typed GET via static_field ----
        // NOTE: returns field_proxy::value_t by COPY (copy-init), never brace-init,
        // to stay MSVC-unambiguous; callers extract with the desired type.
        static auto get_proxy(const char* name) -> std::optional<vmhook::field_proxy>
        {
            return static_field(name);
        }

        // ---- read a static String field through the portable accessor ----
        static auto get_string(const char* name) -> std::string { return static_field(name)->get(); }

        // ---- set a static String field with an ASCII value ----
        static auto set_string(const char* name, std::string_view value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(std::string{ value });
            return true;
        }

        // ---- raw int read (for the size/type guard targets) ----
        static auto get_int(const char* name) -> std::int32_t { return static_field(name)->get(); }

        static auto get_long(const char* name) -> std::int64_t { return static_field(name)->get(); }

        static auto get_char(const char* name) -> std::uint16_t { return static_field(name)->get(); }

        // ---- acquire a published instance wrapper (objA / objB / objRef) ----
        static auto acquire(const char* name) -> std::unique_ptr<fs> { return static_field(name)->get(); }

        // ---- set an object-reference static field to a wrapper (or null) ----
        static auto set_ref(const char* name, const std::unique_ptr<fs>& target) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(target);
            return true;
        }

        // ---- the Java-side "seen*" witnesses (snapshot via real bytecode) ----
        static auto seen_bool(const char* name) -> bool      { return static_field(name)->get(); }
        static auto seen_i8(const char* name) -> std::int8_t  { const std::int8_t v = static_field(name)->get(); return v; }
        static auto seen_i16(const char* name) -> std::int16_t{ const std::int16_t v = static_field(name)->get(); return v; }
        static auto seen_i32(const char* name) -> std::int32_t{ const std::int32_t v = static_field(name)->get(); return v; }
        static auto seen_i64(const char* name) -> std::int64_t{ const std::int64_t v = static_field(name)->get(); return v; }
        static auto seen_u16(const char* name) -> std::uint16_t{ const std::uint16_t v = static_field(name)->get(); return v; }

        // ---- an instance field read (drives the "needs an object" diagnostic
        //      when called via the static accessor instead) ----
        auto instance_only_int() const -> std::int32_t
        {
            const std::int32_t v = get_field("instanceOnlyInt")->get();
            return v;
        }
        auto tag() const -> std::int32_t
        {
            const std::int32_t v = get_field("tag")->get();
            return v;
        }

        // ---- Java getters pulled through static_method (portable path) ----
        static auto call_get_int(const char* method) -> std::int32_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return -1;
            }
            const std::int32_t v = m->call();
            return v;
        }
        static auto call_get_long(const char* method) -> std::int64_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return -1;
            }
            const std::int64_t v = m->call();
            return v;
        }
        static auto call_get_bool(const char* method) -> bool
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return false;
            }
            const bool v = m->call();
            return v;
        }
        static auto call_get_string(const char* method) -> std::string
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return std::string{ "<<no-method>>" };
            }
            // method_proxy String returns: use as_string() (NOT a cast/brace-init).
            return m->call().as_string();
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
    constexpr std::size_t kIdxU32    = 8;

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

    // ---- Phase-8 getter-call observations.  method_proxy::call() needs a live
    //      current_java_thread, so these are captured INSIDE the touch() detour
    //      (which runs on the Java thread) and read back by the module body.
    //      Sentinels chosen so "did the detour run?" is unambiguous. ----
    constexpr std::int64_t k_uncaptured{ static_cast<std::int64_t>(0xDEADBEEFCAFEF00DULL) };

    std::atomic<int>          g_detour_calls{ 0 };
    std::atomic<bool>         g_detour_saw_self{ false };
    std::atomic<int>          g_get_z{ -1 };          // getZ() -> bool
    std::atomic<std::int64_t> g_get_i{ k_uncaptured };// getI()
    std::atomic<std::int64_t> g_get_j{ k_uncaptured };// getJ()
    std::atomic<std::int64_t> g_get_b{ k_uncaptured };// getB() widened
    std::atomic<std::int64_t> g_get_s{ k_uncaptured };// getS() widened
    std::atomic<std::int64_t> g_get_c{ k_uncaptured };// getC() widened (unsigned)
    std::atomic<std::int64_t> g_get_iord{ k_uncaptured };
    std::atomic<std::int64_t> g_get_strlen{ k_uncaptured };
    std::atomic<std::int64_t> g_get_guard_int{ k_uncaptured };
    std::atomic<std::int64_t> g_get_guard_long{ k_uncaptured };
    std::atomic<std::int64_t> g_get_guard_char{ k_uncaptured };
    std::atomic<int>          g_get_objref_is_b{ -1 };
    std::atomic<int>          g_get_objref_is_null{ -1 };
    std::atomic<std::int64_t> g_get_objref_tag{ k_uncaptured };
    std::atomic<bool>         g_get_str_is_world{ false };

    // Drive one probe cycle for `mode`: clears the latched `done` and programs
    // the selector on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    fs::set_done(false);
                    fs::set_mode(mode);
                }
                fs::set_go(value);
            },
            []() { return fs::get_done(); });
    }
}

VMHOOK_JVM_MODULE(field_static)
{
    vmhook::register_class<fs>("vmhook/fixtures/FieldStatic");

    // =====================================================================
    //  0. Sanity: the class resolves and the portable static accessor works.
    // =====================================================================
    ctx.check("fs_class_registered_static_field_resolves", fs::resolves("setI"));
    ctx.check("fs_static_method_resolves", fs::static_method("getI").has_value());

    // A resolved static proxy reports is_static()==true and the right signature.
    {
        auto p{ fs::get_proxy("setI") };
        if (p)
        {
            ctx.check("static_proxy_is_static_true", p->is_static() == true);
            ctx.check("static_proxy_signature_I", std::string{ p->signature() } == "I");
            ctx.check("static_proxy_address_nonnull", p->raw_address() != nullptr);
        }
    }

    // =====================================================================
    //  1. STATIC SET for EVERY primitive + boundary, written BEFORE go.
    //     field_proxy::set mutates the class-mirror slot directly; no bytecode
    //     needed for the write itself.  We assert the proxy resolved AND that an
    //     immediate native re-read reflects the write (round-trip in C++), then
    //     phases 4/5 prove the SAME bytes are visible to Java.
    // =====================================================================

    // ---- primary battery ------------------------------------------------
    ctx.check("set_Z_resolved", fs::set_value<bool>("setZ", true));
    ctx.check("set_B_resolved", fs::set_value<std::int8_t>("setB", std::numeric_limits<std::int8_t>::min())); // -128
    ctx.check("set_C_resolved", fs::set_value<std::uint16_t>("setC", 0xFFFF));
    ctx.check("set_S_resolved", fs::set_value<std::int16_t>("setS", std::numeric_limits<std::int16_t>::min()));
    ctx.check("set_I_resolved", fs::set_value<std::int32_t>("setI", std::numeric_limits<std::int32_t>::min()));
    ctx.check("set_J_resolved", fs::set_value<std::int64_t>("setJ", std::numeric_limits<std::int64_t>::max()));
    ctx.check("set_F_resolved", fs::set_value<float>("setF", 0.15625f)); // 0x3E200000 exact
    ctx.check("set_D_resolved", fs::set_value<double>("setD", 0.1));     // 0x3FB999999999999A

    // ---- secondary battery (different edge patterns) --------------------
    ctx.check("set_Z2_resolved", fs::set_value<bool>("setZ2", false));
    ctx.check("set_B2_resolved", fs::set_value<std::int8_t>("setB2", static_cast<std::int8_t>(0xFF))); // -1
    ctx.check("set_C2_resolved", fs::set_value<std::uint16_t>("setC2", 0x20AC));                       // euro
    ctx.check("set_S2_resolved", fs::set_value<std::int16_t>("setS2", static_cast<std::int16_t>(0xBEEF)));
    ctx.check("set_I2_resolved", fs::set_value<std::int32_t>("setI2", static_cast<std::int32_t>(0xDEADBEEF)));
    ctx.check("set_J2_resolved", fs::set_value<std::int64_t>("setJ2", std::numeric_limits<std::int64_t>::min()));
    ctx.check("set_F2_resolved", fs::set_value<float>("setF2", -std::numeric_limits<float>::infinity()));
    ctx.check("set_D2_resolved", fs::set_value<double>("setD2", std::numeric_limits<double>::quiet_NaN()));

    // ---- ordinary mid-range values --------------------------------------
    ctx.check("set_IOrd_resolved", fs::set_value<std::int32_t>("setIOrd", 123456789));
    ctx.check("set_JOrd_resolved", fs::set_value<std::int64_t>("setJOrd", 0x0123456789ABCDEFLL));
    ctx.check("set_DOrd_resolved", fs::set_value<double>("setDOrd", 3.141592653589793));
    ctx.check("set_FOrd_resolved", fs::set_value<float>("setFOrd", 1.5f)); // exact in binary

    // ---- immediate native re-read round-trip (before any Java involvement) --
    {
        const auto pz{ fs::get_proxy("setZ") };
        if (pz) { const bool v = pz->get(); ctx.check("set_Z_native_reread_true", v == true); }
        const auto pb{ fs::get_proxy("setB") };
        if (pb)
        {
            const auto v{ pb->get() };
            ctx.check("set_B_native_reread_min", static_cast<std::int8_t>(v) == std::numeric_limits<std::int8_t>::min());
            ctx.check("set_B_native_variant_i8", v.data.index() == kIdxI8);
        }
        const auto pc{ fs::get_proxy("setC") };
        if (pc)
        {
            const auto v{ pc->get() };
            ctx.check("set_C_native_reread_FFFF", static_cast<std::uint16_t>(v) == 0xFFFF);
            ctx.check("set_C_native_variant_u16", v.data.index() == kIdxU16);
        }
        const auto ps{ fs::get_proxy("setS") };
        if (ps) { const std::int16_t v = ps->get(); ctx.check("set_S_native_reread_min", v == std::numeric_limits<std::int16_t>::min()); }
        const auto pi{ fs::get_proxy("setI") };
        if (pi) { const std::int32_t v = pi->get(); ctx.check("set_I_native_reread_min", v == std::numeric_limits<std::int32_t>::min()); }
        const auto pj{ fs::get_proxy("setJ") };
        if (pj) { const std::int64_t v = pj->get(); ctx.check("set_J_native_reread_max", v == std::numeric_limits<std::int64_t>::max()); }
        const auto pf{ fs::get_proxy("setF") };
        if (pf) { const float v = pf->get(); ctx.check("set_F_native_reread_bits", float_bits(v) == 0x3E200000u); }
        const auto pd{ fs::get_proxy("setD") };
        if (pd) { const double v = pd->get(); ctx.check("set_D_native_reread_bits", double_bits(v) == 0x3FB999999999999AULL); }
        const auto pf2{ fs::get_proxy("setF2") };
        if (pf2) { const float v = pf2->get(); ctx.check("set_F2_native_reread_neg_inf", std::isinf(v) && v < 0.0f); }
        const auto pd2{ fs::get_proxy("setD2") };
        if (pd2) { const double v = pd2->get(); ctx.check("set_D2_native_reread_nan", std::isnan(v)); }
    }

    // =====================================================================
    //  2. STRING static SET (ASCII, length-preserving on all JDKs).
    // =====================================================================
    // CONTRACT of the write path exercised here (field_proxy::set(std::string)
    // -> vmhook::set_str_field -> vmhook::write_java_string, vmhook.hpp ~15667):
    // write_java_string is an IN-PLACE, LENGTH-PRESERVING, MIN-LENGTH overwrite.
    // It overwrites exactly min(value.size(), existing_backing_length) code units
    // of the String's existing backing array and NEVER changes the String's
    // logical length, never reallocates, and never replaces the String reference.
    // Consequences proven by this phase:
    //   * equal-length write ("AAAAA" <- "world") fully replaces the content;
    //   * SHORTER write ("world" <- "hi") overwrites only the first 2 units and
    //     LEAVES THE TAIL, so the field reads back "hi"+"rld" == "hirld" with
    //     length still 5 (NOT "hi" with length 2) -- a real ergonomic limitation:
    //     callers who need to truly shorten/grow/replace a String must build a
    //     new String and rewrite the reference (the field still aliases the old
    //     backing otherwise).  A longer-than-backing write would be truncated.
    // The fixture's setStr/setStrShort are deliberately new String(char[])
    // (private backing), never bare interned literals: an in-place write into an
    // interned-literal backing corrupts that shared constant-pool String JVM-wide
    // (see FieldStatic.java + audit/findings/field_proxy_string_set.md).
    ctx.record("[INFO] field_static: write_java_string is in-place + "
               "length-preserving + min-length overwrite -- a SHORTER write "
               "leaves the tail (\"world\"<-\"hi\" reads back \"hirld\", len stays "
               "5) and a longer write truncates; it never resizes or replaces the "
               "String reference (vmhook.hpp ~15667). Targets must own a private "
               "backing (new String(char[])) or the write corrupts the shared "
               "interned literal process-wide.");

    ctx.check("set_str_resolved", fs::set_string("setStr", "world"));        // "AAAAA" <- "world" (len 5)
    ctx.check("set_str_short_resolved", fs::set_string("setStrShort", "hi")); // "world" <- "hi" -> "hirld"

    // Immediate native re-read of the String set.
    ctx.check("set_str_native_reread_world", fs::get_string("setStr") == "world");
    ctx.check("set_str_short_native_reread_hirld", fs::get_string("setStrShort") == "hirld");

    // =====================================================================
    //  3. SIZE / TYPE GUARD (audit: field_proxy_set_size_guard.md).
    //     Mistyped writes into a primitive static field must be REFUSED with
    //     the field's JVM-visible value left byte-for-byte unchanged.
    // =====================================================================

    // guardInt initial value before any write.
    ctx.check("guard_int_initial", fs::get_int("guardInt") == 0x11223344);

    // (a) set(int64) into an "I" (4-byte) field -> too wide -> refused.
    fs::set_value<std::int64_t>("guardInt", std::int64_t{ 0x7766554433221100LL });
    ctx.check("guard_int_too_wide_refused", fs::get_int("guardInt") == 0x11223344);

    // (b) set(std::string) into an "I" field -> non-primitive into primitive -> refused.
    fs::set_string("guardInt", "99999");
    ctx.check("guard_int_string_refused", fs::get_int("guardInt") == 0x11223344);

    // (c) set(int32) into a "J" (8-byte) field -> too narrow -> refused.
    ctx.check("guard_long_initial", fs::get_long("guardLong") == 0x1122334455667788LL);
    fs::set_value<std::int32_t>("guardLong", std::int32_t{ 0x09ABCDEF });
    ctx.check("guard_long_too_narrow_refused", fs::get_long("guardLong") == 0x1122334455667788LL);

    // (d) "C" 1-byte->2-byte widening shortcut: a C++ char 'Z' (0x5A) must land
    //     the full 2-byte Java char 0x005A, not a half-written / clobbered value.
    fs::set_value<char>("guardChar", 'Z');
    ctx.check("guard_char_widened_to_005A", fs::get_char("guardChar") == 0x005A);
    // A high-bit char byte 0xE9 widens to 0x00E9 (high byte zero), never sign-extended.
    fs::set_value<char>("guardChar", static_cast<char>(0xE9));
    ctx.check("guard_char_high_byte_zero_extended", fs::get_char("guardChar") == 0x00E9);

    // (e) correctly-sized write into "I" SUCCEEDS (control for the guard).
    fs::set_value<std::int32_t>("guardInt", 0x55667788);
    ctx.check("guard_int_right_size_succeeds", fs::get_int("guardInt") == 0x55667788);
    // restore for the Java snapshot (mode 3 also resets, but be explicit).
    fs::set_value<std::int32_t>("guardInt", 0x11223344);
    ctx.check("guard_int_restored", fs::get_int("guardInt") == 0x11223344);

    // =====================================================================
    //  4. OBJECT-REFERENCE static SET via unique_ptr<wrapper>.
    //     objRef starts at objA; rewrite it to objB, prove identity flips, then
    //     null it.  Java-side identity confirmation happens in phase 6.
    // =====================================================================
    {
        const auto objA{ fs::acquire("objA") };
        const auto objB{ fs::acquire("objB") };
        ctx.check("objA_acquired", objA != nullptr);
        ctx.check("objB_acquired", objB != nullptr);
        ctx.check("objA_tag_is_A", objA != nullptr && objA->tag() == 0xA);
        ctx.check("objB_tag_is_B", objB != nullptr && objB->tag() == 0xB);

        // objRef initially aliases objA: reading its tag yields 0xA.
        {
            const auto ref0{ fs::acquire("objRef") };
            ctx.check("objRef_initially_A", ref0 != nullptr && ref0->tag() == 0xA);
        }

        // Rewrite objRef = objB (compressed-OOP write into the static slot).
        ctx.check("set_ref_to_B_resolved", fs::set_ref("objRef", objB));
        {
            const auto ref1{ fs::acquire("objRef") };
            ctx.check("objRef_now_B_native_reread", ref1 != nullptr && ref1->tag() == 0xB);
        }

        // Null the reference via an empty unique_ptr -> compressed 0 -> Java null.
        {
            const std::unique_ptr<fs> empty{};
            ctx.check("set_ref_to_null_resolved", fs::set_ref("objRef", empty));
            const auto refN{ fs::acquire("objRef") };
            ctx.check("objRef_now_null_native_reread", refN == nullptr);
        }

        // Put it back to objB so the Java snapshot sees a non-null, identity==B.
        ctx.check("set_ref_back_to_B_resolved", fs::set_ref("objRef", objB));
    }

    // =====================================================================
    //  5. STATIC GET battery (independent of field_primitives_get): every
    //     primitive boundary + a String, through the SAME portable accessor.
    // =====================================================================
    {
        const auto z1{ fs::get_proxy("gZTrue") };
        if (z1) { const auto v{ z1->get() }; ctx.check("get_gZTrue", static_cast<bool>(v) == true);  ctx.check("get_gZTrue_variant", v.data.index() == kIdxBool); }
        const auto z0{ fs::get_proxy("gZFalse") };
        if (z0) { const bool v = z0->get(); ctx.check("get_gZFalse", v == false); }
        const auto bmin{ fs::get_proxy("gBMin") };
        if (bmin) { const std::int8_t v = bmin->get(); ctx.check("get_gBMin", v == std::numeric_limits<std::int8_t>::min()); }
        const auto bmax{ fs::get_proxy("gBMax") };
        if (bmax) { const std::int8_t v = bmax->get(); ctx.check("get_gBMax", v == std::numeric_limits<std::int8_t>::max()); }
        const auto smin{ fs::get_proxy("gSMin") };
        if (smin) { const std::int16_t v = smin->get(); ctx.check("get_gSMin", v == std::numeric_limits<std::int16_t>::min()); }
        const auto smax{ fs::get_proxy("gSMax") };
        if (smax) { const std::int16_t v = smax->get(); ctx.check("get_gSMax", v == std::numeric_limits<std::int16_t>::max()); }
        const auto cmax{ fs::get_proxy("gCMax") };
        if (cmax) { const std::uint16_t v = cmax->get(); ctx.check("get_gCMax", v == 0xFFFF); }
        const auto imin{ fs::get_proxy("gIMin") };
        if (imin) { const std::int32_t v = imin->get(); ctx.check("get_gIMin", v == std::numeric_limits<std::int32_t>::min()); }
        const auto imax{ fs::get_proxy("gIMax") };
        if (imax) { const std::int32_t v = imax->get(); ctx.check("get_gIMax", v == std::numeric_limits<std::int32_t>::max()); }
        const auto jmin{ fs::get_proxy("gJMin") };
        if (jmin) { const std::int64_t v = jmin->get(); ctx.check("get_gJMin", v == std::numeric_limits<std::int64_t>::min()); }
        const auto jmax{ fs::get_proxy("gJMax") };
        if (jmax) { const std::int64_t v = jmax->get(); ctx.check("get_gJMax", v == std::numeric_limits<std::int64_t>::max()); }
        const auto fone{ fs::get_proxy("gFOne") };
        if (fone) { const float v = fone->get(); ctx.check("get_gFOne_bits", float_bits(v) == 0x3F800000u); }
        const auto done1{ fs::get_proxy("gDOne") };
        if (done1) { const double v = done1->get(); ctx.check("get_gDOne_bits", double_bits(v) == 0x3FF0000000000000ULL); }
        ctx.check("get_gStr", fs::get_string("gStr") == "field_static");
    }

    // =====================================================================
    //  6. ERROR / NULL / EDGE paths for the static accessor.
    // =====================================================================
    {
        // Unknown field -> nullopt.
        ctx.check("static_field_unknown_is_nullopt", fs::static_field("noSuchField").has_value() == false);
        // An INSTANCE-only field requested through the STATIC accessor must fail
        // with the "needs an object instance" diagnostic (returns nullopt).
        ctx.check("static_field_on_instance_field_is_nullopt",
                  fs::static_field("instanceOnlyInt").has_value() == false);
        // But the SAME field resolves through an instance wrapper.
        {
            const auto inst{ fs::acquire("objA") };
            if (inst)
            {
                const auto p{ inst->get_field("instanceOnlyInt") };
                ctx.check("instance_field_resolves_via_instance", p.has_value());
                if (p) { const std::int32_t v = p->get(); ctx.check("instance_field_value_4242", v == 4242); }
                ctx.check("instance_field_is_static_false", p.has_value() && p->is_static() == false);
            }
        }
        // Unknown static method -> nullopt.
        ctx.check("static_method_unknown_is_nullopt", fs::static_method("noSuchMethod").has_value() == false);

        // A null-field_pointer proxy: set() must early-return (no crash) and
        // leave nothing to read; get() yields the int32 fallback.
        {
            vmhook::field_proxy np{ nullptr, "I", true };
            np.set(std::int32_t{ 1234 });            // must be a safe no-op
            const auto v{ np.get() };
            ctx.check("null_proxy_get_is_int32_fallback", v.data.index() == kIdxI32);
            const std::int32_t got = v;
            ctx.check("null_proxy_get_value_zero", got == 0);
        }
    }

    // =====================================================================
    //  7. SET-THEN-READ-BACK THROUGH JAVA  (the headline contract).
    //     mode 1: the probe snapshots every set* target into its seen* witness
    //     using genuine getstatic/putstatic, so we read back exactly what the
    //     JVM observed for the native writes performed in phases 1/2/4.
    // =====================================================================
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("snapshot_probe_completed", done);

        if (done)
        {
            // ---- primary battery, as seen by Java ----
            ctx.check("java_seenZ_true",  fs::seen_bool("seenZ") == true);
            ctx.check("java_seenB_min",   fs::seen_i8("seenB") == std::numeric_limits<std::int8_t>::min());
            ctx.check("java_seenC_FFFF",  fs::seen_u16("seenC") == 0xFFFF);
            ctx.check("java_seenS_min",   fs::seen_i16("seenS") == std::numeric_limits<std::int16_t>::min());
            ctx.check("java_seenI_min",   fs::seen_i32("seenI") == std::numeric_limits<std::int32_t>::min());
            ctx.check("java_seenJ_max",   fs::seen_i64("seenJ") == std::numeric_limits<std::int64_t>::max());
            ctx.check("java_seenF_bits",  fs::seen_i32("seenFBits") == static_cast<std::int32_t>(0x3E200000));
            ctx.check("java_seenD_bits",  fs::seen_i64("seenDBits") == static_cast<std::int64_t>(0x3FB999999999999AULL));

            // ---- secondary battery, as seen by Java ----
            ctx.check("java_seenZ2_false", fs::seen_bool("seenZ2") == false);
            ctx.check("java_seenB2_negone", fs::seen_i8("seenB2") == static_cast<std::int8_t>(-1));
            ctx.check("java_seenC2_euro",  fs::seen_u16("seenC2") == 0x20AC);
            ctx.check("java_seenS2_beef",  fs::seen_i16("seenS2") == static_cast<std::int16_t>(0xBEEF));
            ctx.check("java_seenI2_deadbeef", fs::seen_i32("seenI2") == static_cast<std::int32_t>(0xDEADBEEF));
            ctx.check("java_seenJ2_min",   fs::seen_i64("seenJ2") == std::numeric_limits<std::int64_t>::min());
            ctx.check("java_seenF2_neg_inf_bits", fs::seen_i32("seenF2Bits") == static_cast<std::int32_t>(0xFF800000));
            // Canonical double NaN: Java's doubleToRawLongBits of our quiet NaN.
            // We don't pin the exact payload bits (platform qNaN encodings differ);
            // instead require the IEEE NaN exponent/mantissa shape via Java below.
            {
                const std::int64_t nb{ fs::seen_i64("seenD2Bits") };
                const std::uint64_t u{ static_cast<std::uint64_t>(nb) };
                const bool is_nan_shape{ (u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL
                                         && (u & 0x000FFFFFFFFFFFFFULL) != 0 };
                ctx.check("java_seenD2_is_nan_bit_shape", is_nan_shape);
            }

            // ---- ordinary values, as seen by Java ----
            ctx.check("java_seenIOrd",  fs::seen_i32("seenIOrd") == 123456789);
            ctx.check("java_seenJOrd",  fs::seen_i64("seenJOrd") == 0x0123456789ABCDEFLL);
            ctx.check("java_seenDOrd_pi_bits", fs::seen_i64("seenDOrdBits") == static_cast<std::int64_t>(0x400921FB54442D18ULL));
            ctx.check("java_seenFOrd_1p5_bits", fs::seen_i32("seenFOrdBits") == static_cast<std::int32_t>(0x3FC00000));

            // ---- String, as seen by Java ----
            ctx.check("java_seenStr_world", fs::get_string("seenStr") == "world");
            ctx.check("java_seenStr_len_5", fs::seen_i32("seenStrLen") == 5);
            // FIXED (was misdiagnosed as a write_java_string bug): the native
            // reads above prove setStr's backing holds "world", and Java's
            // `"world".equals(setStr)` now also returns TRUE.  The earlier uniform
            // FALSE was a TEST-FIXTURE bug, not a library bug: the legacy driver's
            // set_static_string_array({"alpha","omega","?"}) wrote "omega" IN PLACE
            // over Example.staticStringArray[1], which was the *interned* "world"
            // literal — corrupting the very "world" constant this probe compares
            // against (so the comparison became "omega".equals("world")).  Fixed by
            // giving staticStringArray non-interned backings in Example.java;
            // write_java_string itself was always correct (proven by a live
            // build+inject investigation: setStr equals a char-array-built "world").
            ctx.check("java_seenStr_eq_world",
                      fs::seen_bool("seenStrEqWorld") == true);
            ctx.record("[INFO] field_static: setStr == \"world\" both natively and via Java "
                       ".equals (write_java_string is correct; the prior mismatch was an interned-"
                       "literal corruption from a legacy in-place array write, now fixed).");
            ctx.check("java_seenStrShort_hirld", fs::get_string("seenStrShort") == "hirld");
            ctx.check("java_seenStrShort_len_5", fs::seen_i32("seenStrShortLen") == 5);

            // ---- guard targets unchanged, as seen by Java ----
            ctx.check("java_seenGuardInt_unchanged", fs::seen_i32("seenGuardInt") == 0x11223344);
            ctx.check("java_seenGuardLong_unchanged", fs::seen_i64("seenGuardLong") == 0x1122334455667788LL);
            ctx.check("java_seenGuardChar_high_byte_zero", fs::seen_u16("seenGuardChar") == 0x00E9);

            // ---- object reference identity, as seen by Java ----
            ctx.check("java_objRef_is_B_not_A", fs::seen_bool("seenObjRefIsB") == true);
            ctx.check("java_objRef_not_A", fs::seen_bool("seenObjRefIsA") == false);
            ctx.check("java_objRef_not_null", fs::seen_bool("seenObjRefIsNull") == false);
            ctx.check("java_objRef_tag_is_B", fs::seen_i32("seenObjRefTag") == 0xB);
        }
    }

    // =====================================================================
    //  8. SET-THEN-READ-BACK THROUGH JAVA GETTERS (static_method portability).
    //     Pull each native-written value back through a Java getter method via
    //     static_method("getX")->call(): Java's own bytecode reads the field and
    //     returns it, proving the writes are visible to executing Java code (not
    //     just to a memory peek), and exercising the portable static_method path.
    // =====================================================================
    {
        ctx.check("java_getter_Z_true",  fs::call_get_bool("getZ") == true);
        ctx.check("java_getter_I_min",   fs::call_get_int("getI") == std::numeric_limits<std::int32_t>::min());
        ctx.check("java_getter_J_max",   fs::call_get_long("getJ") == std::numeric_limits<std::int64_t>::max());
        ctx.check("java_getter_B_min",   fs::call_get_int("getB") == -128); // widened to int on return
        ctx.check("java_getter_S_min",   fs::call_get_int("getS") == std::numeric_limits<std::int16_t>::min());
        ctx.check("java_getter_C_FFFF",  fs::call_get_int("getC") == 0xFFFF); // char widened unsigned to int
        ctx.check("java_getter_IOrd",    fs::call_get_int("getIOrd") == 123456789);
        ctx.check("java_getter_Str_world", fs::call_get_string("getStr") == "world");
        ctx.check("java_getter_StrLen_5",  fs::call_get_int("getStrLen") == 5);
        ctx.check("java_getter_GuardInt_unchanged", fs::call_get_int("getGuardInt") == 0x11223344);
        ctx.check("java_getter_GuardLong_unchanged", fs::call_get_long("getGuardLong") == 0x1122334455667788LL);
        ctx.check("java_getter_GuardChar_00E9", fs::call_get_int("getGuardChar") == 0x00E9);
        ctx.check("java_getter_objRefIsB", fs::call_get_bool("objRefIsB") == true);
        ctx.check("java_getter_objRefIsNull_false", fs::call_get_bool("objRefIsNull") == false);
        ctx.check("java_getter_objRefTag_B", fs::call_get_int("getObjRefTag") == 0xB);
    }

    // =====================================================================
    //  9. RUNTIME GET freshness: mode 2 putstatic writes brand-new boundary
    //     values; the static get() must reflect the LIVE post-dispatch state,
    //     not the class-initializer constants (proves get() is not cached/stale).
    // =====================================================================
    {
        // Before the runtime write the r* fields hold their default (false/0).
        ctx.check("runtime_rI_initially_zero", fs::get_int("rI") == 0);

        const bool done{ drive(ctx, 2) };
        ctx.check("runtime_probe_completed", done);

        if (done)
        {
            const auto pz{ fs::get_proxy("rZ") };
            if (pz) { const bool v = pz->get(); ctx.check("runtime_rZ_true", v == true); }
            const auto pi{ fs::get_proxy("rI") };
            if (pi) { const std::int32_t v = pi->get(); ctx.check("runtime_rI_min", v == std::numeric_limits<std::int32_t>::min()); }
            const auto pj{ fs::get_proxy("rJ") };
            if (pj) { const std::int64_t v = pj->get(); ctx.check("runtime_rJ_max", v == std::numeric_limits<std::int64_t>::max()); }
            const auto pd{ fs::get_proxy("rD") };
            if (pd) { const double v = pd->get(); ctx.check("runtime_rD_is_nan", std::isnan(v)); }
            const auto pc{ fs::get_proxy("rC") };
            if (pc) { const std::uint16_t v = pc->get(); ctx.check("runtime_rC_FFFF", v == 0xFFFF); }
        }
    }

    // =====================================================================
    //  10. Repeatability + parity: a second native write to the SAME static
    //      field overwrites cleanly, and reading twice yields identical bytes.
    //      Also proves static_field returns a FRESH proxy each call (no stale
    //      cached field_pointer across writes).
    // =====================================================================
    {
        ctx.check("overwrite_I_first", fs::set_value<std::int32_t>("setI", 0x0BADC0DE));
        ctx.check("overwrite_I_first_reread", fs::get_int("setI") == 0x0BADC0DE);
        ctx.check("overwrite_I_second", fs::set_value<std::int32_t>("setI", 0x600DC0DE));
        ctx.check("overwrite_I_second_reread", fs::get_int("setI") == 0x600DC0DE);

        const std::int32_t a{ fs::get_int("setI") };
        const std::int32_t b{ fs::get_int("setI") };
        ctx.check("repeatable_static_get_same_value", a == b);

        // A second static_field() handle to the same field sees the latest write.
        {
            const auto p1{ fs::get_proxy("setI") };
            const auto p2{ fs::get_proxy("setI") };
            if (p1 && p2)
            {
                ctx.check("two_proxies_same_address", p1->raw_address() == p2->raw_address());
                const std::int32_t v2 = p2->get();
                ctx.check("two_proxies_agree_latest", v2 == 0x600DC0DE);
            }
        }
    }

    // =====================================================================
    //  11. Restore the fixture to a clean state (mode 3) so the suite leaves no
    //      mutated globals behind for any later-running module sharing the JVM.
    // =====================================================================
    {
        const bool done{ drive(ctx, 3) };
        ctx.check("reset_probe_completed", done);
        if (done)
        {
            ctx.check("reset_setStr_back_to_AAAAA", fs::get_string("setStr") == "AAAAA");
            ctx.check("reset_guardInt_back", fs::get_int("guardInt") == 0x11223344);
            const auto ref{ fs::acquire("objRef") };
            ctx.check("reset_objRef_back_to_A", ref != nullptr && ref->tag() == 0xA);
        }
    }
}
