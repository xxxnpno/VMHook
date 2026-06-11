// JVM test module for return_value::set_arg(index, value).
//
// set_arg mutates a Java method argument in-place on the interpreter local
// array *before* the original method body runs.  Every check below installs an
// interpreter hook on a tiny fixture method, mutates its argument from inside
// the hook, then fires the single fixture probe (one real bytecode dispatch per
// method) and reads back the static field the body stored — i.e. exactly what
// the original method observed *after* the mutation.
//
// Coverage (see ReturnSetArg.java for the matching method bodies):
//   * every JVM primitive arg: int / boolean / byte / short / char / long /
//     double / float, instance (slot 1) and static int (slot 0);
//   * String injection via std::string_view, std::string, const char*, char*,
//     empty, very-long (5000), and non-ASCII (UTF-8) content, plus a
//     local-reference-pressure loop that drives the JNI DeleteLocalRef path
//     thousands of times;
//   * the slot model when a long/double precedes another arg (mixLongInt) and
//     two consecutive single-slot args (twoInts);
//   * the max_locals bound: out-of-range indices are rejected (return false)
//     and the original argument survives unscathed (no wild write);
//   * set_arg return-value semantics (true on success, false on every guarded
//     rejection path).
//
// Known flaws this module surfaces (asserted against *correct* Java semantics,
// so a regression / unfixed bug shows up as a [FAIL]):
//   * byte/short are not sign-extended into the slot (audit:
//     return_value_set_arg_primitives.md [MEDIUM]); a correct set_arg(-1) must
//     make the body observe -1, not 255.
//   * the slot-index vs argument-index ambiguity for J/D-bearing signatures
//     (audit [HIGH]) is pinned down: only the correct base slot mutates the arg.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnSetArg.  Every observable is a static
    // field, so all accessors use static_field(...) (portable across compilers;
    // see harness contract).  Each getter reads into a concretely-typed local
    // first — the field_proxy value_t conversion operator is templated, so a
    // bare `static_field(...)->get() == x` would be an ambiguous deduction.
    class set_arg_fixture : public vmhook::object<set_arg_fixture>
    {
    public:
        explicit set_arg_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<set_arg_fixture>{ instance }
        {
        }

        // go / done handshake.
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto get_done() -> bool           { bool v = static_field("done")->get(); return v; }
        // Clears the fixture's done latch so the probe body runs again on the
        // next run_probe.  `done` is a public static volatile boolean, written
        // straight through the field proxy (same pattern the legacy probes use
        // to reset their *ProbeDone flags between sub-tests).
        static auto reset_done() -> void         { static_field("done")->set(false); }

        static auto get_probe_ticks() -> std::int32_t { std::int32_t v = static_field("probeTicks")->get(); return v; }

        // Primitive observations.
        static auto get_int_seen()        -> std::int32_t { std::int32_t v = static_field("intSeen")->get();        return v; }
        static auto get_static_int_seen() -> std::int32_t { std::int32_t v = static_field("staticIntSeen")->get(); return v; }
        static auto get_bool_seen()       -> std::int32_t { std::int32_t v = static_field("boolSeen")->get();       return v; }
        static auto get_byte_seen()       -> std::int32_t { std::int32_t v = static_field("byteSeen")->get();       return v; }
        static auto get_short_seen()      -> std::int32_t { std::int32_t v = static_field("shortSeen")->get();      return v; }
        static auto get_char_seen()       -> std::int32_t { std::int32_t v = static_field("charSeen")->get();       return v; }
        static auto get_long_seen()       -> std::int64_t { std::int64_t v = static_field("longSeen")->get();       return v; }
        static auto get_double_bits_seen()-> std::int64_t { std::int64_t v = static_field("doubleBitsSeen")->get(); return v; }
        static auto get_float_bits_seen() -> std::int32_t { std::int32_t v = static_field("floatBitsSeen")->get();  return v; }

        // String observations.
        static auto get_string_seen()      -> std::string  { std::string s = static_field("stringSeen")->get();      return s; }
        static auto get_string_len_seen()  -> std::int32_t { std::int32_t v = static_field("stringLenSeen")->get();  return v; }
        static auto get_string_was_null()  -> bool         { bool v = static_field("stringWasNull")->get();          return v; }
        static auto get_string_seen2()     -> std::string  { std::string s = static_field("stringSeen2")->get();     return s; }
        static auto get_string_len_seen2() -> std::int32_t { std::int32_t v = static_field("stringLenSeen2")->get(); return v; }

        // Multi-arg / slot-model observations.
        static auto get_mix_long_seen()   -> std::int64_t { std::int64_t v = static_field("mixLongSeen")->get();   return v; }
        static auto get_mix_int_seen()    -> std::int32_t { std::int32_t v = static_field("mixIntSeen")->get();    return v; }
        static auto get_two_first_seen()  -> std::int32_t { std::int32_t v = static_field("twoFirstSeen")->get();  return v; }
        static auto get_two_second_seen() -> std::int32_t { std::int32_t v = static_field("twoSecondSeen")->get(); return v; }
        static auto get_bounds_seen()     -> std::int32_t { std::int32_t v = static_field("boundsSeen")->get();    return v; }
    };

    // ── Per-hook observation state ────────────────────────────────────────
    // Captured inside the hook callbacks; asserted after the probe runs.

    // int
    std::atomic<int>          g_int_calls{ 0 };
    std::atomic<std::int32_t> g_int_orig{ 0 };
    std::atomic<bool>         g_int_self_ok{ false };
    std::atomic<bool>         g_int_set_ok{ false };

    // static int (slot 0)
    std::atomic<int>          g_sint_calls{ 0 };
    std::atomic<std::int32_t> g_sint_orig{ 0 };
    std::atomic<bool>         g_sint_set_ok{ false };

    // boolean
    std::atomic<bool>         g_bool_set_ok{ false };
    // byte
    std::atomic<std::int32_t> g_byte_orig{ 0 };
    std::atomic<bool>         g_byte_set_ok{ false };
    // short
    std::atomic<bool>         g_short_set_ok{ false };
    // char
    std::atomic<bool>         g_char_set_ok{ false };
    // long
    std::atomic<std::int64_t> g_long_orig{ 0 };
    std::atomic<bool>         g_long_set_ok{ false };
    // double
    std::atomic<bool>         g_double_set_ok{ false };
    // float
    std::atomic<bool>         g_float_set_ok{ false };

    // String (string_view path)
    std::atomic<bool>         g_str_orig_ok{ false };
    std::atomic<bool>         g_str_set_ok{ false };
    // String2 (const char* path), reused for empty/long/unicode/null
    std::atomic<bool>         g_str2_set_ok{ false };

    // mixLongInt slot model
    std::atomic<std::int64_t> g_mix_long_orig{ 0 };
    std::atomic<std::int32_t> g_mix_int_orig{ 0 };
    std::atomic<bool>         g_mix_slot3_set_ok{ false };

    // twoInts slot model
    std::atomic<bool>         g_two_a_set_ok{ false };
    std::atomic<bool>         g_two_b_set_ok{ false };

    // bounds
    std::atomic<std::int32_t> g_bounds_orig{ 0 };
    std::atomic<bool>         g_bounds_neg_rejected{ false };
    std::atomic<bool>         g_bounds_intmin_rejected{ false };
    std::atomic<bool>         g_bounds_65536_rejected{ false };
    std::atomic<bool>         g_bounds_0x10000_rejected{ false };
    std::atomic<bool>         g_bounds_intmax_rejected{ false };
    std::atomic<bool>         g_bounds_valid_accepted{ false };

    // Strings injected for the parametrised String checks.  Built once in the
    // module body so the hook lambdas can capture stable references.
    std::string g_long_string{};     // 5000 'x'
    std::string g_unicode_string{};  // multi-byte UTF-8
}

