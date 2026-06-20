// return_frame_raw_access JVM test module  (feature area: hooks)
//
// Exhaustively exercises return_value's RAW interpreter-frame escape hatch on a
// LIVE JVM: from inside a detour we take the frame the trampoline stashed
// (ret.frame()) and drive the low-level primitives the typed hook API is built
// on — frame->get_method(), frame->get_locals(), frame->get_arguments<...>() —
// proving on real bytecode dispatch that:
//
//   * frame() is NON-null inside a real hook, and get_method() is the SAME
//     method the hook was installed on (name + descriptor match).  The existing
//     unit test only covers the null-frame case; this is the live counterpart.
//   * get_locals() is the live local array: slot 0 (locals[-0]) holds `this`
//     for an INSTANCE method — its decoded oop == the receiver `self`, and the
//     receiver's own `tag` field is readable THROUGH that recovered oop.
//   * raw primitive arg slots reproduce the Java args, respecting the HotSpot
//     two-slot rule: a long/double occupies TWO slots and its 64-bit value is
//     stored at the LOWER address locals[-(slot+1)]; the NEXT arg shifts by two
//     slot offsets.  int a @slot1, long b @slot2(value@-3), double c @slot4
//     (value@-5), int d @slot6 are each read raw AND cross-checked against the
//     public typed get_arguments<int,long,double,int>() tuple.
//   * a STATIC method has NO `this` at slot 0 — slot 0 holds the first PRIMITIVE
//     arg (a small int, not an oop): the raw slot-0 value == the first Java int,
//     and decoding slot 0 as an oop does not yield a plausible receiver.
//   * the locals array frame() exposes is the SAME one set_arg() mutates: write
//     via ret.set_arg(1, v), read back frame()->get_locals()[-1], they agree,
//     and the body observes the mutated value (allow-through).
//   * out-of-range slot reads return a DEFAULT and never crash the JVM (the
//     private get_argument bounds guard reached via the public typed accessor).
//
// SAFETY: this module drives raw pointers itself, so EVERY dereference off a
// frame/locals pointer is gated by vmhook::hotspot::is_valid_pointer first; a
// failed gate downgrades to a recorded [INFO] / false observation, never a deref.
// All hooks are scoped_hook (uninstall on scope exit) so we never tear down
// another module's hooks.  Single probe cycle: each method is hooked
// independently and fires exactly once, so one run() drives every observation.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnFrameRaw.  Deriving from
    // vmhook::object<> gives it a vtable (required by register_class<T>),
    // get_instance() for the receiver-oop compare, and static_field / get_field
    // for the handshake and the `tag` cross-check.  Each typed getter reads into
    // a concretely-typed local first — field_proxy's value_t conversion operator
    // is templated, so a bare `static_field(...)->get() == x` is an ambiguous
    // deduction (see harness contract).
    class frame_raw_fixture : public vmhook::object<frame_raw_fixture>
    {
    public:
        explicit frame_raw_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<frame_raw_fixture>{ instance }
        {
        }

        // go / done handshake.
        static auto set_go(bool value) -> void  { static_field("go")->set(value); }
        static auto get_done() -> bool          { bool v = static_field("done")->get(); return v; }
        static auto reset_done() -> void        { static_field("done")->set(false); }
        static auto get_probe_ticks() -> std::int32_t { std::int32_t v = static_field("probeTicks")->get(); return v; }

        // Echoed observations (what each Java body actually received).
        static auto get_simple_seen()      -> std::int32_t { std::int32_t v = static_field("simpleSeen")->get();      return v; }
        static auto get_wide_a_seen()      -> std::int32_t { std::int32_t v = static_field("wideASeen")->get();       return v; }
        static auto get_wide_b_seen()      -> std::int64_t { std::int64_t v = static_field("wideBSeen")->get();       return v; }
        static auto get_wide_c_bits_seen() -> std::int64_t { std::int64_t v = static_field("wideCBitsSeen")->get();   return v; }
        static auto get_wide_d_seen()      -> std::int32_t { std::int32_t v = static_field("wideDSeen")->get();       return v; }
        static auto get_static_a_seen()    -> std::int32_t { std::int32_t v = static_field("staticASeen")->get();     return v; }
        static auto get_static_b_seen()    -> std::int64_t { std::int64_t v = static_field("staticBSeen")->get();     return v; }
        static auto get_static_c_seen()    -> std::int32_t { std::int32_t v = static_field("staticCSeen")->get();     return v; }
        static auto get_round_trip_seen()  -> std::int32_t { std::int32_t v = static_field("roundTripSeen")->get();   return v; }

        // Deepening-method observations.
        static auto get_narrow_z_seen()    -> std::int32_t { std::int32_t v = static_field("narrowZSeen")->get();     return v; }
        static auto get_narrow_b_seen()    -> std::int32_t { std::int32_t v = static_field("narrowBSeen")->get();     return v; }
        static auto get_narrow_c_seen()    -> std::int32_t { std::int32_t v = static_field("narrowCSeen")->get();     return v; }
        static auto get_narrow_s_seen()    -> std::int32_t { std::int32_t v = static_field("narrowSSeen")->get();     return v; }
        static auto get_narrow_i_seen()    -> std::int32_t { std::int32_t v = static_field("narrowISeen")->get();     return v; }
        static auto get_float_f_bits_seen()-> std::int64_t { std::int64_t v = static_field("floatFBitsSeen")->get();  return v; }
        static auto get_float_tail_seen()  -> std::int32_t { std::int32_t v = static_field("floatTailSeen")->get();   return v; }
        static auto get_edge_l_seen()      -> std::int64_t { std::int64_t v = static_field("edgeLSeen")->get();       return v; }
        static auto get_edge_d_bits_seen() -> std::int64_t { std::int64_t v = static_field("edgeDBitsSeen")->get();   return v; }
        static auto get_slf_l0_seen()      -> std::int64_t { std::int64_t v = static_field("slfL0Seen")->get();       return v; }
        static auto get_slf_tail_seen()    -> std::int32_t { std::int32_t v = static_field("slfTailSeen")->get();     return v; }
        static auto get_wide_rt_seen()     -> std::int64_t { std::int64_t v = static_field("wideRoundTripSeen")->get(); return v; }

        // Deepening batch-20 observations.
        static auto get_many5_seen()       -> std::int32_t { std::int32_t v = static_field("many5Seen")->get();       return v; }
        static auto get_many8_seen()       -> std::int32_t { std::int32_t v = static_field("many8Seen")->get();       return v; }
        static auto get_many_sum_seen()    -> std::int32_t { std::int32_t v = static_field("manySumSeen")->get();     return v; }
        static auto get_smw_l0_seen()      -> std::int64_t { std::int64_t v = static_field("smwL0Seen")->get();       return v; }
        static auto get_smw_d_bits_seen()  -> std::int64_t { std::int64_t v = static_field("smwDBitsSeen")->get();    return v; }
        static auto get_smw_l1_seen()      -> std::int64_t { std::int64_t v = static_field("smwL1Seen")->get();       return v; }
        static auto get_smw_t_seen()       -> std::int32_t { std::int32_t v = static_field("smwTSeen")->get();        return v; }
        static auto get_wtn_l_seen()       -> std::int64_t { std::int64_t v = static_field("wtnLSeen")->get();        return v; }
        static auto get_wtn_i_seen()       -> std::int32_t { std::int32_t v = static_field("wtnISeen")->get();        return v; }
        static auto get_wtn_d_bits_seen()  -> std::int64_t { std::int64_t v = static_field("wtnDBitsSeen")->get();    return v; }
        static auto get_wtn_tail_seen()    -> std::int32_t { std::int32_t v = static_field("wtnTailSeen")->get();     return v; }
        static auto get_zero_rt_seen()     -> std::int32_t { std::int32_t v = static_field("zeroRtSeen")->get();      return v; }

        // The receiver's per-instance fingerprint, read THROUGH whatever oop the
        // test recovered from local slot 0.  Proves slot 0 is the real receiver.
        auto tag() const -> std::int32_t { std::int32_t v = get_field("tag")->get(); return v; }
    };

    // ── Fixture-mirrored constants (kept in lockstep with ReturnFrameRaw.java) ─
    constexpr std::int32_t INSTANCE_TAG{ 0x5A11AB1E };
    constexpr std::int32_t SIMPLE_X{ 0x1234 };
    constexpr std::int32_t WIDE_A{ 0x0A0B0C0D };
    constexpr std::int64_t WIDE_B{ 0x1122334455667788LL };
    constexpr double       WIDE_C{ 3.141592653589793 };
    constexpr std::int32_t WIDE_D{ -0x0708090A };
    constexpr std::int32_t STATIC_A{ 0x00C0FFEE };
    constexpr std::int64_t STATIC_B{ 0x7EDCBA9876543210LL };
    constexpr std::int32_t STATIC_C{ 0x1BADD00D };

    // Narrow one-slot primitives.  Each value is what the Java body echoes after
    // its widening-to-int conversion: boolean->1, byte 0x80 sign-extends to -128,
    // char 0xBEEF zero-extends to 0xBEEF, short 0x8001 sign-extends to -32767,
    // int = Integer.MIN_VALUE.  The RAW low-32 slot bits, however, carry the
    // NARROW pattern the interpreter stored (boolean as 1, byte as 0xFFFFFF80,
    // char as 0x0000BEEF, short as 0xFFFF8001), which is what we read off locals.
    constexpr std::int32_t NARROW_Z_INT{ 1 };
    constexpr std::int32_t NARROW_B_INT{ -128 };
    constexpr std::int32_t NARROW_C_INT{ 0xBEEF };
    constexpr std::int32_t NARROW_S_INT{ -32767 };
    constexpr std::int32_t NARROW_I_INT{ INT32_C(-2147483648) };   // Integer.MIN_VALUE

    // float ~PI: raw IEEE-754 bits 0x40490FDB; the trailing int.
    constexpr std::int32_t FLOAT_F_BITS{ 0x40490FDB };
    constexpr std::int32_t FLOAT_TAIL{ 0x6C0FFEE5 };

    // Boundary wides.
    constexpr std::int64_t EDGE_LMIN{ INT64_MIN };
    constexpr std::int64_t EDGE_DNAN_BITS{ 0x7FF8000ABCDEF123LL };

    // Static leading-long.
    constexpr std::int64_t SLF_L0{ 0x0102030405060708LL };
    constexpr std::int32_t SLF_TAIL{ 0x55AA55AA };

    // Wide set_arg round-trip injected value.
    constexpr std::int64_t WIDE_RT_INJECT{ 0x0BADF00DDEADBEEFLL };

    // Eight one-slot ints (this@0, args@1..8).
    constexpr std::int32_t MANY_1{ 0x11111111 };
    constexpr std::int32_t MANY_2{ 0x22222222 };
    constexpr std::int32_t MANY_3{ 0x33333333 };
    constexpr std::int32_t MANY_4{ 0x44444444 };
    constexpr std::int32_t MANY_5{ 0x55555555 };
    constexpr std::int32_t MANY_6{ 0x66666666 };
    constexpr std::int32_t MANY_7{ 0x77777777 };
    constexpr std::int32_t MANY_8{ 0x7EEEEEEE };
    constexpr std::int32_t MANY_5_INJECT{ 0x0AB1CD05 };

    // The JVM max_locals boundary: get_argument / set_arg reject index > 0xFFFF.
    // The first rejected index is exactly 0x10000 (max_jvm_locals + 1) — the
    // off-by-one boundary the existing 0x7FFFFFFF test does not pin.
    constexpr std::int32_t MAX_JVM_LOCALS{ 0xFFFF };
    constexpr std::int32_t FIRST_OOB_SLOT{ MAX_JVM_LOCALS + 1 };   // 0x10000

    // Static three-wides-then-int.  The double bit pattern mirrors the Java
    // Double.longBitsToDouble(0x4005BF0995AAF790L); compared by raw bits, never
    // by value, so the exact decimal is irrelevant.
    constexpr std::int64_t SMW_L0{ 0x0A0A0A0A0B0B0B0BLL };
    constexpr std::int64_t SMW_D_BITS{ 0x4005BF0995AAF790LL };
    constexpr std::int64_t SMW_L1{ 0x0C0C0C0C0D0D0D0DLL };
    constexpr std::int32_t SMW_T{ 0x0E0E0E0E };

    // Instance long,int,double,int interleave.  The double is 2*PI; compared by
    // raw bits (0x400921FB54442D18) to stay exact.
    constexpr std::int64_t WTN_L{ 0x1314151617181920LL };
    constexpr std::int32_t WTN_I{ 0x21222324 };
    constexpr std::int64_t WTN_D_BITS{ 0x400921FB54442D18LL };
    constexpr std::int32_t WTN_TAIL{ 0x31323334 };

    // Narrow set_arg zero injection.
    constexpr std::int32_t ZERO_RT_INPUT{ 0x1357ACE0 };

    // ── Raw-slot read helpers (every deref gated by is_valid_pointer) ─────────

    // Reads the raw 64-bit machine word at local slot `base` (locals[-base]),
    // i.e. the verbatim slot contents BEFORE any oop-decode / primitive widen.
    // Returns false (and leaves out untouched) if the computed slot address
    // fails the safe-pointer gate — never dereferences a bad pointer.
    auto read_raw_slot(void** locals, std::int32_t base, std::uint64_t& out) noexcept -> bool
    {
        if (locals == nullptr || base < 0)
        {
            return false;
        }
        // The slot lives at &locals[-base]; validate that address itself.
        void* const slot_addr{ static_cast<void*>(locals - base) };
        if (!vmhook::hotspot::is_valid_pointer(slot_addr))
        {
            return false;
        }
        void* const raw{ locals[-base] };
        std::uint64_t bits{ 0 };
        std::memcpy(&bits, &raw, sizeof(bits));
        out = bits;
        return true;
    }

    // Decodes the slot-`base` value as a Java oop using the SAME convention the
    // library uses (frame::get_argument<void*> / get_arguments()): a narrow
    // (<= 0xFFFFFFFF) slot is run through decode_oop_pointer; a wider value is a
    // direct pointer.  Returns nullptr on any failure or invalid result.
    auto decode_slot_oop(void** locals, std::int32_t base) noexcept -> void*
    {
        std::uint64_t bits{ 0 };
        if (!read_raw_slot(locals, base, bits) || bits == 0)
        {
            return nullptr;
        }
        void* decoded{ nullptr };
        if (bits <= 0xFFFFFFFFull)
        {
            decoded = vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(bits));
        }
        else
        {
            decoded = reinterpret_cast<void*>(bits);
        }
        if (decoded == nullptr || !vmhook::hotspot::is_valid_pointer(decoded))
        {
            return nullptr;
        }
        return decoded;
    }

    // ── Per-hook observation state (reset per module run) ─────────────────────

    // instanceSimple: method identity + this-slot + bounds.
    std::atomic<std::int32_t> g_simple_calls{ 0 };
    std::atomic<bool>         g_simple_frame_nonnull{ false };
    std::atomic<bool>         g_simple_method_nonnull{ false };
    std::atomic<bool>         g_simple_name_ok{ false };
    std::atomic<bool>         g_simple_sig_ok{ false };
    std::atomic<bool>         g_simple_locals_nonnull{ false };
    std::atomic<bool>         g_simple_self_nonnull{ false };
    std::atomic<bool>         g_simple_slot0_oop_matches_self{ false };
    std::atomic<bool>         g_simple_slot0_tag_ok{ false };
    std::atomic<bool>         g_simple_typed_arg0_matches_self{ false };
    std::atomic<bool>         g_simple_raw_slot1_ok{ false };
    std::atomic<bool>         g_simple_bounds_no_crash{ false };
    std::atomic<std::int32_t> g_simple_bounds_overread_value{ 0 };
    std::atomic<bool>         g_simple_oob_index_rejected{ false };
    std::atomic<bool>         g_simple_neg_index_rejected{ false };

    // instanceWide: full slot model (int / long@2slots / double@2slots / int).
    std::atomic<std::int32_t> g_wide_calls{ 0 };
    std::atomic<bool>         g_wide_frame_nonnull{ false };
    std::atomic<bool>         g_wide_locals_nonnull{ false };
    std::atomic<bool>         g_wide_self_ok{ false };
    std::atomic<bool>         g_wide_raw_a_ok{ false };   // int  @ slot 1
    std::atomic<bool>         g_wide_raw_b_ok{ false };   // long @ slot 2, value@-3
    std::atomic<bool>         g_wide_raw_c_ok{ false };   // double @ slot 4, value@-5
    std::atomic<bool>         g_wide_raw_d_ok{ false };   // int  @ slot 6
    std::atomic<bool>         g_wide_typed_a_ok{ false };
    std::atomic<bool>         g_wide_typed_b_ok{ false };
    std::atomic<bool>         g_wide_typed_c_ok{ false };
    std::atomic<bool>         g_wide_typed_d_ok{ false };

    // staticWide: NO this at slot 0.
    std::atomic<std::int32_t> g_static_calls{ 0 };
    std::atomic<bool>         g_static_frame_nonnull{ false };
    std::atomic<bool>         g_static_locals_nonnull{ false };
    std::atomic<bool>         g_static_slot0_is_first_int{ false };
    std::atomic<bool>         g_static_slot0_not_receiver{ false };
    std::atomic<bool>         g_static_typed_args_ok{ false };

    // roundTrip: frame()'s locals alias the array set_arg mutates.
    std::atomic<std::int32_t> g_rt_calls{ 0 };
    std::atomic<bool>         g_rt_set_arg_ok{ false };
    std::atomic<bool>         g_rt_raw_readback_ok{ false };
    // Negative-index set_arg guard (shares get_argument's index<0 reject).
    std::atomic<bool>         g_rt_neg_index_rejected{ false };
    // Same frame() pointer returned twice (stash identity).
    std::atomic<bool>         g_rt_frame_stable{ false };
    // get_arguments<>() with an empty pack is a safe no-op.
    std::atomic<bool>         g_rt_empty_pack_ok{ false };

    // instanceSimple extras: frame()->get_method() vs the kind (NOT static),
    // and caller()/stack_trace() off the live frame.
    std::atomic<bool>         g_simple_not_static{ false };
    std::atomic<bool>         g_simple_static_flag_readable{ false };
    std::atomic<bool>         g_simple_caller_valid{ false };
    std::atomic<bool>         g_simple_caller_is_runall{ false };
    std::atomic<bool>         g_simple_stacktrace_nonempty{ false };
    std::atomic<bool>         g_simple_stacktrace_head_is_caller{ false };

    // staticWide extra: frame()->get_method() reports ACC_STATIC.
    std::atomic<bool>         g_static_is_static{ false };
    std::atomic<bool>         g_static_static_flag_readable{ false };

    // instanceNarrow: five one-slot primitives at consecutive base slots 1..5.
    std::atomic<std::int32_t> g_narrow_calls{ 0 };
    std::atomic<bool>         g_narrow_frame_nonnull{ false };
    std::atomic<bool>         g_narrow_locals_nonnull{ false };
    std::atomic<bool>         g_narrow_raw_z_ok{ false };
    std::atomic<bool>         g_narrow_raw_b_ok{ false };
    std::atomic<bool>         g_narrow_raw_c_ok{ false };
    std::atomic<bool>         g_narrow_raw_s_ok{ false };
    std::atomic<bool>         g_narrow_raw_i_ok{ false };
    std::atomic<bool>         g_narrow_typed_ok{ false };

    // instanceFloat: float is ONE slot -> trailing int at slot 2, not slot 3.
    std::atomic<std::int32_t> g_float_calls{ 0 };
    std::atomic<bool>         g_float_frame_nonnull{ false };
    std::atomic<bool>         g_float_locals_nonnull{ false };
    std::atomic<bool>         g_float_raw_f_ok{ false };
    std::atomic<bool>         g_float_raw_tail_ok{ false };
    std::atomic<bool>         g_float_typed_ok{ false };

    // instanceEdgeWide: Long.MIN then NaN double, both two-slot.
    std::atomic<std::int32_t> g_edge_calls{ 0 };
    std::atomic<bool>         g_edge_frame_nonnull{ false };
    std::atomic<bool>         g_edge_locals_nonnull{ false };
    std::atomic<bool>         g_edge_raw_l_ok{ false };
    std::atomic<bool>         g_edge_raw_d_ok{ false };
    std::atomic<bool>         g_edge_typed_ok{ false };

    // staticLeadingLong: a long at slot 0 of a STATIC frame (no this).
    std::atomic<std::int32_t> g_slf_calls{ 0 };
    std::atomic<bool>         g_slf_frame_nonnull{ false };
    std::atomic<bool>         g_slf_locals_nonnull{ false };
    std::atomic<bool>         g_slf_raw_l0_ok{ false };
    std::atomic<bool>         g_slf_raw_tail_ok{ false };
    std::atomic<bool>         g_slf_typed_ok{ false };

    // wideRoundTrip: set_arg of a long writes the lower slot (locals[-2]).
    std::atomic<std::int32_t> g_wrt_calls{ 0 };
    std::atomic<bool>         g_wrt_set_arg_ok{ false };
    std::atomic<bool>         g_wrt_raw_readback_ok{ false };

    // instanceManyLocals: eight one-slot ints deep into the array + boundary
    // set_arg cases (in-range high slot writes; first-OOB index rejected).
    std::atomic<std::int32_t> g_many_calls{ 0 };
    std::atomic<bool>         g_many_frame_nonnull{ false };
    std::atomic<bool>         g_many_locals_nonnull{ false };
    std::atomic<bool>         g_many_self_ok{ false };
    std::atomic<bool>         g_many_raw_all_ok{ false };    // every raw slot 1..8 (except 5) matches
    std::atomic<bool>         g_many_typed_all_ok{ false };  // typed get_arguments<oop,8x int>
    std::atomic<bool>         g_many_high_set_arg_ok{ false }; // set_arg(5, in-range) returned true
    std::atomic<bool>         g_many_high_readback_ok{ false };// raw slot 5 == injected
    std::atomic<bool>         g_many_first_oob_rejected{ false }; // set_arg(0x10000) rejected
    std::atomic<bool>         g_many_last_inrange_accepts{ false }; // set_arg(0xFFFF) NOT index-rejected

    // staticManyWide: three consecutive two-slot args (no this).
    std::atomic<std::int32_t> g_smw_calls{ 0 };
    std::atomic<bool>         g_smw_frame_nonnull{ false };
    std::atomic<bool>         g_smw_locals_nonnull{ false };
    std::atomic<bool>         g_smw_raw_l0_ok{ false };
    std::atomic<bool>         g_smw_raw_d_ok{ false };
    std::atomic<bool>         g_smw_raw_l1_ok{ false };
    std::atomic<bool>         g_smw_raw_t_ok{ false };
    std::atomic<bool>         g_smw_typed_ok{ false };

    // instanceWideThenNarrow: long,int,double,int interleave (instance, this@0).
    std::atomic<std::int32_t> g_wtn_calls{ 0 };
    std::atomic<bool>         g_wtn_frame_nonnull{ false };
    std::atomic<bool>         g_wtn_locals_nonnull{ false };
    std::atomic<bool>         g_wtn_raw_l_ok{ false };
    std::atomic<bool>         g_wtn_raw_i_ok{ false };
    std::atomic<bool>         g_wtn_raw_d_ok{ false };
    std::atomic<bool>         g_wtn_raw_tail_ok{ false };
    std::atomic<bool>         g_wtn_typed_ok{ false };

    // zeroRoundTrip: set_arg(1, 0) — the all-zero one-slot write path.
    std::atomic<std::int32_t> g_zrt_calls{ 0 };
    std::atomic<bool>         g_zrt_set_arg_ok{ false };
    std::atomic<bool>         g_zrt_raw_readback_ok{ false };

    auto reset_observations() -> void
    {
        g_simple_calls.store(0);
        g_simple_frame_nonnull.store(false);
        g_simple_method_nonnull.store(false);
        g_simple_name_ok.store(false);
        g_simple_sig_ok.store(false);
        g_simple_locals_nonnull.store(false);
        g_simple_self_nonnull.store(false);
        g_simple_slot0_oop_matches_self.store(false);
        g_simple_slot0_tag_ok.store(false);
        g_simple_typed_arg0_matches_self.store(false);
        g_simple_raw_slot1_ok.store(false);
        g_simple_bounds_no_crash.store(false);
        g_simple_bounds_overread_value.store(0);
        g_simple_oob_index_rejected.store(false);
        g_simple_neg_index_rejected.store(false);

        g_wide_calls.store(0);
        g_wide_frame_nonnull.store(false);
        g_wide_locals_nonnull.store(false);
        g_wide_self_ok.store(false);
        g_wide_raw_a_ok.store(false);
        g_wide_raw_b_ok.store(false);
        g_wide_raw_c_ok.store(false);
        g_wide_raw_d_ok.store(false);
        g_wide_typed_a_ok.store(false);
        g_wide_typed_b_ok.store(false);
        g_wide_typed_c_ok.store(false);
        g_wide_typed_d_ok.store(false);

        g_static_calls.store(0);
        g_static_frame_nonnull.store(false);
        g_static_locals_nonnull.store(false);
        g_static_slot0_is_first_int.store(false);
        g_static_slot0_not_receiver.store(false);
        g_static_typed_args_ok.store(false);

        g_rt_calls.store(0);
        g_rt_set_arg_ok.store(false);
        g_rt_raw_readback_ok.store(false);
        g_rt_neg_index_rejected.store(false);
        g_rt_frame_stable.store(false);
        g_rt_empty_pack_ok.store(false);

        g_simple_not_static.store(false);
        g_simple_static_flag_readable.store(false);
        g_simple_caller_valid.store(false);
        g_simple_caller_is_runall.store(false);
        g_simple_stacktrace_nonempty.store(false);
        g_simple_stacktrace_head_is_caller.store(false);

        g_static_is_static.store(false);
        g_static_static_flag_readable.store(false);

        g_narrow_calls.store(0);
        g_narrow_frame_nonnull.store(false);
        g_narrow_locals_nonnull.store(false);
        g_narrow_raw_z_ok.store(false);
        g_narrow_raw_b_ok.store(false);
        g_narrow_raw_c_ok.store(false);
        g_narrow_raw_s_ok.store(false);
        g_narrow_raw_i_ok.store(false);
        g_narrow_typed_ok.store(false);

        g_float_calls.store(0);
        g_float_frame_nonnull.store(false);
        g_float_locals_nonnull.store(false);
        g_float_raw_f_ok.store(false);
        g_float_raw_tail_ok.store(false);
        g_float_typed_ok.store(false);

        g_edge_calls.store(0);
        g_edge_frame_nonnull.store(false);
        g_edge_locals_nonnull.store(false);
        g_edge_raw_l_ok.store(false);
        g_edge_raw_d_ok.store(false);
        g_edge_typed_ok.store(false);

        g_slf_calls.store(0);
        g_slf_frame_nonnull.store(false);
        g_slf_locals_nonnull.store(false);
        g_slf_raw_l0_ok.store(false);
        g_slf_raw_tail_ok.store(false);
        g_slf_typed_ok.store(false);

        g_wrt_calls.store(0);
        g_wrt_set_arg_ok.store(false);
        g_wrt_raw_readback_ok.store(false);

        g_many_calls.store(0);
        g_many_frame_nonnull.store(false);
        g_many_locals_nonnull.store(false);
        g_many_self_ok.store(false);
        g_many_raw_all_ok.store(false);
        g_many_typed_all_ok.store(false);
        g_many_high_set_arg_ok.store(false);
        g_many_high_readback_ok.store(false);
        g_many_first_oob_rejected.store(false);
        g_many_last_inrange_accepts.store(false);

        g_smw_calls.store(0);
        g_smw_frame_nonnull.store(false);
        g_smw_locals_nonnull.store(false);
        g_smw_raw_l0_ok.store(false);
        g_smw_raw_d_ok.store(false);
        g_smw_raw_l1_ok.store(false);
        g_smw_raw_t_ok.store(false);
        g_smw_typed_ok.store(false);

        g_wtn_calls.store(0);
        g_wtn_frame_nonnull.store(false);
        g_wtn_locals_nonnull.store(false);
        g_wtn_raw_l_ok.store(false);
        g_wtn_raw_i_ok.store(false);
        g_wtn_raw_d_ok.store(false);
        g_wtn_raw_tail_ok.store(false);
        g_wtn_typed_ok.store(false);

        g_zrt_calls.store(0);
        g_zrt_set_arg_ok.store(false);
        g_zrt_raw_readback_ok.store(false);
    }
}

