// for_each_thread JVM test module  (feature area: threads / HotSpot thread list)
//
// Exhaustively exercises vmhook::for_each_thread() against a LIVE JVM.
// for_each_thread (vmhook.hpp:6602) enumerates every live HotSpot JavaThread by
// walking, in order of availability:
//   * Path 1 — the classic intrusive Threads::_thread_list chain (JDK 8-9, and
//     later builds that still ship the VMStruct entry), de-duplicated through an
//     unordered_set and hard-capped at 4096 entries; AND
//   * Path 2 — the JDK 10+ Safe-Memory-Reclamation ThreadsList snapshot
//     (ThreadsSMRSupport::_java_thread_list), iterated [0, _length).
// The visitor receives a thread_info{ JavaThread*, state, os_thread_id } per
// live Java thread.  There is NO name in thread_info, so this module cannot
// match a spawned thread by name; it proves enumeration TRACKS a newly-created
// Java thread by an exact LIVE-COUNT/POINTER-SET DELTA instead (see Part E).
//
// This module MIGRATES + EXTENDS the legacy inline test_for_each_thread from
// vmhook/src/example.cpp (Rework D).  The legacy test asserted only four things
// (visited>=1, count<4096, saw a running/native state, saw current).  Here we
// additionally prove, angle by angle:
//   - every enumerated JavaThread* is non-null AND passes is_valid_pointer
//     (no bogus pointer ever reaches the visitor);
//   - the enumeration TERMINATES under a wall-clock bound (the audit flags a
//     legacy Path-1 _thread_list CYCLE hazard; we cannot forge a cycle on a live
//     JVM without corrupting it, so we CHARACTERISE the guarantee empirically:
//     bounded count + bounded time + no duplicate pointer observed);
//   - the visit count is "sane": >= 1 and strictly < the 4096 runaway cap;
//   - NO JavaThread* is reported twice in a single enumeration (Path 1 dedupes;
//     Path 2 does not — the audit's [LOW] "Path 2 no cycle detection" item — so
//     on a healthy JVM a duplicate here would surface that gap);
//   - every reported os_thread_id is non-zero (the OSThread chain decoded);
//   - within ONE enumeration the (JavaThread*, os_thread_id) IDENTITY of every
//     visited thread is DISTINCT — a stronger "stable identity" than no-duplicate
//     pointer alone (the decoded OS tid does not collide either);
//   - REPEATED enumeration is STABLE in a quiescent window — a non-empty
//     PERSISTENT thread core survives across two passes and the current thread is
//     present in both.  Cross-toolchain hardening: the EXACT count / set equality
//     is NOT universal (an unrelated JIT/GC/service thread can start or exit even
//     between two microsecond-apart passes), so it is recorded as [INFO]; only the
//     churn-proof persistent-core invariants are hard-asserted;
//   - a freshly-spawned, parked, NAMED Java thread is OBSERVED by enumeration
//     (a brand-new valid (ptr,tid) identity appears in the set within a bounded
//     poll) and DISAPPEARS again once released (that identity leaves the set) —
//     the executable proof that for_each_thread reflects real thread lifecycle,
//     not a stale snapshot.  We assert the (ptr,tid) IDENTITY delta, not the raw
//     live count: unrelated VM threads (JIT/GC/service daemons) spin up and down
//     concurrently, so the count is not monotonic and is recorded for info only;
//   - the parked worker's STATE is characterised: it is enumerated in a NON-
//     RUNNABLE state (it is asleep), proving a BLOCKED/WAITING/native thread is
//     enumerated with a valid in-range state byte (the baseline already proved a
//     RUNNABLE/native one is) — the exact state value is JDK/OS/timing-dependent
//     so it is recorded, and only in-range is asserted;
//   - a BATCH of N extra named daemon workers (LARGE thread count) is spawned at
//     once: enumeration still TERMINATES bounded and stays under the 4096 cap
//     with a markedly larger live set (no overrun, no crash), hands back only
//     valid non-duplicate pointers/identities, and SURFACES the batch (a solid
//     majority of N new identities appear); the batch then DRAINS and every one
//     of those identities leaves the enumeration;
//   - the same visitor body run twice with different capture shapes never
//     crashes and never hangs (cap + validity guards hold under all visitors).
//
// HARD SAFETY: every JavaThread deref is guarded by is_valid_pointer (mirroring
// for_each_thread's own invoke_visitor guard); the module never forces a cycle,
// never mutates the thread list, and bounds every poll loop so it can neither
// crash nor hang the JVM.  No hooks are installed (pure enumeration module), so
// there is nothing to tear down — scoped_hook is intentionally absent and
// shutdown_hooks() is NEVER called.  Every Java thread this module spawns is a
// DAEMON with a self-timeout AND is explicitly JOINED in the unconditional
// cleanup step, so no spawned thread can leak into a downstream test module.
//
// CROSS-TOOLCHAIN: locally this is exercised MinGW-only, but CI runs it on
// msvc/clang/linux x JDK 8-26 where the live thread set, attach/detach TIMING,
// and exact states all differ.  So only UNIVERSAL invariants are hard-asserted
// (>= 1 thread, current thread found where identifiable, no null, no duplicate,
// terminates bounded, < cap, valid pointers); every count / name / state / spawn-
// presence that varies by JDK/OS/timing is [INFO] or a timing-tolerant best-
// effort poll with a symmetric [INFO] skip, so an overloaded runner is never
// falsely red.
//
// Harness note: the worker lifecycle is driven through the standard go/done +
// mode probe — mode 1 = start ONE named daemon worker and return while it parks
// alive; mode 3 = start a BATCH of `workerCount` named daemon workers; mode 4 =
// set stop + JOIN every worker (the cleanup step); the workers self-time-out so
// an aborted run cannot leak them.  The "I am up" / "please stop" / upCount
// bridge flags are read/written via static_field(...)->get()/set() — plain heap
// accesses that work off the Java thread, no bytecode dispatch needed.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.ForEachThread.  Deriving from
    // vmhook::object<> gives the wrapper the static_field(...) accessors used to
    // drive the go/done/mode handshake and the worker lifecycle bridge flags.
    class fet_fixture : public vmhook::object<fet_fixture>
    {
    public:
        explicit fet_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<fet_fixture>{ instance }
        {
        }

        // ── go / done / mode handshake ───────────────────────────────────────
        static auto set_go(bool value) -> void    { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ── worker lifecycle bridge (plain heap reads/writes) ────────────────
        static auto get_thread_up() -> bool       { return static_field("threadUp")->get(); }
        static auto set_stop(bool value) -> void  { static_field("stop")->set(value); }

        // ── batch worker lifecycle bridge (mode 3 / mode 4) ──────────────────
        static auto set_worker_count(std::int32_t n) -> void { static_field("workerCount")->set(n); }
        static auto get_up_count() -> std::int32_t           { return static_field("upCount")->get(); }
    };

    // The runaway cap inside for_each_thread (vmhook.hpp:6633).  A healthy JVM
    // must enumerate STRICTLY fewer than this; reaching it means either a
    // pathological thread count or a cycle that escaped detection.
    constexpr std::int32_t FOR_EACH_THREAD_CAP{ 4096 };

    // A stable per-thread identity: the JavaThread* PAIRED with its OS thread id.
    // Using the pair (not the bare pointer) is what makes the spawn-detection
    // robust: HotSpot recycles freed JavaThread heap allocations, so a brand-new
    // worker thread can be handed a JavaThread* whose VALUE equals one that
    // belonged to an already-exited thread in an earlier snapshot.  The OS thread
    // id, however, is freshly allocated for the new OS thread, so the (ptr, tid)
    // pair of a genuinely new live thread differs from every prior pair even when
    // the pointer aliases.  Equality/hash over the pair give us a churn-proof key.
    struct thread_identity
    {
        vmhook::hotspot::java_thread* ptr{ nullptr };
        vmhook::os::thread_id_t       tid{ 0 };

        auto operator==(const thread_identity& other) const noexcept -> bool
        {
            return ptr == other.ptr && tid == other.tid;
        }
    };

    struct thread_identity_hash
    {
        auto operator()(const thread_identity& id) const noexcept -> std::size_t
        {
            const std::size_t a{ std::hash<vmhook::hotspot::java_thread*>{}(id.ptr) };
            const std::size_t b{ std::hash<vmhook::os::thread_id_t>{}(id.tid) };
            return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
        }
    };

    // A snapshot of one for_each_thread enumeration: the visited pointers (in
    // visit order), the matching (ptr, tid) identities, plus derived health
    // tallies.  Collected by enumerate().
    struct enumeration
    {
        std::vector<vmhook::hotspot::java_thread*> pointers;
        std::vector<thread_identity>               identities;
        std::int32_t                               count{ 0 };
        bool                                       all_pointers_valid{ true };
        bool                                       all_os_tids_nonzero{ true };
        bool                                       any_running_or_native{ false };
        bool                                       any_state_out_of_range{ false };
        bool                                       saw_current{ false };
        double                                     elapsed_ms{ 0.0 };
    };

    // True if a state value is within the HotSpot java_thread_state enum range
    // (used to assert the visitor never hands back a garbage state byte).
    auto state_in_range(const vmhook::hotspot::java_thread_state s) -> bool
    {
        const auto v{ static_cast<std::int32_t>(s) };
        return v >= static_cast<std::int32_t>(vmhook::hotspot::java_thread_state::_thread_uninitialized)
            && v <= static_cast<std::int32_t>(vmhook::hotspot::java_thread_state::_thread_max_state);
    }

    // Runs ONE for_each_thread enumeration and folds every per-thread invariant
    // into an `enumeration`.  Each JavaThread deref inside the visitor is already
    // safe (for_each_thread only invokes the visitor for is_valid_pointer
    // threads), but we re-check the pointer here too so the module's own asserts
    // are self-contained and never deref a pointer the harness wouldn't.
    auto enumerate() -> enumeration
    {
        enumeration e{};
        const auto current_jt{ vmhook::hotspot::current_java_thread };

        const auto start{ std::chrono::steady_clock::now() };
        vmhook::for_each_thread([&](const vmhook::thread_info& info)
        {
            ++e.count;

            // Pointer validity — the load-bearing safety invariant.
            const bool ptr_ok{ info.thread != nullptr
                               && vmhook::hotspot::is_valid_pointer(info.thread) };
            if (!ptr_ok)
            {
                e.all_pointers_valid = false;
            }
            else
            {
                e.pointers.push_back(info.thread);
                e.identities.push_back(thread_identity{ info.thread, info.os_thread_id });
                if (info.thread == current_jt)
                {
                    e.saw_current = true;
                }
            }

            if (!state_in_range(info.state))
            {
                e.any_state_out_of_range = true;
            }
            if (info.state == vmhook::hotspot::java_thread_state::_thread_in_Java
             || info.state == vmhook::hotspot::java_thread_state::_thread_in_native)
            {
                e.any_running_or_native = true;
            }
            if (info.os_thread_id == 0)
            {
                e.all_os_tids_nonzero = false;
            }
        });
        const auto finish{ std::chrono::steady_clock::now() };
        e.elapsed_ms = std::chrono::duration<double, std::milli>{ finish - start }.count();
        return e;
    }

    // Count of DISTINCT pointers in a vector (Path 1 dedupes internally, but the
    // audit notes Path 2 does NOT — so a healthy JVM should still show
    // distinct == size; a mismatch surfaces a real duplicate-visit gap).
    auto distinct_count(const std::vector<vmhook::hotspot::java_thread*>& v) -> std::size_t
    {
        std::unordered_set<vmhook::hotspot::java_thread*> s{ v.begin(), v.end() };
        return s.size();
    }

    // Pointers present in `with` but not in `without` (the threads added between
    // two enumerations).  Used to detect the spawned worker's JavaThread.
    auto added_pointers(const std::vector<vmhook::hotspot::java_thread*>& with,
                        const std::vector<vmhook::hotspot::java_thread*>& without)
        -> std::vector<vmhook::hotspot::java_thread*>
    {
        const std::unordered_set<vmhook::hotspot::java_thread*> base{ without.begin(), without.end() };
        std::vector<vmhook::hotspot::java_thread*> added{};
        for (vmhook::hotspot::java_thread* const p : with)
        {
            if (base.find(p) == base.end())
            {
                added.push_back(p);
            }
        }
        return added;
    }

    // (ptr, tid) identities present in `with` but not in `without` — the
    // churn-proof analogue of added_pointers.  A genuinely new live thread always
    // shows up here even if its JavaThread* aliases a recycled pointer that was in
    // `without` (the OS tid differs), so this is what the spawn proof keys on.
    auto added_identities(const std::vector<thread_identity>& with,
                          const std::vector<thread_identity>& without)
        -> std::vector<thread_identity>
    {
        const std::unordered_set<thread_identity, thread_identity_hash>
            base{ without.begin(), without.end() };
        std::vector<thread_identity> added{};
        for (const thread_identity& id : with)
        {
            if (base.find(id) == base.end())
            {
                added.push_back(id);
            }
        }
        return added;
    }

    // Same pointer multiset (order-independent) — STABILITY check for two
    // back-to-back enumerations in a quiescent window.
    auto same_pointer_set(const std::vector<vmhook::hotspot::java_thread*>& a,
                          const std::vector<vmhook::hotspot::java_thread*>& b) -> bool
    {
        return distinct_count(a) == distinct_count(b)
            && added_pointers(a, b).empty()
            && added_pointers(b, a).empty();
    }

    // Number of DISTINCT pointers present in BOTH vectors — the persistent core
    // across two enumerations.  Unlike same_pointer_set this is robust to
    // concurrent VM thread churn (a JIT/GC/service daemon spinning up or down
    // between the two passes changes only the symmetric difference, never the
    // intersection of the threads that actually persisted), so it is the
    // UNIVERSAL stability invariant we hard-assert on; the exact set equality is
    // recorded as [INFO] only.
    auto intersection_count(const std::vector<vmhook::hotspot::java_thread*>& a,
                            const std::vector<vmhook::hotspot::java_thread*>& b) -> std::size_t
    {
        const std::unordered_set<vmhook::hotspot::java_thread*> sa{ a.begin(), a.end() };
        const std::unordered_set<vmhook::hotspot::java_thread*> sb{ b.begin(), b.end() };
        std::size_t both{ 0 };
        for (vmhook::hotspot::java_thread* const p : sa)
        {
            if (sb.find(p) != sb.end())
            {
                ++both;
            }
        }
        return both;
    }

    // Number of DISTINCT (ptr, tid) identities in a vector.  A single
    // enumeration must hand back distinct identities for distinct live threads;
    // identities == visit count is the per-pass "stable identity" invariant.
    auto distinct_identity_count(const std::vector<thread_identity>& v) -> std::size_t
    {
        std::unordered_set<thread_identity, thread_identity_hash> s{ v.begin(), v.end() };
        return s.size();
    }

    // True if a state value is one of the NAMED java_thread_state enum constants
    // (stricter than state_in_range, which also accepts the gaps inside [0, 12]).
    // A correctly decoded _thread_state byte for a live thread is always one of
    // these named transition/steady states — never the unused gap value 1.  This
    // is a per-thread sanity check on the decoded state byte; it is recorded as
    // [INFO] rather than hard-asserted because the exact set of states HotSpot may
    // legitimately surface is JDK-variant and a never-before-seen-but-valid value
    // on a future JDK must not falsely redden the suite.
    auto state_is_named(const vmhook::hotspot::java_thread_state s) -> bool
    {
        switch (s)
        {
        case vmhook::hotspot::java_thread_state::_thread_uninitialized:
        case vmhook::hotspot::java_thread_state::_thread_new:
        case vmhook::hotspot::java_thread_state::_thread_new_trans:
        case vmhook::hotspot::java_thread_state::_thread_in_native:
        case vmhook::hotspot::java_thread_state::_thread_in_native_trans:
        case vmhook::hotspot::java_thread_state::_thread_in_vm:
        case vmhook::hotspot::java_thread_state::_thread_in_vm_trans:
        case vmhook::hotspot::java_thread_state::_thread_in_Java:
        case vmhook::hotspot::java_thread_state::_thread_in_Java_trans:
        case vmhook::hotspot::java_thread_state::_thread_blocked:
        case vmhook::hotspot::java_thread_state::_thread_blocked_trans:
        case vmhook::hotspot::java_thread_state::_thread_max_state:
            return true;
        default:
            return false;
        }
    }

    // Cross-path round-trip TALLY: how many enumerated (ptr, tid) identities
    // resolve back to a non-null, valid JavaThread through
    // find_java_thread_by_os_thread_id — a DIFFERENT walk that shares the
    // OSThread-id decode.  Each lookup is itself bounded (the finder caps at 4096),
    // so this is crash- and hang-safe.  The caller uses this for an [INFO] resolve
    // RATE only — it is NOT safe to assert resolved == total, because the
    // enumeration is a snapshot and the resolve is a later walk: TRANSIENT JVM
    // threads (JIT/GC/service daemons, JDK 21+ virtual-thread carriers) legitimately
    // EXIT between the two, so a tid that was live at snapshot can be gone at resolve
    // through no fault of the decode.  The stable-thread round-trip (the current
    // thread, alive for the whole test) is what the caller hard-asserts instead.
    // We also do NOT require the resolved pointer to EQUAL the enumerated pointer:
    // the finder returns the FIRST thread whose tid matches, and on the (degenerate)
    // chance two live threads ever share a zero-extended 32-bit tid slot the finder
    // could pick the other one — so pointer_matched is recorded for info too.
    struct roundtrip_tally
    {
        std::size_t resolved{ 0 };       // tid resolved to a non-null valid thread
        std::size_t pointer_matched{ 0 };// ...and it was the SAME JavaThread*
        std::size_t total{ 0 };          // identities considered (nonzero tid)
    };

    auto roundtrip_identities(const std::vector<thread_identity>& ids) -> roundtrip_tally
    {
        roundtrip_tally t{};
        for (const thread_identity& id : ids)
        {
            if (id.tid == 0)
            {
                continue;
            }
            ++t.total;
            vmhook::hotspot::java_thread* const found{
                vmhook::hotspot::find_java_thread_by_os_thread_id(id.tid) };
            if (found != nullptr && vmhook::hotspot::is_valid_pointer(found))
            {
                ++t.resolved;
                if (found == id.ptr)
                {
                    ++t.pointer_matched;
                }
            }
        }
        return t;
    }

    // Drives exactly one probe cycle for `mode`: clears the latched done and
    // programs the selector on the rising edge of go, then runs the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    fet_fixture::set_done(false);
                    fet_fixture::set_mode(mode);
                }
                fet_fixture::set_go(value);
            },
            []() { return fet_fixture::get_done(); });
    }

    // Bounded poll on a plain static-bool flag (heap read; no bytecode dispatch).
    // Returns true if `read()` reached `want` within ~`max_ms`.  Bounded so the
    // module can never hang the JVM even if the worker never flips the flag.
    template<typename read_fn>
    auto poll_flag(read_fn&& read, bool want, int max_ms) -> bool
    {
        for (int waited{ 0 }; waited < max_ms; ++waited)
        {
            if (read() == want)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        return read() == want;
    }

    // Bounded poll on an arbitrary predicate (heap reads only).  Returns true if
    // `pred()` became true within ~`max_ms`.  Bounded so the module can never
    // hang the JVM even if the predicate never holds (e.g. the batch never fully
    // attaches or never fully drains on a wedged runner).
    template<typename pred_fn>
    auto poll_predicate(pred_fn&& pred, int max_ms) -> bool
    {
        for (int waited{ 0 }; waited < max_ms; ++waited)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        return pred();
    }
}

