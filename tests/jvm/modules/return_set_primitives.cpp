// return_set_primitives JVM test module — exhaustively exercises
// vmhook::return_value::set(value) (the force-return path) for every primitive
// return type on a LIVE JVM.
//
// Feature under test: vmhook/ext/vmhook/vmhook.hpp:1147-1176.
//   return_value::set<T>(value) sets return_slot::cancel = true and writes the
//   user value into the 64-bit return_slot::retval cell.  Signed integrals < 8
//   bytes are sign-extended via static_cast<int64_t>; everything else takes a
//   zero-fill + memcpy path.  The trampoline epilogue
//   (vmhook.hpp:5314-5315 Win64 / 5411-5412 SysV) loads that cell into BOTH rax
//   (integer return) and xmm0 (float/double return), so the same slot bits
//   serve every Java return descriptor.
//
// Strategy: the fixture is a dumb actor that calls each orig* method once and
// stores what the Java caller OBSERVED into a per-type field.  Each orig*
// method returns a fixed value the native side NEVER forces, so a passing
// check proves the original body was skipped and the forced slot was delivered.
// This module re-arms its 16 hooks (8 instance + 8 static) with a fresh value
// vector each "round", runs ONE run_probe handshake, then asserts every
// observed field equals what it forced — covering canonical, boundary, and
// IEEE-754 special values across many rounds.
//
// Mirrors pilot.cpp's register_class + scoped_hook + run_probe shape.  Uses
// scoped_hook only (never shutdown_hooks): each round's handles live in a
// nested block and disarm when the block ends, so the next round installs
// clean.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnSetPrimitives.
    class rsp_fixture : public vmhook::object<rsp_fixture>
    {
    public:
        explicit rsp_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<rsp_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }

        // Observed-value readers (instance dispatch path).
        static auto obs_bool()   -> bool          { return static_field("obsBool")->get(); }
        static auto obs_byte()   -> std::int8_t   { return static_field("obsByte")->get(); }
        static auto obs_short()  -> std::int16_t  { return static_field("obsShort")->get(); }
        static auto obs_int()    -> std::int32_t  { return static_field("obsInt")->get(); }
        static auto obs_int_readback() -> std::int32_t { return static_field("obsIntReadback")->get(); }
        static auto obs_long()   -> std::int64_t  { return static_field("obsLong")->get(); }
        static auto obs_float()  -> float         { return static_field("obsFloat")->get(); }
        static auto obs_double() -> double         { return static_field("obsDouble")->get(); }
        static auto obs_char()   -> std::uint16_t { return static_field("obsChar")->get(); }

        // Observed-value readers (static dispatch path).
        static auto obs_s_bool()   -> bool          { return static_field("obsStaticBool")->get(); }
        static auto obs_s_byte()   -> std::int8_t   { return static_field("obsStaticByte")->get(); }
        static auto obs_s_short()  -> std::int16_t  { return static_field("obsStaticShort")->get(); }
        static auto obs_s_int()    -> std::int32_t  { return static_field("obsStaticInt")->get(); }
        static auto obs_s_long()   -> std::int64_t  { return static_field("obsStaticLong")->get(); }
        static auto obs_s_float()  -> float         { return static_field("obsStaticFloat")->get(); }
        static auto obs_s_double() -> double         { return static_field("obsStaticDouble")->get(); }
        static auto obs_s_char()   -> std::uint16_t { return static_field("obsStaticChar")->get(); }

        // Sub-int returns captured WIDENED to int (the caller-side mask+widen
        // view: byte/short sign-extend, char zero-extends).
        static auto obs_byte_as_int()  -> std::int32_t { return static_field("obsByteAsInt")->get(); }
        static auto obs_short_as_int() -> std::int32_t { return static_field("obsShortAsInt")->get(); }
        static auto obs_char_as_int()  -> std::int32_t { return static_field("obsCharAsInt")->get(); }
        static auto obs_s_byte_as_int()  -> std::int32_t { return static_field("obsStaticByteAsInt")->get(); }
        static auto obs_s_short_as_int() -> std::int32_t { return static_field("obsStaticShortAsInt")->get(); }
        static auto obs_s_char_as_int()  -> std::int32_t { return static_field("obsStaticCharAsInt")->get(); }

        static auto saw_exception() -> bool        { return static_field("sawException")->get(); }
        static auto round_count()   -> std::int32_t { return static_field("roundCount")->get(); }
    };

    // ---- One value vector forced in a given round -------------------------
    struct forced_values
    {
        bool          b;
        std::int8_t   by;
        std::int16_t  s;
        std::int32_t  i;
        std::int64_t  l;
        float         f;
        double        d;
        std::uint16_t c; // jchar is unsigned 16-bit; we force a char16_t value.
    };

    // Per-hook fire counters + self-observation, reset before each round.
    std::atomic<int>  g_inst_fires{ 0 };
    std::atomic<int>  g_stat_fires{ 0 };
    std::atomic<bool> g_inst_all_saw_self{ true };

    auto reset_round_counters() -> void
    {
        g_inst_fires.store(0, std::memory_order_relaxed);
        g_stat_fires.store(0, std::memory_order_relaxed);
        g_inst_all_saw_self.store(true, std::memory_order_relaxed);
    }

    auto note_self(const std::unique_ptr<rsp_fixture>& self) -> void
    {
        g_inst_fires.fetch_add(1, std::memory_order_relaxed);
        if (self == nullptr)
        {
            g_inst_all_saw_self.store(false, std::memory_order_relaxed);
        }
    }

    auto note_static() -> void
    {
        g_stat_fires.fetch_add(1, std::memory_order_relaxed);
    }

    // Bit-exact float/double comparison (so NaN==NaN and -0.0 != +0.0 are
    // detected): the JVM caller must receive the EXACT bit pattern we forced.
    auto same_bits(float a, float b) -> bool
    {
        std::uint32_t ua{}, ub{};
        std::memcpy(&ua, &a, sizeof(ua));
        std::memcpy(&ub, &b, sizeof(ub));
        return ua == ub;
    }
    auto same_bits(double a, double b) -> bool
    {
        std::uint64_t ua{}, ub{};
        std::memcpy(&ua, &a, sizeof(ua));
        std::memcpy(&ub, &b, sizeof(ub));
        return ua == ub;
    }

    // Reinterpret a raw 32/64-bit pattern AS a float/double.  Used to forge an
    // exact IEEE-754 bit pattern (e.g. a NaN with a SPECIFIC mantissa payload, a
    // chosen subnormal) without relying on a literal that a compiler might fold.
    // The value forced into the slot and the value the JVM caller must observe
    // are then compared bit-for-bit by same_bits().
    auto bits_to_float(std::uint32_t bits) -> float
    {
        float out{};
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }
    auto bits_to_double(std::uint64_t bits) -> double
    {
        double out{};
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    // Arm all 16 hooks for one value vector, run ONE probe, leave the observed
    // fields populated for the caller to assert on.  Returns whether the probe
    // completed.  All handles are local, so they disarm when this function
    // returns — the next round re-installs cleanly without shutdown_hooks().
    auto run_round(vmhook_test::context& ctx,
                   const std::string&    tag,
                   const forced_values&  v) -> bool
    {
        reset_round_counters();
        rsp_fixture::set_done(false);

        // INSTANCE hooks: signature is (return_value&, const unique_ptr<T>&).
        auto h_bool  { vmhook::scoped_hook<rsp_fixture>("origBool",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.b); }) };
        auto h_byte  { vmhook::scoped_hook<rsp_fixture>("origByte",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.by); }) };
        auto h_short { vmhook::scoped_hook<rsp_fixture>("origShort",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.s); }) };
        auto h_int   { vmhook::scoped_hook<rsp_fixture>("origInt",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.i); }) };
        auto h_long  { vmhook::scoped_hook<rsp_fixture>("origLong",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.l); }) };
        auto h_float { vmhook::scoped_hook<rsp_fixture>("origFloat",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.f); }) };
        auto h_double{ vmhook::scoped_hook<rsp_fixture>("origDouble",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.d); }) };
        // jchar is unsigned 16-bit: force a char16_t (NOT plain 'char', whose
        // signedness is implementation-defined — see audit Bug #1).
        auto h_char  { vmhook::scoped_hook<rsp_fixture>("origChar",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(static_cast<char16_t>(v.c)); }) };

        // STATIC hooks: signature is (return_value&) — no 'this'.
        auto hs_bool  { vmhook::scoped_hook<rsp_fixture>("origStaticBool",
            [v](vmhook::return_value& r) { note_static(); r.set(v.b); }) };
        auto hs_byte  { vmhook::scoped_hook<rsp_fixture>("origStaticByte",
            [v](vmhook::return_value& r) { note_static(); r.set(v.by); }) };
        auto hs_short { vmhook::scoped_hook<rsp_fixture>("origStaticShort",
            [v](vmhook::return_value& r) { note_static(); r.set(v.s); }) };
        auto hs_int   { vmhook::scoped_hook<rsp_fixture>("origStaticInt",
            [v](vmhook::return_value& r) { note_static(); r.set(v.i); }) };
        auto hs_long  { vmhook::scoped_hook<rsp_fixture>("origStaticLong",
            [v](vmhook::return_value& r) { note_static(); r.set(v.l); }) };
        auto hs_float { vmhook::scoped_hook<rsp_fixture>("origStaticFloat",
            [v](vmhook::return_value& r) { note_static(); r.set(v.f); }) };
        auto hs_double{ vmhook::scoped_hook<rsp_fixture>("origStaticDouble",
            [v](vmhook::return_value& r) { note_static(); r.set(v.d); }) };
        auto hs_char  { vmhook::scoped_hook<rsp_fixture>("origStaticChar",
            [v](vmhook::return_value& r) { note_static(); r.set(static_cast<char16_t>(v.c)); }) };

        // Each round must (re)install all 16 hooks; if any failed to install
        // the round's value checks are meaningless, so surface it once.
        const bool all_installed{
            h_bool.installed()  && h_byte.installed()  && h_short.installed() &&
            h_int.installed()   && h_long.installed()  && h_float.installed() &&
            h_double.installed()&& h_char.installed()  &&
            hs_bool.installed() && hs_byte.installed() && hs_short.installed() &&
            hs_int.installed()  && hs_long.installed() && hs_float.installed() &&
            hs_double.installed()&& hs_char.installed() };
        ctx.check(tag + "_all_16_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value) { rsp_fixture::set_go(value); },
            []() { return rsp_fixture::get_done(); }) };
        ctx.check(tag + "_probe_completed", done);
        return done;
    }

    // Assert every observed field == the forced value for this round.
    auto check_round(vmhook_test::context& ctx,
                     const std::string&    tag,
                     const forced_values&  v) -> void
    {
        // --- Each instance hook fires once per Java call.  The fixture invokes
        // origInt() TWICE (obsInt + obsIntReadback stability readback), and
        // origByte/origShort/origChar TWICE each (narrow obs* + widened
        // obs*AsInt), and the rest once: 8 origs + 1 int-readback + 3
        // byte/short/char widened readbacks = 12 instance fires.  The static
        // path calls each of its 8 origs once plus 3 widened byte/short/char
        // readbacks = 11 fires.
        ctx.check(tag + "_instance_hooks_fired", g_inst_fires.load() == 12);
        ctx.check(tag + "_static_hooks_fired",   g_stat_fires.load() == 11);
        ctx.check(tag + "_instance_hooks_saw_self", g_inst_all_saw_self.load());
        ctx.check(tag + "_no_java_exception",       !rsp_fixture::saw_exception());

        // --- INSTANCE: forced value observed by the Java caller.
        ctx.check(tag + "_inst_bool",   rsp_fixture::obs_bool()  == v.b);
        ctx.check(tag + "_inst_byte",   rsp_fixture::obs_byte()  == v.by);
        ctx.check(tag + "_inst_short",  rsp_fixture::obs_short() == v.s);
        ctx.check(tag + "_inst_int",    rsp_fixture::obs_int()   == v.i);
        ctx.check(tag + "_inst_long",   rsp_fixture::obs_long()  == v.l);
        ctx.check(tag + "_inst_float",  same_bits(rsp_fixture::obs_float(),  v.f));
        ctx.check(tag + "_inst_double", same_bits(rsp_fixture::obs_double(), v.d));
        ctx.check(tag + "_inst_char",   rsp_fixture::obs_char()  == v.c);
        // Stability: a second read of the forced int yields the same value.
        ctx.check(tag + "_inst_int_stable", rsp_fixture::obs_int_readback() == v.i);

        // --- STATIC: forced value observed by the Java caller.
        ctx.check(tag + "_stat_bool",   rsp_fixture::obs_s_bool()  == v.b);
        ctx.check(tag + "_stat_byte",   rsp_fixture::obs_s_byte()  == v.by);
        ctx.check(tag + "_stat_short",  rsp_fixture::obs_s_short() == v.s);
        ctx.check(tag + "_stat_int",    rsp_fixture::obs_s_int()   == v.i);
        ctx.check(tag + "_stat_long",   rsp_fixture::obs_s_long()  == v.l);
        ctx.check(tag + "_stat_float",  same_bits(rsp_fixture::obs_s_float(),  v.f));
        ctx.check(tag + "_stat_double", same_bits(rsp_fixture::obs_s_double(), v.d));
        ctx.check(tag + "_stat_char",   rsp_fixture::obs_s_char()  == v.c);

        // --- Caller-side mask + widen: the same forced narrow value, read back
        // through a byte/short/char method into an int local, must surface the
        // JVM-correct 32-bit widening.  byte/short SIGN-extend (C++ int8_t/
        // int16_t -> int32_t does too); jchar ZERO-extends (uint16_t -> int32_t
        // does too).  This is the load-bearing proof that the forced 64-bit slot
        // is masked to the declared sub-int width AND widened with the correct
        // signedness at the ireturn boundary — distinct from the narrow-field
        // reads above, which the JVM had already masked before storing.
        ctx.check(tag + "_inst_byte_widened",  rsp_fixture::obs_byte_as_int()  == static_cast<std::int32_t>(v.by));
        ctx.check(tag + "_inst_short_widened", rsp_fixture::obs_short_as_int() == static_cast<std::int32_t>(v.s));
        ctx.check(tag + "_inst_char_widened",  rsp_fixture::obs_char_as_int()  == static_cast<std::int32_t>(v.c));
        ctx.check(tag + "_stat_byte_widened",  rsp_fixture::obs_s_byte_as_int()  == static_cast<std::int32_t>(v.by));
        ctx.check(tag + "_stat_short_widened", rsp_fixture::obs_s_short_as_int() == static_cast<std::int32_t>(v.s));
        ctx.check(tag + "_stat_char_widened",  rsp_fixture::obs_s_char_as_int()  == static_cast<std::int32_t>(v.c));

        // jchar must NEVER sign-extend: the widened-to-int view of any forced
        // char keeps the upper 16 bits CLEAR (value in [0, 0xFFFF]).  A regression
        // that sign-extended the char path would surface a negative obsCharAsInt
        // for any code unit >= 0x8000 (e.g. the surrogate / 0xFFFF rounds).
        ctx.check(tag + "_inst_char_widened_nonneg",
                  rsp_fixture::obs_char_as_int() >= 0 && rsp_fixture::obs_char_as_int() <= 0xFFFF);
        ctx.check(tag + "_stat_char_widened_nonneg",
                  rsp_fixture::obs_s_char_as_int() >= 0 && rsp_fixture::obs_s_char_as_int() <= 0xFFFF);
    }

    auto run_and_check(vmhook_test::context& ctx,
                       const std::string&    tag,
                       const forced_values&  v) -> void
    {
        if (run_round(ctx, tag, v))
        {
            check_round(ctx, tag, v);
        }
    }

    // ---- OVER-WIDE force vector --------------------------------------------
    // The force-return template (vmhook.hpp:1353-1382) does NOT consult the
    // hooked method's Java return descriptor: it writes whatever the caller
    // hands it into the 64-bit slot.  When the value forced is WIDER than the
    // method's declared narrow return type, the JVM's ireturn family masks the
    // slot to the declared width on the CALLER side.  This vector forces a value
    // strictly wider than the method's type and pins the masked result the Java
    // caller must observe — the dimension the matching-width rounds cannot reach.
    struct overwide_values
    {
        std::int32_t over_byte;   // forced int32_t into a `byte`  method
        std::int32_t over_short;  // forced int32_t into a `short` method
        std::int32_t over_char;   // forced int32_t into a `char`  method
        std::int64_t over_int;    // forced int64_t into an `int`  method
        bool         b;           // canonical bool (only {0,1} is JDK-portable)
    };

    // The Java-visible masked-and-widened expectations for an over-wide vector.
    auto over_exp_byte(std::int32_t over)  -> std::int8_t  { return static_cast<std::int8_t>(static_cast<std::uint32_t>(over) & 0xFFu); }
    auto over_exp_short(std::int32_t over) -> std::int16_t { return static_cast<std::int16_t>(static_cast<std::uint32_t>(over) & 0xFFFFu); }
    auto over_exp_char(std::int32_t over)  -> std::uint16_t{ return static_cast<std::uint16_t>(static_cast<std::uint32_t>(over) & 0xFFFFu); }
    auto over_exp_int(std::int64_t over)   -> std::int32_t { return static_cast<std::int32_t>(static_cast<std::uint64_t>(over) & 0xFFFFFFFFULL); }

    auto run_overwide(vmhook_test::context& ctx,
                      const std::string&    tag,
                      const overwide_values& v) -> bool
    {
        reset_round_counters();
        rsp_fixture::set_done(false);

        // The long/float/double methods are still hooked (so the fire counts
        // stay at the standard 12/11) but forced to harmless sentinels; only the
        // byte/short/char/int slots carry the over-wide payloads.
        const std::int64_t l_sent{ 0x0123456789ABCDEFLL };
        const float        f_sent{ 4.5f };
        const double       d_sent{ 6.25 };

        auto h_bool  { vmhook::scoped_hook<rsp_fixture>("origBool",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.b); }) };
        // Force a WIDE int32_t into the byte method.
        auto h_byte  { vmhook::scoped_hook<rsp_fixture>("origByte",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.over_byte); }) };
        auto h_short { vmhook::scoped_hook<rsp_fixture>("origShort",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.over_short); }) };
        auto h_int   { vmhook::scoped_hook<rsp_fixture>("origInt",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.over_int); }) }; // int64_t into int method
        auto h_long  { vmhook::scoped_hook<rsp_fixture>("origLong",
            [l_sent](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(l_sent); }) };
        auto h_float { vmhook::scoped_hook<rsp_fixture>("origFloat",
            [f_sent](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(f_sent); }) };
        auto h_double{ vmhook::scoped_hook<rsp_fixture>("origDouble",
            [d_sent](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(d_sent); }) };
        // Force a WIDE int32_t into the char method; only the low 16 bits are jchar.
        auto h_char  { vmhook::scoped_hook<rsp_fixture>("origChar",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.over_char); }) };

        auto hs_bool  { vmhook::scoped_hook<rsp_fixture>("origStaticBool",
            [v](vmhook::return_value& r) { note_static(); r.set(v.b); }) };
        auto hs_byte  { vmhook::scoped_hook<rsp_fixture>("origStaticByte",
            [v](vmhook::return_value& r) { note_static(); r.set(v.over_byte); }) };
        auto hs_short { vmhook::scoped_hook<rsp_fixture>("origStaticShort",
            [v](vmhook::return_value& r) { note_static(); r.set(v.over_short); }) };
        auto hs_int   { vmhook::scoped_hook<rsp_fixture>("origStaticInt",
            [v](vmhook::return_value& r) { note_static(); r.set(v.over_int); }) };
        auto hs_long  { vmhook::scoped_hook<rsp_fixture>("origStaticLong",
            [l_sent](vmhook::return_value& r) { note_static(); r.set(l_sent); }) };
        auto hs_float { vmhook::scoped_hook<rsp_fixture>("origStaticFloat",
            [f_sent](vmhook::return_value& r) { note_static(); r.set(f_sent); }) };
        auto hs_double{ vmhook::scoped_hook<rsp_fixture>("origStaticDouble",
            [d_sent](vmhook::return_value& r) { note_static(); r.set(d_sent); }) };
        auto hs_char  { vmhook::scoped_hook<rsp_fixture>("origStaticChar",
            [v](vmhook::return_value& r) { note_static(); r.set(v.over_char); }) };

        const bool all_installed{
            h_bool.installed()  && h_byte.installed()  && h_short.installed() &&
            h_int.installed()   && h_long.installed()  && h_float.installed() &&
            h_double.installed()&& h_char.installed()  &&
            hs_bool.installed() && hs_byte.installed() && hs_short.installed() &&
            hs_int.installed()  && hs_long.installed() && hs_float.installed() &&
            hs_double.installed()&& hs_char.installed() };
        ctx.check(tag + "_all_16_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value) { rsp_fixture::set_go(value); },
            []() { return rsp_fixture::get_done(); }) };
        ctx.check(tag + "_probe_completed", done);
        return done;
    }

    auto run_and_check_overwide(vmhook_test::context& ctx,
                                const std::string&     tag,
                                const overwide_values& v) -> void
    {
        if (!run_overwide(ctx, tag, v))
        {
            return;
        }
        // Fire counts unchanged: all 16 hooks armed, fixture dispatch identical.
        ctx.check(tag + "_instance_hooks_fired", g_inst_fires.load() == 12);
        ctx.check(tag + "_static_hooks_fired",   g_stat_fires.load() == 11);
        ctx.check(tag + "_no_java_exception",    !rsp_fixture::saw_exception());

        const std::int8_t   eb{ over_exp_byte(v.over_byte) };
        const std::int16_t  es{ over_exp_short(v.over_short) };
        const std::uint16_t ec{ over_exp_char(v.over_char) };
        const std::int32_t  ei{ over_exp_int(v.over_int) };

        // INSTANCE: the JVM masked the over-wide slot to the declared width.
        ctx.check(tag + "_inst_byte_masked",  rsp_fixture::obs_byte()  == eb);
        ctx.check(tag + "_inst_short_masked", rsp_fixture::obs_short() == es);
        ctx.check(tag + "_inst_char_masked",  rsp_fixture::obs_char()  == ec);
        ctx.check(tag + "_inst_int_masked",   rsp_fixture::obs_int()   == ei);
        ctx.check(tag + "_inst_bool",         rsp_fixture::obs_bool()  == v.b);
        // Widened-to-int views: byte/short sign-extend the masked value, char
        // zero-extends — exactly the matching-width helper's expectation, proving
        // the masked-then-widened result is identical whether the C++ side forced
        // a narrow or an over-wide value.
        ctx.check(tag + "_inst_byte_widened",  rsp_fixture::obs_byte_as_int()  == static_cast<std::int32_t>(eb));
        ctx.check(tag + "_inst_short_widened", rsp_fixture::obs_short_as_int() == static_cast<std::int32_t>(es));
        ctx.check(tag + "_inst_char_widened",  rsp_fixture::obs_char_as_int()  == static_cast<std::int32_t>(ec));

        // STATIC: same masking on the no-'this' dispatch path.
        ctx.check(tag + "_stat_byte_masked",  rsp_fixture::obs_s_byte()  == eb);
        ctx.check(tag + "_stat_short_masked", rsp_fixture::obs_s_short() == es);
        ctx.check(tag + "_stat_char_masked",  rsp_fixture::obs_s_char()  == ec);
        ctx.check(tag + "_stat_int_masked",   rsp_fixture::obs_s_int()   == ei);
        ctx.check(tag + "_stat_bool",         rsp_fixture::obs_s_bool()  == v.b);
        ctx.check(tag + "_stat_byte_widened",  rsp_fixture::obs_s_byte_as_int()  == static_cast<std::int32_t>(eb));
        ctx.check(tag + "_stat_short_widened", rsp_fixture::obs_s_short_as_int() == static_cast<std::int32_t>(es));
        ctx.check(tag + "_stat_char_widened",  rsp_fixture::obs_s_char_as_int()  == static_cast<std::int32_t>(ec));

        // jchar masked from an over-wide payload is still in [0, 0xFFFF].
        ctx.check(tag + "_inst_char_widened_nonneg",
                  rsp_fixture::obs_char_as_int() >= 0 && rsp_fixture::obs_char_as_int() <= 0xFFFF);
        ctx.check(tag + "_stat_char_widened_nonneg",
                  rsp_fixture::obs_s_char_as_int() >= 0 && rsp_fixture::obs_s_char_as_int() <= 0xFFFF);
    }
}

