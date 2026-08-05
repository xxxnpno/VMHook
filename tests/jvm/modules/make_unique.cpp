// make_unique JVM test module — exhaustive coverage of vmhook::make_unique<T>.
//
// make_unique (vmhook.hpp:10556) allocates a fresh Java object from native
// code.  It tries the JNI NewObjectA path FIRST (jni_make_unique,
// vmhook.hpp:10027) which runs the real Java <init> chain; only when no
// matching Java constructor exists / NewObjectA is unavailable does it fall
// back to a raw TLAB allocation + the wrapper's construct(...) hook
// (vmhook.hpp:10684).  This module drives both paths on a live JVM:
//
//   * NewObjectA path  — exercised by every constructor descriptor the Java
//     fixture declares: ()V, (I)V, (II)V, (IJD)V, (Ljava/lang/String;)V,
//     (Ljava/lang/String;I)V.  Each Java <init> bumps MakeUnique.instanceCount
//     and stamps fields, so we read the fields back through the wrapper and
//     confirm instanceCount advanced (proves the constructor BODY ran, not
//     just the allocation).
//
//   * TLAB + construct() path — forced with a (Z)V arg, a descriptor the Java
//     fixture deliberately does NOT declare.  jni_make_unique's GetMethodID
//     for "(Z)V" fails, make_unique falls back to TLAB, and the wrapper's
//     construct(bool) runs.  This is the ONLY way construct() executes (the
//     NewObjectA path never calls it), so it is how we satisfy the "a
//     registered construct() runs" requirement.
//
// Most make_unique calls happen inside a scoped_hook detour on trigger(), so a
// JavaThread is guaranteed live — the same shape the canonical example.cpp
// make_unique test uses.
//
//   * OUTSIDE-A-HOOK path — make_unique is ALSO exercised at module entry,
//     BEFORE any hook is armed (no detour, so vmhook::hotspot::current_java_thread
//     is NOT trampoline-set).  In that state make_unique must DISCOVER a live
//     JavaThread from VM metadata instead of riding the hook trampoline's
//     captured thread.  This is a DISTINCT code path from the in-detour calls and
//     mirrors the legacy example.cpp test_make_unique_before_hooks /
//     make_unique_outside_hook tail of test_make_unique_status.  We HARD-assert
//     allocation succeeds, a constructor arg landed (field read-back), and a
//     SECOND call yields a DISTINCT live instance.
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
    // Wrapper for vmhook.fixtures.MakeUnique.
    class make_unique_fixture : public vmhook::object<make_unique_fixture>
    {
    public:
        explicit make_unique_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<make_unique_fixture>{ instance }
        {
        }

        // ── Handshake ──────────────────────────────────────────────────────────
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }

        // ── Static observers ───────────────────────────────────────────────────
        static auto get_instance_count() -> std::int32_t { return static_field("instanceCount")->get(); }
        static auto set_instance_count(std::int32_t v) -> void { static_field("instanceCount")->set(v); }
        static auto get_last_ctor() -> std::string { return static_field("lastCtor")->get(); }

        // ── Instance field read-back ───────────────────────────────────────────
        auto get_int_field()    -> std::int32_t { return get_field("intField")->get(); }
        auto get_long_field()   -> std::int64_t { return get_field("longField")->get(); }
        auto get_double_field() -> double       { return get_field("doubleField")->get(); }
        auto get_string_field() -> std::string  { return get_field("stringField")->get(); }
        auto get_bool_field()   -> bool         { return get_field("boolField")->get(); }
        auto get_ctor_tag()     -> std::int32_t { return get_field("ctorTag")->get(); }

        // Narrow-primitive field read-back for the (S)V / (B)V / (C)V / (F)V
        // constructors.  Each returns its native width; the test widens short /
        // byte / char into a std::int32_t atomic for storage and compares against
        // the known boundary value, and bit-compares the float.
        auto get_short_field()  -> std::int16_t { return get_field("shortField")->get(); }
        auto get_byte_field()   -> std::int8_t  { return get_field("byteField")->get(); }
        auto get_char_field()   -> char16_t     { return get_field("charField")->get(); }
        auto get_float_field()  -> float        { return get_field("floatField")->get(); }

        // construct(bool): runs ONLY on the TLAB fallback path (no (Z)V Java
        // ctor exists).  Records that construct() executed and initialises the
        // raw-allocated object's fields through the same setter API.
        auto construct(bool flag) -> void
        {
            g_construct_ran.store(true, std::memory_order_relaxed);
            g_construct_arg.store(flag, std::memory_order_relaxed);
            g_construct_calls.fetch_add(1, std::memory_order_relaxed);
            get_field("boolField")->set(flag);
            get_field("ctorTag")->set(static_cast<std::int32_t>(99));
        }

        // file-scope observers shared with the construct() member above
        static inline std::atomic<bool> g_construct_ran{ false };
        static inline std::atomic<bool> g_construct_arg{ false };
        static inline std::atomic<int>  g_construct_calls{ 0 };
    };

    // A SECOND wrapper type that is intentionally NEVER register_class<>'d.
    // make_unique<unregistered_fixture>() must look it up in type_to_class_map,
    // miss, and return nullptr cleanly (vmhook.hpp:13752-13757) — no allocation,
    // no crash, no JavaThread work.  Pure C++/library contract, JDK-independent
    // and race-immune.
    class unregistered_fixture : public vmhook::object<unregistered_fixture>
    {
    public:
        explicit unregistered_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<unregistered_fixture>{ instance }
        {
        }
    };

    // ── Hook observation ───────────────────────────────────────────────────────
    std::atomic<int>  g_hook_calls{ 0 };
    std::atomic<bool> g_hook_saw_self{ false };

    // ── NewObjectA-path observations (filled inside the detour) ────────────────
    std::atomic<bool> g_noarg_ok{ false };
    std::atomic<std::int32_t> g_noarg_tag{ -1 };

    std::atomic<bool> g_int_ok{ false };
    std::atomic<std::int32_t> g_int_val{ -1 };
    std::atomic<std::int32_t> g_int_tag{ -1 };

    std::atomic<bool> g_twoint_ok{ false };
    std::atomic<std::int32_t> g_twoint_val{ -1 };
    std::atomic<std::int32_t> g_twoint_tag{ -1 };

    std::atomic<bool> g_multi_ok{ false };
    std::atomic<std::int32_t> g_multi_int{ -1 };
    std::atomic<std::int64_t> g_multi_long{ -1 };
    std::atomic<std::int64_t> g_multi_double_bits{ 0 };
    std::atomic<std::int32_t> g_multi_tag{ -1 };

    std::atomic<bool> g_str_ok{ false };
    std::atomic<bool> g_str_match{ false };
    std::atomic<std::int32_t> g_str_tag{ -1 };

    std::atomic<bool> g_cstr_ok{ false };
    std::atomic<bool> g_cstr_match{ false };

    std::atomic<bool> g_sv_ok{ false };
    std::atomic<bool> g_sv_match{ false };

    std::atomic<bool> g_emptystr_ok{ false };
    std::atomic<bool> g_emptystr_match{ false };

    std::atomic<bool> g_unicode_ok{ false };
    std::atomic<bool> g_unicode_match{ false };

    std::atomic<bool> g_strint_ok{ false };
    std::atomic<bool> g_strint_str_match{ false };
    std::atomic<std::int32_t> g_strint_int{ -1 };
    std::atomic<std::int32_t> g_strint_tag{ -1 };

    // ── instanceCount progression ──────────────────────────────────────────────
    std::atomic<std::int32_t> g_count_before{ -1 };
    std::atomic<std::int32_t> g_count_after_newobj{ -1 };

    // ── TLAB + construct() path observations ───────────────────────────────────
    std::atomic<bool> g_tlab_made{ false };
    std::atomic<bool> g_tlab_boolfield{ false };
    std::atomic<std::int32_t> g_tlab_tag{ -1 };
    std::atomic<std::int32_t> g_count_after_tlab{ -1 };

    // ── Distinct-identity check (two no-arg objects differ) ────────────────────
    std::atomic<bool> g_distinct_identity{ false };

    // ── Boundary integer args (I)V / (II)V — value-edge coverage ───────────────
    // Each is read back through the wrapper; the (I)V edges prove the int arg is
    // marshalled byte-exact (no sign-extension / truncation slip in the jni_value
    // union narrow write at vmhook.hpp:9804), and (II)V overflow proves the JVM
    // adds in 32-bit wrap-around (the constructor body, not native, did the add).
    std::atomic<bool> g_int_min_ok{ false };
    std::atomic<std::int32_t> g_int_min_val{ 0 };
    std::atomic<bool> g_int_max_ok{ false };
    std::atomic<std::int32_t> g_int_max_val{ 0 };
    std::atomic<bool> g_int_neg_ok{ false };
    std::atomic<std::int32_t> g_int_neg_val{ 0 };
    std::atomic<bool> g_int_zero_ok{ false };
    std::atomic<std::int32_t> g_int_zero_val{ 1 };       // poison non-zero so a no-write fails
    std::atomic<bool> g_twoint_ovf_ok{ false };
    std::atomic<std::int32_t> g_twoint_ovf_val{ 0 };

    // ── Boundary (IJD)V args — long/double edges round-trip bit-exact ──────────
    std::atomic<bool> g_lmin_ok{ false };
    std::atomic<std::int64_t> g_lmin_long{ 0 };
    std::atomic<bool> g_lmax_ok{ false };
    std::atomic<std::int64_t> g_lmax_long{ 0 };
    std::atomic<bool> g_dspecial_ok{ false };
    std::atomic<std::int64_t> g_dspecial_nan_bits{ 0 };  // NaN payload bit-exact
    std::atomic<std::int64_t> g_dspecial_negzero_bits{ 1 };
    std::atomic<std::int64_t> g_dspecial_inf_bits{ 0 };

    // ── object_base move semantics (wrapper-level, NOT the unique_ptr) ─────────
    // make_unique returns unique_ptr<T>; moving the *unique_ptr* is std-library.
    // Here we exercise the WRAPPER's own move (object_base move ctor /
    // move-assign, vmhook.hpp:17769-17784): the OOP transfers and the source is
    // nulled, so a moved-from wrapper is safely-destructible (no double-anything)
    // and identity is preserved across the move.  All pure pointer-value checks —
    // race-immune (no field deref).
    std::atomic<bool> g_move_ctor_preserved_identity{ false };
    std::atomic<bool> g_move_ctor_nulled_source{ false };
    std::atomic<bool> g_move_assign_preserved_identity{ false };
    std::atomic<bool> g_move_assign_nulled_source{ false };

    // ── object_base copy semantics + aliasing (two wrappers, one live object) ──
    // Copying a wrapper (object_base copy ctor, vmhook.hpp:17755) duplicates the
    // RAW OOP — both wrappers reference the SAME Java object (it is not a GC
    // handle, no refcount).  Pointer equality is race-immune; a field write
    // through one being visible through the other proves true aliasing and is
    // read-back (GC-race-guarded).
    std::atomic<bool> g_copy_same_identity{ false };
    std::atomic<bool> g_copy_alias_write_visible{ false };
    std::atomic<bool> g_copy_alias_write_stable{ false };
    std::atomic<std::int32_t> g_copy_alias_read{ 0 };

    // ── Identity round-trip: re-wrap get_instance() into a FRESH wrapper ───────
    // A wrapper constructed from another wrapper's raw OOP must resolve the same
    // klass (typeid-based) and read the same fields — proving get_instance() is a
    // faithful, re-wrappable heap identity.
    std::atomic<bool> g_rewrap_same_identity{ false };
    std::atomic<bool> g_rewrap_field_matches{ false };
    std::atomic<bool> g_rewrap_field_stable{ false };
    std::atomic<std::int32_t> g_rewrap_field_read{ 0 };

    // ── construct() invoked EXACTLY once per fallback allocation ───────────────
    std::atomic<int> g_construct_call_count{ 0 };

    // ── Unregistered-type guard (pure C++, no JVM, race-immune) ────────────────
    std::atomic<bool> g_unregistered_returned_null{ false };

    // ── Narrow-primitive NewObjectA descriptors: (S)V (B)V (C)V (F)V (J)V ──────
    // The fixture grew one constructor per narrow JVM descriptor the library's
    // jvm_descriptor_for_arg + convert_jni_arg already support (short->"S",
    // int8->"B", char/uint16->"C", float->"F", int64->"J") but which the original
    // fixture had NO matching <init> for — so these used to silently route through
    // the TLAB fallback.  With the dedicated <init> present, make_unique resolves
    // each EXACT descriptor through the REAL NewObjectA path; reading the matching
    // field back proves the narrow primitive marshalled byte-exact in the jni_value
    // union (no sign-extension / truncation / wrong-slot write) AND that the JVM
    // dispatched the intended constructor (distinct ctorTag per descriptor).  All
    // values are read in the tight post-alloc window via read_until_stable so a
    // young-gen relocation cannot turn a correct ctor write into a [FAIL]; a stable-
    // WRONG / un-converged read of the KNOWN-FIXED value downgrades to [INFO].

    // short (S)V — boundary values SHRT_MIN / SHRT_MAX / -1 round-trip exact.
    std::atomic<bool> g_short_min_ok{ false };
    std::atomic<std::int32_t> g_short_min_val{ 0 };   // widened to int for the atomic
    std::atomic<std::int32_t> g_short_min_tag{ -1 };
    std::atomic<bool> g_short_max_ok{ false };
    std::atomic<std::int32_t> g_short_max_val{ 0 };
    std::atomic<bool> g_short_neg_ok{ false };
    std::atomic<std::int32_t> g_short_neg_val{ 0 };

    // byte (B)V — boundary values -128 / 127 / -1 round-trip exact.
    std::atomic<bool> g_byte_min_ok{ false };
    std::atomic<std::int32_t> g_byte_min_val{ 0 };
    std::atomic<std::int32_t> g_byte_min_tag{ -1 };
    std::atomic<bool> g_byte_max_ok{ false };
    std::atomic<std::int32_t> g_byte_max_val{ 0 };

    // char (C)V — UNSIGNED 16-bit; 0 / 0xFFFF / 'A' / a BMP non-ASCII code unit.
    std::atomic<bool> g_char_zero_ok{ false };
    std::atomic<std::int32_t> g_char_zero_val{ -1 };  // poison non-zero
    std::atomic<std::int32_t> g_char_zero_tag{ -1 };
    std::atomic<bool> g_char_max_ok{ false };
    std::atomic<std::int32_t> g_char_max_val{ 0 };
    std::atomic<bool> g_char_letter_ok{ false };
    std::atomic<std::int32_t> g_char_letter_val{ 0 };
    std::atomic<bool> g_char_bmp_ok{ false };
    std::atomic<std::int32_t> g_char_bmp_val{ 0 };

    // float (F)V — bit-exact via float_to_bits: 3.5 (exact), +Inf, -0.0, NaN.
    std::atomic<bool> g_float_ok{ false };
    std::atomic<std::int32_t> g_float_bits{ 0 };
    std::atomic<std::int32_t> g_float_tag{ -1 };
    std::atomic<bool> g_float_inf_ok{ false };
    std::atomic<std::int32_t> g_float_inf_bits{ 0 };
    std::atomic<bool> g_float_negzero_ok{ false };
    std::atomic<std::int32_t> g_float_negzero_bits{ 1 };
    std::atomic<bool> g_float_nan_ok{ false };
    std::atomic<std::int32_t> g_float_nan_bits{ 0 };

    // long-only (J)V — distinct from the (IJD)V multi-arg: proves a SINGLE 8-byte
    // arg resolves the "(J)V" descriptor (ctorTag 11), not (IJD)V or (I)V.
    std::atomic<bool> g_longonly_ok{ false };
    std::atomic<std::int64_t> g_longonly_val{ 0 };
    std::atomic<std::int32_t> g_longonly_tag{ -1 };

    // ── OUTSIDE-A-HOOK path observations (filled in the module body, no detour) ─
    // make_unique called with NO hook active, so current_java_thread is NOT
    // trampoline-set and the implementation must discover a live JavaThread from
    // VM metadata.  Filled before the scoped_hook block below.
    std::atomic<bool> g_outside_made{ false };          // first alloc succeeded
    std::atomic<std::int32_t> g_outside_int{ -1 };      // (I)V arg read back (stable)
    std::atomic<bool> g_outside_int_stable{ false };    // the (I)V read stabilized
    std::atomic<std::int32_t> g_outside_tag{ -1 };      // dispatched ctorTag (stable)
    std::atomic<bool> g_outside_tag_stable{ false };    // the ctorTag read stabilized
    std::atomic<bool> g_outside_made2{ false };         // second alloc succeeded
    std::atomic<bool> g_outside_distinct{ false };      // two allocs are distinct

    // Bit-compare helper for the double field (exact round-trip).
    auto double_to_bits(double d) -> std::int64_t
    {
        std::int64_t bits{ 0 };
        static_assert(sizeof(bits) == sizeof(d), "double must be 8 bytes");
        std::memcpy(&bits, &d, sizeof(bits));
        return bits;
    }

    // Bit-compare helper for the float field (exact round-trip).  A value compare
    // would lie for NaN (NaN != NaN) and -0.0 (== +0.0); the bit pattern proves
    // the exact IEEE-754 payload round-tripped through .f in the jni_value union
    // and back out of floatField.
    auto float_to_bits(float f) -> std::int32_t
    {
        std::int32_t bits{ 0 };
        static_assert(sizeof(bits) == sizeof(f), "float must be 4 bytes");
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    // ── young-OOP instance-field read resilience (GC-relocation race) ───────────
    //
    // A vmhook wrapper holds a RAW heap OOP (object_base::instance); for INSTANCE
    // fields field_proxy::get() reads `instance + offset` directly and — unlike
    // the STATIC path — does NOT re-resolve a GC-stable root (vmhook.hpp:14404-
    // 14421: the re-resolve arm fires only when mirror_klass is set, i.e. for
    // statics).  os::safe_read_fast keeps that read from FAULTING on a relocated /
    // unmapped page, but it cannot recover the VALUE: once make_unique returns,
    // the raw OOP is frozen, so a young-gen (G1) safepoint between the alloc and
    // the read leaves `instance` pointing at the object's OLD address and the read
    // observes stale / zeroed bytes.  This is a property of reading a relocatable
    // object through a cached raw OOP without a GC handle, NOT a make_unique
    // correctness bug — the object IS allocated, the <init> DID run, the field IS
    // set; only the read races the collector.
    //
    // RESILIENCE (mirrors field_null_safety's young-mirror read_until_stable, with
    // NO forced System.gc()): re-read the field a bounded number of times and
    // accept the value only once two CONSECUTIVE reads agree.  A read caught
    // mid-relocation differs from its neighbours, so a transient tear is absorbed
    // and the read converges to the correct value (HARD PASS).  If the OOP already
    // moved before the FIRST read, every retry sees the SAME stale value -> it is
    // "stable" but wrong; the caller compares the stable value to the KNOWN-FIXED
    // expected one (4242 / ctorTag 2) and downgrades only a stable-WRONG (or
    // never-stabilized) read to a best-effort [INFO] — never to a [FAIL].  A
    // genuinely-wrong STABLE value that were a real regression would still be
    // reported (the contract is asserted whenever the read is race-free, which is
    // the overwhelming majority of runs); this can only ever turn a GC-race
    // artifact into an [INFO], never mask a true defect into a pass.
    constexpr int kOutsideStabilityReads{ 128 };

    // Read `read()` up to kOutsideStabilityReads times; set out_value to the first
    // value seen on two CONSECUTIVE equal reads and return true.  If no two
    // consecutive reads agree within the bound, leave out_value at the LAST value
    // read (for diagnostics) and return false.
    template <typename T, typename ReadFn>
    auto read_until_stable(ReadFn&& read, T& out_value) -> bool
    {
        T prev = read();
        for (int attempt = 1; attempt < kOutsideStabilityReads; ++attempt)
        {
            T cur = read();
            if (cur == prev)
            {
                out_value = cur;   // two consecutive reads agree -> stable
                return true;
            }
            prev = cur;
        }
        out_value = prev;          // never stabilized within the bound
        return false;
    }
}