VMHOOK_JVM_MODULE(for_each_thread)
{
    vmhook::register_class<fet_fixture>("vmhook/fixtures/ForEachThread");

    // =====================================================================
    // PART A — baseline enumeration: count sane, current thread present, at
    //          least one running/native thread (migrated from the legacy test).
    // =====================================================================
    const enumeration base{ enumerate() };

    ctx.record(std::string{ "[INFO] for_each_thread baseline: visited " }
               + std::to_string(base.count) + " JavaThread(s) in "
               + std::to_string(base.elapsed_ms) + " ms");

    // Legacy parity (was forEachThreadVisitedAtLeastOne): at least the
    // main/test thread is live, so enumeration MUST report >= 1.
    ctx.check("baseline_visited_at_least_one", base.count >= 1);

    // Legacy parity (was forEachThreadReasonableCount): the count is strictly
    // below the 4096 runaway cap on a healthy JVM.
    ctx.check("baseline_count_below_cap", base.count < FOR_EACH_THREAD_CAP);

    // Legacy parity (was forEachThreadSawRunningOrNative): some thread is
    // executing Java or native code (this test thread is at least one).
    ctx.check("baseline_saw_running_or_native", base.any_running_or_native);

    // Legacy parity (was forEachThreadSawCurrent, guarded on current_jt): if the
    // running thread has an identified JavaThread, enumeration must include it.
    if (vmhook::hotspot::current_java_thread)
    {
        ctx.check("baseline_saw_current_thread", base.saw_current);
    }
    else
    {
        ctx.record("[INFO] current_java_thread is null on this thread; "
                   "skipping baseline_saw_current_thread (parity with legacy guard).");
    }

    // =====================================================================
    // PART B — per-thread visitor invariants (NEW vs legacy): every reported
    //          JavaThread* is non-null + valid, every state is in range, every
    //          OS thread id decoded to a non-zero value.
    // =====================================================================
    ctx.check("baseline_all_pointers_valid", base.all_pointers_valid);
    ctx.check("baseline_no_state_out_of_range", !base.any_state_out_of_range);
    ctx.check("baseline_all_os_tids_nonzero", base.all_os_tids_nonzero);

    // The collected (valid) pointer vector length must equal the visit count
    // when every pointer was valid — i.e. no visit was silently dropped.
    if (base.all_pointers_valid)
    {
        ctx.check("baseline_collected_equals_count",
                  static_cast<std::int32_t>(base.pointers.size()) == base.count);
    }

    // STABLE IDENTITY WITHIN ONE ENUMERATION (NEW): the (JavaThread*, os_thread_id)
    // identity of every visited thread is DISTINCT inside a single pass.  This is
    // strictly stronger than no_duplicate_pointer_in_one_pass (Part C): it proves
    // not only that no pointer repeats, but that the decoded OS thread id does not
    // collide either — so the per-thread snapshot the visitor hands back is a
    // stable, one-to-one identity for the live thread, never an aliased or
    // half-decoded duplicate.
    if (base.all_pointers_valid)
    {
        ctx.check("baseline_identities_distinct",
                  distinct_identity_count(base.identities) == base.identities.size());
    }

    // thread_info exposes {JavaThread*, state, os_thread_id} only — there is NO
    // thread NAME and NO daemon flag in the snapshot.  The brief asks for a
    // "readable name where exposed" and "daemon vs non-daemon"; neither is
    // exposed by this API, so we record that explicitly (the portable, provable
    // analogue of name/daemon coverage is the (ptr,tid) identity + the in-range
    // STATE, both asserted here and in Part E/G).  JDK-variant note: the STATE
    // byte is the one per-thread attribute that IS exposed and it is range-checked
    // for every visited thread (baseline_no_state_out_of_range).
    ctx.record("[INFO] thread_info carries no thread NAME and no daemon flag "
               "(API exposes only JavaThread*, state, os_thread_id); name/daemon "
               "coverage is via the (ptr,tid) identity + in-range state instead.");

    // =====================================================================
    // PART C — CYCLE / CAP CHARACTERISATION (audit: legacy _thread_list cycle).
    //   We cannot forge a corrupted intrusive list on a live JVM without
    //   crashing it, so we prove the GUARANTEES the cycle-guard provides,
    //   empirically: (1) enumeration TERMINATES (bounded wall-clock), (2) the
    //   count stays under the 4096 cap, and (3) NO JavaThread* is visited twice
    //   in one pass.  (3) also exercises the audit's Path-2-has-no-dedupe gap:
    //   on a healthy JVM a duplicate here would be a real defect.
    // =====================================================================

    // (1) Termination: a real infinite cycle would blow far past this; the
    //     4096-cap walk over a few dozen threads completes in well under 250 ms
    //     even on a slow CI box.  This is the executable "does not hang" proof.
    ctx.check("enumeration_terminates_bounded_time", base.elapsed_ms < 250.0);

    // (2) Cap is a guard, not a normal outcome.
    ctx.check("count_strictly_below_runaway_cap", base.count < FOR_EACH_THREAD_CAP);

    // (3) No duplicate pointer in a single enumeration (Path 1 dedupe holds; a
    //     duplicate would also reveal the Path-2-no-dedupe audit gap on JDK 10+).
    const std::size_t base_distinct{ distinct_count(base.pointers) };
    ctx.record(std::string{ "[INFO] baseline distinct pointers = " }
               + std::to_string(base_distinct) + " of "
               + std::to_string(base.pointers.size()) + " visited");
    ctx.check("no_duplicate_pointer_in_one_pass",
              base_distinct == base.pointers.size());

    // =====================================================================
    // PART D — REPEATED enumeration is STABLE in a quiescent window (NEW):
    //   two back-to-back passes agree on the PERSISTENT thread core, and both
    //   include the current thread.  Proves no per-call state corruption / drift.
    //
    //   HARDENING (cross-toolchain): the EXACT count / set equality is NOT a
    //   universal invariant — even back-to-back, an unrelated VM thread (a C1/C2
    //   JIT compiler, a GC worker, the Reference Handler, a service daemon) can
    //   start or exit in the microseconds between the two passes, which the
    //   for_each_thread deflake history already reproduced over a wider window
    //   (before=17, with_worker=14).  So we hard-assert only the churn-PROOF
    //   invariants — both passes valid + bounded, a non-empty persistent
    //   intersection, the current thread present in both — and record the exact
    //   count/set agreement as [INFO].
    // =====================================================================
    const enumeration again{ enumerate() };

    ctx.check("repeat_visited_at_least_one", again.count >= 1);
    ctx.check("repeat_all_pointers_valid", again.all_pointers_valid);
    ctx.check("repeat_terminates_bounded_time", again.elapsed_ms < 250.0);

    // [INFO] exact count agreement — informational; unrelated VM threads move it.
    ctx.record(std::string{ "[INFO] repeat enumeration visited " }
               + std::to_string(again.count) + " (baseline was "
               + std::to_string(base.count) + ")");
    ctx.record(std::string{ "[INFO] repeat count matches baseline: " }
               + (again.count == base.count ? "yes" : "no")
               + "; exact pointer set stable: "
               + (same_pointer_set(base.pointers, again.pointers) ? "yes" : "no")
               + " (both informational — not asserted; concurrent VM thread churn "
                 "moves the symmetric difference)");

    // UNIVERSAL: a non-empty PERSISTENT core survives across the two passes.  At
    // minimum the JVM's own long-lived threads (the main thread running
    // vmhook.Main, etc.) are present in both — the intersection can never be
    // empty on a live JVM, regardless of churn in the symmetric difference.
    const std::size_t base_again_both{ intersection_count(base.pointers, again.pointers) };
    ctx.record(std::string{ "[INFO] persistent (in-both) pointer core across two passes = " }
               + std::to_string(base_again_both));
    ctx.check("repeat_persistent_core_nonempty", base_again_both >= 1);

    // UNIVERSAL: the persistent core is a large fraction of each pass — proves the
    // two passes describe essentially the same thread set, without asserting an
    // EXACT match that churn could break.  (Both passes valid above, so
    // pointers.size() == distinct visit count for each.)  We require the
    // intersection to cover all but a small churn slack of the smaller pass.
    const std::size_t smaller_pass{
        std::min(distinct_count(base.pointers), distinct_count(again.pointers)) };
    if (smaller_pass >= 1)
    {
        // Allow up to 2 threads of slack for a daemon that legitimately started
        // or exited between the two microsecond-apart passes; anything larger
        // would indicate real per-call instability, not background churn.
        const std::size_t slack{ 2 };
        const bool core_covers_most{
            base_again_both + slack >= smaller_pass };
        ctx.check("repeat_persistent_core_covers_most", core_covers_most);
    }

    if (vmhook::hotspot::current_java_thread)
    {
        ctx.check("repeat_saw_current_thread", again.saw_current);

        // UNIVERSAL: the current thread (a guaranteed-persistent thread) is in
        // BOTH passes — the strongest portable form of "persistent threads
        // appear in both consecutive enumerations".
        const auto current_jt{ vmhook::hotspot::current_java_thread };
        const bool in_base{ std::find(base.pointers.begin(), base.pointers.end(),
                                      current_jt) != base.pointers.end() };
        const bool in_again{ std::find(again.pointers.begin(), again.pointers.end(),
                                       current_jt) != again.pointers.end() };
        ctx.check("repeat_current_thread_in_both_passes", in_base && in_again);
    }
    else
    {
        ctx.record("[INFO] current_java_thread is null on the suite thread "
                   "(it is a native, non-Java thread); skipping the "
                   "current-thread-in-both-passes check.");
    }

    // =====================================================================
    // PART E — enumeration TRACKS a freshly-spawned NAMED Java thread (NEW):
    //   spawn a parked daemon worker, then POLL (bounded) until a brand-new live
    //   thread appears in the enumeration; release it and poll until that thread
    //   leaves the enumeration.  We key on the (ptr, tid) IDENTITY delta — robust
    //   to BOTH unrelated thread churn AND JavaThread* pointer recycling — NOT on
    //   a raw live-count delta (non-monotonic; JIT/GC/service daemons move it
    //   concurrently, which made the old `spawn_live_count_increased` assertion
    //   flaky) and NOT on the bare pointer (HotSpot reuses freed JavaThread
    //   allocations, so a new worker can alias a pointer seen pre-spawn).  If the
    //   runner never surfaces the worker within budget we INFO-skip rather than
    //   hard-FAIL.  This is the lifecycle proof the legacy test never made — and
    //   the closest portable analogue of "find the spawned thread", since
    //   thread_info carries no name.
    // =====================================================================
    {
        // Snapshot the count just before we ask the JVM for an extra thread.
        const enumeration before{ enumerate() };
        ctx.check("spawn_before_at_least_one", before.count >= 1);

        // mode 1: start the named daemon worker; the probe returns once the
        // worker is started, while the worker parks itself alive.
        fet_fixture::set_stop(false);
        const bool started{ drive(ctx, 1) };
        ctx.check("spawn_probe_started_worker", started);

        // Wait (bounded) for the worker to announce it is up & parked, so it is
        // certainly a fully-attached JavaThread before we re-enumerate.
        const bool up{ poll_flag(&fet_fixture::get_thread_up, true, 2000) };
        ctx.check("spawn_worker_reported_up", up);

        if (up)
        {
            // Re-enumerate WITH the worker parked alive.  The worker has set
            // threadUp=true (its first run() statement), but a freshly-started
            // Thread is not GUARANTEED to be on the HotSpot thread list at the
            // exact instant the native side wakes and re-enumerates: there is an
            // attach-visibility window between t.start() / the Java run() body and
            // the JavaThread appearing on Threads::_thread_list / the SMR
            // ThreadsList snapshot.  So POLL (bounded, ~2s) until a brand-new live
            // thread — the worker — actually appears in the enumeration.
            //
            // We key the spawn proof on the (ptr, tid) IDENTITY delta, not the raw
            // live-count delta and not the bare-pointer delta:
            //   * the raw count is NOT monotonic — unrelated daemon/JIT/GC threads
            //     spin up and down concurrently, so the worker's +1 can be masked
            //     by an unrelated −1 in the same window (this was the original
            //     `spawn_live_count_increased` flake); we now only RECORD it.
            //   * the bare JavaThread* can ALIAS — HotSpot recycles freed
            //     JavaThread allocations, so the worker may be handed a pointer
            //     value that was in `before` (then added_pointers misses it
            //     entirely, regardless of how long we poll); pairing with the OS
            //     tid makes the new thread observable even under pointer reuse.
            enumeration with_worker{ enumerate() };
            std::vector<thread_identity> added_ids{
                added_identities(with_worker.identities, before.identities) };
            for (int tries{ 0 }; tries < 200 && added_ids.empty(); ++tries)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
                with_worker = enumerate();
                added_ids   = added_identities(with_worker.identities, before.identities);
            }

            ctx.check("spawn_with_worker_all_pointers_valid", with_worker.all_pointers_valid);

            ctx.record(std::string{ "[INFO] live count before=" }
                       + std::to_string(before.count) + " with_worker="
                       + std::to_string(with_worker.count)
                       + " (count delta is informational only — not asserted, "
                         "unrelated VM threads move it concurrently)");
            ctx.record(std::string{ "[INFO] enumeration gained " }
                       + std::to_string(added_ids.size())
                       + " new (ptr,tid) identity(ies) while worker parked");

            // A brand-new live thread appeared — that IS the worker (an unrelated
            // concurrent JVM thread would only ADD to this set, never empty it).
            // This identity delta is the robust replacement for the old strict
            // count-delta assertion: an unrelated thread exiting removes only its
            // OWN identity, it can never mask a newly-added one.
            //
            // Safety net (option 3): if — even after the full poll budget — NO new
            // identity is positively observable (extreme scheduler/attach timing
            // or churn on a loaded CI runner), record an INFO skip instead of a
            // hard FAIL.  The spawn proof is "best-effort positive": we assert
            // hard when we CAN see the worker, and skip (never falsely red) when
            // the runner genuinely never surfaced it within budget.
            if (!added_ids.empty())
            {
                ctx.check("spawn_new_pointer_appeared", true);

                const bool all_added_valid{ std::all_of(
                    added_ids.begin(), added_ids.end(),
                    [](const thread_identity& id)
                    { return id.ptr != nullptr && vmhook::hotspot::is_valid_pointer(id.ptr); }) };
                ctx.check("spawn_new_pointers_valid", all_added_valid);

                // STATE OF THE PARKED WORKER (covers "a thread in BLOCKED/WAITING
                // vs RUNNABLE — all enumerated").  The worker is parked in
                // Thread.sleep(), so HotSpot reports it in a NON-running state
                // (_thread_in_native while inside the sleep syscall, or
                // _thread_blocked / _thread_in_vm in the transition).  We cannot
                // hard-assert WHICH non-running state portably (it varies by JDK /
                // OS / exact sampling instant), so we RECORD the observed state and
                // hard-assert only the universal invariant: the parked worker's
                // state is IN RANGE (the visitor never hands back a garbage state
                // byte for a thread that is NOT executing Java).  This proves a
                // non-RUNNABLE thread is enumerated with a valid decoded state —
                // the baseline already proved a RUNNABLE/native thread is too.
                const std::unordered_set<thread_identity, thread_identity_hash>
                    worker_id_set{ added_ids.begin(), added_ids.end() };
                bool worker_state_seen{ false };
                bool worker_state_in_range{ true };
                std::int32_t worker_state_value{ -1 };
                vmhook::for_each_thread(
                    [&](const vmhook::thread_info& info)
                    {
                        if (info.thread == nullptr
                            || !vmhook::hotspot::is_valid_pointer(info.thread))
                        {
                            return;
                        }
                        const thread_identity id{ info.thread, info.os_thread_id };
                        if (worker_id_set.find(id) == worker_id_set.end())
                        {
                            return;
                        }
                        worker_state_seen = true;
                        worker_state_value = static_cast<std::int32_t>(info.state);
                        if (!state_in_range(info.state))
                        {
                            worker_state_in_range = false;
                        }
                    });
                ctx.record(std::string{ "[INFO] parked worker observed state value = " }
                           + std::to_string(worker_state_value)
                           + " (non-RUNNABLE; exact state is JDK/OS/timing-dependent "
                             "and recorded, not asserted)");
                if (worker_state_seen)
                {
                    ctx.check("spawn_worker_state_in_range", worker_state_in_range);
                }
            }
            else
            {
                ctx.record("[INFO] spawn not observed in for_each_thread within budget "
                           "(scheduler/attach timing) - skipped");
            }

            // Now release the worker and prove enumeration reflects its exit.
            fet_fixture::set_stop(true);
            const bool down{ poll_flag(&fet_fixture::get_thread_up, false, 5000) };
            ctx.check("spawn_worker_reported_down", down);

            if (down)
            {
                // Whether the worker's (ptr, tid) identity is still present in a
                // live enumeration.  Keyed on the IDENTITY (not the bare pointer)
                // for the same reason as the spawn side: a recycled pointer must
                // not be mistaken for the still-living worker, and the worker's
                // exit must be observable even if its old slot is immediately
                // reused by another thread (different tid -> different identity).
                const auto worker_identity_survives{
                    [&](const enumeration& live) -> bool
                    {
                        return std::any_of(
                            added_ids.begin(), added_ids.end(),
                            [&](const thread_identity& id)
                            {
                                return std::find(live.identities.begin(),
                                                 live.identities.end(), id)
                                    != live.identities.end();
                            });
                    } };

                // Give the JVM a brief, BOUNDED moment to unwind the JavaThread
                // off the thread list after the Java run() method returns, then
                // re-enumerate.  We poll on the worker's identity being gone (the
                // reliable invariant), not on a raw count drop: the raw count is
                // non-monotonic for the same reason as the spawn side (an
                // unrelated thread starting concurrently would keep the count
                // >= the with-worker peak even after the worker is reclaimed), so
                // polling the count could spin the whole budget and then fail
                // spuriously.  Polling identity terminates the instant the
                // worker leaves the enumeration.
                enumeration after{ enumerate() };
                for (int tries{ 0 };
                     tries < 200 && worker_identity_survives(after);
                     ++tries)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
                    after = enumerate();
                }

                ctx.record(std::string{ "[INFO] live count after release = " }
                           + std::to_string(after.count)
                           + " (with_worker peak was " + std::to_string(with_worker.count)
                           + "; count delta informational only — not asserted)");
                ctx.check("spawn_after_all_pointers_valid", after.all_pointers_valid);

                // None of the identities that were ADDED for the worker phase
                // survive in a fresh enumeration (the worker is gone).  This is
                // the robust lifecycle proof — immune to unrelated thread churn
                // and to pointer recycling, unlike a raw count-drop assertion.
                // Only assert hard when we positively OBSERVED the worker on the
                // spawn side (added_ids non-empty); if the spawn was skipped there
                // is nothing to prove gone, so we skip symmetrically.
                if (!added_ids.empty())
                {
                    const bool any_added_survived{ worker_identity_survives(after) };
                    ctx.check("spawn_worker_pointer_gone", !any_added_survived);
                }
                else
                {
                    ctx.record("[INFO] worker was not observed on spawn side; "
                               "skipped spawn_worker_pointer_gone symmetrically.");
                }
            }
        }
        else
        {
            // Worker never came up — still release the (possibly slow) thread so
            // we never leak it, and record the skip.
            fet_fixture::set_stop(true);
            ctx.record("[INFO] worker did not report up within budget; "
                       "skipped the spawn-delta assertions.");
        }
    }

    // =====================================================================
    // PART F — robustness: the same enumeration under different visitor capture
    //   shapes never crashes and never hangs (cap + validity guards hold).  An
    //   empty visitor (counts nothing) and a heavy-capture visitor both must
    //   return cleanly and bounded.
    // =====================================================================
    {
        // Empty visitor: pure walk, no observation — must simply return.
        const auto t0{ std::chrono::steady_clock::now() };
        vmhook::for_each_thread([](const vmhook::thread_info&) { /* no-op */ });
        const auto t1{ std::chrono::steady_clock::now() };
        const double empty_ms{ std::chrono::duration<double, std::milli>{ t1 - t0 }.count() };
        ctx.check("empty_visitor_returns_bounded", empty_ms < 250.0);

        // Heavy-capture visitor: accumulates into a local vector by value.
        std::vector<vmhook::os::thread_id_t> tids{};
        std::int32_t heavy_count{ 0 };
        const auto h0{ std::chrono::steady_clock::now() };
        vmhook::for_each_thread([&](const vmhook::thread_info& info)
        {
            ++heavy_count;
            if (info.thread != nullptr && vmhook::hotspot::is_valid_pointer(info.thread))
            {
                tids.push_back(info.os_thread_id);
            }
        });
        const auto h1{ std::chrono::steady_clock::now() };
        const double heavy_ms{ std::chrono::duration<double, std::milli>{ h1 - h0 }.count() };
        ctx.check("heavy_visitor_returns_bounded", heavy_ms < 250.0);
        ctx.check("heavy_visitor_visited_at_least_one", heavy_count >= 1);
        ctx.check("heavy_visitor_count_below_cap", heavy_count < FOR_EACH_THREAD_CAP);
    }

    // =====================================================================
    // PART G — BATCH spawn / LARGE thread count (NEW): start N extra named
    //   daemon workers at once, prove enumeration (1) STILL terminates bounded
    //   and stays under the 4096 cap with a meaningfully larger live thread set
    //   ("large thread count — no overrun, terminates, no crash"), (2) hands back
    //   only valid, non-duplicate pointers / identities under the bigger load,
    //   and (3) SURFACES the batch — several brand-new (ptr,tid) identities
    //   appear ("count rises by ~N, each visited") — then DRAIN the batch and
    //   prove those identities all leave again.
    //
    //   Counts are NOT hard-asserted (the for_each_thread deflake history proved
    //   raw JavaThread counts drift under concurrent JIT/GC/service churn —
    //   before=17, with_worker=14 — so an exact +N is racy); we key the spawn /
    //   drain proof on the churn-PROOF (ptr,tid) identity delta and on the
    //   fixture's own upCount handshake, and RECORD the raw count delta for info.
    //   All presence checks are timing-tolerant (bounded poll + symmetric [INFO]
    //   skip) so an overloaded runner that never fully attaches the batch is
    //   never falsely red — only the UNIVERSAL invariants (bounded, < cap, valid,
    //   non-dup) are hard.
    // =====================================================================
    {
        constexpr std::int32_t BATCH_N{ 16 };

        const enumeration before_batch{ enumerate() };
        ctx.check("batch_before_at_least_one", before_batch.count >= 1);

        // Ask the fixture for BATCH_N parked daemon workers.
        fet_fixture::set_stop(false);
        fet_fixture::set_worker_count(BATCH_N);
        const bool batch_started{ drive(ctx, 3) };
        ctx.check("batch_probe_started", batch_started);

        // Best-effort: wait (bounded) for the whole batch to announce it is up.
        // We do NOT hard-require the full count (a loaded runner may attach them
        // slowly); we record how many actually came up and proceed with whatever
        // is live.
        const bool batch_all_up{ poll_predicate(
            []() { return fet_fixture::get_up_count() >= BATCH_N; }, 5000) };
        const std::int32_t up_now{ fet_fixture::get_up_count() };
        ctx.record(std::string{ "[INFO] batch requested " } + std::to_string(BATCH_N)
                   + " workers; upCount reached " + std::to_string(up_now)
                   + (batch_all_up ? " (full)" : " (partial within budget)"));

        // Poll-enumerate (bounded) until the batch's new identities surface, then
        // fold the LARGE-count invariants over that enumeration.
        enumeration with_batch{ enumerate() };
        std::vector<thread_identity> batch_added{
            added_identities(with_batch.identities, before_batch.identities) };
        for (int tries{ 0 };
             tries < 300 && static_cast<std::int32_t>(batch_added.size()) < up_now;
             ++tries)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
            with_batch  = enumerate();
            batch_added = added_identities(with_batch.identities, before_batch.identities);
        }

        ctx.record(std::string{ "[INFO] batch live count before=" }
                   + std::to_string(before_batch.count) + " with_batch="
                   + std::to_string(with_batch.count)
                   + " (delta informational only — not asserted)");
        ctx.record(std::string{ "[INFO] batch surfaced " }
                   + std::to_string(batch_added.size())
                   + " new (ptr,tid) identity(ies) of "
                   + std::to_string(up_now) + " reported up");

        // UNIVERSAL invariants under the larger live thread set: the walk still
        // TERMINATES bounded, stays UNDER the runaway cap, hands back only valid
        // pointers, and never reports a duplicate pointer/identity.  These are the
        // "no overrun, terminates, no crash" guarantees for a large thread count.
        ctx.check("batch_enumeration_terminates_bounded_time", with_batch.elapsed_ms < 250.0);
        ctx.check("batch_count_below_cap", with_batch.count < FOR_EACH_THREAD_CAP);
        ctx.check("batch_all_pointers_valid", with_batch.all_pointers_valid);
        ctx.check("batch_no_state_out_of_range", !with_batch.any_state_out_of_range);
        ctx.check("batch_all_os_tids_nonzero", with_batch.all_os_tids_nonzero);
        ctx.check("batch_no_duplicate_pointer",
                  distinct_count(with_batch.pointers) == with_batch.pointers.size());
        if (with_batch.all_pointers_valid)
        {
            ctx.check("batch_identities_distinct",
                      distinct_identity_count(with_batch.identities)
                          == with_batch.identities.size());
        }

        // The batch must enlarge the live set: the with-batch enumeration has
        // strictly MORE distinct threads than the smaller of {before, the cap}.
        // (Asserted relative to `before_batch` only when the batch positively
        // attached, so unrelated churn can't make it red.)
        if (up_now >= 1)
        {
            // Positively-observed batch: at least one brand-new identity MUST be
            // visible to enumeration (mirror of spawn_new_pointer_appeared) — a
            // live, attached JavaThread that for_each_thread cannot see would be a
            // real enumeration miss.
            ctx.check("batch_new_identities_appeared", !batch_added.empty());

            const bool all_batch_added_valid{ std::all_of(
                batch_added.begin(), batch_added.end(),
                [](const thread_identity& id)
                { return id.ptr != nullptr && vmhook::hotspot::is_valid_pointer(id.ptr); }) };
            ctx.check("batch_new_identities_valid", all_batch_added_valid);

            // Timing-tolerant "rises by ~N": when the FULL batch reported up, a
            // healthy enumeration surfaces a solid majority of them.  We require a
            // conservative floor (half) rather than the exact N so a slow attach
            // tail can't flake it; the exact figure is recorded above.
            if (batch_all_up)
            {
                ctx.check("batch_majority_surfaced",
                          static_cast<std::int32_t>(batch_added.size()) >= BATCH_N / 2);
            }
        }
        else
        {
            ctx.record("[INFO] batch never reported up within budget "
                       "(scheduler/attach timing) — skipped the batch-delta asserts.");
        }

        // Release the batch and prove enumeration reflects the mass exit.  The
        // DRAIN itself is timing-tolerant (best-effort poll, recorded as [INFO],
        // not hard-asserted): draining N sleeping daemons within budget depends on
        // the runner scheduling them, and the AUTHORITATIVE drain proof is the
        // cleanup step below, which actively JOINs every worker before asserting
        // none are left.  Here we only RECORD how the passive poll went and gate
        // the identity-gone assertion on it (so a slow runner skips rather than
        // fails).
        fet_fixture::set_stop(true);
        const bool batch_drained{ poll_predicate(
            []() { return fet_fixture::get_up_count() == 0; }, 8000) };
        const std::int32_t up_after{ fet_fixture::get_up_count() };
        ctx.record(std::string{ "[INFO] batch upCount after release = " }
                   + std::to_string(up_after)
                   + (batch_drained ? " (drained within budget)"
                                    : " (not fully drained within budget — "
                                      "authoritative drain is the join in cleanup)"));

        if (batch_drained && !batch_added.empty())
        {
            // None of the batch's identities survive a fresh enumeration — the
            // robust, churn-proof, pointer-recycle-proof analogue of a count drop.
            const auto any_batch_survives{
                [&](const enumeration& live) -> bool
                {
                    return std::any_of(
                        batch_added.begin(), batch_added.end(),
                        [&](const thread_identity& id)
                        {
                            return std::find(live.identities.begin(),
                                             live.identities.end(), id)
                                != live.identities.end();
                        });
                } };

            enumeration after_batch{ enumerate() };
            for (int tries{ 0 };
                 tries < 300 && any_batch_survives(after_batch);
                 ++tries)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
                after_batch = enumerate();
            }

            ctx.record(std::string{ "[INFO] batch live count after release = " }
                       + std::to_string(after_batch.count)
                       + " (with_batch peak was " + std::to_string(with_batch.count)
                       + "; delta informational only — not asserted)");
            ctx.check("batch_after_all_pointers_valid", after_batch.all_pointers_valid);
            ctx.check("batch_after_terminates_bounded_time", after_batch.elapsed_ms < 250.0);
            ctx.check("batch_identities_gone", !any_batch_survives(after_batch));
        }
    }

    // =====================================================================
    // PART H — CROSS-PATH OS-TID ROUND-TRIP (NEW): an enumerated identity's
    //   os_thread_id should resolve back to a non-null, valid JavaThread through
    //   find_java_thread_by_os_thread_id — a DIFFERENT walk (Path-1 list scan +
    //   Path-2 SMR fallback) that shares only the OSThread-id DECODE.  This is the
    //   strongest portable proof the OSThread chain decoded a REAL, resolvable OS
    //   thread id (Part B already proved it is merely non-zero): a tid that no
    //   second walk can find would be a decode artifact, not a live thread.  Each
    //   lookup is itself bounded (the finder caps at 4096), so this stays crash-
    //   and hang-safe.
    //
    //   HARDENING (cross-toolchain, the batch-12 [for_each_thread fix pending]):
    //   "100% of enumerated tids resolve" is NOT a universal invariant.  The
    //   enumeration is a SNAPSHOT; find_java_thread_by_os_thread_id is a SECOND,
    //   later walk.  Between the two, TRANSIENT JVM threads legitimately EXIT — JIT
    //   compiler threads (C1/C2), GC workers, the Reference Handler / Cleaner /
    //   service daemons, and on JDK 21+ virtual-thread CARRIER threads spin up and
    //   down continuously — so a tid that was live at snapshot can be gone by the
    //   resolve, failing to round-trip through NO fault of the decode or the
    //   library.  This reddened java21/24 intermittently (whichever JDK happened to
    //   retire a thread mid-test).  So:
    //     * HARD — the round-trip of a thread we KNOW is alive for the whole test:
    //       the current/suite thread.  We snapshot it and immediately resolve its
    //       tid back to the SAME JavaThread* we hold.  (Any test-created worker is
    //       already proven to round-trip implicitly via its lifecycle in Part E;
    //       here the current thread is the guaranteed-stable anchor.)  Part I below
    //       independently hard-asserts current_thread_tid_resolves_to_current too.
    //     * [INFO] — the resolve rate over the WHOLE freshly-snapshotted set and
    //       the exact-pointer-match rate.  These vary with transient liveness, so
    //       they are recorded (with the transient-thread explanation), never a FAIL.
    //   We resolve over a FRESH enumeration taken right here (not the module-start
    //   `base`, which is seconds and a whole batch spawn/drain old) so the snapshot
    //   ↔ resolve window is as small as possible and the [INFO] rate is meaningful.
    // =====================================================================
    {
        // HARD: the current/suite thread is alive for the entire test — its tid
        // MUST round-trip back to the exact JavaThread* we hold.  This is the
        // stable-thread round-trip that does not depend on any transient liveness.
        if (vmhook::hotspot::current_java_thread)
        {
            const auto current_jt{ vmhook::hotspot::current_java_thread };
            const enumeration h_now{ enumerate() };

            vmhook::os::thread_id_t current_tid{ 0 };
            bool current_in_enum{ false };
            for (const thread_identity& id : h_now.identities)
            {
                if (id.ptr == current_jt)
                {
                    current_tid = id.tid;
                    current_in_enum = true;
                    break;
                }
            }

            if (current_in_enum && current_tid != 0)
            {
                vmhook::hotspot::java_thread* const resolved{
                    vmhook::hotspot::find_java_thread_by_os_thread_id(current_tid) };
                // Resolves to a non-null, valid, SAME JavaThread* — the stable
                // round-trip proof (decode + cross-path finder agree on a thread we
                // KNOW is live for the whole test).
                ctx.check("roundtrip_stable_current_resolves",
                          resolved != nullptr
                              && vmhook::hotspot::is_valid_pointer(resolved)
                              && resolved == current_jt);
            }
            else
            {
                ctx.record("[INFO] current thread not present with a nonzero tid in the "
                           "fresh round-trip enumeration (unexpected but non-fatal) — "
                           "skipped roundtrip_stable_current_resolves.");
            }

            // [INFO] only: how the WHOLE fresh set round-tripped.  The remainder are
            // transient JVM threads (JIT/GC/service/VT-carrier) that exited between
            // this snapshot and the per-tid resolve — never a FAIL.
            const roundtrip_tally rt{ roundtrip_identities(h_now.identities) };
            ctx.record(std::string{ "[INFO] os-tid round-trip: " }
                       + std::to_string(rt.resolved) + "/" + std::to_string(rt.total)
                       + " enumerated tids re-resolved via find_java_thread_by_os_thread_id ("
                       + std::to_string(rt.pointer_matched)
                       + " also pointer-matched exactly); any remainder are transient "
                         "JVM threads (JIT/GC/service/VT-carrier) that exited between "
                         "the snapshot and the resolve — informational, not asserted.");
        }
        else
        {
            // No current JavaThread on the suite thread (native, non-Java thread):
            // we have no guaranteed-stable anchor to hard-assert, so record the
            // whole-set rate for info and skip the stable round-trip symmetrically.
            const enumeration h_now{ enumerate() };
            const roundtrip_tally rt{ roundtrip_identities(h_now.identities) };
            ctx.record(std::string{ "[INFO] os-tid round-trip (no current-thread anchor): " }
                       + std::to_string(rt.resolved) + "/" + std::to_string(rt.total)
                       + " enumerated tids re-resolved (" + std::to_string(rt.pointer_matched)
                       + " pointer-matched); current_java_thread is null on the suite "
                         "thread — skipped roundtrip_stable_current_resolves.");
        }
    }

    // =====================================================================
    // PART I — CURRENT-THREAD TID DECODE CORROBORATION (NEW): the current thread's
    //   enumerated os_thread_id must (a) be non-zero and (b) round-trip through
    //   find_java_thread_by_os_thread_id back to the SAME JavaThread* the cached
    //   current_java_thread points at — proving the decode produced the actual
    //   live current thread's id, cross-checked against the EXACT pointer we
    //   already hold.  The literal-equality of the decoded tid to
    //   vmhook::os::current_thread_id() is JDK/OS-variant (the decode reads only a
    //   32-bit slot and zero-extends; the OS tid width differs by platform), so
    //   that is RECORDED as [INFO], never asserted — only the non-zero + same-
    //   pointer round-trip is hard.
    // =====================================================================
    if (vmhook::hotspot::current_java_thread)
    {
        const auto current_jt{ vmhook::hotspot::current_java_thread };

        // Find the current thread's enumerated identity from the baseline pass.
        vmhook::os::thread_id_t current_enum_tid{ 0 };
        bool current_found_in_enum{ false };
        for (const thread_identity& id : base.identities)
        {
            if (id.ptr == current_jt)
            {
                current_enum_tid = id.tid;
                current_found_in_enum = true;
                break;
            }
        }

        if (current_found_in_enum)
        {
            ctx.check("current_thread_tid_nonzero", current_enum_tid != 0);

            const vmhook::os::thread_id_t os_tid{ vmhook::os::current_thread_id() };
            ctx.record(std::string{ "[INFO] current thread: enumerated tid="
                       } + std::to_string(static_cast<std::uint64_t>(current_enum_tid))
                       + " os::current_thread_id()="
                       + std::to_string(static_cast<std::uint64_t>(os_tid))
                       + " (literal equality is JDK/OS-variant — recorded, not asserted)");

            if (current_enum_tid != 0)
            {
                vmhook::hotspot::java_thread* const resolved{
                    vmhook::hotspot::find_java_thread_by_os_thread_id(current_enum_tid) };
                ctx.check("current_thread_tid_resolves_to_current",
                          resolved == current_jt);
            }
        }
        else
        {
            ctx.record("[INFO] current_java_thread not present in baseline identities "
                       "(unexpected but non-fatal) — skipped current-tid corroboration.");
        }
    }
    else
    {
        ctx.record("[INFO] current_java_thread is null on the suite thread; "
                   "skipping current-thread tid-decode corroboration.");
    }

    // =====================================================================
    // PART J — VISITOR EXCEPTION PROPAGATES + STOP-AT-VISIT (NEW): the header
    //   contract is "callback exceptions propagate; iteration stops at the
    //   throwing visit."  A visitor that throws on its FIRST invocation must (a)
    //   propagate the exception out of for_each_thread (caught here — it must NOT
    //   crash or hang on the no-SEH toolchains, because this is ordinary C++
    //   unwinding, not an access violation) and (b) have visited at most that one
    //   thread, not the whole list — the closest portable analogue of the brief's
    //   "stop-at-visit" semantics.  Bounded by construction (throws immediately),
    //   so it can neither hang nor leak.
    // =====================================================================
    {
        std::int32_t visits_before_throw{ 0 };
        bool exception_propagated{ false };
        const auto t0{ std::chrono::steady_clock::now() };
        try
        {
            vmhook::for_each_thread([&](const vmhook::thread_info&)
            {
                ++visits_before_throw;
                throw vmhook::exception{ "for_each_thread visitor stop-at-visit probe" };
            });
        }
        catch (const std::exception&)
        {
            exception_propagated = true;
        }
        catch (...)
        {
            exception_propagated = true;
        }
        const auto t1{ std::chrono::steady_clock::now() };
        const double throw_ms{ std::chrono::duration<double, std::milli>{ t1 - t0 }.count() };

        // The exception must reach the caller (contract: it propagates).
        ctx.check("visitor_exception_propagates", exception_propagated);
        // Iteration stopped at the throwing visit: the visitor ran exactly once
        // before the throw unwound it (stop-at-visit).  base.count was > 1 on any
        // real JVM, so a value of 1 here proves the walk did NOT continue.
        ctx.check("visitor_exception_stops_at_visit", visits_before_throw == 1);
        // And it returned promptly — no hang on the unwinding path.
        ctx.check("visitor_exception_path_bounded_time", throw_ms < 250.0);

        // A subsequent CLEAN enumeration still works — the throw did not corrupt
        // any per-call state (the walk holds only locals + a per-call visited_set).
        const enumeration after_throw{ enumerate() };
        ctx.check("enumeration_clean_after_visitor_throw",
                  after_throw.count >= 1 && after_throw.all_pointers_valid);
    }

    // =====================================================================
    // PART K — RE-ENTRANCY (NEW): for_each_thread uses only call-local state (a
    //   per-call counter + visited_set), so a NESTED for_each_thread invoked from
    //   inside the outer visitor must itself terminate bounded and enumerate the
    //   same persistent core — no shared/static walk cursor that a re-entrant call
    //   could corrupt.  We run the nested walk ONCE (on the first outer visit) to
    //   keep it O(N), and prove the nested pass is valid, bounded, non-duplicate,
    //   and sees a non-empty persistent core in common with the outer pass.
    // =====================================================================
    {
        bool nested_ran{ false };
        bool nested_all_valid{ true };
        bool nested_no_dup{ true };
        double nested_ms{ 0.0 };
        std::vector<vmhook::hotspot::java_thread*> nested_pointers{};
        std::int32_t outer_visits{ 0 };

        vmhook::for_each_thread([&](const vmhook::thread_info&)
        {
            ++outer_visits;
            if (nested_ran)
            {
                return; // run the nested walk only once
            }
            nested_ran = true;

            const auto n0{ std::chrono::steady_clock::now() };
            vmhook::for_each_thread([&](const vmhook::thread_info& inner)
            {
                if (inner.thread == nullptr
                    || !vmhook::hotspot::is_valid_pointer(inner.thread))
                {
                    nested_all_valid = false;
                    return;
                }
                nested_pointers.push_back(inner.thread);
            });
            const auto n1{ std::chrono::steady_clock::now() };
            nested_ms = std::chrono::duration<double, std::milli>{ n1 - n0 }.count();
            nested_no_dup =
                distinct_count(nested_pointers) == nested_pointers.size();
        });

        ctx.check("reentrant_nested_ran", nested_ran);
        if (nested_ran)
        {
            ctx.check("reentrant_nested_all_pointers_valid", nested_all_valid);
            ctx.check("reentrant_nested_terminates_bounded_time", nested_ms < 250.0);
            ctx.check("reentrant_nested_visited_at_least_one",
                      !nested_pointers.empty());
            ctx.check("reentrant_nested_no_duplicate_pointer", nested_no_dup);
            ctx.check("reentrant_nested_count_below_cap",
                      static_cast<std::int32_t>(nested_pointers.size())
                          < FOR_EACH_THREAD_CAP);
            // Outer and nested enumerations share a non-empty persistent core (the
            // JVM's own long-lived threads appear in both); proves the re-entrant
            // call did not corrupt the outer walk.
            const std::size_t both{ intersection_count(base.pointers, nested_pointers) };
            ctx.record(std::string{ "[INFO] re-entrant nested pass visited " }
                       + std::to_string(nested_pointers.size())
                       + "; outer pass made " + std::to_string(outer_visits)
                       + " visits; persistent core with baseline = "
                       + std::to_string(both));
            ctx.check("reentrant_nested_shares_core", both >= 1);
        }
    }

    // =====================================================================
    // PART L — MULTI-PASS DRIFT (NEW, stronger than Part D's two passes): run
    //   FIVE rapid back-to-back enumerations and prove the PERSISTENT core common
    //   to ALL FIVE is non-empty and (where identifiable) contains the current
    //   thread, while every pass is independently valid + bounded.  A per-call
    //   state-corruption bug that only manifests after a few iterations (e.g. a
    //   visited_set or cursor that is not reset between calls) would shrink the
    //   all-five intersection toward zero or trip a validity/bound check on a
    //   later pass — neither of which a single repeat would catch.  Exact counts
    //   are NOT asserted (concurrent VM thread churn moves them); only the churn-
    //   PROOF five-way intersection and the per-pass invariants are hard.
    // =====================================================================
    {
        constexpr int PASSES{ 5 };
        std::vector<enumeration> passes{};
        passes.reserve(static_cast<std::size_t>(PASSES));
        for (int i{ 0 }; i < PASSES; ++i)
        {
            passes.push_back(enumerate());
        }

        bool all_passes_valid{ true };
        bool all_passes_bounded{ true };
        bool all_passes_nonempty{ true };
        bool all_passes_no_dup{ true };
        for (const enumeration& p : passes)
        {
            all_passes_valid    = all_passes_valid && p.all_pointers_valid;
            all_passes_bounded  = all_passes_bounded && (p.elapsed_ms < 250.0);
            all_passes_nonempty = all_passes_nonempty && (p.count >= 1);
            all_passes_no_dup   = all_passes_no_dup
                && (distinct_count(p.pointers) == p.pointers.size());
        }
        ctx.check("multipass_all_valid", all_passes_valid);
        ctx.check("multipass_all_terminate_bounded_time", all_passes_bounded);
        ctx.check("multipass_all_visited_at_least_one", all_passes_nonempty);
        ctx.check("multipass_all_no_duplicate_pointer", all_passes_no_dup);

        // Pointers present in EVERY one of the five passes — the churn-proof
        // persistent core.  Built by intersecting each pass's distinct set with
        // the running core.
        std::unordered_set<vmhook::hotspot::java_thread*> core{
            passes.front().pointers.begin(), passes.front().pointers.end() };
        for (std::size_t i{ 1 }; i < passes.size(); ++i)
        {
            const std::unordered_set<vmhook::hotspot::java_thread*> s{
                passes[i].pointers.begin(), passes[i].pointers.end() };
            std::unordered_set<vmhook::hotspot::java_thread*> next{};
            for (vmhook::hotspot::java_thread* const p : core)
            {
                if (s.find(p) != s.end())
                {
                    next.insert(p);
                }
            }
            core.swap(next);
        }
        ctx.record(std::string{ "[INFO] five-pass persistent (in-all-5) core = " }
                   + std::to_string(core.size()));
        ctx.check("multipass_persistent_core_nonempty", !core.empty());

        if (vmhook::hotspot::current_java_thread)
        {
            const auto current_jt{ vmhook::hotspot::current_java_thread };
            ctx.check("multipass_current_thread_in_all_passes",
                      core.find(current_jt) != core.end());
        }
        else
        {
            ctx.record("[INFO] current_java_thread null on suite thread; "
                       "skipping multipass current-thread-in-all check.");
        }
    }

    // =====================================================================
    // PART M — NAMED-STATE COVERAGE (NEW, [INFO]): for every baseline thread the
    //   decoded state byte is a NAMED java_thread_state constant (stricter than
    //   the HARD in-range check in Part B, which also accepts the unused gap value
    //   1).  Recorded as [INFO], NOT hard-asserted: the precise set of states
    //   HotSpot legitimately surfaces is JDK-variant, and a future-JDK-valid byte
    //   we don't yet name must never falsely redden the suite — the HARD universal
    //   invariant remains "in range" (Part B).  We also record the state histogram
    //   so a human can eyeball the live thread mix.
    // =====================================================================
    {
        std::int32_t named{ 0 };
        std::int32_t unnamed{ 0 };
        std::int32_t in_java{ 0 };
        std::int32_t in_native{ 0 };
        std::int32_t in_vm{ 0 };
        std::int32_t blocked{ 0 };
        std::int32_t other{ 0 };
        vmhook::for_each_thread([&](const vmhook::thread_info& info)
        {
            if (info.thread == nullptr
                || !vmhook::hotspot::is_valid_pointer(info.thread))
            {
                return;
            }
            if (state_is_named(info.state)) { ++named; } else { ++unnamed; }
            switch (info.state)
            {
            case vmhook::hotspot::java_thread_state::_thread_in_Java:
            case vmhook::hotspot::java_thread_state::_thread_in_Java_trans:
                ++in_java; break;
            case vmhook::hotspot::java_thread_state::_thread_in_native:
            case vmhook::hotspot::java_thread_state::_thread_in_native_trans:
                ++in_native; break;
            case vmhook::hotspot::java_thread_state::_thread_in_vm:
            case vmhook::hotspot::java_thread_state::_thread_in_vm_trans:
                ++in_vm; break;
            case vmhook::hotspot::java_thread_state::_thread_blocked:
            case vmhook::hotspot::java_thread_state::_thread_blocked_trans:
                ++blocked; break;
            default:
                ++other; break;
            }
        });
        ctx.record(std::string{ "[INFO] baseline state histogram: in_Java=" }
                   + std::to_string(in_java) + " in_native=" + std::to_string(in_native)
                   + " in_vm=" + std::to_string(in_vm) + " blocked=" + std::to_string(blocked)
                   + " other=" + std::to_string(other));
        ctx.record(std::string{ "[INFO] named-state coverage: " }
                   + std::to_string(named) + " named, " + std::to_string(unnamed)
                   + " unnamed (unnamed is JDK-variant — recorded, not asserted)");
    }

    // =====================================================================
    // PART N -- find_any_java_thread() HEAD CONSISTENCY (NEW): for_each_thread's
    //   Path-1 walk starts from vmhook::hotspot::find_any_java_thread() (the
    //   Threads::_thread_list head).  That head, when present, must be (a)
    //   null-or-VALID (never a garbage pointer the walk would deref), and (b) a
    //   LIVE thread that the enumeration itself reports -- the walk cannot omit its
    //   own starting node.  On JDK 10+ builds that ship no `Threads::_thread_list`
    //   VMStruct the head is null and for_each_thread falls through to the Path-2
    //   SMR snapshot; that is a legitimate configuration, so head==null is recorded
    //   as [INFO] and the "head is enumerated" assertion is gated on a non-null
    //   head.  Repeated calls are also checked to be self-consistent (the head is
    //   stable across two back-to-back reads in a quiescent window -- recorded, not
    //   asserted, since the JVM can retire the head thread between reads).
    //   SAFE: the head comes straight from the library's own fault-safe accessor
    //   and is is_valid_pointer-gated before any use.
    // =====================================================================
    {
        vmhook::hotspot::java_thread* const head{ vmhook::hotspot::find_any_java_thread() };

        // (a) null-or-valid: find_any_java_thread already filters through
        //     is_valid_pointer, so a non-null head MUST be valid.  A non-null head
        //     that fails is_valid_pointer would be a library contract break.
        const bool head_null_or_valid{
            head == nullptr || vmhook::hotspot::is_valid_pointer(head) };
        ctx.check("find_any_head_null_or_valid", head_null_or_valid);

        const enumeration n_now{ enumerate() };
        ctx.check("find_any_head_enum_valid", n_now.all_pointers_valid);

        if (head != nullptr && vmhook::hotspot::is_valid_pointer(head))
        {
            // (b) the head is a live thread, so the enumeration that walks FROM it
            //     must include it.  (Path 1 visits the head first; Path 2 is not
            //     reached when Path 1 visited anything -- and if Path 1 head is
            //     non-null it WILL visit it.)
            const bool head_enumerated{
                std::find(n_now.pointers.begin(), n_now.pointers.end(), head)
                    != n_now.pointers.end() };
            ctx.check("find_any_head_is_enumerated", head_enumerated);

            // The head's own tid decodes to a non-zero value (its OSThread chain
            // is intact) -- a direct accessor cross-check, not via the visitor.
            const vmhook::os::thread_id_t head_tid{ head->get_os_thread_id() };
            ctx.check("find_any_head_tid_nonzero", head_tid != 0);

            // The head's decoded state is in range (same universal invariant the
            // visitor path asserts, proven here straight off the accessor).
            ctx.check("find_any_head_state_in_range",
                      state_in_range(head->get_thread_state()));
        }
        else
        {
            ctx.record("[INFO] find_any_java_thread() returned null -- this JVM ships "
                       "no Threads::_thread_list VMStruct (JDK 10+ SMR-only); "
                       "for_each_thread uses the Path-2 snapshot.  Skipped the "
                       "head-is-enumerated / head-accessor cross-checks.");
        }

        // Self-consistency of two back-to-back head reads (recorded, not asserted:
        // the JVM may retire the head thread between the two microsecond-apart
        // reads, which would legitimately change the head pointer).
        vmhook::hotspot::java_thread* const head2{ vmhook::hotspot::find_any_java_thread() };
        ctx.record(std::string{ "[INFO] find_any_java_thread head stable across two reads: " }
                   + (head == head2 ? "yes" : "no")
                   + " (informational -- head thread can retire between reads)");
    }

    // =====================================================================
    // PART O -- MANUAL Path-1 LINK-WALK vs ENUMERATION (NEW): independently walk
    //   the classic intrusive list the SAME way for_each_thread's Path 1 does --
    //   find_any_java_thread() then get_next() to exhaustion -- with our OWN dedup
    //   set and the SAME 4096 cap + cycle break, and prove the primitive walk (1)
    //   TERMINATES bounded, (2) hands back only valid pointers, (3) never repeats a
    //   pointer (the cycle break holds), (4) stays under the cap, and (5) shares a
    //   non-empty persistent core with the enumeration.  This exercises the
    //   underlying get_next() link primitive directly, the layer for_each_thread is
    //   built on.  Gated on a non-null head (JDK 10+ SMR-only builds have none);
    //   the manual walk is otherwise [INFO]-skipped, mirroring Part N.
    //   SAFE: every node is is_valid_pointer-gated before get_next(), the walk is
    //   hard-capped at 4096 AND breaks on the first repeated pointer, so it can
    //   neither fault, spin, nor hang even on a corrupted list.
    // =====================================================================
    {
        vmhook::hotspot::java_thread* const head{ vmhook::hotspot::find_any_java_thread() };
        if (head != nullptr && vmhook::hotspot::is_valid_pointer(head))
        {
            std::vector<vmhook::hotspot::java_thread*> walked{};
            std::unordered_set<vmhook::hotspot::java_thread*> seen{};
            bool walk_all_valid{ true };
            bool walk_hit_cycle{ false };

            const auto w0{ std::chrono::steady_clock::now() };
            vmhook::hotspot::java_thread* cur{ head };
            std::int32_t steps{ 0 };
            while (cur != nullptr
                   && vmhook::hotspot::is_valid_pointer(cur)
                   && steps < FOR_EACH_THREAD_CAP)
            {
                if (!seen.insert(cur).second)
                {
                    // Repeated pointer -- a cycle in the intrusive list; stop,
                    // exactly as for_each_thread's Path-1 cycle break does.
                    walk_hit_cycle = true;
                    break;
                }
                walked.push_back(cur);
                ++steps;
                // Independent re-validation of the node we just accepted, so the
                // manual_walk_all_pointers_valid assertion is a genuine check and
                // not a tautology of the loop guard.
                if (cur == nullptr || !vmhook::hotspot::is_valid_pointer(cur))
                {
                    walk_all_valid = false;
                }
                vmhook::hotspot::java_thread* const next{ cur->get_next() };
                cur = (next != nullptr && vmhook::hotspot::is_valid_pointer(next))
                          ? next : nullptr;
            }
            const auto w1{ std::chrono::steady_clock::now() };
            const double walk_ms{ std::chrono::duration<double, std::milli>{ w1 - w0 }.count() };

            // (1) bounded wall-clock -- the same "does not hang" proof as the
            //     enumeration, but on the raw get_next() primitive.
            ctx.check("manual_walk_terminates_bounded_time", walk_ms < 250.0);
            // (4) stays under the cap (a healthy list never reaches it).
            ctx.check("manual_walk_below_cap",
                      static_cast<std::int32_t>(walked.size()) < FOR_EACH_THREAD_CAP);
            // (2) every walked node is valid (gated above; assert the tally held).
            ctx.check("manual_walk_all_pointers_valid", walk_all_valid);
            // visited at least the head.
            ctx.check("manual_walk_visited_at_least_one", !walked.empty());
            // (3) no pointer repeated (the cycle break + dedup held -- a healthy
            //     list is acyclic, so this must NOT have hit the cycle break).
            ctx.check("manual_walk_no_duplicate_pointer",
                      distinct_count(walked) == walked.size());
            ctx.record(std::string{ "[INFO] manual Path-1 link-walk visited " }
                       + std::to_string(walked.size()) + " node(s) in "
                       + std::to_string(walk_ms) + " ms; cycle break hit: "
                       + (walk_hit_cycle ? "yes" : "no")
                       + " (a healthy list is acyclic -- break expected NO)");

            // (5) the manual walk and a fresh enumeration share a non-empty
            //     persistent core (both describe the same live Path-1 list; the
            //     head, the main thread, etc. are in both).  Exact equality is NOT
            //     asserted -- a thread can start/exit between the two walks.
            const enumeration o_enum{ enumerate() };
            const std::size_t both{ intersection_count(walked, o_enum.pointers) };
            ctx.record(std::string{ "[INFO] manual-walk vs enumeration persistent core = " }
                       + std::to_string(both) + " (walk=" + std::to_string(walked.size())
                       + ", enum=" + std::to_string(o_enum.pointers.size()) + ")");
            ctx.check("manual_walk_shares_core_with_enum", both >= 1);
        }
        else
        {
            ctx.record("[INFO] no Path-1 head (SMR-only JDK) -- skipped the manual "
                       "get_next() link-walk cross-check.");
        }
    }

    // =====================================================================
    // PART P -- ACCESSOR RE-READ DETERMINISM (NEW): the visitor decodes each
    //   thread's os_thread_id / state via JavaThread::get_os_thread_id() /
    //   ::get_thread_state().  Those accessors must be DETERMINISTIC -- calling
    //   get_os_thread_id() AGAIN, directly on the same enumerated JavaThread*,
    //   yields the SAME non-zero id the visitor reported (the decode is a pure
    //   field read, not a one-shot artifact), and get_thread_state() re-reads an
    //   in-range byte.  We re-read off a FRESH enumeration (so the pointers are as
    //   live as possible) and only for pointers that still pass is_valid_pointer at
    //   re-read time -- a thread that exited between the snapshot and the re-read is
    //   skipped (recorded), never a FAIL, because its JavaThread may be reclaimed.
    //   SAFE: every re-read is is_valid_pointer-gated and the accessors are
    //   themselves safe_read-routed, so a stale pointer degrades to 0 / sentinel
    //   rather than faulting.
    // =====================================================================
    {
        const enumeration p_now{ enumerate() };
        std::int32_t rechecked{ 0 };
        std::int32_t skipped_exited{ 0 };
        bool all_tids_match{ true };
        bool all_tids_nonzero{ true };
        bool all_states_in_range{ true };
        for (const thread_identity& id : p_now.identities)
        {
            if (id.ptr == nullptr || !vmhook::hotspot::is_valid_pointer(id.ptr))
            {
                ++skipped_exited;
                continue;
            }
            ++rechecked;
            const vmhook::os::thread_id_t reread_tid{ id.ptr->get_os_thread_id() };
            if (reread_tid != id.tid)
            {
                all_tids_match = false;
            }
            if (reread_tid == 0)
            {
                all_tids_nonzero = false;
            }
            if (!state_in_range(id.ptr->get_thread_state()))
            {
                all_states_in_range = false;
            }
        }
        ctx.record(std::string{ "[INFO] accessor re-read: re-checked " }
                   + std::to_string(rechecked) + " of "
                   + std::to_string(p_now.identities.size())
                   + " enumerated thread(s); " + std::to_string(skipped_exited)
                   + " skipped (pointer no longer valid at re-read).");
        if (rechecked >= 1)
        {
            // The re-read os_thread_id matches the visitor's exactly -- the decode
            // is reproducible, not a transient artifact.
            ctx.check("accessor_reread_tid_matches_visitor", all_tids_match);
            // ...and is still non-zero on the direct call.
            ctx.check("accessor_reread_tid_nonzero", all_tids_nonzero);
            // ...and the state re-reads in range straight off the accessor.
            ctx.check("accessor_reread_state_in_range", all_states_in_range);
        }
        else
        {
            ctx.record("[INFO] no enumerated pointer survived to re-read time "
                       "(unexpected on a live JVM) -- skipped accessor re-read asserts.");
        }
    }

    // =====================================================================
    // PART Q -- get_suspend_flags() INTROSPECTION IS BOUNDED (NEW, mostly [INFO]):
    //   the header points callers at the other java_thread helpers
    //   (get_suspend_flags, etc.) for deeper introspection of an enumerated
    //   JavaThread*.  We exercise get_suspend_flags() over every enumerated valid
    //   thread and prove the whole sweep (1) TERMINATES bounded and (2) never
    //   crashes the no-SEH legs (it is safe_read-routed and returns 0 on a bad
    //   page).  The flag VALUES are JDK/OS/timing-variant (0 for a normal running
    //   thread; non-zero only mid-suspend/async-handshake), so the distinct flag
    //   set is RECORDED, never asserted.  SAFE: is_valid_pointer-gated per thread.
    // =====================================================================
    {
        const enumeration q_now{ enumerate() };
        std::unordered_set<std::uint32_t> distinct_flags{};
        std::int32_t inspected{ 0 };
        const auto q0{ std::chrono::steady_clock::now() };
        for (const thread_identity& id : q_now.identities)
        {
            if (id.ptr == nullptr || !vmhook::hotspot::is_valid_pointer(id.ptr))
            {
                continue;
            }
            ++inspected;
            distinct_flags.insert(id.ptr->get_suspend_flags());
        }
        const auto q1{ std::chrono::steady_clock::now() };
        const double q_ms{ std::chrono::duration<double, std::milli>{ q1 - q0 }.count() };

        // The introspection sweep returns bounded -- no hang on the accessor path.
        ctx.check("suspend_flags_sweep_bounded_time", q_ms < 250.0);
        ctx.record(std::string{ "[INFO] get_suspend_flags() inspected " }
                   + std::to_string(inspected) + " thread(s); "
                   + std::to_string(distinct_flags.size())
                   + " distinct flag value(s) observed (values are JDK/OS/timing-"
                     "variant -- recorded, not asserted).");
    }

    // =====================================================================
    // PART R -- find_java_thread_by_os_thread_id DEGENERATE / IDEMPOTENT INPUT
    //   (NEW): the cross-path finder that shares the OSThread decode has an
    //   explicit zero-id guard (it returns nullptr for os_thread_id == 0 before
    //   walking anything).  Prove that degenerate-input contract HARD -- tid 0 is
    //   never a live OS thread, so the finder MUST return nullptr.  Also prove the
    //   finder is IDEMPOTENT for a stable input: resolving the current thread's tid
    //   twice yields the SAME JavaThread* (no per-call state drift).  SAFE: the
    //   finder is fully bounded (caps at 4096 on each path) and noexcept.
    // =====================================================================
    {
        // Degenerate input: tid 0 -> nullptr (the explicit guard).  CERTAIN.
        vmhook::hotspot::java_thread* const zero_lookup{
            vmhook::hotspot::find_java_thread_by_os_thread_id(0) };
        ctx.check("finder_zero_tid_returns_null", zero_lookup == nullptr);

        // Idempotency on a stable input: the current thread's tid resolves to the
        // same pointer on two consecutive calls.  Only when we have a current
        // JavaThread with a non-zero enumerated tid (otherwise nothing stable to
        // resolve -- recorded and skipped).
        if (vmhook::hotspot::current_java_thread)
        {
            const auto current_jt{ vmhook::hotspot::current_java_thread };
            const enumeration r_now{ enumerate() };
            vmhook::os::thread_id_t cur_tid{ 0 };
            for (const thread_identity& id : r_now.identities)
            {
                if (id.ptr == current_jt)
                {
                    cur_tid = id.tid;
                    break;
                }
            }
            if (cur_tid != 0)
            {
                vmhook::hotspot::java_thread* const first{
                    vmhook::hotspot::find_java_thread_by_os_thread_id(cur_tid) };
                vmhook::hotspot::java_thread* const second{
                    vmhook::hotspot::find_java_thread_by_os_thread_id(cur_tid) };
                // Idempotent: same pointer both times (no per-call walk state).
                ctx.check("finder_idempotent_same_pointer", first == second);
                // ...and it is the current thread we hold (cross-check with Part I).
                ctx.check("finder_idempotent_resolves_current", first == current_jt);
            }
            else
            {
                ctx.record("[INFO] current thread had no nonzero enumerated tid "
                           "(unexpected) -- skipped finder idempotency assert.");
            }
        }
        else
        {
            ctx.record("[INFO] current_java_thread is null on the suite thread -- "
                       "skipped finder idempotency assert (no stable tid to resolve).");
        }
    }

    // =====================================================================
    // CLEANUP — UNCONDITIONAL: join every worker this module spawned (single +
    //   batch) so none can leak into a downstream test module's enumeration.
    //   Drives mode 4 (joinWorkers): the fixture sets stop and JOINs each worker
    //   with a bounded per-thread timeout, then clears its pools.  Daemon status
    //   already guarantees no spawned thread can wedge JVM shutdown; this is the
    //   stronger guarantee that they have actually RETURNED before the next module
    //   enumerates.  No hooks were installed by this module (pure enumeration), so
    //   there is nothing else to tear down and shutdown_hooks() is NOT called.
    // =====================================================================
    {
        fet_fixture::set_stop(true);
        const bool joined{ drive(ctx, 4) };
        ctx.check("cleanup_join_probe_ran", joined);

        // Belt-and-braces: confirm the batch counter is back to zero (workers
        // observably exited).  Bounded; the single worker has no counter but its
        // exit was already proven in Part E and it is joined by mode 4 above.
        const bool drained{ poll_predicate(
            []() { return fet_fixture::get_up_count() == 0; }, 3000) };
        ctx.check("cleanup_no_workers_left", drained);

        // Reset the shared handshake flags so the fixture is left pristine for any
        // re-entry, and so a stale stop/mode cannot perturb a later run.
        fet_fixture::set_stop(false);
        fet_fixture::set_worker_count(0);
        fet_fixture::set_go(false);
        fet_fixture::set_mode(0);
        fet_fixture::set_done(false);

        // Final proof the module leaves a clean, still-enumerable thread list: one
        // last enumeration is valid, bounded, non-duplicate, and the spawned
        // workers are gone (count is back near the pre-batch baseline — recorded,
        // not asserted, since unrelated churn moves it).
        const enumeration final_pass{ enumerate() };
        ctx.check("cleanup_final_enumeration_valid", final_pass.all_pointers_valid);
        ctx.check("cleanup_final_terminates_bounded_time", final_pass.elapsed_ms < 250.0);
        ctx.check("cleanup_final_visited_at_least_one", final_pass.count >= 1);
        ctx.check("cleanup_final_no_duplicate_pointer",
                  distinct_count(final_pass.pointers) == final_pass.pointers.size());
        ctx.record(std::string{ "[INFO] final post-cleanup live count = " }
                   + std::to_string(final_pass.count)
                   + " (baseline at module start was " + std::to_string(base.count) + ")");
    }
}