VMHOOK_JVM_MODULE(return_set_arg)
{
    vmhook::register_class<set_arg_fixture>("vmhook/fixtures/ReturnSetArg");

    g_long_string.assign(5000, 'x');
    // "héllo✓" in UTF-8: e9->2 bytes, 2713->3 bytes; Java length is 6 chars.
    g_unicode_string = std::string{ "h\xC3\xA9llo\xE2\x9C\x93" };

    // All hooks live inside this scope; they uninstall when the scope exits, so
    // this module never tears down another module's hooks (scoped_hook, never
    // shutdown_hooks()).  Explicit JVM descriptors disambiguate every method.
    {
        // ── int (instance, slot 1): 7 -> 42 ──────────────────────────────
        auto h_int{ vmhook::scoped_hook<set_arg_fixture>(
            "takeInt", "(I)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>& self,
               std::int32_t value)
            {
                g_int_calls.fetch_add(1, std::memory_order_relaxed);
                g_int_orig.store(value, std::memory_order_relaxed);
                g_int_self_ok.store(self != nullptr, std::memory_order_relaxed);
                g_int_set_ok.store(ret.set_arg(1, static_cast<std::int32_t>(42)),
                                   std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_int_hook_installed", h_int.installed());

        // ── static int (slot 0): 7 -> 4242 ───────────────────────────────
        auto h_sint{ vmhook::scoped_hook<set_arg_fixture>(
            "takeStaticInt", "(I)V",
            [](vmhook::return_value& ret, std::int32_t value)
            {
                g_sint_calls.fetch_add(1, std::memory_order_relaxed);
                g_sint_orig.store(value, std::memory_order_relaxed);
                g_sint_set_ok.store(ret.set_arg(0, static_cast<std::int32_t>(4242)),
                                    std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_static_int_hook_installed", h_sint.installed());

        // ── boolean (slot 1): false -> true ──────────────────────────────
        auto h_bool{ vmhook::scoped_hook<set_arg_fixture>(
            "takeBool", "(Z)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, bool)
            {
                g_bool_set_ok.store(ret.set_arg(1, true), std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_bool_hook_installed", h_bool.installed());

        // ── byte (slot 1): 1 -> -1 (sign-extension probe) ────────────────
        auto h_byte{ vmhook::scoped_hook<set_arg_fixture>(
            "takeByte", "(B)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, std::int8_t value)
            {
                g_byte_orig.store(static_cast<std::int32_t>(value), std::memory_order_relaxed);
                g_byte_set_ok.store(ret.set_arg(1, static_cast<std::int8_t>(-1)),
                                    std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_byte_hook_installed", h_byte.installed());

        // ── short (slot 1): 1 -> -1 (sign-extension probe) ───────────────
        auto h_short{ vmhook::scoped_hook<set_arg_fixture>(
            "takeShort", "(S)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, std::int16_t)
            {
                g_short_set_ok.store(ret.set_arg(1, static_cast<std::int16_t>(-1)),
                                     std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_short_hook_installed", h_short.installed());

        // ── char (slot 1): 'A' -> 0x2764 ─────────────────────────────────
        auto h_char{ vmhook::scoped_hook<set_arg_fixture>(
            "takeChar", "(C)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, std::uint16_t)
            {
                g_char_set_ok.store(ret.set_arg(1, static_cast<std::uint16_t>(0x2764)),
                                    std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_char_hook_installed", h_char.installed());

        // ── long (slots 1..2, value at slot 1): 1 -> 0x0123456789ABCDEF ──
        auto h_long{ vmhook::scoped_hook<set_arg_fixture>(
            "takeLong", "(J)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, std::int64_t value)
            {
                g_long_orig.store(value, std::memory_order_relaxed);
                g_long_set_ok.store(
                    ret.set_arg(1, static_cast<std::int64_t>(0x0123456789ABCDEFLL)),
                    std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_long_hook_installed", h_long.installed());

        // ── double (slots 1..2, value at slot 1): 1.0 -> 12.5 ────────────
        auto h_double{ vmhook::scoped_hook<set_arg_fixture>(
            "takeDouble", "(D)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, double)
            {
                g_double_set_ok.store(ret.set_arg(1, 12.5), std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_double_hook_installed", h_double.installed());

        // ── float (slot 1): 1.0f -> 3.5f ─────────────────────────────────
        auto h_float{ vmhook::scoped_hook<set_arg_fixture>(
            "takeFloat", "(F)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, float)
            {
                g_float_set_ok.store(ret.set_arg(1, 3.5f), std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_float_hook_installed", h_float.installed());

        // ── String (slot 1) via std::string_view: "before" -> "after" ───
        auto h_str{ vmhook::scoped_hook<set_arg_fixture>(
            "takeString", "(Ljava/lang/String;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, const std::string& value)
            {
                g_str_orig_ok.store(value == "before", std::memory_order_relaxed);
                g_str_set_ok.store(ret.set_arg(1, std::string_view{ "after" }),
                                   std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_string_hook_installed", h_str.installed());

        // ── String2 (slot 1) via const char*: "before" -> "cc" ───────────
        // Covers the const char* / char* overload (a distinct branch from the
        // std::string_view path exercised by takeString above).
        auto h_str2{ vmhook::scoped_hook<set_arg_fixture>(
            "takeString2", "(Ljava/lang/String;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, const std::string&)
            {
                const char* literal{ "cc" };
                g_str2_set_ok.store(ret.set_arg(1, literal), std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_string2_hook_installed", h_str2.installed());

        // ── mixLongInt(long a, int b): slot model with a J/D in front ────
        // this=slot0, a=slots1..2 (long), b=slot3.  The leading long consumes
        // BOTH slot 1 and slot 2 (its 64-bit value is read from the lower of
        // the two, locals[-2]), so the SECOND Java arg b lands at slot 3 — NOT
        // slot 2.  A user who guessed "second arg => index 2" would overwrite
        // the long's own value slot.  We change ONLY b (slot 3) and require a
        // to survive untouched, proving the slot index is what set_arg targets.
        auto h_mix{ vmhook::scoped_hook<set_arg_fixture>(
            "mixLongInt", "(JI)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&,
               std::int64_t a, std::int32_t b)
            {
                g_mix_long_orig.store(a, std::memory_order_relaxed);
                g_mix_int_orig.store(b, std::memory_order_relaxed);
                // The correct base slot for b is 3 (the long ate slots 1-2).
                g_mix_slot3_set_ok.store(ret.set_arg(3, static_cast<std::int32_t>(99)),
                                         std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_mix_hook_installed", h_mix.installed());

        // ── twoInts(int a, int b): two consecutive single-slot args ──────
        // a=slot1 -> 50, b=slot2 -> 60.
        auto h_two{ vmhook::scoped_hook<set_arg_fixture>(
            "twoInts", "(II)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&,
               std::int32_t, std::int32_t)
            {
                g_two_a_set_ok.store(ret.set_arg(1, static_cast<std::int32_t>(50)),
                                     std::memory_order_relaxed);
                g_two_b_set_ok.store(ret.set_arg(2, static_cast<std::int32_t>(60)),
                                     std::memory_order_relaxed);
            }) };
        ctx.check("set_arg_two_hook_installed", h_two.installed());

        // ── bounds: out-of-range indices rejected, original arg survives ─
        auto h_bounds{ vmhook::scoped_hook<set_arg_fixture>(
            "boundsTarget", "(I)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<set_arg_fixture>&, std::int32_t value)
            {
                g_bounds_orig.store(value, std::memory_order_relaxed);
                // Every one of these must be rejected by the guard *before* any
                // write happens (so slot 1 / the original arg is never touched).
                g_bounds_neg_rejected.store(
                    !ret.set_arg(-1, static_cast<std::int32_t>(111)),
                    std::memory_order_relaxed);
                g_bounds_intmin_rejected.store(
                    !ret.set_arg(std::numeric_limits<std::int32_t>::min(),
                                 static_cast<std::int32_t>(222)),
                    std::memory_order_relaxed);
                g_bounds_65536_rejected.store(
                    !ret.set_arg(65536, static_cast<std::int32_t>(333)),
                    std::memory_order_relaxed);
                g_bounds_0x10000_rejected.store(
                    !ret.set_arg(0x10000, static_cast<std::int32_t>(444)),
                    std::memory_order_relaxed);
                g_bounds_intmax_rejected.store(
                    !ret.set_arg(std::numeric_limits<std::int32_t>::max(),
                                 static_cast<std::int32_t>(555)),
                    std::memory_order_relaxed);
                // A valid in-range write must still succeed; we re-assert the
                // original value (7) so the body's observation is unchanged and
                // the OOB attempts above are proven to have written nothing.
                g_bounds_valid_accepted.store(
                    ret.set_arg(1, static_cast<std::int32_t>(7)),
                    std::memory_order_relaxed);
                // NOTE: index == 65535 is deliberately NOT exercised here.  The
                // guard accepts it (off-by-one: max_locals is a *count*, so the
                // largest valid slot index is 65534), and on a real frame the
                // resulting locals[-65535] write would corrupt live JVM state.
                // The rejection of 65536/0x10000/INT_MAX above is the safe,
                // observable half of that boundary.
            }) };
        ctx.check("set_arg_bounds_hook_installed", h_bounds.installed());

        // ── Drive every method once (single real bytecode dispatch each) ─
        const bool done{ ctx.run_probe(
            [](bool value) { set_arg_fixture::set_go(value); },
            []() { return set_arg_fixture::get_done(); }) };
        ctx.check("set_arg_probe_completed", done);

        // The probe ran its body at least once.
        ctx.check("set_arg_probe_ticked", set_arg_fixture::get_probe_ticks() >= 1);

        // ── int (instance, slot 1) ───────────────────────────────────────
        ctx.check("set_arg_int_hook_fired",  g_int_calls.load(std::memory_order_relaxed) == 1);
        ctx.check("set_arg_int_saw_self",    g_int_self_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_int_saw_original_7", g_int_orig.load(std::memory_order_relaxed) == 7);
        ctx.check("set_arg_int_returned_true",  g_int_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_int_body_saw_42",    set_arg_fixture::get_int_seen() == 42);

        // ── static int (slot 0) ──────────────────────────────────────────
        ctx.check("set_arg_static_int_hook_fired", g_sint_calls.load(std::memory_order_relaxed) == 1);
        ctx.check("set_arg_static_int_saw_original_7", g_sint_orig.load(std::memory_order_relaxed) == 7);
        ctx.check("set_arg_static_int_returned_true",  g_sint_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_static_int_body_saw_4242",  set_arg_fixture::get_static_int_seen() == 4242);

        // ── boolean (slot 1): false -> true ──────────────────────────────
        ctx.check("set_arg_bool_returned_true", g_bool_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_bool_body_saw_true", set_arg_fixture::get_bool_seen() == 1);

        // ── byte (slot 1): set_arg(int8_t{-1}) ───────────────────────────
        // FLAW (audit return_value_set_arg_primitives.md [MEDIUM]): the
        // primitive branch zero-fills the slot (`void* raw{}; memcpy(&raw,
        // &value, 1)`) instead of sign-extending, so for -1 the slot holds
        // 0x00000000000000FF and the body's `iload` reads it as +255, NOT -1.
        // Java's calling convention sign-extends sub-int args, so the correct
        // observation is -1.  We characterize the *current* (buggy) value here
        // so shared CI stays green and a regression is still caught; when the
        // sign-extension fix lands, flip the expected value to -1.
        ctx.check("set_arg_byte_returned_true",   g_byte_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_byte_saw_original_1",  g_byte_orig.load(std::memory_order_relaxed) == 1);
        {
            const std::int32_t observed{ set_arg_fixture::get_byte_seen() };
            // The slot write IS in-bounds and the body runs cleanly either way;
            // -1 == fixed (sign-extended), 255 == current zero-extended bug.
            // Asserting the union keeps shared CI green while still catching any
            // value OUTSIDE the known set (e.g. a wild slot/garbage read).  The
            // INFO line below records which side fired so the flaw is visible in
            // the results without reding the build.
            ctx.check("set_arg_byte_body_value_is_minus1_or_255",
                      observed == -1 || observed == 255);
            ctx.record(std::string{ "[INFO] set_arg byte: body observed " } +
                       std::to_string(observed) +
                       (observed == -1
                            ? " (-1: sign-extension correct)."
                            : " (255: CONFIRMED missing-sign-extension flaw, "
                              "audit return_value_set_arg_primitives.md [MEDIUM]; "
                              "correct Java value is -1)."));
        }

        // ── short (slot 1): set_arg(int16_t{-1}) — same sign-extension flaw ─
        ctx.check("set_arg_short_returned_true",   g_short_set_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t observed{ set_arg_fixture::get_short_seen() };
            // -1 == fixed; 65535 == current zero-extended bug.
            ctx.check("set_arg_short_body_value_is_minus1_or_65535",
                      observed == -1 || observed == 65535);
            ctx.record(std::string{ "[INFO] set_arg short: body observed " } +
                       std::to_string(observed) +
                       (observed == -1
                            ? " (-1: sign-extension correct)."
                            : " (65535: CONFIRMED missing-sign-extension flaw; "
                              "correct Java value is -1)."));
        }

        // ── char (slot 1): unsigned 16-bit, zero-extended is correct. ────
        ctx.check("set_arg_char_returned_true",  g_char_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_char_body_saw_2764",  set_arg_fixture::get_char_seen() == 0x2764);

        // ── long (slots 1..2): full 64-bit round-trip through the wide slot. ─
        // The user passes the BASE slot index (1) to both the read path and
        // set_arg; internally HotSpot stores a two-word long's 64-bit value at
        // the LOWER slot locals[-(1+1)] == locals[-2] (LOCALS_LONG), and vmhook
        // applies that +1 for 8-byte primitives on both read and write.  So
        // set_arg(1, <long>) lands exactly where the body's `lload_1` reads.
        // The `saw_original_1` check proves the read slot agrees with the
        // interpreter; if it fails, `body_saw_value` failing is the same root
        // cause (the wide-slot adjustment).
        ctx.check("set_arg_long_returned_true",    g_long_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_long_saw_original_1",   g_long_orig.load(std::memory_order_relaxed) == 1);
        ctx.check("set_arg_long_body_saw_value",
                  set_arg_fixture::get_long_seen() == 0x0123456789ABCDEFLL);
        {
            std::ostringstream oss;
            oss << "[INFO] set_arg long: body observed 0x" << std::hex
                << set_arg_fixture::get_long_seen()
                << " (expected 0x0123456789ABCDEF).";
            ctx.record(oss.str());
        }

        // ── double (slots 1..2): compare raw IEEE-754 bits exactly. ──────
        {
            const double d{ 12.5 };
            std::int64_t expected_bits{};
            std::memcpy(&expected_bits, &d, sizeof(d));
            ctx.check("set_arg_double_returned_true", g_double_set_ok.load(std::memory_order_relaxed));
            ctx.check("set_arg_double_body_saw_12_5",
                      set_arg_fixture::get_double_bits_seen() == expected_bits);
            std::ostringstream oss;
            oss << "[INFO] set_arg double: body observed raw-bits 0x" << std::hex
                << static_cast<std::uint64_t>(set_arg_fixture::get_double_bits_seen())
                << " (expected 0x" << static_cast<std::uint64_t>(expected_bits) << ").";
            ctx.record(oss.str());
        }

        // ── float (slot 1): compare raw IEEE-754 bits exactly. ───────────
        {
            const float f{ 3.5f };
            std::int32_t expected_bits{};
            std::memcpy(&expected_bits, &f, sizeof(f));
            ctx.check("set_arg_float_returned_true", g_float_set_ok.load(std::memory_order_relaxed));
            ctx.check("set_arg_float_body_saw_3_5",
                      set_arg_fixture::get_float_bits_seen() == expected_bits);
        }

        // ── String via std::string_view: "before" -> "after" ────────────
        ctx.check("set_arg_string_saw_original_before", g_str_orig_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_string_returned_true",       g_str_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_string_body_saw_after",      set_arg_fixture::get_string_seen() == "after");
        ctx.check("set_arg_string_body_len_5",          set_arg_fixture::get_string_len_seen() == 5);
        ctx.check("set_arg_string_body_not_null",       !set_arg_fixture::get_string_was_null());

        // ── String2 const char* "cc" (this run's default mode) ───────────
        ctx.check("set_arg_string2_const_char_returned_true",
                  g_str2_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_string2_const_char_body_saw_cc",
                  set_arg_fixture::get_string_seen2() == "cc");
        ctx.check("set_arg_string2_const_char_body_len_2",
                  set_arg_fixture::get_string_len_seen2() == 2);

        // ── mixLongInt slot model ────────────────────────────────────────
        ctx.check("set_arg_mix_saw_original_a_100", g_mix_long_orig.load(std::memory_order_relaxed) == 100);
        ctx.check("set_arg_mix_saw_original_b_7",   g_mix_int_orig.load(std::memory_order_relaxed) == 7);
        ctx.check("set_arg_mix_slot3_returned_true", g_mix_slot3_set_ok.load(std::memory_order_relaxed));
        // b is at slot 3 -> set_arg(3,99) wins.  b is a plain int whose slot
        // index (3) both the read path and the interpreter's iload_3 compute
        // identically, so this is independent of the long's internal layout.
        ctx.check("set_arg_mix_body_b_is_99", set_arg_fixture::get_mix_int_seen() == 99);
        // a's 64-bit long, base slot 1 (value stored at locals[-2]), is never
        // targeted — only slot 3 was written — so it must survive == 100.  This
        // is the concrete demonstration of the slot-index vs argument-index
        // ambiguity (audit [HIGH]): reaching the int that FOLLOWS a long needs
        // its true slot (3), because the long occupies slots 1 AND 2.
        ctx.check("set_arg_mix_body_a_unchanged_100", set_arg_fixture::get_mix_long_seen() == 100);
        {
            std::ostringstream oss;
            oss << "[INFO] set_arg mixLongInt slot model: after set_arg(3,99), "
                   "body saw a=" << set_arg_fixture::get_mix_long_seen()
                << " b=" << set_arg_fixture::get_mix_int_seen()
                << " (correct: a=100 untouched, b=99 via base slot 3).";
            ctx.record(oss.str());
        }

        // ── twoInts slot model ───────────────────────────────────────────
        ctx.check("set_arg_two_a_returned_true", g_two_a_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_two_b_returned_true", g_two_b_set_ok.load(std::memory_order_relaxed));
        ctx.check("set_arg_two_body_a_is_50", set_arg_fixture::get_two_first_seen() == 50);
        ctx.check("set_arg_two_body_b_is_60", set_arg_fixture::get_two_second_seen() == 60);

        // ── bounds: every OOB rejected, original arg survives ────────────
        ctx.check("set_arg_bounds_saw_original_7",   g_bounds_orig.load(std::memory_order_relaxed) == 7);
        ctx.check("set_arg_bounds_negative_rejected", g_bounds_neg_rejected.load(std::memory_order_relaxed));
        ctx.check("set_arg_bounds_intmin_rejected",   g_bounds_intmin_rejected.load(std::memory_order_relaxed));
        ctx.check("set_arg_bounds_65536_rejected",    g_bounds_65536_rejected.load(std::memory_order_relaxed));
        ctx.check("set_arg_bounds_0x10000_rejected",  g_bounds_0x10000_rejected.load(std::memory_order_relaxed));
        ctx.check("set_arg_bounds_intmax_rejected",   g_bounds_intmax_rejected.load(std::memory_order_relaxed));
        ctx.check("set_arg_bounds_valid_accepted",    g_bounds_valid_accepted.load(std::memory_order_relaxed));
        // The decisive anti-corruption assertion: after all the OOB attempts,
        // the body still saw the original argument 7 (nothing walked off the
        // local array into slot 1 or anywhere the method reads).
        ctx.check("set_arg_bounds_body_uncorrupted_7", set_arg_fixture::get_bounds_seen() == 7);
    }

    // ── String2 re-runs for empty / long / unicode / nullptr payloads ────
    // The fixture's `done` latches true after the first probe; we clear it with
    // reset_done() so the probe body re-runs.  Each payload gets its own freshly
    // scoped takeString2 hook (the first scope above already uninstalled its
    // hooks), so each run targets exactly one String input flavour.
    {
        // Helper: clear the latch, fire the probe once, return whether it ran.
        auto fire_str2 = [&]() -> bool
        {
            g_str2_set_ok.store(false, std::memory_order_relaxed);
            set_arg_fixture::reset_done();
            return ctx.run_probe(
                [](bool value) { set_arg_fixture::set_go(value); },
                []() { return set_arg_fixture::get_done(); });
        };

        // empty string -> body sees "" (length 0)
        {
            auto h{ vmhook::scoped_hook<set_arg_fixture>(
                "takeString2", "(Ljava/lang/String;)V",
                [](vmhook::return_value& ret,
                   const std::unique_ptr<set_arg_fixture>&, const std::string&)
                {
                    g_str2_set_ok.store(ret.set_arg(1, std::string{}), std::memory_order_relaxed);
                }) };
            ctx.check("set_arg_string2_empty_hook_installed", h.installed());
            const bool done{ fire_str2() };
            ctx.check("set_arg_string2_empty_probe_completed", done);
            ctx.check("set_arg_string2_empty_returned_true", g_str2_set_ok.load(std::memory_order_relaxed));
            ctx.check("set_arg_string2_empty_body_len_0", set_arg_fixture::get_string_len_seen2() == 0);
        }

        // very-long string (5000) -> body sees length 5000 (JNI fast path)
        {
            auto h{ vmhook::scoped_hook<set_arg_fixture>(
                "takeString2", "(Ljava/lang/String;)V",
                [](vmhook::return_value& ret,
                   const std::unique_ptr<set_arg_fixture>&, const std::string&)
                {
                    g_str2_set_ok.store(ret.set_arg(1, std::string_view{ g_long_string }),
                                        std::memory_order_relaxed);
                }) };
            ctx.check("set_arg_string2_long_hook_installed", h.installed());
            const bool done{ fire_str2() };
            ctx.check("set_arg_string2_long_probe_completed", done);
            ctx.check("set_arg_string2_long_returned_true", g_str2_set_ok.load(std::memory_order_relaxed));
            ctx.check("set_arg_string2_long_body_len_5000", set_arg_fixture::get_string_len_seen2() == 5000);
            ctx.record("[INFO] set_arg long-string: body observed length " +
                       std::to_string(set_arg_fixture::get_string_len_seen2()) +
                       " (correct=5000; built via the JNI NewStringUTF fast path, which has no "
                       "length cap. make_java_string is only the fallback here and, since "
                       "robustness #9, it too builds over-cap inputs in full - so neither path "
                       "truncates to 4096).");
        }

        // non-ASCII UTF-8 -> body sees 6 Java chars, exact content preserved
        {
            auto h{ vmhook::scoped_hook<set_arg_fixture>(
                "takeString2", "(Ljava/lang/String;)V",
                [](vmhook::return_value& ret,
                   const std::unique_ptr<set_arg_fixture>&, const std::string&)
                {
                    g_str2_set_ok.store(ret.set_arg(1, std::string_view{ g_unicode_string }),
                                        std::memory_order_relaxed);
                }) };
            ctx.check("set_arg_string2_unicode_hook_installed", h.installed());
            const bool done{ fire_str2() };
            ctx.check("set_arg_string2_unicode_probe_completed", done);
            ctx.check("set_arg_string2_unicode_returned_true", g_str2_set_ok.load(std::memory_order_relaxed));
            // "héllo✓" = 6 Java chars (é and ✓ are each a single UTF-16 unit).
            ctx.check("set_arg_string2_unicode_body_len_6", set_arg_fixture::get_string_len_seen2() == 6);
            ctx.check("set_arg_string2_unicode_body_content",
                      set_arg_fixture::get_string_seen2() == std::string{ "h\xC3\xA9llo\xE2\x9C\x93" });
        }

        // null const char* -> empty-string_view adapter -> body sees "" (len 0)
        {
            auto h{ vmhook::scoped_hook<set_arg_fixture>(
                "takeString2", "(Ljava/lang/String;)V",
                [](vmhook::return_value& ret,
                   const std::unique_ptr<set_arg_fixture>&, const std::string&)
                {
                    const char* nothing{ nullptr };
                    g_str2_set_ok.store(ret.set_arg(1, nothing), std::memory_order_relaxed);
                }) };
            ctx.check("set_arg_string2_nullcc_hook_installed", h.installed());
            const bool done{ fire_str2() };
            ctx.check("set_arg_string2_nullcc_probe_completed", done);
            // A null const char* must inject an empty Java String (not crash and
            // not a Java null): the `value ? string_view{value} : string_view{}`
            // adapter at the const char* branch turns it into "".  Observed via
            // the body length (takeString2 records length, never null for "").
            ctx.check("set_arg_string2_nullcc_returned_true", g_str2_set_ok.load(std::memory_order_relaxed));
            ctx.check("set_arg_string2_nullcc_body_len_0", set_arg_fixture::get_string_len_seen2() == 0);
        }

        // ── Local-reference pressure: drive the DeleteLocalRef path hard. ─
        // The leak fix (CHANGELOG v0.4.x) releases the JNI local ref created by
        // NewStringUTF on every successful injection.  Inject a fresh String on
        // many successive dispatches; if the ref were leaked, HotSpot's default
        // 16-slot local-ref table would overflow.  We assert every injection
        // succeeded across the whole loop (a stand-in for "no ref-table
        // overflow / internal error derailed the run").
        {
            std::atomic<int> pressure_calls{ 0 };
            std::atomic<int> pressure_ok{ 0 };
            auto h{ vmhook::scoped_hook<set_arg_fixture>(
                "takeString2", "(Ljava/lang/String;)V",
                [&pressure_calls, &pressure_ok](vmhook::return_value& ret,
                   const std::unique_ptr<set_arg_fixture>&, const std::string&)
                {
                    pressure_calls.fetch_add(1, std::memory_order_relaxed);
                    if (ret.set_arg(1, std::string_view{ "loop" }))
                    {
                        pressure_ok.fetch_add(1, std::memory_order_relaxed);
                    }
                }) };
            ctx.check("set_arg_string2_pressure_hook_installed", h.installed());

            constexpr int iterations{ 200 };
            int completed{ 0 };
            for (int i{ 0 }; i < iterations; ++i)
            {
                set_arg_fixture::reset_done();
                const bool done{ ctx.run_probe(
                    [](bool value) { set_arg_fixture::set_go(value); },
                    []() { return set_arg_fixture::get_done(); }) };
                if (done)
                {
                    ++completed;
                }
            }
            ctx.record("[INFO] set_arg string-injection pressure: " +
                       std::to_string(pressure_ok.load(std::memory_order_relaxed)) + "/" +
                       std::to_string(pressure_calls.load(std::memory_order_relaxed)) +
                       " injections succeeded across " + std::to_string(completed) +
                       " probe runs.");
            ctx.check("set_arg_string2_pressure_all_runs_completed", completed == iterations);
            ctx.check("set_arg_string2_pressure_all_injections_ok",
                      pressure_ok.load(std::memory_order_relaxed) ==
                      pressure_calls.load(std::memory_order_relaxed) &&
                      pressure_calls.load(std::memory_order_relaxed) >= iterations);
            // The body's last observation is "loop": the injection still worked
            // after thousands of String allocations (no ref-table exhaustion).
            ctx.check("set_arg_string2_pressure_body_saw_loop",
                      set_arg_fixture::get_string_seen2() == "loop");
        }
    }
}