VMHOOK_JVM_MODULE(return_set_primitives)
{
    vmhook::register_class<rsp_fixture>("vmhook/fixtures/ReturnSetPrimitives");

    // Constants for readability.
    constexpr float  f_pos_inf{  std::numeric_limits<float>::infinity() };
    constexpr float  f_neg_inf{ -std::numeric_limits<float>::infinity() };
    constexpr double d_pos_inf{  std::numeric_limits<double>::infinity() };
    constexpr double d_neg_inf{ -std::numeric_limits<double>::infinity() };
    const     float  f_qnan{ std::numeric_limits<float>::quiet_NaN() };
    const     double d_qnan{ std::numeric_limits<double>::quiet_NaN() };

    float  f_neg_zero{ -0.0f };
    double d_neg_zero{ -0.0 };

    // Largest/smallest finite magnitudes and the smallest positive SUBNORMAL.
    // max()      = largest finite (sign clear).
    // lowest()   = most-negative finite (== -max(); sign bit set).
    // min()      = smallest POSITIVE NORMAL (exponent field == 1, mantissa 0).
    // denorm_min = smallest POSITIVE SUBNORMAL (exponent field 0, mantissa == 1)
    //              — a flush-to-zero FPU path would corrupt it, so a bit-exact
    //              round-trip proves the slot/movq-xmm0 epilogue copies, never
    //              arithmetises, the value.
    constexpr float  f_max{ std::numeric_limits<float>::max() };
    constexpr double d_max{ std::numeric_limits<double>::max() };
    constexpr float  f_lowest{ std::numeric_limits<float>::lowest() };
    constexpr double d_lowest{ std::numeric_limits<double>::lowest() };
    constexpr float  f_min_normal{ std::numeric_limits<float>::min() };
    constexpr double d_min_normal{ std::numeric_limits<double>::min() };
    const     float  f_subnormal{ std::numeric_limits<float>::denorm_min() };
    const     double d_subnormal{ std::numeric_limits<double>::denorm_min() };

    // NaNs carrying a SPECIFIC, non-canonical mantissa payload (distinct from
    // the library/std quiet-NaN used in the "qnan" round).  The exponent field
    // is all-ones and a hand-chosen payload sits in the mantissa; same_bits()
    // then demands the EXACT payload survive the movq xmm0 epilogue, proving the
    // slot is not "normalised" to a canonical NaN.  Bit 22 (float) / 51 (double)
    // is set so the value is a quiet NaN regardless of platform sNaN handling.
    const     float  f_nan_payload{ bits_to_float(0x7FAB1234u) };
    const     double d_nan_payload{ bits_to_double(0x7FF8ABCDEF012345ULL) };

    // POSITIVE zero — the bit-exact twin of neg_zero (0x00000000 /
    // 0x0000000000000000).  Forced in its own round so +0.0 and -0.0 are each
    // pinned bit-for-bit; same_bits() distinguishes them, so a path that dropped
    // the sign bit would pass pos_zero yet fail neg_zero (and vice-versa).
    const     float  f_pos_zero{ bits_to_float(0x00000000u) };
    const     double d_pos_zero{ bits_to_double(0x0000000000000000ULL) };

    // LARGEST positive subnormal (float 0x007FFFFF / double
    // 0x000FFFFFFFFFFFFF): exponent field 0, mantissa all-ones — the very top of
    // the subnormal range, one ULP below min_normal.  Pairs with the
    // denorm_min() round to bracket BOTH ends of the subnormal band, so a
    // flush-to-zero arithmetic path is caught at the large end too.
    const     float  f_subnormal_max{ bits_to_float(0x007FFFFFu) };
    const     double d_subnormal_max{ bits_to_double(0x000FFFFFFFFFFFFFULL) };

    // A NEGATIVE quiet NaN: sign bit set on top of the all-ones exponent and a
    // quiet mantissa (float 0xFFC00000 / double 0xFFF8000000000000).  The
    // movq-xmm0 epilogue must carry bit 63/31 too, so a sign-stripping
    // "abs(NaN)" mishandling would flip these bits and same_bits() would catch
    // it — a dimension the positive qNaN rounds cannot reach.
    const     float  f_neg_qnan{ bits_to_float(0xFFC00000u) };
    const     double d_neg_qnan{ bits_to_double(0xFFF8000000000000ULL) };

    // Clean powers of two: float/double 1.0 and 2.0 have a non-trivial biased
    // exponent with a ZERO mantissa (1.0f = 0x3F800000, 2.0f = 0x40000000,
    // 1.0d = 0x3FF0000000000000).  Distinct from the fractional witnesses
    // (3.5f/2.5/PI) because the mantissa is empty — proving the exponent field
    // alone rides the slot intact.
    const     float  f_one{ bits_to_float(0x3F800000u) };
    const     double d_two{ bits_to_double(0x4000000000000000ULL) };

    // ROUND 1 — canonical "obviously not the original" values, all distinct
    // from every orig* return.  The bedrock that the force-return path works
    // at all for each primitive, on both instance and static dispatch.
    run_and_check(ctx, "canonical", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(99),
        /*s */ static_cast<std::int16_t>(31000),
        /*i */ 0x12345678,
        /*l */ static_cast<std::int64_t>(0x0123456789ABCDEFLL),
        /*f */ 3.5f,
        /*d */ 2.5,
        /*c */ static_cast<std::uint16_t>(0x263A) }); // ☺

    // ROUND 2 — forced bool FALSE over an original that also returns false is
    // weak; instead force bool=false here while the canonical round forced
    // true.  This proves set(false) takes the cancel path (forces the return)
    // rather than "doing nothing": the original origBool() returns false too,
    // but origByte..origChar prove the slot was delivered, and the static
    // bool=false path is asserted via obsStaticBool.  Also: minimum/zero
    // boundaries for every integral.
    run_and_check(ctx, "min_zero", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0),
        /*s */ static_cast<std::int16_t>(0),
        /*i */ 0,
        /*l */ static_cast<std::int64_t>(0),
        /*f */ 0.0f,
        /*d */ 0.0,
        /*c */ static_cast<std::uint16_t>(0) }); // U+0000

    // ROUND 3 — signed MINIMUMS.  This is the load-bearing sign-extension
    // angle (vmhook.hpp:1165-1169): static_cast<int64_t>(INT8_MIN/…/INT32_MIN)
    // must land 0xFFFFFFFF80000000-style bits so the interpreter's ireturn
    // pops the negative value, not the zero-extended positive.  Long takes the
    // memcpy path (sizeof==8), so INT64_MIN proves the full 8 bytes land.
    run_and_check(ctx, "signed_min", forced_values{
        /*b */ true,
        /*by*/ std::numeric_limits<std::int8_t>::min(),    // -128
        /*s */ std::numeric_limits<std::int16_t>::min(),   // -32768
        /*i */ std::numeric_limits<std::int32_t>::min(),   // INT_MIN
        /*l */ std::numeric_limits<std::int64_t>::min(),   // LONG_MIN
        /*f */ -1.0f,
        /*d */ -1.0,
        /*c */ static_cast<std::uint16_t>(0x0001) });

    // ROUND 4 — signed MAXIMUMS for every integral; char MAX (0xFFFF, unsigned)
    // proves jchar is NOT sign-extended (the upper 48 bits must stay zero).
    run_and_check(ctx, "signed_max", forced_values{
        /*b */ true,
        /*by*/ std::numeric_limits<std::int8_t>::max(),    // 127
        /*s */ std::numeric_limits<std::int16_t>::max(),   // 32767
        /*i */ std::numeric_limits<std::int32_t>::max(),   // INT_MAX
        /*l */ std::numeric_limits<std::int64_t>::max(),   // LONG_MAX
        /*f */ 1.0f,
        /*d */ 1.0,
        /*c */ static_cast<std::uint16_t>(0xFFFF) });      // jchar max

    // ROUND 5 — "minus one" everywhere: the classic sign-extension trap.  A
    // byte/short/int of -1 that is zero-extended would surface as +255/+65535/
    // +4294967295 on the Java side; this round fails loudly if the
    // sign-extension branch ever regresses.  char 0xFFFF is the unsigned twin.
    run_and_check(ctx, "minus_one", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(-1),
        /*s */ static_cast<std::int16_t>(-1),
        /*i */ -1,
        /*l */ static_cast<std::int64_t>(-1),
        /*f */ -123.0f,
        /*d */ -456.0,
        /*c */ static_cast<std::uint16_t>(0xFFFF) });

    // ROUND 6 — high-bit-set sub-max values that are NOT min/max but exercise
    // the boundary between sign- and zero-extension precisely:
    //   byte  0x80 = -128, short 0x8000 = -32768, int 0x80000000 = INT_MIN,
    //   char  0x8000 (unsigned 32768).  These overlap min by design but use the
    //   raw hex form a user is most likely to type, catching off-by-one in the
    //   predicate.
    run_and_check(ctx, "high_bit", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0x80),
        /*s */ static_cast<std::int16_t>(0x8000),
        /*i */ static_cast<std::int32_t>(0x80000000),
        /*l */ static_cast<std::int64_t>(0x8000000000000000ULL),
        /*f */ 7.0f,
        /*d */ 7.0,
        /*c */ static_cast<std::uint16_t>(0x8000) });

    // ROUND 7 — IEEE-754 NEGATIVE ZERO.  -0.0 must round-trip bit-exactly:
    //   float  0x80000000, double 0x8000000000000000.  same_bits() distinguishes
    // it from +0.0, proving the memcpy path preserved the sign bit through the
    // movq xmm0 epilogue.  (Integrals carry small sentinels so their checks
    // stay meaningful.)
    run_and_check(ctx, "neg_zero", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(7),
        /*s */ static_cast<std::int16_t>(7),
        /*i */ 7,
        /*l */ static_cast<std::int64_t>(7),
        /*f */ f_neg_zero,
        /*d */ d_neg_zero,
        /*c */ static_cast<std::uint16_t>(7) });

    // ROUND 8 — IEEE-754 POSITIVE INFINITY for float and double.
    run_and_check(ctx, "pos_inf", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(8),
        /*s */ static_cast<std::int16_t>(8),
        /*i */ 8,
        /*l */ static_cast<std::int64_t>(8),
        /*f */ f_pos_inf,
        /*d */ d_pos_inf,
        /*c */ static_cast<std::uint16_t>(8) });

    // ROUND 9 — IEEE-754 NEGATIVE INFINITY for float and double.
    run_and_check(ctx, "neg_inf", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(9),
        /*s */ static_cast<std::int16_t>(9),
        /*i */ 9,
        /*l */ static_cast<std::int64_t>(9),
        /*f */ f_neg_inf,
        /*d */ d_neg_inf,
        /*c */ static_cast<std::uint16_t>(9) });

    // ROUND 10 — IEEE-754 quiet NaN.  NaN must NOT be normalized or rendered
    // as 0; same_bits() requires the exact qNaN payload to survive.  This is
    // the angle most likely to break if anyone "cleans up" the retval value.
    run_and_check(ctx, "qnan", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(10),
        /*s */ static_cast<std::int16_t>(10),
        /*i */ 10,
        /*l */ static_cast<std::int64_t>(10),
        /*f */ f_qnan,
        /*d */ d_qnan,
        /*c */ static_cast<std::uint16_t>(10) });

    // ROUND 11 — float/double precision witnesses: values that are NOT exactly
    // representable as the OTHER width, proving the slot carries the true
    // 32-bit / 64-bit pattern and the JVM reads the correct register width.
    //   float  Float.intBitsToFloat(0x3FC00000) == 1.5f (exact),
    //   double Math.PI (not representable in float).
    run_and_check(ctx, "precision", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(11),
        /*s */ static_cast<std::int16_t>(11),
        /*i */ 11,
        /*l */ static_cast<std::int64_t>(11),
        /*f */ 0.1f,                       // 0.1f != (float)0.1d round-trip trap
        /*d */ 3.141592653589793,          // Math.PI
        /*c */ static_cast<std::uint16_t>(11) });

    // ROUND 12 — long boundary that spills past 32 bits: a value whose low 32
    // bits are zero but whose high bits are non-zero, proving the FULL 64-bit
    // long is delivered (not just the low dword).  char ASCII boundary 0x7F.
    run_and_check(ctx, "long_high_dword", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(0x7F),  // byte max via hex
        /*s */ static_cast<std::int16_t>(0x7FFF),
        /*i */ 0x7FFFFFFF,
        /*l */ static_cast<std::int64_t>(0x7FFFFFFF00000000LL),
        /*f */ 12.0f,
        /*d */ 12.0,
        /*c */ static_cast<std::uint16_t>(0x007F) }); // DEL

    // ROUND 13 — char-specific coverage: a surrogate-range code unit (0xD83D)
    // and an astral-plane low surrogate are perfectly valid jchar values (a
    // jchar is just an unsigned 16-bit code unit, not a validated codepoint).
    // Forcing 0xD83D proves no surrogate filtering / no sign extension.
    run_and_check(ctx, "char_surrogate", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(13),
        /*s */ static_cast<std::int16_t>(13),
        /*i */ 13,
        /*l */ static_cast<std::int64_t>(13),
        /*f */ 13.0f,
        /*d */ 13.0,
        /*c */ static_cast<std::uint16_t>(0xD83D) }); // high surrogate

    // ROUND 14 — signed-integral MIN+1: the off-by-one neighbour just ABOVE the
    // minimum.  A sign-extension predicate that is wrong only at exactly MIN
    // would still pass signed_min by luck; -127 / INT16_MIN+1 / INT32_MIN+1 /
    // INT64_MIN+1 are negative non-extreme values that MUST keep their sign
    // through static_cast<int64_t> (byte/short/int) and the memcpy path (long).
    run_and_check(ctx, "boundary_plus_one", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(std::numeric_limits<std::int8_t>::min() + 1),    // -127
        /*s */ static_cast<std::int16_t>(std::numeric_limits<std::int16_t>::min() + 1),  // -32767
        /*i */ std::numeric_limits<std::int32_t>::min() + 1,                             // INT_MIN+1
        /*l */ std::numeric_limits<std::int64_t>::min() + 1,                             // LONG_MIN+1
        /*f */ 14.5f,
        /*d */ 14.25,
        /*c */ static_cast<std::uint16_t>(0x0002) });

    // ROUND 15 — signed-integral MAX-1: the off-by-one neighbour just BELOW the
    // maximum (positive, non-extreme).  char 0xFFFE is jchar-max minus one and
    // must keep the upper 48 bits CLEAR (no accidental sign extension next to
    // the top of the unsigned range).
    run_and_check(ctx, "boundary_minus_one", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(std::numeric_limits<std::int8_t>::max() - 1),    // 126
        /*s */ static_cast<std::int16_t>(std::numeric_limits<std::int16_t>::max() - 1),  // 32766
        /*i */ std::numeric_limits<std::int32_t>::max() - 1,                             // INT_MAX-1
        /*l */ std::numeric_limits<std::int64_t>::max() - 1,                             // LONG_MAX-1
        /*f */ 15.5f,
        /*d */ 15.25,
        /*c */ static_cast<std::uint16_t>(0xFFFE) });

    // ROUND 16 — LONG low-dword saturated, high-dword zero (0x00000000FFFFFFFF).
    // This is the 32->64 boundary the OTHER way from long_high_dword: a 64-bit
    // long whose low 32 bits are all ones must NOT be sign-extended (long takes
    // the memcpy path, sizeof==8), so the high dword stays 0 — i.e. the Java
    // caller sees +4294967295L, never -1L.  byte/short/int carry their own
    // low-bits-set sentinels; char 0x0100 is the first 2-byte UTF-8 boundary.
    run_and_check(ctx, "long_low_dword_max", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(16),
        /*s */ static_cast<std::int16_t>(16),
        /*i */ 16,
        /*l */ static_cast<std::int64_t>(0x00000000FFFFFFFFLL), // 4294967295
        /*f */ 16.5f,
        /*d */ 16.25,
        /*c */ static_cast<std::uint16_t>(0x0100) });

    // ROUND 17 — LONG exact 32->64 carry point (0x0000000100000000 == 2^32):
    // bit 32 set, every lower bit clear.  Proves the single bit that straddles
    // the two 32-bit halves is delivered, so a truncating "low dword only" path
    // would surface 0 here and fail loudly.  char 0x0200 keeps the char slot
    // distinct.
    run_and_check(ctx, "long_carry_2pow32", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(17),
        /*s */ static_cast<std::int16_t>(17),
        /*i */ 17,
        /*l */ static_cast<std::int64_t>(0x0000000100000000LL), // 2^32
        /*f */ 17.5f,
        /*d */ 17.25,
        /*c */ static_cast<std::uint16_t>(0x0200) });

    // ROUND 18 — float/double TYPE MAX (largest finite magnitude), bit-exact.
    //   float  0x7F7FFFFF, double 0x7FEFFFFFFFFFFFFF.  Distinct from +Inf: the
    // exponent is all-ones-minus-one with a full mantissa, so a mishandled
    // overflow-to-Inf would change the bits and same_bits() would catch it.
    run_and_check(ctx, "float_double_type_max", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(18),
        /*s */ static_cast<std::int16_t>(18),
        /*i */ 18,
        /*l */ static_cast<std::int64_t>(18),
        /*f */ f_max,
        /*d */ d_max,
        /*c */ static_cast<std::uint16_t>(0x4D41) }); // 'MA'

    // ROUND 19 — float/double smallest POSITIVE NORMAL (min()), bit-exact.
    //   float  0x00800000, double 0x0010000000000000.  Exponent field == 1,
    // mantissa 0 — the very edge above the subnormal range.
    run_and_check(ctx, "float_double_min_normal", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(19),
        /*s */ static_cast<std::int16_t>(19),
        /*i */ 19,
        /*l */ static_cast<std::int64_t>(19),
        /*f */ f_min_normal,
        /*d */ d_min_normal,
        /*c */ static_cast<std::uint16_t>(0x4D49) }); // 'MI'

    // ROUND 20 — float/double LOWEST finite (== -max(); sign bit set),
    // bit-exact.  The most-negative representable finite value, distinct from
    // type_max only by the sign bit, proving the sign rides through the slot for
    // the maximum-magnitude operand.
    run_and_check(ctx, "float_double_lowest", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(20),
        /*s */ static_cast<std::int16_t>(20),
        /*i */ 20,
        /*l */ static_cast<std::int64_t>(20),
        /*f */ f_lowest,
        /*d */ d_lowest,
        /*c */ static_cast<std::uint16_t>(0x4C4F) }); // 'LO'

    // ROUND 21 — float/double smallest POSITIVE SUBNORMAL (denorm_min()),
    // bit-exact.  float 0x00000001, double 0x0000000000000001: exponent field 0,
    // mantissa == 1.  An FPU path with flush-to-zero / denormals-are-zero would
    // turn this into +0.0; same_bits() proves the slot is copied verbatim and
    // never passed through an arithmetic unit.
    run_and_check(ctx, "float_double_subnormal", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(21),
        /*s */ static_cast<std::int16_t>(21),
        /*i */ 21,
        /*l */ static_cast<std::int64_t>(21),
        /*f */ f_subnormal,
        /*d */ d_subnormal,
        /*c */ static_cast<std::uint16_t>(0x5355) }); // 'SU'

    // ROUND 22 — NaN with a SPECIFIC mantissa payload (not the canonical quiet
    // NaN of the qnan round).  float 0x7FAB1234, double 0x7FF8ABCDEF012345 must
    // arrive with their payload intact: same_bits() fails if the value were
    // canonicalised to 0x7FC00000 / 0x7FF8000000000000.  This is the strongest
    // guard that the slot delivers ARBITRARY bit patterns, not just "a NaN".
    run_and_check(ctx, "float_double_nan_payload", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(22),
        /*s */ static_cast<std::int16_t>(22),
        /*i */ 22,
        /*l */ static_cast<std::int64_t>(22),
        /*f */ f_nan_payload,
        /*d */ d_nan_payload,
        /*c */ static_cast<std::uint16_t>(0x4E50) }); // 'NP'

    // ROUND 23 — positive sign-bit-neighbour integrals: MAX-with-low-bit-clear
    //   byte 0x7E, short 0x7FFE, int 0x7FFFFFFE, long 0x7FFFFFFFFFFFFFFE.  The
    // largest-even positive in each width, sitting just below the sign flip,
    // confirming the top positive region is delivered without tipping into the
    // negative (extension) branch.  char 0x7FFE is the BMP value just below the
    // surrogate-adjacent range.
    run_and_check(ctx, "int_sign_neighbors", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0x7E),
        /*s */ static_cast<std::int16_t>(0x7FFE),
        /*i */ 0x7FFFFFFE,
        /*l */ static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFELL),
        /*f */ 23.5f,
        /*d */ 23.25,
        /*c */ static_cast<std::uint16_t>(0x7FFE) });

    // ROUND 24 — char LOW surrogate (0xDC00), the twin of the high surrogate
    // (0xD83D) covered earlier, plus the 2-byte/3-byte UTF-8 BMP boundaries on
    // the integral slots (short 0x07FF, int 0x0800).  A lone low surrogate is a
    // valid 16-bit jchar code unit; forcing it proves the char path applies no
    // surrogate validation and no sign extension across the full unsigned range.
    run_and_check(ctx, "char_low_surrogate", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(24),
        /*s */ static_cast<std::int16_t>(0x07FF), // last 2-byte UTF-8 codepoint
        /*i */ 0x0800,                            // first 3-byte UTF-8 codepoint
        /*l */ static_cast<std::int64_t>(24),
        /*f */ 24.5f,
        /*d */ 24.25,
        /*c */ static_cast<std::uint16_t>(0xDC00) }); // lone low surrogate

    // ROUND 25 — POSITIVE ZERO for float and double, bit-exact, in the SAME
    // pass that forces -0.0's exact complement.  Together with the neg_zero
    // round (ROUND 7) this pins BOTH signed zeros: a memcpy that preserved the
    // sign bit must yield 0x00000000 here and 0x80000000 there, and same_bits()
    // catches any conflation.  Integrals carry alternating-nibble sentinels so
    // every bit of the integral slots is exercised in at least one round
    // (byte 0x55=+85, short 0x5555, int 0x55555555, long 0x5555555555555555 —
    // the positive alternating pattern; the negative twin lands in ROUND 27).
    run_and_check(ctx, "pos_zero_alt_bits", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(0x55),                       // +85
        /*s */ static_cast<std::int16_t>(0x5555),                    // +21845
        /*i */ 0x55555555,                                           // +1431655765
        /*l */ static_cast<std::int64_t>(0x5555555555555555LL),      // +6148914691236517205
        /*f */ f_pos_zero,
        /*d */ d_pos_zero,
        /*c */ static_cast<std::uint16_t>(0x5555) });

    // ROUND 26 — NEGATIVE alternating-nibble integrals: byte 0xAA=-86,
    // short 0xAAAA=-21846, int 0xAAAAAAAA=INT_MIN-region negative,
    // long 0xAAAAAAAAAAAAAAAA (sign bit set).  The bitwise complement of the
    // pos_zero round's integrals: every OTHER bit set, top bit set, so the
    // sign-extension branch (byte/short/int) and the long memcpy path both have
    // to deliver a dense negative pattern — the strongest single-round check
    // that no stray bit is dropped or flipped on the signed path.  Floats carry
    // the clean powers of two (1.0f / 2.0d) for an exponent-only witness.
    run_and_check(ctx, "neg_alt_bits", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0xAA),                       // -86
        /*s */ static_cast<std::int16_t>(0xAAAA),                    // -21846
        /*i */ static_cast<std::int32_t>(0xAAAAAAAA),                // -1431655766
        /*l */ static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAULL),     // negative
        /*f */ f_one,
        /*d */ d_two,
        /*c */ static_cast<std::uint16_t>(0xAAAA) });

    // ROUND 27 — float/double LARGEST positive subnormal (0x007FFFFF /
    // 0x000FFFFFFFFFFFFF), bit-exact: exponent field 0, mantissa all-ones.  The
    // top of the subnormal band, one ULP below min_normal; with the denorm_min
    // round (ROUND 21) this brackets the subnormal range at BOTH ends, so a
    // flush-to-zero / DAZ FPU path is caught no matter where in the band it
    // would trigger.  char 0xE000 is the first code unit just AFTER the UTF-16
    // surrogate block (D800–DFFF) — a normal BMP scalar value bordering the
    // surrogate range.
    run_and_check(ctx, "float_double_subnormal_max", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(27),
        /*s */ static_cast<std::int16_t>(27),
        /*i */ 27,
        /*l */ static_cast<std::int64_t>(27),
        /*f */ f_subnormal_max,
        /*d */ d_subnormal_max,
        /*c */ static_cast<std::uint16_t>(0xE000) }); // first post-surrogate BMP

    // ROUND 28 — NEGATIVE quiet NaN (float 0xFFC00000 / double
    // 0xFFF8000000000000): the sign bit set ON TOP of the qNaN pattern.  The
    // movq-xmm0 epilogue must deliver bit 31 (float) / bit 63 (double) too, so a
    // path that ever masked the NaN sign would fail here while passing every
    // positive-NaN round.  char 0xFFFD is the Unicode REPLACEMENT CHARACTER, the
    // top-of-BMP non-noncharacter sentinel.
    run_and_check(ctx, "float_double_neg_nan", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(28),
        /*s */ static_cast<std::int16_t>(28),
        /*i */ 28,
        /*l */ static_cast<std::int64_t>(28),
        /*f */ f_neg_qnan,
        /*d */ d_neg_qnan,
        /*c */ static_cast<std::uint16_t>(0xFFFD) }); // U+FFFD REPLACEMENT CHAR

    // ROUND 29 — FORCED-EQUALS-ORIGINAL: every slot is forced to the SAME value
    // the orig* body would have returned anyway (origByte=11, origShort=111,
    // origInt=1111, origLong=1111, origFloat=11.5f, origDouble=11.25,
    // origChar='A', origBool=false).  The OBSERVED value matching is then NOT
    // sufficient on its own — but the per-round hook-fire counters (asserted in
    // check_round: 9 instance + 8 static) PROVE the cancel/force path executed
    // even when the forced value is indistinguishable from the original.  This
    // is the one round that proves set() is not silently a no-op when value ==
    // original.  (Static origs differ from instance origs, but the static slots
    // are forced to the instance-orig values here; that's fine — we only assert
    // observed == forced, and the fire count proves the body was skipped.)
    run_and_check(ctx, "forced_equals_original", forced_values{
        /*b */ false,                                  // == origBool()
        /*by*/ static_cast<std::int8_t>(11),           // == origByte()
        /*s */ static_cast<std::int16_t>(111),         // == origShort()
        /*i */ 1111,                                   // == origInt()
        /*l */ static_cast<std::int64_t>(1111),        // == origLong()
        /*f */ 11.5f,                                  // == origFloat()
        /*d */ 11.25,                                  // == origDouble()
        /*c */ static_cast<std::uint16_t>('A') });     // == origChar()

    // ROUND 30 — float/double clean POWERS OF TWO (1.0 / 2.0) bit-exact on BOTH
    // float and double slots simultaneously (the prior rounds spread powers of
    // two across f and d separately).  1.0f=0x3F800000, 2.0d=0x4000000000000000:
    // a meaningful biased exponent with an empty mantissa, so the exponent field
    // alone must survive the slot.  Integrals carry small primes to keep their
    // checks distinct from neighbouring rounds.
    run_and_check(ctx, "float_double_powers_of_two", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(31),
        /*s */ static_cast<std::int16_t>(127),
        /*i */ 65537,                                  // 2^16 + 1
        /*l */ static_cast<std::int64_t>(4294967297LL),// 2^32 + 1
        /*f */ f_one,
        /*d */ d_two,
        /*c */ static_cast<std::uint16_t>(0x0041) }); // 'A'

    // ROUND 31 — float/double "narrowing trap" pair: a float (0.2f) whose exact
    // value is NOT (float)(double)0.2, and a double (1.0/3.0) that loses
    // precision if it were ever truncated to float width.  Forcing each through
    // its OWN-width slot and comparing bit-exactly proves the epilogue reads the
    // correct register width (movq xmm0 delivers the full 32/64 bits; the JVM
    // freturn/dreturn reads the matching width) — a swapped-width read would
    // surface a wildly different pattern and same_bits() fails.
    run_and_check(ctx, "float_double_narrowing", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(33),
        /*s */ static_cast<std::int16_t>(333),
        /*i */ 3333,
        /*l */ static_cast<std::int64_t>(33333333333LL),
        /*f */ 0.2f,
        /*d */ 1.0 / 3.0,
        /*c */ static_cast<std::uint16_t>(0x0033) }); // '3'

    // ROUND 32 — char NULL and char MAX-NONSURROGATE neighbours plus short/int
    // unsigned-top witnesses.  char 0x0000 was covered in min_zero alongside a
    // false bool; here the char slot rides 0xD7FF — the LAST BMP code unit
    // before the surrogate block (D800) — so the boundary just BELOW the
    // surrogates is pinned, complementing 0xE000 (just above) and the surrogate
    // rounds (inside).  short 0x6000 / int 0x40000000 are clean high-bit-but-
    // positive patterns.
    run_and_check(ctx, "char_pre_surrogate", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(0x40),         // '@' = +64
        /*s */ static_cast<std::int16_t>(0x6000),      // +24576
        /*i */ 0x40000000,                             // +1073741824
        /*l */ static_cast<std::int64_t>(0x4000000000000000LL),
        /*f */ 32.5f,
        /*d */ 32.25,
        /*c */ static_cast<std::uint16_t>(0xD7FF) }); // last pre-surrogate BMP

    // ROUND 33 — last canonical RE-RUN before the final stability witness, but
    // with a DIFFERENT canonical vector (distinct from ROUND 1) so the repeat
    // also re-exercises a fresh value set rather than only re-confirming round 1.
    // Mixed signs across every slot.
    run_and_check(ctx, "second_canonical", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(-42),
        /*s */ static_cast<std::int16_t>(-12345),
        /*i */ -2000000000,
        /*l */ static_cast<std::int64_t>(-1234567890123456789LL),
        /*f */ -98.75f,
        /*d */ 271828.18284590452,
        /*c */ static_cast<std::uint16_t>(0x4F4B) }); // 'OK'

    // ROUND 34 — re-run the canonical vector a SECOND time at the very end to
    // prove the force-return path is stable across repeated arm/disarm cycles
    // (each round installs fresh scoped_hooks; this guards against state left
    // behind by a previous round's teardown).
    run_and_check(ctx, "canonical_repeat", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(99),
        /*s */ static_cast<std::int16_t>(31000),
        /*i */ 0x12345678,
        /*l */ static_cast<std::int64_t>(0x0123456789ABCDEFLL),
        /*f */ 3.5f,
        /*d */ 2.5,
        /*c */ static_cast<std::uint16_t>(0x263A) });

    // ROUND 35 — float/double exact dyadic fractions and the powers-of-two ULP
    // neighbours not yet pinned: 0.5f/0.25d are exactly representable (clean
    // mantissa), and 1.0f's NEXT representable value (1.0f + 1 ULP =
    // Float.intBitsToFloat(0x3F800001)) plus 1.0d + 1 ULP
    // (Double.longBitsToDouble(0x3FF0000000000001)) prove a one-bit mantissa
    // difference rides the slot intact (a slot that dropped the low mantissa bit
    // would collapse 1.0+ULP back to 1.0 and same_bits() would fail).  Integrals
    // carry single-bit-walk witnesses (byte 0x40, short 0x4000, int 0x40000000,
    // long 0x4000000000000000) — one set bit per width below the sign bit.
    run_and_check(ctx, "float_double_ulp_neighbors", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0x40),                          // +64, bit 6
        /*s */ static_cast<std::int16_t>(0x4000),                       // +16384, bit 14
        /*i */ 0x40000000,                                              // +2^30, bit 30
        /*l */ static_cast<std::int64_t>(0x4000000000000000LL),         // +2^62, bit 62
        /*f */ bits_to_float(0x3F800001u),                              // 1.0f + 1 ULP
        /*d */ bits_to_double(0x3FF0000000000001ULL),                   // 1.0d + 1 ULP
        /*c */ static_cast<std::uint16_t>(0x0040) });                   // '@'

    // ROUND 36 — single-bit-walk integrals at the LOW end + clean dyadic floats.
    // byte 0x01 / short 0x0001 / int 0x00000001 / long 0x0000000000000001: only
    // bit 0 set, the minimal non-zero positive in each width, complementing the
    // high-bit rounds.  float 0.5f (0x3F000000) and double 0.25d
    // (0x3FD0000000000000) are exact dyadic fractions with a non-trivial
    // exponent and empty low mantissa.
    run_and_check(ctx, "low_bit_walk", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(0x01),
        /*s */ static_cast<std::int16_t>(0x0001),
        /*i */ 0x00000001,
        /*l */ static_cast<std::int64_t>(0x0000000000000001LL),
        /*f */ bits_to_float(0x3F000000u),                              // 0.5f
        /*d */ bits_to_double(0x3FD0000000000000ULL),                   // 0.25d
        /*c */ static_cast<std::uint16_t>(0x0001) });

    // ---- OVER-WIDE rounds: force a value WIDER than the Java return type and
    // assert the JVM masks the slot to the declared width on the caller side. --

    // ROUND 37 (over-wide) — high garbage bits in the upper part of an int32_t
    // forced into byte/short/char methods, and an int64_t forced into the int
    // method.  The JVM masks: byte keeps low 8 (0x34, +52), short keeps low 16
    // (0x5678, +22136), char keeps low 16 (0xCDEF, +52719, ZERO-extended),
    // int keeps low 32 (0x9ABCDEF0).  The over-wide upper bytes (0x12.., 0xAB..)
    // MUST be discarded — a path that delivered them would surface a wildly
    // different value and the masked-equality check fails.
    run_and_check_overwide(ctx, "overwide_positive_garbage", overwide_values{
        /*over_byte */ 0x12345634,                       // -> byte 0x34
        /*over_short*/ 0x12345678,                       // -> short 0x5678
        /*over_char */ static_cast<std::int32_t>(0xABCDCDEFu), // -> char 0xCDEF
        /*over_int  */ static_cast<std::int64_t>(0x123456789ABCDEF0LL), // -> int 0x9ABCDEF0
        /*b         */ true });

    // ROUND 38 (over-wide) — all-ones upper bits (sign-bit garbage in the high
    // part) that MUST be masked away.  byte 0xFFFFFF80 -> 0x80 (-128),
    // short 0xFFFF8000 -> 0x8000 (-32768), char 0xFFFF8000 -> 0x8000 (+32768,
    // jchar zero-extends so it is NOT -32768), int 0xFFFFFFFF00000001 ->
    // 0x00000001 (+1, NOT -1).  This is the strongest guard that the upper bits
    // of an over-wide slot never leak into the narrow Java return: every
    // expectation here would flip sign if the mask were skipped.
    run_and_check_overwide(ctx, "overwide_high_ones", overwide_values{
        /*over_byte */ static_cast<std::int32_t>(0xFFFFFF80u),          // -> byte -128
        /*over_short*/ static_cast<std::int32_t>(0xFFFF8000u),          // -> short -32768
        /*over_char */ static_cast<std::int32_t>(0xFFFF8000u),          // -> char +32768
        /*over_int  */ static_cast<std::int64_t>(0xFFFFFFFF00000001LL), // -> int +1
        /*b         */ false });

    // ROUND 39 (over-wide) — char zero-extension stress: over_char 0x0000FFFF
    // masks to jchar 0xFFFF, which the JVM ZERO-extends to int 65535, NOT -1.
    // A char path that sign-extended (the audit's Flaw #1 failure mode) would
    // make obsCharAsInt == -1 here and the widened check fails loudly.  byte/
    // short/int carry low-16/low-8 boundary garbage that masks to their MAX.
    run_and_check_overwide(ctx, "overwide_char_zero_extend", overwide_values{
        /*over_byte */ static_cast<std::int32_t>(0xDEADBE7Fu),          // -> byte 0x7F (+127)
        /*over_short*/ static_cast<std::int32_t>(0xDEAD7FFFu),          // -> short 0x7FFF (+32767)
        /*over_char */ 0x0000FFFF,                                      // -> char 0xFFFF (+65535)
        /*over_int  */ static_cast<std::int64_t>(0x00000000FFFFFFFFLL), // -> int -1 (low 32 all ones)
        /*b         */ true });

    // ---- Lifecycle sanity: 39 rounds ran, so roundCount must be 39. -------
    ctx.check("ran_39_rounds", rsp_fixture::round_count() == 39);

    // ---- Control angle: with NO hooks installed, the original values flow
    // through unchanged (proves the force-return is what changed the result,
    // not some ambient effect).  We run the probe once more with zero hooks;
    // every observed field must equal the ORIGINAL orig* return value.
    {
        reset_round_counters();
        rsp_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rsp_fixture::set_go(value); },
            []() { return rsp_fixture::get_done(); }) };
        ctx.check("baseline_probe_completed", done);
        if (done)
        {
            ctx.check("baseline_no_hook_fired", g_inst_fires.load() == 0 && g_stat_fires.load() == 0);
            // Instance originals.
            ctx.check("baseline_inst_bool_false",  rsp_fixture::obs_bool()  == false);
            ctx.check("baseline_inst_byte_11",     rsp_fixture::obs_byte()  == static_cast<std::int8_t>(11));
            ctx.check("baseline_inst_short_111",   rsp_fixture::obs_short() == static_cast<std::int16_t>(111));
            ctx.check("baseline_inst_int_1111",    rsp_fixture::obs_int()   == 1111);
            ctx.check("baseline_inst_long_1111",   rsp_fixture::obs_long()  == static_cast<std::int64_t>(1111));
            ctx.check("baseline_inst_float_11_5",  same_bits(rsp_fixture::obs_float(),  11.5f));
            ctx.check("baseline_inst_double_11_25",same_bits(rsp_fixture::obs_double(), 11.25));
            ctx.check("baseline_inst_char_A",      rsp_fixture::obs_char()  == static_cast<std::uint16_t>('A'));
            // Static originals.
            ctx.check("baseline_stat_byte_22",     rsp_fixture::obs_s_byte()  == static_cast<std::int8_t>(22));
            ctx.check("baseline_stat_int_2222",    rsp_fixture::obs_s_int()   == 2222);
            ctx.check("baseline_stat_long_2222",   rsp_fixture::obs_s_long()  == static_cast<std::int64_t>(2222));
            ctx.check("baseline_stat_double_22_25",same_bits(rsp_fixture::obs_s_double(), 22.25));
            ctx.check("baseline_stat_char_B",      rsp_fixture::obs_s_char()  == static_cast<std::uint16_t>('B'));
            // Widened-to-int views of the unhooked originals: origByte=11 ->
            // obsByteAsInt 11 (sign-extend of +11), origChar='A'=65 ->
            // obsCharAsInt 65 (zero-extend), origStaticChar='B'=66.  Proves the
            // SECOND (widening) dispatch path is also untouched without a hook.
            ctx.check("baseline_inst_byte_as_int_11",  rsp_fixture::obs_byte_as_int()  == 11);
            ctx.check("baseline_inst_short_as_int_111", rsp_fixture::obs_short_as_int() == 111);
            ctx.check("baseline_inst_char_as_int_A",    rsp_fixture::obs_char_as_int()  == static_cast<std::int32_t>('A'));
            ctx.check("baseline_stat_byte_as_int_22",   rsp_fixture::obs_s_byte_as_int()  == 22);
            ctx.check("baseline_stat_char_as_int_B",    rsp_fixture::obs_s_char_as_int()  == static_cast<std::int32_t>('B'));
        }
    }

    ctx.record("[INFO] return_set_primitives: forced bool/byte/short/int/long/float/double/char "
               "on instance+static dispatch across 39 value rounds (canonical, min/zero, signed "
               "min/max, minus-one, high-bit, -0.0, +Inf, -Inf, qNaN, precision, long-high-dword, "
               "surrogate char, MIN+1, MAX-1, long-low-dword-max, long-2^32-carry, float/double "
               "type-max, min-normal, lowest, subnormal, custom-NaN-payload, int-sign-neighbors, "
               "low-surrogate char, +0.0/alt-bits, neg-alt-bits, subnormal-max, neg-NaN, "
               "forced==original, powers-of-two, narrowing-trap, pre-surrogate char, second-canonical, "
               "canonical-repeat, ulp-neighbors, low-bit-walk, and THREE over-wide rounds) plus a "
               "no-hook baseline.  Every sub-int return is ALSO read back widened to int "
               "(obs*AsInt), pinning the JVM caller-side mask-and-widen: byte/short sign-extend, "
               "jchar zero-extends.");

    // [INFO] OVER-WIDE coverage: the force-return template does NOT check the
    // hooked method's Java return descriptor (vmhook.hpp:1353-1382 — the only
    // type-checked overload is the nullptr one).  The over-wide rounds force a
    // value strictly wider than the declared narrow return (int32_t into
    // byte/short/char, int64_t into int) and assert the JVM masks the slot to
    // the declared width on the CALLER side (byte&0xFF, short/char&0xFFFF,
    // int&0xFFFFFFFF), with jchar ZERO-extended and byte/short SIGN-extended on
    // the subsequent widen-to-int.  This pins the current (mask-on-caller)
    // behaviour so a future BasicType-aware fix cannot silently change it.
    ctx.record("[INFO] return_set_primitives: over-wide force (value wider than the Java return "
               "type) is masked to the declared width by the JVM ireturn family on the caller side; "
               "verified for int32_t->byte/short/char and int64_t->int on instance AND static "
               "dispatch.  The force-return API itself performs no descriptor check.");

    // [INFO] Boolean force-return is intentionally bounded to {true,false}: the
    // return_value::set(value) API takes its argument BY TYPE, so a boolean slot
    // can only be driven through a C++ `bool`, whose object representation is
    // {0,1}.  A "raw non-canonical byte" (e.g. 0x02) cannot be expressed through
    // this typed path WITHOUT constructing a `bool` with an invalid value, which
    // is undefined behaviour in C++ — so it is deliberately NOT exercised here.
    // The byte-width force-return is covered exhaustively by the origByte rounds
    // (0, 1, -1, 0x80, MIN/MAX, +/-1 neighbours) instead; the {true,false} edges
    // of the boolean path are both asserted (canonical=true, min_zero=false,
    // high_bit=false) on instance AND static dispatch.
    ctx.record("[INFO] return_set_primitives: bool force-return is type-bounded to {true,false} by "
               "set(value); a raw non-canonical boolean byte is not expressible without UB, so the "
               "raw-byte boolean case is documented rather than tested (byte-width raw patterns are "
               "covered by the origByte rounds).");
}