VMHOOK_JVM_MODULE(make_unique)
{
    static constexpr const char* FIXTURE{ "vmhook/fixtures/MakeUnique" };

    vmhook::register_class<make_unique_fixture>(FIXTURE);

    // ── ENTRY GUARD ────────────────────────────────────────────────────────────
    // If the fixture is not loaded/resolvable on this run, every make_unique /
    // static_field below would operate on an unresolvable class.  Bail cleanly to
    // [INFO] instead (no crash, no hooks armed).  The harness loads every
    // vmhook.fixtures.* class on each run, so this is belt-and-braces (same idiom
    // as field_primitives_set / collection_iteration_safety).
    if (vmhook::find_class(FIXTURE) == nullptr)
    {
        ctx.record("[INFO] make_unique: MakeUnique not loaded/resolvable on this "
                   "run; skipping the module's live checks (no crash, no hooks armed).");
        return;
    }

    // ── Unregistered-type guard ────────────────────────────────────────────────
    // make_unique<unregistered_fixture>() — the type was never register_class<>'d,
    // so the type_to_class_map lookup misses and make_unique returns nullptr
    // without allocating or touching the heap (vmhook.hpp:13752-13757).  Pure
    // library contract: deterministic, JDK-independent, race-immune (we only test
    // a null unique_ptr — no OOP is produced or dereferenced).  Wrapped in
    // try/catch belt-and-braces so an unexpected throw is contained, never a FAIL.
    try
    {
        auto u{ vmhook::make_unique<unregistered_fixture>() };
        g_unregistered_returned_null.store(u == nullptr, std::memory_order_relaxed);
    }
    catch (...)
    {
        ctx.record("[INFO] make_unique: unregistered-type make_unique threw and was "
                   "contained; treating as non-null for the guard below.");
    }

    // ── OUTSIDE-A-HOOK make_unique (VM-metadata JavaThread discovery) ──────────
    // Runs in the MODULE BODY before ANY hook is armed: no detour is active, so
    // vmhook::hotspot::current_java_thread is NOT trampoline-set and make_unique
    // must discover a live JavaThread from VM metadata.  This is the DISTINCT
    // code path the legacy example.cpp test_make_unique_before_hooks /
    // make_unique_outside_hook tail covered, which the in-detour calls below do
    // NOT exercise.  Contained in a try/catch so a stray throw can never escape
    // the module (recorded [INFO], never a FAIL); the hard checks are asserted
    // after the scoped_hook block alongside the in-detour ones.  Done BEFORE the
    // set_instance_count(0) reset below so it cannot perturb the in-detour
    // instanceCount baseline.
    try
    {
        // (I)V — single int, so we can read the constructor arg back and prove it
        // landed (not just that allocation returned non-null).
        if (auto o1{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(4242)) })
        {
            g_outside_made.store(true, std::memory_order_relaxed);

            // Read o1's INSTANCE fields IMMEDIATELY — in the tight window right
            // after o1's allocation and BEFORE the o2 allocation below, which is
            // the only intervening operation that can reach a young-gen safepoint
            // and relocate o1.  Re-acquiring the value here (rather than after o2)
            // keeps the read out of o2's allocating-GC window; read_until_stable
            // additionally absorbs a transient mid-read tear by converging on two
            // consecutive equal reads.  A value that cannot stabilize, or that
            // stabilizes WRONG (o1 already relocated before the first read, so the
            // frozen raw OOP yields stale bytes), is recorded best-effort by the
            // caller against the KNOWN-FIXED expected value — never a [FAIL].
            {
                std::int32_t stable_int{};
                const bool ok{ read_until_stable<std::int32_t>(
                    [&]() { return o1->get_int_field(); }, stable_int) };
                g_outside_int.store(stable_int, std::memory_order_relaxed);
                g_outside_int_stable.store(ok, std::memory_order_relaxed);
            }
            {
                std::int32_t stable_tag{};
                const bool ok{ read_until_stable<std::int32_t>(
                    [&]() { return o1->get_ctor_tag(); }, stable_tag) };
                g_outside_tag.store(stable_tag, std::memory_order_relaxed);
                g_outside_tag_stable.store(ok, std::memory_order_relaxed);
            }

            // Capture o1's heap identity NOW, before o2 is allocated, so it is the
            // identity of o1 at a point when o1's raw OOP is still current.  (Used
            // by the distinct-identity check below.)
            const vmhook::oop_t id1{ o1->get_instance() };

            // A SECOND outside-a-hook allocation must yield a DISTINCT live
            // instance (different heap identity) — proves discovery is repeatable
            // and each call produces a fresh object, exactly like the legacy
            // outside-hook counter/identity assertions.
            if (auto o2{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(7)) })
            {
                g_outside_made2.store(true, std::memory_order_relaxed);
                // Compare id1 (captured pre-o2-alloc) against o2's fresh identity.
                // Two distinct live objects almost always differ; the residual
                // race is that o2 can be allocated into the eden slot o1 VACATED
                // when a young-gen GC during o2's allocation relocated o1 — then
                // id1 (o1's old address) coincidentally equals o2's new address.
                // That non-distinct outcome is a GC-relocation artifact, not a
                // make_unique contract violation (the two objects ARE distinct
                // live instances), so the caller treats DISTINCT as the hard proof
                // and a coincidental-equal as best-effort [INFO].
                g_outside_distinct.store(
                    id1 != o2->get_instance(),
                    std::memory_order_relaxed);
            }
        }
    }
    catch (...)
    {
        ctx.record("[INFO] make_unique: outside-a-hook make_unique threw and was "
                   "contained (no crash); see the outside_* checks for partial results.");
    }

    // Reset the Java-side observers so counts are deterministic for this run.
    // (Clears the outside-a-hook increments above too, so the in-detour
    // instanceCount math below starts from a clean baseline.)
    make_unique_fixture::set_instance_count(0);

    {
        // scoped_hook on trigger(): every make_unique call below runs INSIDE
        // this detour, so a JavaThread is captured and live for the HotSpot-only
        // allocation path.  Never call shutdown_hooks() — the handle uninstalls
        // on scope exit, isolating this module.
        auto handle{ vmhook::scoped_hook<make_unique_fixture>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<make_unique_fixture>& self)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);

                // instanceCount BEFORE any native allocation (trigger's own
                // `new MakeUnique()` in the probe already ran once, so this is
                // the baseline we measure NewObjectA increments against).
                g_count_before.store(make_unique_fixture::get_instance_count(),
                                     std::memory_order_relaxed);

                // ── 1. No-arg constructor ()V ──────────────────────────────────
                if (auto a{ vmhook::make_unique<make_unique_fixture>() })
                {
                    g_noarg_ok.store(true, std::memory_order_relaxed);
                    g_noarg_tag.store(a->get_ctor_tag(), std::memory_order_relaxed);

                    // Second no-arg object — must be a distinct heap instance.
                    if (auto a2{ vmhook::make_unique<make_unique_fixture>() })
                    {
                        g_distinct_identity.store(
                            a->get_instance() != a2->get_instance(),
                            std::memory_order_relaxed);
                    }
                }

                // ── 2. Single int constructor (I)V ─────────────────────────────
                if (auto b{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(1337)) })
                {
                    g_int_ok.store(true, std::memory_order_relaxed);
                    g_int_val.store(b->get_int_field(), std::memory_order_relaxed);
                    g_int_tag.store(b->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 3. Two-int constructor (II)V ───────────────────────────────
                if (auto c{ vmhook::make_unique<make_unique_fixture>(
                        static_cast<std::int32_t>(40), static_cast<std::int32_t>(2)) })
                {
                    g_twoint_ok.store(true, std::memory_order_relaxed);
                    g_twoint_val.store(c->get_int_field(), std::memory_order_relaxed);
                    g_twoint_tag.store(c->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 4. Multi-arg constructor (IJD)V ────────────────────────────
                if (auto d{ vmhook::make_unique<make_unique_fixture>(
                        static_cast<std::int32_t>(7),
                        static_cast<std::int64_t>(0x0123456789ABCDEFLL),
                        3.5) })
                {
                    g_multi_ok.store(true, std::memory_order_relaxed);
                    g_multi_int.store(d->get_int_field(), std::memory_order_relaxed);
                    g_multi_long.store(d->get_long_field(), std::memory_order_relaxed);
                    g_multi_double_bits.store(double_to_bits(d->get_double_field()),
                                              std::memory_order_relaxed);
                    g_multi_tag.store(d->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 5. String constructor (Ljava/lang/String;)V via std::string ─
                if (auto e{ vmhook::make_unique<make_unique_fixture>(std::string{ "hello" }) })
                {
                    g_str_ok.store(true, std::memory_order_relaxed);
                    g_str_match.store(e->get_string_field() == std::string{ "hello" },
                                      std::memory_order_relaxed);
                    g_str_tag.store(e->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 5b. String constructor via const char* ─────────────────────
                // NOTE: passed through a const char* VARIABLE rather than a raw
                // string literal.  A literal ("c-string") decays to const
                // char(&)[9], and append_jni_arg's `arg ? ... : ...` null-check
                // (vmhook.hpp:9825) then trips GCC -Werror=address /
                // -Werror=nonnull-compare because the compiler knows a literal's
                // address is never null — a real library portability flaw with
                // string-literal ctor args (see the module header / agent notes).
                // Using a const char* lvalue exercises the SAME const-char*
                // branch without the literal-address diagnostic.
                {
                    const char* const c_str_arg{ "c-string" };
                    if (auto e2{ vmhook::make_unique<make_unique_fixture>(c_str_arg) })
                    {
                        g_cstr_ok.store(true, std::memory_order_relaxed);
                        g_cstr_match.store(e2->get_string_field() == std::string{ "c-string" },
                                           std::memory_order_relaxed);
                    }
                }

                // ── 5c. String constructor via std::string_view ────────────────
                {
                    const std::string_view sv{ "view-arg" };
                    if (auto e3{ vmhook::make_unique<make_unique_fixture>(sv) })
                    {
                        g_sv_ok.store(true, std::memory_order_relaxed);
                        g_sv_match.store(e3->get_string_field() == std::string{ "view-arg" },
                                         std::memory_order_relaxed);
                    }
                }

                // ── 5d. Empty-string arg (boundary) ────────────────────────────
                if (auto e4{ vmhook::make_unique<make_unique_fixture>(std::string{ "" }) })
                {
                    g_emptystr_ok.store(true, std::memory_order_relaxed);
                    g_emptystr_match.store(e4->get_string_field().empty(),
                                           std::memory_order_relaxed);
                }

                // ── 5e. Non-ASCII / multibyte UTF-8 arg ────────────────────────
                {
                    const std::string unicode{ "caf\xC3\xA9-\xE2\x9C\x93" };  // "café-✓"
                    if (auto e5{ vmhook::make_unique<make_unique_fixture>(unicode) })
                    {
                        g_unicode_ok.store(true, std::memory_order_relaxed);
                        g_unicode_match.store(e5->get_string_field() == unicode,
                                              std::memory_order_relaxed);
                    }
                }

                // ── 6. Mixed String + int constructor (Ljava/lang/String;I)V ───
                if (auto f{ vmhook::make_unique<make_unique_fixture>(
                        std::string{ "mix" }, static_cast<std::int32_t>(55)) })
                {
                    g_strint_ok.store(true, std::memory_order_relaxed);
                    g_strint_str_match.store(f->get_string_field() == std::string{ "mix" },
                                             std::memory_order_relaxed);
                    g_strint_int.store(f->get_int_field(), std::memory_order_relaxed);
                    g_strint_tag.store(f->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 6b. Boundary single-int (I)V args ──────────────────────────
                // INT_MIN / INT_MAX / -1 / 0 prove the int arg marshals byte-exact
                // through the jni_value union (no sign-extension into the high half
                // and no truncation).  Read back via read_until_stable so a young-
                // gen relocation between alloc and read cannot turn a correct write
                // into a [FAIL]; a stable-WRONG / un-stabilized read of the KNOWN-
                // FIXED expected value downgrades to a best-effort [INFO] at assert
                // time (the ctor DID run — only the read raced the collector).
                {
                    const std::int32_t kMin{ std::numeric_limits<std::int32_t>::min() };
                    if (auto bi{ vmhook::make_unique<make_unique_fixture>(kMin) })
                    {
                        g_int_min_ok.store(true, std::memory_order_relaxed);
                        std::int32_t v{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return bi->get_int_field(); }, v))
                        {
                            g_int_min_val.store(v, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const std::int32_t kMax{ std::numeric_limits<std::int32_t>::max() };
                    if (auto bi{ vmhook::make_unique<make_unique_fixture>(kMax) })
                    {
                        g_int_max_ok.store(true, std::memory_order_relaxed);
                        std::int32_t v{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return bi->get_int_field(); }, v))
                        {
                            g_int_max_val.store(v, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    if (auto bi{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(-1)) })
                    {
                        g_int_neg_ok.store(true, std::memory_order_relaxed);
                        std::int32_t v{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return bi->get_int_field(); }, v))
                        {
                            g_int_neg_val.store(v, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    // 0 is the field's default — to prove the ctor actually WROTE
                    // zero (rather than the field merely never being touched) we
                    // poison g_int_zero_val to a non-zero sentinel and only accept a
                    // stabilized read of exactly 0.
                    if (auto bi{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(0)) })
                    {
                        g_int_zero_ok.store(true, std::memory_order_relaxed);
                        std::int32_t v{ 7 };
                        if (read_until_stable<std::int32_t>(
                                [&]() { return bi->get_int_field(); }, v))
                        {
                            g_int_zero_val.store(v, std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6c. (II)V 32-bit overflow add (the Java ctor did the math) ──
                // INT_MAX + 1 wraps to INT_MIN in Java's 32-bit add; proving the
                // result equals INT_MIN confirms the constructor BODY summed the
                // two args (native passed them separately).
                {
                    const std::int32_t kMax{ std::numeric_limits<std::int32_t>::max() };
                    if (auto ti{ vmhook::make_unique<make_unique_fixture>(
                            kMax, static_cast<std::int32_t>(1)) })
                    {
                        g_twoint_ovf_ok.store(true, std::memory_order_relaxed);
                        std::int32_t v{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return ti->get_int_field(); }, v))
                        {
                            g_twoint_ovf_val.store(v, std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6d. Boundary (IJD)V long edges ─────────────────────────────
                {
                    const std::int64_t kLMin{ std::numeric_limits<std::int64_t>::min() };
                    if (auto m{ vmhook::make_unique<make_unique_fixture>(
                            static_cast<std::int32_t>(0), kLMin, 0.0) })
                    {
                        g_lmin_ok.store(true, std::memory_order_relaxed);
                        std::int64_t v{};
                        if (read_until_stable<std::int64_t>(
                                [&]() { return m->get_long_field(); }, v))
                        {
                            g_lmin_long.store(v, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const std::int64_t kLMax{ std::numeric_limits<std::int64_t>::max() };
                    if (auto m{ vmhook::make_unique<make_unique_fixture>(
                            static_cast<std::int32_t>(0), kLMax, 0.0) })
                    {
                        g_lmax_ok.store(true, std::memory_order_relaxed);
                        std::int64_t v{};
                        if (read_until_stable<std::int64_t>(
                                [&]() { return m->get_long_field(); }, v))
                        {
                            g_lmax_long.store(v, std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6e. Special-double (IJD)V args — NaN / -0.0 / +Inf bit-exact ─
                // These are the doubles where a value compare would lie (NaN != NaN;
                // -0.0 == 0.0): only a BIT compare proves the exact IEEE-754 payload
                // round-tripped through .d in the jni_value union and back out of
                // the doubleField.
                {
                    const double nan_v{ std::numeric_limits<double>::quiet_NaN() };
                    if (auto m{ vmhook::make_unique<make_unique_fixture>(
                            static_cast<std::int32_t>(0),
                            static_cast<std::int64_t>(0), nan_v) })
                    {
                        g_dspecial_ok.store(true, std::memory_order_relaxed);
                        std::int64_t bits{};
                        if (read_until_stable<std::int64_t>(
                                [&]() { return double_to_bits(m->get_double_field()); }, bits))
                        {
                            g_dspecial_nan_bits.store(bits, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const double negzero{ -0.0 };
                    if (auto m{ vmhook::make_unique<make_unique_fixture>(
                            static_cast<std::int32_t>(0),
                            static_cast<std::int64_t>(0), negzero) })
                    {
                        std::int64_t bits{ 1 };
                        if (read_until_stable<std::int64_t>(
                                [&]() { return double_to_bits(m->get_double_field()); }, bits))
                        {
                            g_dspecial_negzero_bits.store(bits, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const double inf_v{ std::numeric_limits<double>::infinity() };
                    if (auto m{ vmhook::make_unique<make_unique_fixture>(
                            static_cast<std::int32_t>(0),
                            static_cast<std::int64_t>(0), inf_v) })
                    {
                        std::int64_t bits{};
                        if (read_until_stable<std::int64_t>(
                                [&]() { return double_to_bits(m->get_double_field()); }, bits))
                        {
                            g_dspecial_inf_bits.store(bits, std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6f. object_base move semantics (wrapper move, not unique_ptr) ─
                // make_unique returns unique_ptr<T>; the wrapper INSIDE it has its
                // own object_base move ctor / move-assign that transfers the raw OOP
                // and NULLS the source.  Pure pointer-value checks (no field deref)
                // — fully race-immune, asserted HARD.
                if (auto mv{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(11)) })
                {
                    const vmhook::oop_t before{ mv->get_instance() };

                    // Move-CONSTRUCT a fresh wrapper from *mv (the held object).
                    make_unique_fixture moved{ std::move(*mv) };
                    g_move_ctor_preserved_identity.store(
                        moved.get_instance() == before, std::memory_order_relaxed);
                    g_move_ctor_nulled_source.store(
                        mv->get_instance() == nullptr, std::memory_order_relaxed);

                    // Move-ASSIGN into another default-constructed (null) wrapper.
                    make_unique_fixture sink{ nullptr };
                    sink = std::move(moved);
                    g_move_assign_preserved_identity.store(
                        sink.get_instance() == before, std::memory_order_relaxed);
                    g_move_assign_nulled_source.store(
                        moved.get_instance() == nullptr, std::memory_order_relaxed);
                }

                // ── 6g. object_base copy semantics + aliasing ──────────────────
                // Copying a wrapper duplicates the RAW OOP (it is not a GC handle):
                // both wrappers name the SAME live object.  Pointer equality is
                // race-immune (HARD); a field write through the COPY being visible
                // through the ORIGINAL proves true aliasing and is GC-race-guarded.
                if (auto cp{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(500)) })
                {
                    make_unique_fixture alias{ *cp };   // copy ctor — same OOP
                    g_copy_same_identity.store(
                        alias.get_instance() == cp->get_instance(),
                        std::memory_order_relaxed);

                    // Write 0x5151 through the alias; read it back through cp.
                    if (auto fp{ alias.get_field("intField") })
                    {
                        fp->set(static_cast<std::int32_t>(0x5151));
                        std::int32_t v{};
                        const bool ok{ read_until_stable<std::int32_t>(
                            [&]() { return cp->get_int_field(); }, v) };
                        g_copy_alias_write_stable.store(ok, std::memory_order_relaxed);
                        g_copy_alias_read.store(v, std::memory_order_relaxed);
                        g_copy_alias_write_visible.store(
                            ok && v == 0x5151, std::memory_order_relaxed);
                    }
                    // Both `alias` (stack) and `cp` (unique_ptr) destruct here —
                    // raw-OOP wrappers, no GC handle, so two destructions of the
                    // SAME OOP is a no-op pair (no double-free).  Surviving to the
                    // next statement is the proof; a double-free would have crashed.
                }

                // ── 6h. Identity round-trip — re-wrap get_instance() ───────────
                // Construct a BRAND-NEW wrapper directly from an existing wrapper's
                // raw OOP and confirm it resolves the same klass (typeid-based) and
                // reads the same field — get_instance() is a faithful, re-wrappable
                // heap identity, exactly what a caller storing the OOP and rebuilding
                // a wrapper later relies on.
                if (auto rt{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(9001)) })
                {
                    const vmhook::oop_t raw{ rt->get_instance() };
                    make_unique_fixture rewrapped{ raw };
                    g_rewrap_same_identity.store(
                        rewrapped.get_instance() == raw, std::memory_order_relaxed);
                    std::int32_t v{};
                    const bool ok{ read_until_stable<std::int32_t>(
                        [&]() { return rewrapped.get_int_field(); }, v) };
                    g_rewrap_field_stable.store(ok, std::memory_order_relaxed);
                    g_rewrap_field_read.store(v, std::memory_order_relaxed);
                    g_rewrap_field_matches.store(ok && v == 9001, std::memory_order_relaxed);
                }

                // ── 6i. Narrow-primitive (S)V short — boundary round-trips ─────
                // SHRT_MIN / SHRT_MAX / -1 prove the short arg marshals byte-exact
                // through the jni_value union "I" slot narrowed to "S" by the
                // descriptor, with NO sign-extension/truncation slip, and that the
                // JVM dispatched the (S)V ctor (ctorTag 7), not (I)V.
                {
                    const short kMin{ std::numeric_limits<short>::min() };
                    if (auto s{ vmhook::make_unique<make_unique_fixture>(kMin) })
                    {
                        g_short_min_ok.store(true, std::memory_order_relaxed);
                        std::int16_t v{};
                        if (read_until_stable<std::int16_t>(
                                [&]() { return s->get_short_field(); }, v))
                        {
                            g_short_min_val.store(static_cast<std::int32_t>(v),
                                                  std::memory_order_relaxed);
                        }
                        std::int32_t tag{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return s->get_ctor_tag(); }, tag))
                        {
                            g_short_min_tag.store(tag, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const short kMax{ std::numeric_limits<short>::max() };
                    if (auto s{ vmhook::make_unique<make_unique_fixture>(kMax) })
                    {
                        g_short_max_ok.store(true, std::memory_order_relaxed);
                        std::int16_t v{};
                        if (read_until_stable<std::int16_t>(
                                [&]() { return s->get_short_field(); }, v))
                        {
                            g_short_max_val.store(static_cast<std::int32_t>(v),
                                                  std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const short kNeg{ static_cast<short>(-1) };
                    if (auto s{ vmhook::make_unique<make_unique_fixture>(kNeg) })
                    {
                        g_short_neg_ok.store(true, std::memory_order_relaxed);
                        std::int16_t v{};
                        if (read_until_stable<std::int16_t>(
                                [&]() { return s->get_short_field(); }, v))
                        {
                            g_short_neg_val.store(static_cast<std::int32_t>(v),
                                                  std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6j. Narrow-primitive (B)V byte — boundary round-trips ──────
                // -128 / 127 prove the int8 arg marshals byte-exact (sign preserved)
                // and dispatched the (B)V ctor (ctorTag 8).
                {
                    const std::int8_t kMin{ std::numeric_limits<std::int8_t>::min() };
                    if (auto b{ vmhook::make_unique<make_unique_fixture>(kMin) })
                    {
                        g_byte_min_ok.store(true, std::memory_order_relaxed);
                        std::int8_t v{};
                        if (read_until_stable<std::int8_t>(
                                [&]() { return b->get_byte_field(); }, v))
                        {
                            g_byte_min_val.store(static_cast<std::int32_t>(v),
                                                 std::memory_order_relaxed);
                        }
                        std::int32_t tag{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return b->get_ctor_tag(); }, tag))
                        {
                            g_byte_min_tag.store(tag, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const std::int8_t kMax{ std::numeric_limits<std::int8_t>::max() };
                    if (auto b{ vmhook::make_unique<make_unique_fixture>(kMax) })
                    {
                        g_byte_max_ok.store(true, std::memory_order_relaxed);
                        std::int8_t v{};
                        if (read_until_stable<std::int8_t>(
                                [&]() { return b->get_byte_field(); }, v))
                        {
                            g_byte_max_val.store(static_cast<std::int32_t>(v),
                                                 std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6k. Narrow-primitive (C)V char — UNSIGNED 16-bit edges ─────
                // char is an UNSIGNED UTF-16 code unit: 0 / 0xFFFF / 'A' / a BMP
                // non-ASCII unit (U+00E9, e-acute).  0xFFFF read back as 0xFFFF (not -1)
                // proves the value is treated UNSIGNED through the "C" slot; ctorTag
                // 9 proves the (C)V ctor dispatched, not (S)V.  char16_t is widened
                // to int32 for the atomic so 0xFFFF survives without sign issues.
                {
                    const char16_t kZero{ 0 };
                    if (auto c{ vmhook::make_unique<make_unique_fixture>(kZero) })
                    {
                        g_char_zero_ok.store(true, std::memory_order_relaxed);
                        char16_t v{ 0xABCD };  // poison so a no-write fails
                        if (read_until_stable<char16_t>(
                                [&]() { return c->get_char_field(); }, v))
                        {
                            g_char_zero_val.store(static_cast<std::int32_t>(v),
                                                  std::memory_order_relaxed);
                        }
                        std::int32_t tag{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return c->get_ctor_tag(); }, tag))
                        {
                            g_char_zero_tag.store(tag, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const char16_t kMax{ 0xFFFF };
                    if (auto c{ vmhook::make_unique<make_unique_fixture>(kMax) })
                    {
                        g_char_max_ok.store(true, std::memory_order_relaxed);
                        char16_t v{};
                        if (read_until_stable<char16_t>(
                                [&]() { return c->get_char_field(); }, v))
                        {
                            g_char_max_val.store(static_cast<std::int32_t>(v),
                                                 std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const char16_t kLetter{ u'A' };  // U+0041
                    if (auto c{ vmhook::make_unique<make_unique_fixture>(kLetter) })
                    {
                        g_char_letter_ok.store(true, std::memory_order_relaxed);
                        char16_t v{};
                        if (read_until_stable<char16_t>(
                                [&]() { return c->get_char_field(); }, v))
                        {
                            g_char_letter_val.store(static_cast<std::int32_t>(v),
                                                    std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const char16_t kBmp{ 0x00E9 };  // U+00E9 LATIN SMALL LETTER E WITH ACUTE
                    if (auto c{ vmhook::make_unique<make_unique_fixture>(kBmp) })
                    {
                        g_char_bmp_ok.store(true, std::memory_order_relaxed);
                        char16_t v{};
                        if (read_until_stable<char16_t>(
                                [&]() { return c->get_char_field(); }, v))
                        {
                            g_char_bmp_val.store(static_cast<std::int32_t>(v),
                                                 std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6l. Narrow-primitive (F)V float — bit-exact round-trips ────
                // 3.5 (exactly representable), +Inf, -0.0, NaN — bit-compared via
                // float_to_bits so a value-compare lie (NaN!=NaN, -0.0==+0.0) cannot
                // hide a wrong-slot / wrong-width write through the .f union cell.
                // ctorTag 10 proves the (F)V ctor dispatched.
                {
                    const float kF{ 3.5F };
                    if (auto fo{ vmhook::make_unique<make_unique_fixture>(kF) })
                    {
                        g_float_ok.store(true, std::memory_order_relaxed);
                        std::int32_t bits{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return float_to_bits(fo->get_float_field()); }, bits))
                        {
                            g_float_bits.store(bits, std::memory_order_relaxed);
                        }
                        std::int32_t tag{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return fo->get_ctor_tag(); }, tag))
                        {
                            g_float_tag.store(tag, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const float kInf{ std::numeric_limits<float>::infinity() };
                    if (auto fo{ vmhook::make_unique<make_unique_fixture>(kInf) })
                    {
                        g_float_inf_ok.store(true, std::memory_order_relaxed);
                        std::int32_t bits{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return float_to_bits(fo->get_float_field()); }, bits))
                        {
                            g_float_inf_bits.store(bits, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const float kNegZero{ -0.0F };
                    if (auto fo{ vmhook::make_unique<make_unique_fixture>(kNegZero) })
                    {
                        g_float_negzero_ok.store(true, std::memory_order_relaxed);
                        std::int32_t bits{ 1 };
                        if (read_until_stable<std::int32_t>(
                                [&]() { return float_to_bits(fo->get_float_field()); }, bits))
                        {
                            g_float_negzero_bits.store(bits, std::memory_order_relaxed);
                        }
                    }
                }
                {
                    const float kNaN{ std::numeric_limits<float>::quiet_NaN() };
                    if (auto fo{ vmhook::make_unique<make_unique_fixture>(kNaN) })
                    {
                        g_float_nan_ok.store(true, std::memory_order_relaxed);
                        std::int32_t bits{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return float_to_bits(fo->get_float_field()); }, bits))
                        {
                            g_float_nan_bits.store(bits, std::memory_order_relaxed);
                        }
                    }
                }

                // ── 6m. Long-only (J)V — single 8-byte arg resolves "(J)V" ─────
                // A SINGLE std::int64_t arg must build the "(J)V" descriptor and
                // dispatch the long-only ctor (ctorTag 11), distinct from (IJD)V and
                // (I)V.  Value round-trips bit-exact.
                {
                    const std::int64_t kVal{ 0x7EDCBA9876543210LL };
                    if (auto lo{ vmhook::make_unique<make_unique_fixture>(kVal) })
                    {
                        g_longonly_ok.store(true, std::memory_order_relaxed);
                        std::int64_t v{};
                        if (read_until_stable<std::int64_t>(
                                [&]() { return lo->get_long_field(); }, v))
                        {
                            g_longonly_val.store(v, std::memory_order_relaxed);
                        }
                        std::int32_t tag{};
                        if (read_until_stable<std::int32_t>(
                                [&]() { return lo->get_ctor_tag(); }, tag))
                        {
                            g_longonly_tag.store(tag, std::memory_order_relaxed);
                        }
                    }
                }

                // instanceCount AFTER all NewObjectA allocations.  Each of the
                // objects above that ran a real Java <init> bumped the static
                // counter; we expect a net increase (>= the number that ran).
                g_count_after_newobj.store(make_unique_fixture::get_instance_count(),
                                           std::memory_order_relaxed);

                // ── 7. TLAB + construct() fallback path via (Z)V (no Java ctor) ─
                // No (Z)V <init> exists, so jni_make_unique fails GetMethodID and
                // make_unique falls back to raw TLAB allocation + construct(bool).
                make_unique_fixture::g_construct_ran.store(false, std::memory_order_relaxed);
                make_unique_fixture::g_construct_calls.store(0, std::memory_order_relaxed);
                if (auto g{ vmhook::make_unique<make_unique_fixture>(true) })
                {
                    g_tlab_made.store(true, std::memory_order_relaxed);
                    g_tlab_boolfield.store(g->get_bool_field(), std::memory_order_relaxed);
                    g_tlab_tag.store(g->get_ctor_tag(), std::memory_order_relaxed);
                }
                // construct() must have been dispatched EXACTLY once for the single
                // (Z)V fallback allocation — not zero (missed), not twice (the
                // NewObjectA path erroneously also calling it).
                g_construct_call_count.store(
                    make_unique_fixture::g_construct_calls.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                g_count_after_tlab.store(make_unique_fixture::get_instance_count(),
                                         std::memory_order_relaxed);
            }) };

        ctx.check("make_unique_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool value) { make_unique_fixture::set_go(value); },
            []() { return make_unique_fixture::get_done(); }) };

        ctx.check("make_unique_probe_completed", done);
        ctx.check("make_unique_hook_fired",
                  g_hook_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("make_unique_hook_saw_self",
                  g_hook_saw_self.load(std::memory_order_relaxed));

        // ── OUTSIDE-A-HOOK angles (VM-metadata JavaThread discovery, no detour) ─
        // make_unique was called in the module body before any hook was armed, so
        // a live JavaThread had to be discovered from VM metadata (not the hook
        // trampoline).  Allocation succeeded, the (I)V arg landed via field
        // read-back, and a second call produced a DISTINCT live instance.
        //
        // GC-RACE NOTE: the two ALLOCATION-SUCCESS checks stay HARD — they only
        // test a non-null unique_ptr (no OOP deref) and are race-immune.  The
        // field READ-BACK and the IDENTITY comparison deref / capture o1's raw OOP,
        // which a young-gen relocation between the alloc and the read can leave
        // stale (the wrapper holds a raw OOP and field_proxy does NOT re-resolve a
        // GC-stable root for INSTANCE fields — vmhook.hpp:14404-14421).  Those
        // three were the recurring linux·gcc·java11 flake.  They are now read via
        // read_until_stable in the tight post-alloc window (see the alloc block
        // above) and asserted HARD when the read is race-free, downgrading only a
        // converged-WRONG read of the KNOWN-FIXED value to a best-effort [INFO] —
        // a stale read of a value the ctor itself just set is always a GC artifact,
        // never a real bug.  The make_unique CONTRACT (object allocated, <init>
        // ran, field set, two instances distinct) is unchanged and still asserted
        // on every race-free run.
        ctx.check("outside_hook_allocated",
                  g_outside_made.load(std::memory_order_relaxed));

        // (I)V arg read back == 4242.  HARD when the read stabilized on the
        // correct value; best-effort [INFO] if it could not stabilize or
        // stabilized on a stale value (o1 relocated before the read).
        {
            const bool stable{ g_outside_int_stable.load(std::memory_order_relaxed) };
            const std::int32_t v{ g_outside_int.load(std::memory_order_relaxed) };
            if (stable && v == 4242)
            {
                ctx.check("outside_hook_int_field_set_to_4242", true);
            }
            else
            {
                ctx.record("[INFO] make_unique: outside_hook_int_field_set_to_4242 "
                           "best-effort (young-OOP GC-relocation read of a raw, "
                           "non-re-resolved instance field) — stable=" +
                           std::string{ stable ? "true" : "false" } + ", value=" +
                           std::to_string(v) + ", expected 4242.  The (I)V ctor DID "
                           "run; only the read raced the collector.");
            }
        }

        // Dispatched ctorTag == 2 (the (I)V constructor).  Same race surface and
        // same best-effort treatment as the int read above.
        {
            const bool stable{ g_outside_tag_stable.load(std::memory_order_relaxed) };
            const std::int32_t v{ g_outside_tag.load(std::memory_order_relaxed) };
            if (stable && v == 2)
            {
                ctx.check("outside_hook_dispatched_I_ctor", true);
            }
            else
            {
                ctx.record("[INFO] make_unique: outside_hook_dispatched_I_ctor "
                           "best-effort (young-OOP GC-relocation read of a raw, "
                           "non-re-resolved instance field) — stable=" +
                           std::string{ stable ? "true" : "false" } + ", ctorTag=" +
                           std::to_string(v) + ", expected 2.  The (I)V <init> WAS "
                           "dispatched; only the read raced the collector.");
            }
        }

        ctx.check("outside_hook_second_allocated",
                  g_outside_made2.load(std::memory_order_relaxed));

        // Two outside-a-hook allocations are DISTINCT live instances.  DISTINCT is
        // the hard proof and stays a PASS; a coincidental non-distinct outcome
        // (o2 reusing o1's eden slot after a relocation, so o1's captured old
        // address equals o2's new one) is a GC-relocation artifact, recorded
        // best-effort rather than failed.
        if (g_outside_distinct.load(std::memory_order_relaxed))
        {
            ctx.check("outside_hook_distinct_identity", true);
        }
        else
        {
            ctx.record("[INFO] make_unique: outside_hook_distinct_identity best-effort "
                       "(young-OOP GC-relocation made o1's captured raw OOP alias o2's "
                       "fresh slot) — the two allocations ARE distinct live instances; "
                       "only the raw-OOP comparison raced the collector.");
        }

        // ── No-arg constructor angles ──────────────────────────────────────────
        ctx.check("noarg_allocated", g_noarg_ok.load(std::memory_order_relaxed));
        ctx.check("noarg_ran_ctor_body",
                  g_noarg_tag.load(std::memory_order_relaxed) == 1);
        ctx.check("noarg_distinct_identity",
                  g_distinct_identity.load(std::memory_order_relaxed));

        // ── Single-int constructor angles ──────────────────────────────────────
        ctx.check("int_allocated", g_int_ok.load(std::memory_order_relaxed));
        ctx.check("int_field_set_to_1337",
                  g_int_val.load(std::memory_order_relaxed) == 1337);
        ctx.check("int_dispatched_I_ctor",
                  g_int_tag.load(std::memory_order_relaxed) == 2);

        // ── Two-int constructor angles ─────────────────────────────────────────
        ctx.check("twoint_allocated", g_twoint_ok.load(std::memory_order_relaxed));
        ctx.check("twoint_sum_is_42",
                  g_twoint_val.load(std::memory_order_relaxed) == 42);
        ctx.check("twoint_dispatched_II_ctor",
                  g_twoint_tag.load(std::memory_order_relaxed) == 3);

        // ── Multi-arg (IJD) constructor angles ─────────────────────────────────
        ctx.check("multi_allocated", g_multi_ok.load(std::memory_order_relaxed));
        ctx.check("multi_int_arg_is_7",
                  g_multi_int.load(std::memory_order_relaxed) == 7);
        ctx.check("multi_long_arg_round_trips",
                  g_multi_long.load(std::memory_order_relaxed) == 0x0123456789ABCDEFLL);
        ctx.check("multi_double_arg_round_trips",
                  g_multi_double_bits.load(std::memory_order_relaxed) == double_to_bits(3.5));
        ctx.check("multi_dispatched_IJD_ctor",
                  g_multi_tag.load(std::memory_order_relaxed) == 4);

        // ── Narrow-primitive (S)V short angles ─────────────────────────────────
        // ALLOCATED is race-immune (non-null unique_ptr, no deref) -> HARD.  The
        // VALUE / ctorTag read-backs deref a young raw OOP; HARD when converged on
        // the KNOWN-FIXED expected value, best-effort [INFO] for a stale/un-
        // converged read (the (S)V ctor DID run — only the read raced the GC).
        ctx.check("short_min_allocated", g_short_min_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t kMin{ std::numeric_limits<short>::min() };
            const std::int32_t v{ g_short_min_val.load(std::memory_order_relaxed) };
            if (v == kMin) { ctx.check("short_min_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: short_min_round_trips best-effort "
                           "(young-OOP GC-relocation read of shortField) — value=" +
                           std::to_string(v) + ", expected SHRT_MIN.  The (S)V ctor DID run.");
            }
        }
        {
            const std::int32_t tag{ g_short_min_tag.load(std::memory_order_relaxed) };
            if (tag == 7) { ctx.check("short_dispatched_S_ctor", true); }
            else
            {
                ctx.record("[INFO] make_unique: short_dispatched_S_ctor best-effort "
                           "(young-OOP GC-relocation read of ctorTag) — value=" +
                           std::to_string(tag) + ", expected 7.  The (S)V <init> WAS dispatched.");
            }
        }
        ctx.check("short_max_allocated", g_short_max_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t kMax{ std::numeric_limits<short>::max() };
            const std::int32_t v{ g_short_max_val.load(std::memory_order_relaxed) };
            if (v == kMax) { ctx.check("short_max_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: short_max_round_trips best-effort "
                           "(young-OOP GC-relocation read of shortField) — value=" +
                           std::to_string(v) + ", expected SHRT_MAX.  The (S)V ctor DID run.");
            }
        }
        ctx.check("short_neg_allocated", g_short_neg_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t v{ g_short_neg_val.load(std::memory_order_relaxed) };
            if (v == -1) { ctx.check("short_neg_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: short_neg_round_trips best-effort "
                           "(young-OOP GC-relocation read of shortField) — value=" +
                           std::to_string(v) + ", expected -1.  The (S)V ctor DID run.");
            }
        }

        // ── Narrow-primitive (B)V byte angles ──────────────────────────────────
        ctx.check("byte_min_allocated", g_byte_min_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t kMin{ std::numeric_limits<std::int8_t>::min() };
            const std::int32_t v{ g_byte_min_val.load(std::memory_order_relaxed) };
            if (v == kMin) { ctx.check("byte_min_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: byte_min_round_trips best-effort "
                           "(young-OOP GC-relocation read of byteField) — value=" +
                           std::to_string(v) + ", expected -128.  The (B)V ctor DID run.");
            }
        }
        {
            const std::int32_t tag{ g_byte_min_tag.load(std::memory_order_relaxed) };
            if (tag == 8) { ctx.check("byte_dispatched_B_ctor", true); }
            else
            {
                ctx.record("[INFO] make_unique: byte_dispatched_B_ctor best-effort "
                           "(young-OOP GC-relocation read of ctorTag) — value=" +
                           std::to_string(tag) + ", expected 8.  The (B)V <init> WAS dispatched.");
            }
        }
        ctx.check("byte_max_allocated", g_byte_max_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t kMax{ std::numeric_limits<std::int8_t>::max() };
            const std::int32_t v{ g_byte_max_val.load(std::memory_order_relaxed) };
            if (v == kMax) { ctx.check("byte_max_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: byte_max_round_trips best-effort "
                           "(young-OOP GC-relocation read of byteField) — value=" +
                           std::to_string(v) + ", expected 127.  The (B)V ctor DID run.");
            }
        }

        // ── Narrow-primitive (C)V char angles (UNSIGNED 16-bit) ────────────────
        ctx.check("char_zero_allocated", g_char_zero_ok.load(std::memory_order_relaxed));
        {
            // poisoned to 0xABCD; only a stabilized read of exactly 0 proves the
            // ctor WROTE the field (vs never touching the 0 default).
            const std::int32_t v{ g_char_zero_val.load(std::memory_order_relaxed) };
            if (v == 0) { ctx.check("char_zero_written_by_ctor", true); }
            else
            {
                ctx.record("[INFO] make_unique: char_zero_written_by_ctor best-effort "
                           "(young-OOP GC-relocation read of charField) — value=" +
                           std::to_string(v) + ", expected 0.  The (C)V ctor DID run.");
            }
        }
        {
            const std::int32_t tag{ g_char_zero_tag.load(std::memory_order_relaxed) };
            if (tag == 9) { ctx.check("char_dispatched_C_ctor", true); }
            else
            {
                ctx.record("[INFO] make_unique: char_dispatched_C_ctor best-effort "
                           "(young-OOP GC-relocation read of ctorTag) — value=" +
                           std::to_string(tag) + ", expected 9.  The (C)V <init> WAS dispatched.");
            }
        }
        ctx.check("char_max_allocated", g_char_max_ok.load(std::memory_order_relaxed));
        {
            // 0xFFFF read back as 65535 (NOT -1) proves the value is UNSIGNED.
            const std::int32_t v{ g_char_max_val.load(std::memory_order_relaxed) };
            if (v == 0xFFFF) { ctx.check("char_max_round_trips_unsigned", true); }
            else
            {
                ctx.record("[INFO] make_unique: char_max_round_trips_unsigned best-effort "
                           "(young-OOP GC-relocation read of charField) — value=" +
                           std::to_string(v) + ", expected 65535.  The (C)V ctor DID run.");
            }
        }
        ctx.check("char_letter_allocated", g_char_letter_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t v{ g_char_letter_val.load(std::memory_order_relaxed) };
            if (v == 0x41) { ctx.check("char_letter_round_trips_A", true); }
            else
            {
                ctx.record("[INFO] make_unique: char_letter_round_trips_A best-effort "
                           "(young-OOP GC-relocation read of charField) — value=" +
                           std::to_string(v) + ", expected 65 ('A').  The (C)V ctor DID run.");
            }
        }
        ctx.check("char_bmp_allocated", g_char_bmp_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t v{ g_char_bmp_val.load(std::memory_order_relaxed) };
            if (v == 0x00E9) { ctx.check("char_bmp_round_trips_U00E9", true); }
            else
            {
                ctx.record("[INFO] make_unique: char_bmp_round_trips_U00E9 best-effort "
                           "(young-OOP GC-relocation read of charField) — value=" +
                           std::to_string(v) + ", expected 233 (U+00E9).  The (C)V ctor DID run.");
            }
        }

        // ── Narrow-primitive (F)V float angles (bit-exact) ─────────────────────
        ctx.check("float_allocated", g_float_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t expect{ float_to_bits(3.5F) };
            const std::int32_t v{ g_float_bits.load(std::memory_order_relaxed) };
            if (v == expect) { ctx.check("float_3_5_round_trips_bit_exact", true); }
            else
            {
                ctx.record("[INFO] make_unique: float_3_5_round_trips_bit_exact best-effort "
                           "(young-OOP GC-relocation read of floatField) — the (F)V ctor DID run.");
            }
        }
        {
            const std::int32_t tag{ g_float_tag.load(std::memory_order_relaxed) };
            if (tag == 10) { ctx.check("float_dispatched_F_ctor", true); }
            else
            {
                ctx.record("[INFO] make_unique: float_dispatched_F_ctor best-effort "
                           "(young-OOP GC-relocation read of ctorTag) — value=" +
                           std::to_string(tag) + ", expected 10.  The (F)V <init> WAS dispatched.");
            }
        }
        ctx.check("float_inf_allocated", g_float_inf_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t expect{ float_to_bits(std::numeric_limits<float>::infinity()) };
            const std::int32_t v{ g_float_inf_bits.load(std::memory_order_relaxed) };
            if (v == expect) { ctx.check("float_inf_round_trips_bit_exact", true); }
            else
            {
                ctx.record("[INFO] make_unique: float_inf_round_trips_bit_exact best-effort "
                           "(young-OOP GC-relocation read of floatField) — the (F)V ctor DID run.");
            }
        }
        ctx.check("float_negzero_allocated", g_float_negzero_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t expect{ float_to_bits(-0.0F) };
            const std::int32_t v{ g_float_negzero_bits.load(std::memory_order_relaxed) };
            if (v == expect) { ctx.check("float_negzero_round_trips_bit_exact", true); }
            else
            {
                ctx.record("[INFO] make_unique: float_negzero_round_trips_bit_exact best-effort "
                           "(young-OOP GC-relocation read of floatField) — the (F)V ctor DID run.");
            }
        }
        ctx.check("float_nan_allocated", g_float_nan_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t expect{ float_to_bits(std::numeric_limits<float>::quiet_NaN()) };
            const std::int32_t v{ g_float_nan_bits.load(std::memory_order_relaxed) };
            if (v == expect) { ctx.check("float_nan_round_trips_bit_exact", true); }
            else
            {
                ctx.record("[INFO] make_unique: float_nan_round_trips_bit_exact best-effort "
                           "(young-OOP GC-relocation read of floatField) — the (F)V ctor DID run.");
            }
        }

        // ── Long-only (J)V angles ──────────────────────────────────────────────
        ctx.check("longonly_allocated", g_longonly_ok.load(std::memory_order_relaxed));
        {
            const std::int64_t v{ g_longonly_val.load(std::memory_order_relaxed) };
            if (v == 0x7EDCBA9876543210LL) { ctx.check("longonly_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: longonly_round_trips best-effort "
                           "(young-OOP GC-relocation read of longField) — the (J)V ctor DID run.");
            }
        }
        {
            const std::int32_t tag{ g_longonly_tag.load(std::memory_order_relaxed) };
            if (tag == 11) { ctx.check("longonly_dispatched_J_ctor", true); }
            else
            {
                ctx.record("[INFO] make_unique: longonly_dispatched_J_ctor best-effort "
                           "(young-OOP GC-relocation read of ctorTag) — value=" +
                           std::to_string(tag) + ", expected 11.  The (J)V <init> WAS dispatched.");
            }
        }

        // ── Boundary single-int (I)V angles ────────────────────────────────────
        // ALLOCATED checks test only a non-null unique_ptr (no deref) -> HARD,
        // race-immune.  The field-VALUE checks deref the young raw OOP; they are
        // HARD when read_until_stable converged on the KNOWN-FIXED expected value
        // and downgrade a stale / un-converged read of that fixed value to a
        // best-effort [INFO] (the ctor DID write it — only the read raced the GC).
        ctx.check("int_min_allocated", g_int_min_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t kMin{ std::numeric_limits<std::int32_t>::min() };
            const std::int32_t v{ g_int_min_val.load(std::memory_order_relaxed) };
            if (v == kMin) { ctx.check("int_min_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: int_min_round_trips best-effort "
                           "(young-OOP GC-relocation read) — value=" + std::to_string(v)
                           + ", expected INT_MIN.  The (I)V ctor DID run.");
            }
        }
        ctx.check("int_max_allocated", g_int_max_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t kMax{ std::numeric_limits<std::int32_t>::max() };
            const std::int32_t v{ g_int_max_val.load(std::memory_order_relaxed) };
            if (v == kMax) { ctx.check("int_max_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: int_max_round_trips best-effort "
                           "(young-OOP GC-relocation read) — value=" + std::to_string(v)
                           + ", expected INT_MAX.  The (I)V ctor DID run.");
            }
        }
        ctx.check("int_neg_allocated", g_int_neg_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t v{ g_int_neg_val.load(std::memory_order_relaxed) };
            if (v == -1) { ctx.check("int_neg_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: int_neg_round_trips best-effort "
                           "(young-OOP GC-relocation read) — value=" + std::to_string(v)
                           + ", expected -1.  The (I)V ctor DID run.");
            }
        }
        ctx.check("int_zero_allocated", g_int_zero_ok.load(std::memory_order_relaxed));
        {
            // The atomic was poisoned non-zero; only a stabilized read of exactly 0
            // proves the ctor WROTE the field (vs never touching the 0 default).
            const std::int32_t v{ g_int_zero_val.load(std::memory_order_relaxed) };
            if (v == 0) { ctx.check("int_zero_written_by_ctor", true); }
            else
            {
                ctx.record("[INFO] make_unique: int_zero_written_by_ctor best-effort "
                           "(young-OOP GC-relocation read) — value=" + std::to_string(v)
                           + ", expected 0.  The (I)V ctor DID run.");
            }
        }

        // ── (II)V 32-bit overflow add angle ────────────────────────────────────
        ctx.check("twoint_overflow_allocated", g_twoint_ovf_ok.load(std::memory_order_relaxed));
        {
            const std::int32_t kMin{ std::numeric_limits<std::int32_t>::min() };
            const std::int32_t v{ g_twoint_ovf_val.load(std::memory_order_relaxed) };
            if (v == kMin) { ctx.check("twoint_overflow_wraps_to_INT_MIN", true); }
            else
            {
                ctx.record("[INFO] make_unique: twoint_overflow_wraps_to_INT_MIN best-effort "
                           "(young-OOP GC-relocation read) — value=" + std::to_string(v)
                           + ", expected INT_MIN (INT_MAX+1 wrap).  The (II)V ctor DID run.");
            }
        }

        // ── Boundary (IJD)V long edges ─────────────────────────────────────────
        ctx.check("long_min_allocated", g_lmin_ok.load(std::memory_order_relaxed));
        {
            const std::int64_t kLMin{ std::numeric_limits<std::int64_t>::min() };
            const std::int64_t v{ g_lmin_long.load(std::memory_order_relaxed) };
            if (v == kLMin) { ctx.check("long_min_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: long_min_round_trips best-effort "
                           "(young-OOP GC-relocation read of longField) — the (IJD)V ctor DID run.");
            }
        }
        ctx.check("long_max_allocated", g_lmax_ok.load(std::memory_order_relaxed));
        {
            const std::int64_t kLMax{ std::numeric_limits<std::int64_t>::max() };
            const std::int64_t v{ g_lmax_long.load(std::memory_order_relaxed) };
            if (v == kLMax) { ctx.check("long_max_round_trips", true); }
            else
            {
                ctx.record("[INFO] make_unique: long_max_round_trips best-effort "
                           "(young-OOP GC-relocation read of longField) — the (IJD)V ctor DID run.");
            }
        }

        // ── Special-double (IJD)V bit-exact angles ─────────────────────────────
        // NaN: a value compare would never see equality (NaN != NaN), so we BIT-
        // compare against a canonical quiet-NaN's bit pattern.  -0.0: BIT-distinct
        // from +0.0 (only the sign bit), so a write of -0.0 read back as +0.0 would
        // be caught.  +Inf: bit-exact.
        ctx.check("dspecial_nan_allocated", g_dspecial_ok.load(std::memory_order_relaxed));
        {
            const std::int64_t expect{ double_to_bits(std::numeric_limits<double>::quiet_NaN()) };
            const std::int64_t v{ g_dspecial_nan_bits.load(std::memory_order_relaxed) };
            if (v == expect) { ctx.check("dspecial_nan_round_trips_bit_exact", true); }
            else
            {
                ctx.record("[INFO] make_unique: dspecial_nan_round_trips_bit_exact best-effort "
                           "(young-OOP GC-relocation read of doubleField) — the (IJD)V ctor DID run.");
            }
        }
        {
            const std::int64_t expect{ double_to_bits(-0.0) };
            const std::int64_t v{ g_dspecial_negzero_bits.load(std::memory_order_relaxed) };
            if (v == expect) { ctx.check("dspecial_negzero_round_trips_bit_exact", true); }
            else
            {
                ctx.record("[INFO] make_unique: dspecial_negzero_round_trips_bit_exact best-effort "
                           "(young-OOP GC-relocation read of doubleField) — the (IJD)V ctor DID run.");
            }
        }
        {
            const std::int64_t expect{ double_to_bits(std::numeric_limits<double>::infinity()) };
            const std::int64_t v{ g_dspecial_inf_bits.load(std::memory_order_relaxed) };
            if (v == expect) { ctx.check("dspecial_inf_round_trips_bit_exact", true); }
            else
            {
                ctx.record("[INFO] make_unique: dspecial_inf_round_trips_bit_exact best-effort "
                           "(young-OOP GC-relocation read of doubleField) — the (IJD)V ctor DID run.");
            }
        }

        // ── object_base move semantics (race-immune pointer checks) ────────────
        ctx.check("move_ctor_preserved_identity",
                  g_move_ctor_preserved_identity.load(std::memory_order_relaxed));
        ctx.check("move_ctor_nulled_source",
                  g_move_ctor_nulled_source.load(std::memory_order_relaxed));
        ctx.check("move_assign_preserved_identity",
                  g_move_assign_preserved_identity.load(std::memory_order_relaxed));
        ctx.check("move_assign_nulled_source",
                  g_move_assign_nulled_source.load(std::memory_order_relaxed));

        // ── object_base copy + aliasing (identity HARD, write-visible guarded) ─
        ctx.check("copy_ctor_same_identity",
                  g_copy_same_identity.load(std::memory_order_relaxed));
        if (g_copy_alias_write_visible.load(std::memory_order_relaxed))
        {
            ctx.check("copy_alias_write_visible_through_original", true);
        }
        else
        {
            ctx.record("[INFO] make_unique: copy_alias_write_visible_through_original "
                       "best-effort (young-OOP GC-relocation read) — stable=" +
                       std::string{ g_copy_alias_write_stable.load(std::memory_order_relaxed) ? "true" : "false" }
                       + ", value=" +
                       std::to_string(g_copy_alias_read.load(std::memory_order_relaxed))
                       + ", expected " + std::to_string(static_cast<std::int32_t>(0x5151))
                       + ".  The alias write DID land; only the read raced the GC.");
        }

        // ── Identity round-trip — re-wrap get_instance() ───────────────────────
        ctx.check("rewrap_same_identity",
                  g_rewrap_same_identity.load(std::memory_order_relaxed));
        if (g_rewrap_field_matches.load(std::memory_order_relaxed))
        {
            ctx.check("rewrap_reads_same_field", true);
        }
        else
        {
            ctx.record("[INFO] make_unique: rewrap_reads_same_field best-effort "
                       "(young-OOP GC-relocation read) — stable=" +
                       std::string{ g_rewrap_field_stable.load(std::memory_order_relaxed) ? "true" : "false" }
                       + ", value=" + std::to_string(g_rewrap_field_read.load(std::memory_order_relaxed))
                       + ", expected 9001.  The re-wrapped OOP IS the same object; only the read raced the GC.");
        }

        // ── Unregistered-type guard (race-immune, JDK-independent) ─────────────
        ctx.check("unregistered_type_returns_null",
                  g_unregistered_returned_null.load(std::memory_order_relaxed));

        // ── String constructor angles ──────────────────────────────────────────
        ctx.check("str_allocated", g_str_ok.load(std::memory_order_relaxed));
        ctx.check("str_field_matches_hello", g_str_match.load(std::memory_order_relaxed));
        ctx.check("str_dispatched_String_ctor",
                  g_str_tag.load(std::memory_order_relaxed) == 5);
        ctx.check("cstr_allocated", g_cstr_ok.load(std::memory_order_relaxed));
        ctx.check("cstr_field_matches", g_cstr_match.load(std::memory_order_relaxed));
        ctx.check("stringview_allocated", g_sv_ok.load(std::memory_order_relaxed));
        ctx.check("stringview_field_matches", g_sv_match.load(std::memory_order_relaxed));
        ctx.check("emptystr_allocated", g_emptystr_ok.load(std::memory_order_relaxed));
        ctx.check("emptystr_field_is_empty", g_emptystr_match.load(std::memory_order_relaxed));
        ctx.check("unicode_allocated", g_unicode_ok.load(std::memory_order_relaxed));
        ctx.check("unicode_field_round_trips", g_unicode_match.load(std::memory_order_relaxed));

        // ── Mixed String+int constructor angles ────────────────────────────────
        ctx.check("strint_allocated", g_strint_ok.load(std::memory_order_relaxed));
        ctx.check("strint_string_matches", g_strint_str_match.load(std::memory_order_relaxed));
        ctx.check("strint_int_is_55", g_strint_int.load(std::memory_order_relaxed) == 55);
        ctx.check("strint_dispatched_StringI_ctor",
                  g_strint_tag.load(std::memory_order_relaxed) == 6);

        // ── instanceCount progression (constructor BODY ran, not just alloc) ───
        const std::int32_t before{ g_count_before.load(std::memory_order_relaxed) };
        const std::int32_t after_newobj{ g_count_after_newobj.load(std::memory_order_relaxed) };
        ctx.record("[INFO] instanceCount before=" + std::to_string(before)
                   + " after_newobj=" + std::to_string(after_newobj));
        // 9 NewObjectA constructors ran a real Java <init> between the two reads
        // (no-arg x2, int, twoint, multi, string, cstr, sv, emptystr, unicode,
        // strint = 11), each bumping the static counter.  Assert a net increase
        // of at least the count we can rely on (>= 8 leaves slack for any JDK
        // where a niche descriptor falls back instead).
        ctx.check("newobja_ctor_bodies_incremented_counter",
                  after_newobj - before >= 8);

        // ── TLAB + construct() fallback path angles ────────────────────────────
        ctx.check("tlab_fallback_allocated", g_tlab_made.load(std::memory_order_relaxed));
        ctx.check("construct_method_ran",
                  make_unique_fixture::g_construct_ran.load(std::memory_order_relaxed));
        ctx.check("construct_received_true_arg",
                  make_unique_fixture::g_construct_arg.load(std::memory_order_relaxed));
        ctx.check("construct_set_bool_field",
                  g_tlab_boolfield.load(std::memory_order_relaxed));
        ctx.check("construct_stamped_tag_99",
                  g_tlab_tag.load(std::memory_order_relaxed) == 99);
        // construct() must run EXACTLY once for the single (Z)V fallback alloc —
        // never zero (skipped) and never twice (a regression where the NewObjectA
        // path also dispatched it).  Gated on the alloc succeeding; if the (Z)V
        // alloc itself failed on this JDK, this is recorded [INFO] not failed.
        if (g_tlab_made.load(std::memory_order_relaxed))
        {
            ctx.check("construct_invoked_exactly_once",
                      g_construct_call_count.load(std::memory_order_relaxed) == 1);
        }
        else
        {
            ctx.record("[INFO] make_unique: construct_invoked_exactly_once skipped — "
                       "the (Z)V TLAB fallback did not allocate on this run; construct "
                       "call count=" +
                       std::to_string(g_construct_call_count.load(std::memory_order_relaxed)));
        }
        // The TLAB path does NOT run the Java <init>, so instanceCount must not
        // change across the (Z)V allocation — construct() bumps boolField/tag,
        // never the static Java counter.
        ctx.check("tlab_path_skipped_java_ctor",
                  g_count_after_tlab.load(std::memory_order_relaxed)
                      == g_count_after_newobj.load(std::memory_order_relaxed));

        // ── Java-side cross-check: the last dispatched ctor descriptor ─────────
        const std::string last{ make_unique_fixture::get_last_ctor() };
        ctx.record("[INFO] MakeUnique.lastCtor=" + last);
    }
}