VMHOOK_JVM_MODULE(return_frame_raw_access)
{
    vmhook::register_class<frame_raw_fixture>("vmhook/fixtures/ReturnFrameRaw");

    reset_observations();

    // All hooks live in this scope and uninstall on exit.  Explicit JVM
    // descriptors disambiguate every method.
    {
        // ── instanceSimple(int): method identity + this-slot + bounds ───────
        auto h_simple{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceSimple", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& self,
               std::int32_t /*x*/)
            {
                g_simple_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;   // gated: never deref a bad/null frame
                }
                g_simple_frame_nonnull.store(true, std::memory_order_relaxed);

                // get_method(): same method the hook was installed on.
                vmhook::hotspot::method* const m{ fr->get_method() };
                if (m != nullptr && vmhook::hotspot::is_valid_pointer(m))
                {
                    g_simple_method_nonnull.store(true, std::memory_order_relaxed);
                    const std::string name = m->get_name();
                    const std::string sig  = m->get_signature();
                    g_simple_name_ok.store(name == "instanceSimple", std::memory_order_relaxed);
                    g_simple_sig_ok.store(sig == "(I)I", std::memory_order_relaxed);

                    // The frame's method is an INSTANCE method: ACC_STATIC (0x0008)
                    // is NOT set.  safe_access_flags_test reads the u4 through the
                    // kernel-validated path, so a cold Method* yields found=false
                    // (recorded, never a fault) rather than a wrong answer.
                    bool flags_readable{ false };
                    const bool is_static{ m->safe_access_flags_test(0x0008u, flags_readable) };
                    g_simple_static_flag_readable.store(flags_readable, std::memory_order_relaxed);
                    if (flags_readable)
                    {
                        g_simple_not_static.store(!is_static, std::memory_order_relaxed);
                    }
                }

                if (self != nullptr)
                {
                    g_simple_self_nonnull.store(true, std::memory_order_relaxed);
                }

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    // Some JDKs where the locals_offset scan failed legitimately
                    // return null; record it but do not fail the run on it.
                    return;
                }
                g_simple_locals_nonnull.store(true, std::memory_order_relaxed);

                // slot 0 == `this`: decode the raw slot oop and compare to self.
                void* const slot0_oop{ decode_slot_oop(locals, 0) };
                if (slot0_oop != nullptr && self != nullptr)
                {
                    g_simple_slot0_oop_matches_self.store(
                        slot0_oop == self->get_instance(), std::memory_order_relaxed);

                    // Read `tag` THROUGH the recovered oop — proves it is a live,
                    // correct receiver, not just a coincidentally-equal pointer.
                    frame_raw_fixture recovered{ slot0_oop };
                    g_simple_slot0_tag_ok.store(recovered.tag() == INSTANCE_TAG,
                                                std::memory_order_relaxed);
                }

                // Cross-check with the PUBLIC typed accessor: get_arguments<oop>
                // decodes slot 0 the same way the library does internally.
                {
                    auto [decoded_self] = fr->get_arguments<vmhook::oop_t>();
                    if (decoded_self != nullptr && self != nullptr)
                    {
                        g_simple_typed_arg0_matches_self.store(
                            decoded_self == self->get_instance(), std::memory_order_relaxed);
                    }
                }

                // Raw read of the single primitive arg at slot 1 (low 32 bits).
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_simple_raw_slot1_ok.store(
                            static_cast<std::int32_t>(bits) == SIMPLE_X,
                            std::memory_order_relaxed);
                    }
                }

                // BOUNDS — part 1 (NO CRASH on over-read): over-request types so
                // the public typed accessor reads several slots PAST this 1-arg
                // (+this) method's real locals.  Each read goes through the
                // private get_argument bounds guard (index <= 0xFFFF) and then
                // reads adjacent stack words; the contract under test is "this
                // never crashes the JVM".  Reaching the line after the call is
                // the proof.  We do NOT assert the over-read *value* — past-the-
                // end slots alias live operand-stack / saved-register words whose
                // contents are not defined — we only characterise it via [INFO].
                {
                    auto t = fr->get_arguments<vmhook::oop_t,
                                               std::int32_t, std::int32_t,
                                               std::int32_t, std::int32_t,
                                               std::int32_t>();
                    g_simple_bounds_no_crash.store(true, std::memory_order_relaxed);
                    g_simple_bounds_overread_value.store(std::get<5>(t),
                                                         std::memory_order_relaxed);
                }

                // BOUNDS — part 2 (DEFAULT/REJECT on truly out-of-range index):
                // the only place the library can be DRIVEN past 0xFFFF is set_arg
                // (which shares get_argument's `index > 0xFFFF` guard).  A huge
                // index must be rejected (return false) with no wild write and no
                // crash — the deterministic half of the bounds contract.  We use
                // a value the body never expects so a (wrongly) successful write
                // would be detectable; the allow-through check below proves the
                // original arg survived.
                {
                    const bool rejected{ !ret.set_arg(0x7FFFFFFF,
                                                       static_cast<std::int32_t>(0xBADBAD)) };
                    g_simple_oob_index_rejected.store(rejected, std::memory_order_relaxed);
                }

                // BOUNDS — part 3 (NEGATIVE index): set_arg shares get_argument's
                // `index < 0` reject (a negative index makes -index a huge positive
                // offset that walks off the local array).  Must return false, write
                // nothing, never crash — the other half of the bounds guard the
                // typed get_arguments<> path cannot reach (its slot indices are
                // always >= 0).
                {
                    const bool neg_rejected{ !ret.set_arg(-1,
                                                          static_cast<std::int32_t>(0xBADBAD)) };
                    g_simple_neg_index_rejected.store(neg_rejected, std::memory_order_relaxed);
                }

                // caller(): the immediate caller of instanceSimple is the fixture's
                // private runAll() dispatcher.  This walks the saved-rbp chain off
                // the SAME live frame() we hold.  Cross-JDK: if runAll is JIT-
                // compiled / inlined the chain breaks and valid()==false — gated as
                // [INFO] (the HARD part is that the call does not crash and, when it
                // DOES identify a caller, the name is runAll).
                {
                    const vmhook::return_value::caller_info ci{ ret.caller() };
                    if (ci.valid())
                    {
                        g_simple_caller_valid.store(true, std::memory_order_relaxed);
                        g_simple_caller_is_runall.store(ci.method_name == "runAll",
                                                        std::memory_order_relaxed);
                    }

                    // stack_trace(): non-empty when interpreted, and its head entry
                    // is the same method caller() reported.
                    const std::vector<vmhook::return_value::caller_info> trace{ ret.stack_trace() };
                    if (!trace.empty())
                    {
                        g_simple_stacktrace_nonempty.store(true, std::memory_order_relaxed);
                        if (ci.valid())
                        {
                            g_simple_stacktrace_head_is_caller.store(
                                trace.front().method_name == ci.method_name,
                                std::memory_order_relaxed);
                        }
                    }
                }
            }) };
        ctx.check("frame_simple_hook_installed", h_simple.installed());

        // ── instanceWide(int,long,double,int): full slot model ──────────────
        auto h_wide{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceWide", "(IJDI)J",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& self,
               std::int32_t /*a*/, std::int64_t /*b*/, double /*c*/, std::int32_t /*d*/)
            {
                g_wide_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_wide_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_wide_locals_nonnull.store(true, std::memory_order_relaxed);

                // this @ slot 0.
                void* const slot0_oop{ decode_slot_oop(locals, 0) };
                if (slot0_oop != nullptr && self != nullptr)
                {
                    g_wide_self_ok.store(slot0_oop == self->get_instance(),
                                         std::memory_order_relaxed);
                }

                // RAW reads honouring the HotSpot slot model:
                //   a (int)    @ base slot 1        -> locals[-1]    low 32 bits
                //   b (long)   @ base slot 2        -> value@-(2+1) = locals[-3]
                //   c (double) @ base slot 4        -> value@-(4+1) = locals[-5]
                //   d (int)    @ base slot 6        -> locals[-6]    low 32 bits
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_wide_raw_a_ok.store(static_cast<std::int32_t>(bits) == WIDE_A,
                                              std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 3, bits))   // long value at the LOWER slot
                    {
                        g_wide_raw_b_ok.store(static_cast<std::int64_t>(bits) == WIDE_B,
                                              std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 5, bits))   // double value at the LOWER slot
                    {
                        double d{ 0.0 };
                        std::memcpy(&d, &bits, sizeof(d));
                        g_wide_raw_c_ok.store(d == WIDE_C, std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 6, bits))
                    {
                        g_wide_raw_d_ok.store(static_cast<std::int32_t>(bits) == WIDE_D,
                                              std::memory_order_relaxed);
                    }
                }

                // Cross-check against the PUBLIC typed accessor, which widens
                // long/double across two slots internally.  Both raw and typed
                // paths must agree (audit: typed-matches-autodetect).
                //
                // `wide(...)` is an INSTANCE method, so slot 0 holds `this`.
                // get_arguments<Ts...> maps each Ti to a consecutive slot
                // (advancing +2 per long/double), so the receiver MUST be the
                // first template parameter or every subsequent arg is read one
                // slot too early.  The raw path above already accounts for
                // this (a@slot1); mirror it here with an oop placeholder.
                {
                    auto [self_unused, a, b, c, d] =
                        fr->get_arguments<vmhook::oop_t, std::int32_t, std::int64_t, double, std::int32_t>();
                    (void)self_unused;
                    g_wide_typed_a_ok.store(a == WIDE_A, std::memory_order_relaxed);
                    g_wide_typed_b_ok.store(b == WIDE_B, std::memory_order_relaxed);
                    g_wide_typed_c_ok.store(c == WIDE_C, std::memory_order_relaxed);
                    g_wide_typed_d_ok.store(d == WIDE_D, std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_wide_hook_installed", h_wide.installed());

        // ── staticWide(int,long,int): NO this at slot 0 ─────────────────────
        auto h_static{ vmhook::scoped_hook<frame_raw_fixture>(
            "staticWide", "(IJI)J",
            [](vmhook::return_value& ret,
               std::int32_t /*a*/, std::int64_t /*b*/, std::int32_t /*c*/)
            {
                g_static_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_static_frame_nonnull.store(true, std::memory_order_relaxed);

                // The frame's method DOES carry ACC_STATIC (0x0008): the access-flags
                // counterpart of the no-this slot-0 layout below.  Fault-safe u4 read.
                {
                    vmhook::hotspot::method* const m{ fr->get_method() };
                    if (m != nullptr && vmhook::hotspot::is_valid_pointer(m))
                    {
                        bool flags_readable{ false };
                        const bool is_static{ m->safe_access_flags_test(0x0008u, flags_readable) };
                        g_static_static_flag_readable.store(flags_readable, std::memory_order_relaxed);
                        if (flags_readable)
                        {
                            g_static_is_static.store(is_static, std::memory_order_relaxed);
                        }
                    }
                }

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_static_locals_nonnull.store(true, std::memory_order_relaxed);

                // slot 0 is the FIRST PRIMITIVE arg (a), NOT a receiver: a static
                // method's frame has no `this`.  Its raw low-32 value must equal
                // STATIC_A — a small integer, NOT a compressed oop / heap pointer.
                // (We deliberately do NOT decode slot 0 as an oop and chase it:
                // a static frame's slot 0 is a primitive, so decoding it would
                // fabricate a bogus pointer; reading a field through that would
                // violate the never-deref-unchecked rule.)
                std::int32_t static_slot0_raw{ 0 };
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 0, bits))
                    {
                        static_slot0_raw = static_cast<std::int32_t>(bits);
                        g_static_slot0_is_first_int.store(
                            static_slot0_raw == STATIC_A, std::memory_order_relaxed);
                    }
                }

                // Typed accessor decodes the static args from slot 0 onward with
                // NO `this` shift: a@0, b@1(long), c@3(after the long).  That the
                // FIRST typed arg is STATIC_A (the Java arg0) — and not some
                // receiver-shaped value — is the decoder-level confirmation that
                // slot 0 carries arg0, not `this`.
                {
                    auto [a, b, c] =
                        fr->get_arguments<std::int32_t, std::int64_t, std::int32_t>();
                    g_static_typed_args_ok.store(
                        a == STATIC_A && b == STATIC_B && c == STATIC_C,
                        std::memory_order_relaxed);
                    // arg0 lands at slot 0 (no this): the typed read of slot 0 and
                    // the raw read of slot 0 agree, both == STATIC_A.
                    g_static_slot0_not_receiver.store(
                        a == STATIC_A && a == static_slot0_raw,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_static_hook_installed", h_static.installed());

        // ── roundTrip(int): frame() locals alias the set_arg array ──────────
        auto h_rt{ vmhook::scoped_hook<frame_raw_fixture>(
            "roundTrip", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               std::int32_t /*value*/)
            {
                g_rt_calls.fetch_add(1, std::memory_order_relaxed);

                // Mutate slot 1 via the typed API, then read it back RAW through
                // frame()->get_locals() to prove both reach the same array.
                const std::int32_t injected{ 0x4242 };
                const bool set_ok{ ret.set_arg(1, injected) };
                g_rt_set_arg_ok.store(set_ok, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }

                // Stash identity: frame() is a plain accessor over a stored pointer,
                // so a second call returns the SAME frame for the SAME activation.
                {
                    vmhook::hotspot::frame* const fr2{ ret.frame() };
                    g_rt_frame_stable.store(fr2 == fr, std::memory_order_relaxed);
                }

                // Degenerate typed read: get_arguments<>() with an EMPTY pack must
                // return an empty tuple and touch no slot — a safe no-op even on a
                // live frame.  Reaching the line after it (and the std::tuple_size
                // being 0) is the proof.
                {
                    auto empty = fr->get_arguments<>();
                    g_rt_empty_pack_ok.store(std::tuple_size<decltype(empty)>::value == 0,
                                             std::memory_order_relaxed);
                }

                void** const locals{ fr->get_locals() };
                std::uint64_t bits{ 0 };
                if (read_raw_slot(locals, 1, bits))
                {
                    g_rt_raw_readback_ok.store(
                        static_cast<std::int32_t>(bits) == injected,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_round_trip_hook_installed", h_rt.installed());

        // ── instanceNarrow(boolean,byte,char,short,int): five one-slot args ──
        auto h_narrow{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceNarrow", "(ZBCSI)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               bool /*z*/, std::int32_t /*b*/, std::int32_t /*c*/,
               std::int32_t /*s*/, std::int32_t /*i*/)
            {
                g_narrow_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_narrow_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_narrow_locals_nonnull.store(true, std::memory_order_relaxed);

                // No two-slot gaps: boolean@1, byte@2, char@3, short@4, int@5.  The
                // raw low-32 bits carry the interpreter's stored narrow pattern
                // (boolean as 1; byte/short sign-extended; char zero-extended).
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_narrow_raw_z_ok.store(static_cast<std::int32_t>(bits) == NARROW_Z_INT,
                                                std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 2, bits))
                    {
                        g_narrow_raw_b_ok.store(static_cast<std::int32_t>(bits) == NARROW_B_INT,
                                                std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 3, bits))
                    {
                        g_narrow_raw_c_ok.store(static_cast<std::int32_t>(bits) == NARROW_C_INT,
                                                std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 4, bits))
                    {
                        g_narrow_raw_s_ok.store(static_cast<std::int32_t>(bits) == NARROW_S_INT,
                                                std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 5, bits))
                    {
                        g_narrow_raw_i_ok.store(static_cast<std::int32_t>(bits) == NARROW_I_INT,
                                                std::memory_order_relaxed);
                    }
                }

                // Typed cross-check: all five widen to int with NO slot shift.
                {
                    auto [self_unused, z, b, c, s, i] =
                        fr->get_arguments<vmhook::oop_t, std::int32_t, std::int32_t,
                                          std::int32_t, std::int32_t, std::int32_t>();
                    (void)self_unused;
                    g_narrow_typed_ok.store(
                        z == NARROW_Z_INT && b == NARROW_B_INT && c == NARROW_C_INT &&
                        s == NARROW_S_INT && i == NARROW_I_INT,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_narrow_hook_installed", h_narrow.installed());

        // ── instanceFloat(float,int): float is ONE slot ─────────────────────
        auto h_float{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceFloat", "(FI)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               float /*f*/, std::int32_t /*tail*/)
            {
                g_float_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_float_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_float_locals_nonnull.store(true, std::memory_order_relaxed);

                // float @ slot 1 (low 32 bits = raw IEEE-754 bits).
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_float_raw_f_ok.store(static_cast<std::int32_t>(bits) == FLOAT_F_BITS,
                                               std::memory_order_relaxed);
                    }
                }
                // tail int @ slot 2 — NOT slot 3: float did not consume two slots.
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 2, bits))
                    {
                        g_float_raw_tail_ok.store(static_cast<std::int32_t>(bits) == FLOAT_TAIL,
                                                  std::memory_order_relaxed);
                    }
                }

                // Typed cross-check: float occupies one slot, so tail decodes right
                // after it.  (We compare the float's bit pattern, not the value, to
                // stay exact across rounding.)
                {
                    auto [self_unused, f, tail] =
                        fr->get_arguments<vmhook::oop_t, float, std::int32_t>();
                    (void)self_unused;
                    std::int32_t f_bits{ 0 };
                    std::memcpy(&f_bits, &f, sizeof(f_bits));
                    g_float_typed_ok.store(f_bits == FLOAT_F_BITS && tail == FLOAT_TAIL,
                                           std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_float_hook_installed", h_float.installed());

        // ── instanceEdgeWide(long,double): boundary two-slot values ─────────
        auto h_edge{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceEdgeWide", "(JD)J",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               std::int64_t /*l*/, double /*d*/)
            {
                g_edge_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_edge_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_edge_locals_nonnull.store(true, std::memory_order_relaxed);

                // long @ base slot 1, 64-bit value at lower slot locals[-2].
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 2, bits))
                    {
                        g_edge_raw_l_ok.store(static_cast<std::int64_t>(bits) == EDGE_LMIN,
                                              std::memory_order_relaxed);
                    }
                }
                // double @ base slot 3, 64-bit value at lower slot locals[-4].  We
                // compare RAW BITS (the value is a NaN, so == would be false).
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 4, bits))
                    {
                        g_edge_raw_d_ok.store(
                            static_cast<std::int64_t>(bits) == EDGE_DNAN_BITS,
                            std::memory_order_relaxed);
                    }
                }

                // Typed cross-check: long and double each widen across two slots.
                {
                    auto [self_unused, l, d] =
                        fr->get_arguments<vmhook::oop_t, std::int64_t, double>();
                    (void)self_unused;
                    std::int64_t d_bits{ 0 };
                    std::memcpy(&d_bits, &d, sizeof(d_bits));
                    g_edge_typed_ok.store(l == EDGE_LMIN && d_bits == EDGE_DNAN_BITS,
                                          std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_edge_hook_installed", h_edge.installed());

        // ── staticLeadingLong(long,int): a long at slot 0 of a STATIC frame ─
        auto h_slf{ vmhook::scoped_hook<frame_raw_fixture>(
            "staticLeadingLong", "(JI)J",
            [](vmhook::return_value& ret,
               std::int64_t /*l*/, std::int32_t /*tail*/)
            {
                g_slf_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_slf_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_slf_locals_nonnull.store(true, std::memory_order_relaxed);

                // No this: the leading long sits at base slot 0, its 64-bit value at
                // the lower slot locals[-1].  The trailing int is at slot 2.
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_slf_raw_l0_ok.store(static_cast<std::int64_t>(bits) == SLF_L0,
                                              std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 2, bits))
                    {
                        g_slf_raw_tail_ok.store(static_cast<std::int32_t>(bits) == SLF_TAIL,
                                                std::memory_order_relaxed);
                    }
                }

                // Typed cross-check: no this -> long@0(value@1), int@2.
                {
                    auto [l, tail] = fr->get_arguments<std::int64_t, std::int32_t>();
                    g_slf_typed_ok.store(l == SLF_L0 && tail == SLF_TAIL,
                                         std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_slf_hook_installed", h_slf.installed());

        // ── wideRoundTrip(long): set_arg of a long writes the lower slot ────
        auto h_wrt{ vmhook::scoped_hook<frame_raw_fixture>(
            "wideRoundTrip", "(J)J",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               std::int64_t /*value*/)
            {
                g_wrt_calls.fetch_add(1, std::memory_order_relaxed);

                // set_arg(1, <long>) writes the 64-bit value at the LOWER slot
                // locals[-2] (the same place a long arg is read from).
                const bool set_ok{ ret.set_arg(1, WIDE_RT_INJECT) };
                g_wrt_set_arg_ok.store(set_ok, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                void** const locals{ fr->get_locals() };
                std::uint64_t bits{ 0 };
                if (read_raw_slot(locals, 2, bits))   // long value at lower slot
                {
                    g_wrt_raw_readback_ok.store(
                        static_cast<std::int64_t>(bits) == WIDE_RT_INJECT,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_wide_round_trip_hook_installed", h_wrt.installed());

        // ── instanceManyLocals(8x int): deep one-slot walk + set_arg bounds ─
        auto h_many{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceManyLocals", "(IIIIIIII)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& self,
               std::int32_t /*a1*/, std::int32_t /*a2*/, std::int32_t /*a3*/,
               std::int32_t /*a4*/, std::int32_t /*a5*/, std::int32_t /*a6*/,
               std::int32_t /*a7*/, std::int32_t /*a8*/)
            {
                g_many_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_many_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_many_locals_nonnull.store(true, std::memory_order_relaxed);

                // this @ slot 0.
                void* const slot0_oop{ decode_slot_oop(locals, 0) };
                if (slot0_oop != nullptr && self != nullptr)
                {
                    g_many_self_ok.store(slot0_oop == self->get_instance(),
                                         std::memory_order_relaxed);
                }

                // Eight one-slot ints at consecutive base slots 1..8 (no gaps).
                // Slot 5 is excluded from the all-match aggregate because the
                // set_arg(5, ...) below mutates it; it is verified separately by
                // the high-slot readback.
                const std::int32_t expect[]{
                    MANY_1, MANY_2, MANY_3, MANY_4,
                    MANY_5, MANY_6, MANY_7, MANY_8 };
                bool all_ok{ true };
                for (std::int32_t base = 1; base <= 8; ++base)
                {
                    if (base == 5)
                    {
                        continue;
                    }
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, base, bits))
                    {
                        if (static_cast<std::int32_t>(bits) != expect[base - 1])
                        {
                            all_ok = false;
                        }
                    }
                    else
                    {
                        all_ok = false;
                    }
                }
                g_many_raw_all_ok.store(all_ok, std::memory_order_relaxed);

                // Typed cross-check: nine consecutive one-slot reads (oop + 8 int),
                // each at its own base slot.  No long/double, so no accumulator
                // shift — the receiver must still be the FIRST template parameter.
                {
                    auto t = fr->get_arguments<vmhook::oop_t,
                                               std::int32_t, std::int32_t, std::int32_t,
                                               std::int32_t, std::int32_t, std::int32_t,
                                               std::int32_t, std::int32_t>();
                    const bool typed_ok{
                        std::get<1>(t) == MANY_1 && std::get<2>(t) == MANY_2 &&
                        std::get<3>(t) == MANY_3 && std::get<4>(t) == MANY_4 &&
                        std::get<6>(t) == MANY_6 && std::get<7>(t) == MANY_7 &&
                        std::get<8>(t) == MANY_8 };
                    g_many_typed_all_ok.store(typed_ok, std::memory_order_relaxed);
                }

                // set_arg at a HIGH but IN-RANGE logical slot (5).  This is a real
                // local of this 8-arg method, so the write must succeed and the raw
                // readback of locals[-5] must see the injected value — proving the
                // bounds guard accepts high-but-valid indices, not just slot 0/1.
                {
                    const bool set_ok{ ret.set_arg(5, MANY_5_INJECT) };
                    g_many_high_set_arg_ok.store(set_ok, std::memory_order_relaxed);
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 5, bits))
                    {
                        g_many_high_readback_ok.store(
                            static_cast<std::int32_t>(bits) == MANY_5_INJECT,
                            std::memory_order_relaxed);
                    }
                }

                // Bounds off-by-one: the FIRST out-of-range index is exactly
                // max_jvm_locals + 1 (0x10000).  set_arg shares get_argument's
                // `index > 0xFFFF` guard, so this must be rejected (return false,
                // no write, no crash) — the precise boundary the existing
                // 0x7FFFFFFF test steps far past.
                {
                    const bool rejected{ !ret.set_arg(FIRST_OOB_SLOT,
                                                      static_cast<std::int32_t>(0xBADBAD)) };
                    g_many_first_oob_rejected.store(rejected, std::memory_order_relaxed);
                }

                // The LAST in-range index (0xFFFF) is NOT rejected by the index
                // guard.  It is far past this method's real locals, so the write
                // targets an out-of-bounds address that write_slot's
                // is_readable_pointer / safe_write may legitimately refuse — hence
                // the RESULT is platform-variant and NOT asserted.  The contract
                // under test is only that the index guard itself does not reject
                // 0xFFFF the way it rejects 0x10000, AND that the call does not
                // crash.  We characterise the actual result via [INFO] below.
                {
                    const bool wrote{ ret.set_arg(MAX_JVM_LOCALS,
                                                  static_cast<std::int32_t>(0)) };
                    // "accepts" here means: reaching this line crash-free.  Whether
                    // the underlying write succeeded is recorded, not asserted.
                    g_many_last_inrange_accepts.store(true, std::memory_order_relaxed);
                    (void)wrote;
                }
            }) };
        ctx.check("frame_many_hook_installed", h_many.installed());

        // ── staticManyWide(long,double,long,int): three two-slot args, no this ─
        auto h_smw{ vmhook::scoped_hook<frame_raw_fixture>(
            "staticManyWide", "(JDJI)J",
            [](vmhook::return_value& ret,
               std::int64_t /*l0*/, double /*d*/, std::int64_t /*l1*/, std::int32_t /*tail*/)
            {
                g_smw_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_smw_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_smw_locals_nonnull.store(true, std::memory_order_relaxed);

                // NO this.  Three consecutive two-slot args then a tail int:
                //   l0 (long)   @ base slot 0 -> value@-(0+1) = locals[-1]
                //   d  (double) @ base slot 2 -> value@-(2+1) = locals[-3]
                //   l1 (long)   @ base slot 4 -> value@-(4+1) = locals[-5]
                //   tail (int)  @ base slot 6 -> locals[-6]
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_smw_raw_l0_ok.store(static_cast<std::int64_t>(bits) == SMW_L0,
                                              std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 3, bits))
                    {
                        g_smw_raw_d_ok.store(static_cast<std::int64_t>(bits) == SMW_D_BITS,
                                             std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 5, bits))
                    {
                        g_smw_raw_l1_ok.store(static_cast<std::int64_t>(bits) == SMW_L1,
                                              std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 6, bits))
                    {
                        g_smw_raw_t_ok.store(static_cast<std::int32_t>(bits) == SMW_T,
                                             std::memory_order_relaxed);
                    }
                }

                // Typed cross-check: NO this -> long@0, double@2, long@4, int@6,
                // the accumulator advancing +2 per wide from slot 0.
                {
                    auto [l0, d, l1, tail] =
                        fr->get_arguments<std::int64_t, double, std::int64_t, std::int32_t>();
                    std::int64_t d_bits{ 0 };
                    std::memcpy(&d_bits, &d, sizeof(d_bits));
                    g_smw_typed_ok.store(
                        l0 == SMW_L0 && d_bits == SMW_D_BITS && l1 == SMW_L1 && tail == SMW_T,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_static_many_wide_hook_installed", h_smw.installed());

        // ── instanceWideThenNarrow(long,int,double,int): interleave ─────────
        auto h_wtn{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceWideThenNarrow", "(JIDI)J",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               std::int64_t /*l*/, std::int32_t /*i*/, double /*d*/, std::int32_t /*tail*/)
            {
                g_wtn_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_wtn_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_wtn_locals_nonnull.store(true, std::memory_order_relaxed);

                // this@0, then: l (long) @ base slot 1 -> value@-2; i (int) @ slot
                // 3; d (double) @ base slot 4 -> value@-5; tail (int) @ slot 6.
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 2, bits))
                    {
                        g_wtn_raw_l_ok.store(static_cast<std::int64_t>(bits) == WTN_L,
                                             std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 3, bits))
                    {
                        g_wtn_raw_i_ok.store(static_cast<std::int32_t>(bits) == WTN_I,
                                             std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 5, bits))
                    {
                        g_wtn_raw_d_ok.store(static_cast<std::int64_t>(bits) == WTN_D_BITS,
                                             std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 6, bits))
                    {
                        g_wtn_raw_tail_ok.store(static_cast<std::int32_t>(bits) == WTN_TAIL,
                                                std::memory_order_relaxed);
                    }
                }

                // Typed cross-check: this(oop)@0, long@1(+2), int@3, double@4(+2),
                // int@6 — the accumulator must shift correctly across BOTH wides
                // with a narrow int sandwiched between them.
                {
                    auto [self_unused, l, i, d, tail] =
                        fr->get_arguments<vmhook::oop_t, std::int64_t, std::int32_t,
                                          double, std::int32_t>();
                    (void)self_unused;
                    std::int64_t d_bits{ 0 };
                    std::memcpy(&d_bits, &d, sizeof(d_bits));
                    g_wtn_typed_ok.store(
                        l == WTN_L && i == WTN_I && d_bits == WTN_D_BITS && tail == WTN_TAIL,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_wide_then_narrow_hook_installed", h_wtn.installed());

        // ── zeroRoundTrip(int): set_arg(1, 0) — the all-zero one-slot write ──
        auto h_zrt{ vmhook::scoped_hook<frame_raw_fixture>(
            "zeroRoundTrip", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               std::int32_t /*value*/)
            {
                g_zrt_calls.fetch_add(1, std::memory_order_relaxed);

                // Inject 0 into a slot whose original value (ZERO_RT_INPUT) is
                // non-zero, so a dropped write is detectable both raw and via the
                // body echo.  This is the all-zero-slot write the store path's
                // comment flags as a historical corruption class.
                const bool set_ok{ ret.set_arg(1, static_cast<std::int32_t>(0)) };
                g_zrt_set_arg_ok.store(set_ok, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                void** const locals{ fr->get_locals() };
                std::uint64_t bits{ 0 };
                if (read_raw_slot(locals, 1, bits))
                {
                    g_zrt_raw_readback_ok.store(
                        static_cast<std::int32_t>(bits) == 0,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_zero_round_trip_hook_installed", h_zrt.installed());

        // ── Drive every method once (one real bytecode dispatch each) ───────
        const bool done{ ctx.run_probe(
            [](bool value) { frame_raw_fixture::set_go(value); },
            []() { return frame_raw_fixture::get_done(); }) };
        ctx.check("frame_probe_completed", done);
        ctx.check("frame_probe_ticked", frame_raw_fixture::get_probe_ticks() >= 1);

        // ═════════════════════ instanceSimple assertions ════════════════════
        ctx.check("frame_simple_hook_fired", g_simple_calls.load() == 1);
        ctx.check("frame_simple_frame_nonnull", g_simple_frame_nonnull.load());
        ctx.check("frame_simple_get_method_nonnull", g_simple_method_nonnull.load());
        ctx.check("frame_simple_method_name_matches_hooked", g_simple_name_ok.load());
        ctx.check("frame_simple_method_sig_matches_hooked", g_simple_sig_ok.load());
        ctx.check("frame_simple_self_nonnull", g_simple_self_nonnull.load());
        ctx.check("frame_simple_get_locals_nonnull", g_simple_locals_nonnull.load());
        // The headline: slot 0 raw oop IS the receiver.
        ctx.check("frame_simple_slot0_oop_is_receiver", g_simple_slot0_oop_matches_self.load());
        ctx.check("frame_simple_slot0_tag_read_through_oop", g_simple_slot0_tag_ok.load());
        ctx.check("frame_simple_typed_arg0_is_receiver", g_simple_typed_arg0_matches_self.load());
        ctx.check("frame_simple_raw_slot1_matches_arg", g_simple_raw_slot1_ok.load());
        // Allow-through: original body ran (returns tag + x).  This ALSO proves
        // the out-of-range set_arg(0x7FFFFFFF, ...) below wrote nothing: had it
        // corrupted the live local array, the body's observation would differ.
        ctx.check("frame_simple_allow_through_body_ran",
                  frame_raw_fixture::get_simple_seen() == SIMPLE_X);
        // Bounds part 1: over-reading past the real locals did not crash the JVM.
        ctx.check("frame_simple_bounds_overread_no_crash", g_simple_bounds_no_crash.load());
        // Bounds part 2: a truly out-of-range index is rejected (default/no write).
        ctx.check("frame_simple_oob_index_rejected", g_simple_oob_index_rejected.load());
        ctx.record(std::string{ "[INFO] return_frame_raw_access bounds: over-read of slot 5 "
                                "past a 2-local frame returned " } +
                   std::to_string(g_simple_bounds_overread_value.load()) +
                   " (value undefined by contract; the guarantee under test is "
                   "no-crash, which held).");
        // Bounds part 3: a NEGATIVE index is rejected (the index<0 guard half).
        ctx.check("frame_simple_neg_index_rejected", g_simple_neg_index_rejected.load());
        // frame()->get_method() reports the method's KIND: instance => NOT static.
        // Universal invariant (the ACC_STATIC bit is fixed in the classfile); gate
        // only on whether the fault-safe flags read succeeded.
        ctx.check("frame_simple_static_flag_readable", g_simple_static_flag_readable.load());
        if (g_simple_static_flag_readable.load())
        {
            ctx.check("frame_simple_method_is_not_static", g_simple_not_static.load());
        }
        else
        {
            ctx.record("[INFO] return_frame_raw_access: instanceSimple Method._access_flags "
                       "not readable on this run (cold Method*); skipping the not-static "
                       "assertion (fault-safe path returned found=false).");
        }
        // caller() / stack_trace() off the live frame.  HARD: the call did not
        // crash (we reached here) and, WHEN a caller was identified, it is runAll.
        // The identification itself is JDK/JIT-variant (an inlined/compiled runAll
        // breaks the interpreter saved-rbp chain) -> [INFO]-gated.
        if (g_simple_caller_valid.load())
        {
            ctx.check("frame_simple_caller_is_runAll", g_simple_caller_is_runall.load());
            ctx.check("frame_simple_stacktrace_nonempty", g_simple_stacktrace_nonempty.load());
            ctx.check("frame_simple_stacktrace_head_is_caller",
                      g_simple_stacktrace_head_is_caller.load());
        }
        else
        {
            ctx.record("[INFO] return_frame_raw_access: caller() off the live frame did not "
                       "identify an interpreter caller (runAll JIT-compiled/inlined or chain "
                       "broke); caller()/stack_trace() ran crash-free, identification gated.");
        }

        // ═════════════════════ instanceWide assertions ══════════════════════
        ctx.check("frame_wide_hook_fired", g_wide_calls.load() == 1);
        ctx.check("frame_wide_frame_nonnull", g_wide_frame_nonnull.load());
        ctx.check("frame_wide_get_locals_nonnull", g_wide_locals_nonnull.load());
        ctx.check("frame_wide_slot0_oop_is_receiver", g_wide_self_ok.load());
        // Raw slot reads honouring the two-slot rule.
        ctx.check("frame_wide_raw_int_a_slot1", g_wide_raw_a_ok.load());
        ctx.check("frame_wide_raw_long_b_value_at_lower_slot", g_wide_raw_b_ok.load());
        ctx.check("frame_wide_raw_double_c_value_at_lower_slot", g_wide_raw_c_ok.load());
        ctx.check("frame_wide_raw_int_d_after_two_wides", g_wide_raw_d_ok.load());
        // Typed accessor cross-check (widening handled internally).
        ctx.check("frame_wide_typed_int_a", g_wide_typed_a_ok.load());
        ctx.check("frame_wide_typed_long_b", g_wide_typed_b_ok.load());
        ctx.check("frame_wide_typed_double_c", g_wide_typed_c_ok.load());
        ctx.check("frame_wide_typed_int_d", g_wide_typed_d_ok.load());
        // Allow-through: body observed every arg unmodified.
        ctx.check("frame_wide_body_saw_a", frame_raw_fixture::get_wide_a_seen() == WIDE_A);
        ctx.check("frame_wide_body_saw_b", frame_raw_fixture::get_wide_b_seen() == WIDE_B);
        {
            std::int64_t expected_bits{};
            std::memcpy(&expected_bits, &WIDE_C, sizeof(WIDE_C));
            ctx.check("frame_wide_body_saw_c",
                      frame_raw_fixture::get_wide_c_bits_seen() == expected_bits);
        }
        ctx.check("frame_wide_body_saw_d", frame_raw_fixture::get_wide_d_seen() == WIDE_D);

        // ═════════════════════ staticWide assertions ════════════════════════
        ctx.check("frame_static_hook_fired", g_static_calls.load() == 1);
        ctx.check("frame_static_frame_nonnull", g_static_frame_nonnull.load());
        ctx.check("frame_static_get_locals_nonnull", g_static_locals_nonnull.load());
        // The headline: a static method has NO `this` — slot 0 is the first int.
        ctx.check("frame_static_slot0_is_first_arg_not_this", g_static_slot0_is_first_int.load());
        ctx.check("frame_static_slot0_is_not_a_receiver_oop", g_static_slot0_not_receiver.load());
        ctx.check("frame_static_typed_args_decoded", g_static_typed_args_ok.load());
        // Allow-through on the static body.
        ctx.check("frame_static_body_saw_a", frame_raw_fixture::get_static_a_seen() == STATIC_A);
        ctx.check("frame_static_body_saw_b", frame_raw_fixture::get_static_b_seen() == STATIC_B);
        ctx.check("frame_static_body_saw_c", frame_raw_fixture::get_static_c_seen() == STATIC_C);
        // frame()->get_method() reports the method KIND: static => ACC_STATIC set.
        ctx.check("frame_static_static_flag_readable", g_static_static_flag_readable.load());
        if (g_static_static_flag_readable.load())
        {
            ctx.check("frame_static_method_is_static", g_static_is_static.load());
        }
        else
        {
            ctx.record("[INFO] return_frame_raw_access: staticWide Method._access_flags not "
                       "readable on this run (cold Method*); skipping the is-static assertion.");
        }

        // ═════════════════════ roundTrip assertions ═════════════════════════
        ctx.check("frame_round_trip_hook_fired", g_rt_calls.load() == 1);
        ctx.check("frame_round_trip_set_arg_returned_true", g_rt_set_arg_ok.load());
        // frame()'s locals reflect the set_arg write -> same array.
        ctx.check("frame_round_trip_raw_readback_sees_injected", g_rt_raw_readback_ok.load());
        // And the body observed the mutated value (allow-through after mutation).
        ctx.check("frame_round_trip_body_saw_injected",
                  frame_raw_fixture::get_round_trip_seen() == 0x4242);
        // frame() returns the same stash on a repeat call (same activation).
        ctx.check("frame_round_trip_frame_pointer_stable", g_rt_frame_stable.load());
        // get_arguments<>() with an empty pack is a safe no-op (empty tuple).
        ctx.check("frame_round_trip_empty_pack_is_noop", g_rt_empty_pack_ok.load());

        // ═════════════════════ instanceNarrow assertions ════════════════════
        ctx.check("frame_narrow_hook_fired", g_narrow_calls.load() == 1);
        ctx.check("frame_narrow_frame_nonnull", g_narrow_frame_nonnull.load());
        ctx.check("frame_narrow_get_locals_nonnull", g_narrow_locals_nonnull.load());
        // Five one-slot primitives at consecutive base slots 1..5 (no gaps).
        ctx.check("frame_narrow_raw_boolean_slot1", g_narrow_raw_z_ok.load());
        ctx.check("frame_narrow_raw_byte_slot2_sign_extended", g_narrow_raw_b_ok.load());
        ctx.check("frame_narrow_raw_char_slot3_zero_extended", g_narrow_raw_c_ok.load());
        ctx.check("frame_narrow_raw_short_slot4_sign_extended", g_narrow_raw_s_ok.load());
        ctx.check("frame_narrow_raw_int_slot5_min_value", g_narrow_raw_i_ok.load());
        ctx.check("frame_narrow_typed_all_widen_no_shift", g_narrow_typed_ok.load());
        // Allow-through: body saw each value after its widening conversion.
        ctx.check("frame_narrow_body_saw_z", frame_raw_fixture::get_narrow_z_seen() == NARROW_Z_INT);
        ctx.check("frame_narrow_body_saw_b", frame_raw_fixture::get_narrow_b_seen() == NARROW_B_INT);
        ctx.check("frame_narrow_body_saw_c", frame_raw_fixture::get_narrow_c_seen() == NARROW_C_INT);
        ctx.check("frame_narrow_body_saw_s", frame_raw_fixture::get_narrow_s_seen() == NARROW_S_INT);
        ctx.check("frame_narrow_body_saw_i", frame_raw_fixture::get_narrow_i_seen() == NARROW_I_INT);

        // ═════════════════════ instanceFloat assertions ═════════════════════
        ctx.check("frame_float_hook_fired", g_float_calls.load() == 1);
        ctx.check("frame_float_frame_nonnull", g_float_frame_nonnull.load());
        ctx.check("frame_float_get_locals_nonnull", g_float_locals_nonnull.load());
        // float is ONE slot: f@1, tail@2 (NOT @3).
        ctx.check("frame_float_raw_float_slot1", g_float_raw_f_ok.load());
        ctx.check("frame_float_raw_tail_int_slot2_not_shifted", g_float_raw_tail_ok.load());
        ctx.check("frame_float_typed_float_then_int", g_float_typed_ok.load());
        // Allow-through.
        ctx.check("frame_float_body_saw_f_bits",
                  frame_raw_fixture::get_float_f_bits_seen() == (FLOAT_F_BITS & 0xFFFFFFFFLL));
        ctx.check("frame_float_body_saw_tail",
                  frame_raw_fixture::get_float_tail_seen() == FLOAT_TAIL);

        // ═════════════════════ instanceEdgeWide assertions ══════════════════
        ctx.check("frame_edge_hook_fired", g_edge_calls.load() == 1);
        ctx.check("frame_edge_frame_nonnull", g_edge_frame_nonnull.load());
        ctx.check("frame_edge_get_locals_nonnull", g_edge_locals_nonnull.load());
        // Two consecutive two-slot reads: Long.MIN then a NaN double (by bits).
        ctx.check("frame_edge_raw_long_min_value_at_lower_slot", g_edge_raw_l_ok.load());
        ctx.check("frame_edge_raw_nan_double_bits_at_lower_slot", g_edge_raw_d_ok.load());
        ctx.check("frame_edge_typed_long_and_double", g_edge_typed_ok.load());
        // Allow-through.
        ctx.check("frame_edge_body_saw_long_min", frame_raw_fixture::get_edge_l_seen() == EDGE_LMIN);
        ctx.check("frame_edge_body_saw_nan_bits",
                  frame_raw_fixture::get_edge_d_bits_seen() == EDGE_DNAN_BITS);

        // ═════════════════════ staticLeadingLong assertions ═════════════════
        ctx.check("frame_slf_hook_fired", g_slf_calls.load() == 1);
        ctx.check("frame_slf_frame_nonnull", g_slf_frame_nonnull.load());
        ctx.check("frame_slf_get_locals_nonnull", g_slf_locals_nonnull.load());
        // No this: a long sits at base slot 0 (value at lower slot locals[-1]).
        ctx.check("frame_slf_raw_leading_long_at_slot0_no_this", g_slf_raw_l0_ok.load());
        ctx.check("frame_slf_raw_tail_int_at_slot2", g_slf_raw_tail_ok.load());
        ctx.check("frame_slf_typed_long_then_int", g_slf_typed_ok.load());
        // Allow-through.
        ctx.check("frame_slf_body_saw_long", frame_raw_fixture::get_slf_l0_seen() == SLF_L0);
        ctx.check("frame_slf_body_saw_tail", frame_raw_fixture::get_slf_tail_seen() == SLF_TAIL);

        // ═════════════════════ wideRoundTrip assertions ═════════════════════
        ctx.check("frame_wide_round_trip_hook_fired", g_wrt_calls.load() == 1);
        ctx.check("frame_wide_round_trip_set_arg_returned_true", g_wrt_set_arg_ok.load());
        // set_arg of a long lands at the LOWER slot — the raw read of locals[-2]
        // sees the injected 64-bit value.
        ctx.check("frame_wide_round_trip_raw_readback_lower_slot", g_wrt_raw_readback_ok.load());
        // And the body observed the injected long (allow-through after wide mutation).
        ctx.check("frame_wide_round_trip_body_saw_injected",
                  frame_raw_fixture::get_wide_rt_seen() == WIDE_RT_INJECT);

        // ═════════════════════ instanceManyLocals assertions ════════════════
        ctx.check("frame_many_hook_fired", g_many_calls.load() == 1);
        ctx.check("frame_many_frame_nonnull", g_many_frame_nonnull.load());
        ctx.check("frame_many_get_locals_nonnull", g_many_locals_nonnull.load());
        ctx.check("frame_many_slot0_oop_is_receiver", g_many_self_ok.load());
        // Eight one-slot ints walked at consecutive base slots 1..8 (slot 5
        // excluded — mutated below).  Deep no-gap one-slot indexing.
        ctx.check("frame_many_raw_all_one_slot_ints_match", g_many_raw_all_ok.load());
        ctx.check("frame_many_typed_nine_consecutive_slots", g_many_typed_all_ok.load());
        // set_arg at a HIGH but IN-RANGE logical slot (5) succeeds and the raw
        // readback of locals[-5] sees the injected value: the bounds guard accepts
        // high-but-valid indices, not just slot 0/1.
        ctx.check("frame_many_high_slot_set_arg_succeeded", g_many_high_set_arg_ok.load());
        if (g_many_high_set_arg_ok.load())
        {
            ctx.check("frame_many_high_slot_raw_readback_sees_injected",
                      g_many_high_readback_ok.load());
            // Allow-through: the body's slot-5 echo is the injected value.
            ctx.check("frame_many_body_saw_injected_m5",
                      frame_raw_fixture::get_many5_seen() == MANY_5_INJECT);
        }
        else
        {
            ctx.record("[INFO] return_frame_raw_access: set_arg(5,...) on instanceManyLocals "
                       "did not land on this run (cold/relocated frame -> safe no-op); "
                       "high-slot readback and body echo gated.");
        }
        // Bounds off-by-one: 0x10000 (max_jvm_locals+1) is the FIRST rejected
        // index — the precise boundary the 0x7FFFFFFF test steps far past.  HARD:
        // the index guard is a fixed compile-time constant, not frame-dependent.
        ctx.check("frame_many_first_oob_index_0x10000_rejected", g_many_first_oob_rejected.load());
        // 0xFFFF is the LAST in-range index: the index guard does NOT reject it
        // (only the address-level write may refuse).  HARD: reaching here proves
        // the in-range path ran crash-free; the write RESULT is platform-variant.
        ctx.check("frame_many_last_inrange_index_0xFFFF_no_crash", g_many_last_inrange_accepts.load());
        // Allow-through on the un-mutated deep slots (XOR excludes slot 5).
        ctx.check("frame_many_body_saw_m8", frame_raw_fixture::get_many8_seen() == MANY_8);
        ctx.check("frame_many_body_xor_excluding_m5",
                  frame_raw_fixture::get_many_sum_seen() ==
                      (MANY_1 ^ MANY_2 ^ MANY_3 ^ MANY_4 ^ MANY_6 ^ MANY_7 ^ MANY_8));

        // ═════════════════════ staticManyWide assertions ════════════════════
        ctx.check("frame_smw_hook_fired", g_smw_calls.load() == 1);
        ctx.check("frame_smw_frame_nonnull", g_smw_frame_nonnull.load());
        ctx.check("frame_smw_get_locals_nonnull", g_smw_locals_nonnull.load());
        // NO this: three consecutive two-slot args from slot 0, then a tail int.
        ctx.check("frame_smw_raw_long0_value_at_slot0", g_smw_raw_l0_ok.load());
        ctx.check("frame_smw_raw_double_value_at_slot2", g_smw_raw_d_ok.load());
        ctx.check("frame_smw_raw_long1_value_at_slot4", g_smw_raw_l1_ok.load());
        ctx.check("frame_smw_raw_tail_int_at_slot6", g_smw_raw_t_ok.load());
        ctx.check("frame_smw_typed_three_wides_then_int", g_smw_typed_ok.load());
        // Allow-through.
        ctx.check("frame_smw_body_saw_l0", frame_raw_fixture::get_smw_l0_seen() == SMW_L0);
        ctx.check("frame_smw_body_saw_d_bits", frame_raw_fixture::get_smw_d_bits_seen() == SMW_D_BITS);
        ctx.check("frame_smw_body_saw_l1", frame_raw_fixture::get_smw_l1_seen() == SMW_L1);
        ctx.check("frame_smw_body_saw_tail", frame_raw_fixture::get_smw_t_seen() == SMW_T);

        // ═════════════════════ instanceWideThenNarrow assertions ════════════
        ctx.check("frame_wtn_hook_fired", g_wtn_calls.load() == 1);
        ctx.check("frame_wtn_frame_nonnull", g_wtn_frame_nonnull.load());
        ctx.check("frame_wtn_get_locals_nonnull", g_wtn_locals_nonnull.load());
        // long,int,double,int interleave (this@0): the accumulator must shift
        // correctly across BOTH wides with a narrow int sandwiched between them.
        ctx.check("frame_wtn_raw_long_value_at_lower_slot", g_wtn_raw_l_ok.load());
        ctx.check("frame_wtn_raw_int_between_wides_slot3", g_wtn_raw_i_ok.load());
        ctx.check("frame_wtn_raw_double_value_at_lower_slot", g_wtn_raw_d_ok.load());
        ctx.check("frame_wtn_raw_tail_int_after_two_wides", g_wtn_raw_tail_ok.load());
        ctx.check("frame_wtn_typed_interleaved_slot_model", g_wtn_typed_ok.load());
        // Allow-through.
        ctx.check("frame_wtn_body_saw_l", frame_raw_fixture::get_wtn_l_seen() == WTN_L);
        ctx.check("frame_wtn_body_saw_i", frame_raw_fixture::get_wtn_i_seen() == WTN_I);
        ctx.check("frame_wtn_body_saw_d_bits", frame_raw_fixture::get_wtn_d_bits_seen() == WTN_D_BITS);
        ctx.check("frame_wtn_body_saw_tail", frame_raw_fixture::get_wtn_tail_seen() == WTN_TAIL);

        // ═════════════════════ zeroRoundTrip assertions ═════════════════════
        ctx.check("frame_zero_round_trip_hook_fired", g_zrt_calls.load() == 1);
        ctx.check("frame_zero_round_trip_set_arg_returned_true", g_zrt_set_arg_ok.load());
        if (g_zrt_set_arg_ok.load())
        {
            // The all-zero one-slot write landed: raw readback of locals[-1] is 0.
            ctx.check("frame_zero_round_trip_raw_readback_is_zero", g_zrt_raw_readback_ok.load());
            // And the body observed 0 (a zero write is neither dropped nor a no-op).
            ctx.check("frame_zero_round_trip_body_saw_zero",
                      frame_raw_fixture::get_zero_rt_seen() == 0);
        }
        else
        {
            ctx.record("[INFO] return_frame_raw_access: set_arg(1, 0) on zeroRoundTrip did not "
                       "land on this run (cold/relocated frame -> safe no-op); zero-write "
                       "readback and body echo gated.");
        }

        // ── INFO: surface the live raw-frame state for the run log ──────────
        {
            std::ostringstream oss;
            oss << "[INFO] return_frame_raw_access: frame() non-null on instance="
                << (g_simple_frame_nonnull.load() ? "yes" : "no")
                << " static=" << (g_static_frame_nonnull.load() ? "yes" : "no")
                << "; get_locals() non-null instance="
                << (g_simple_locals_nonnull.load() ? "yes" : "no")
                << " static=" << (g_static_locals_nonnull.load() ? "yes" : "no")
                << "; slot0==receiver(simple)="
                << (g_simple_slot0_oop_matches_self.load() ? "yes" : "no")
                << "; static-slot0-has-no-this="
                << (g_static_slot0_is_first_int.load() ? "yes" : "no")
                << "; many-locals deep one-slot walk="
                << (g_many_raw_all_ok.load() ? "yes" : "no")
                << "; static three-wides="
                << (g_smw_typed_ok.load() ? "yes" : "no")
                << "; wide/narrow interleave="
                << (g_wtn_typed_ok.load() ? "yes" : "no") << ".";
            ctx.record(oss.str());
        }

        // INFO: characterise the platform-variant high-index write outcomes that
        // are deliberately NOT asserted (only the index-guard boundary is HARD).
        {
            std::ostringstream oss;
            oss << "[INFO] return_frame_raw_access bounds boundary: set_arg(0x10000) "
                << "rejected=" << (g_many_first_oob_rejected.load() ? "yes" : "no")
                << " (HARD); set_arg(0xFFFF) ran crash-free="
                << (g_many_last_inrange_accepts.load() ? "yes" : "no")
                << " (the write itself targets an out-of-bounds address whose "
                   "is_readable_pointer/safe_write result is platform-variant and "
                   "intentionally unasserted).";
            ctx.record(oss.str());
        }
    }
}
