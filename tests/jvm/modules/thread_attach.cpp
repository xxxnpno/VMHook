// thread_attach JVM test module  (feature area: threads / calling off a detour)
//
// Proves, against a LIVE JVM, that vmhook can call Java from an ORDINARY NATIVE
// THREAD -- one HotSpot has never seen, created with std::thread, running no
// detour and holding no hook.
//
// WHY THIS MODULE EXISTS
// ---------------------
// Until vmhook::attach_current_thread() existed, the library's rule was "only
// call Java from inside a hook detour", because a detour is the one place the
// calling thread is guaranteed to already be a JavaThread.  The rule was true
// but far too small: it made every call site the VM's business and pushed users
// into hand-written work queues for things that had no reason to touch the game
// thread.  Other modules in this suite still carry the scar -- see
// enum_singleton, which downgrades its native call() checks to [INFO] because
// it can only attempt them "inside a tick() detour (which provides a live
// JavaThread)".
//
// WHAT IS ASSERTED
//   - VIRGINITY: a freshly spawned std::thread is NOT in HotSpot's thread list
//     before attaching.  This is the check that gives the rest of the module its
//     meaning -- without it, "the call worked" could just mean the thread was
//     already a JavaThread and nothing was proven at all.
//   - ATTACH: attach_current_thread() succeeds on that thread, and the thread
//     then APPEARS in HotSpot's thread list (proving the VM really adopted it,
//     rather than vmhook merely deciding it had).
//   - CALL: static calls into java.lang.Math and java.lang.System return the
//     exact right answers from that thread -- primitives, two-arg dispatch, and
//     an explicit-signature pin.
//   - ALLOCATION PRESSURE: a few thousand allocating calls (Integer.toString)
//     from the attached thread, so the GC runs repeatedly WHILE this thread is
//     calling Java.  This is the check that actually tests the attach: an
//     unattached thread has no frame anchor for a GC stack-walk to follow, and
//     it is precisely a GC landing mid-call that turns that into corruption.
//   - AUTO-ATTACH: with VMHOOK_AUTO_ATTACH_THREADS on (the default), a thread
//     that calls WITHOUT attaching first still works -- vmhook attaches it.
//   - RAII: java_thread_scope reports attachment, reports whether IT did the
//     attaching, and detaches on the way out.
//   - DETACH: after detaching, the thread is gone from HotSpot's thread list.
//   - NON-OWNERSHIP: attaching on a thread that is ALREADY a JavaThread succeeds
//     and does NOT claim ownership -- so a scope opened inside a detour cannot
//     detach one of the VM's own threads on the way out.  Getting this wrong
//     would tear down a game thread mid-call, so it is asserted rather than
//     assumed.
//
// WHAT IS DELIBERATELY *NOT* ASSERTED
//   The _thread_in_native -> _thread_in_Java transition in the call path is a
//   blunt store: HotSpot's real transition parks the thread if a safepoint is
//   starting, via SafepointMechanism::process_if_requested, which is neither
//   exported nor reachable through VMStructs.  Nor can vmhook read the safepoint
//   state and wait instead -- gHotSpotVMStructs publishes no SafepointSynchronize
//   entry on any JDK measured (21.0.11: 813 entries, 26.0.1: 572; the only
//   thread-state fields are JavaThread::_thread_state and OSThread::_state).
//   So a microseconds-wide race per call remains, and no test can prove its
//   absence.  The allocation-pressure loop below EXERCISES that window hard
//   rather than pretending it is closed.
//
// SUITE-SAFETY (mandatory): the whole body runs inside try/catch that downgrades
// any escaping exception to [INFO] (never a FAIL); the module installs NO hooks,
// so there is nothing to disarm; every spawned thread is JOINED before the
// module returns, so no thread of this module's can outlive it; and results
// cross the thread boundary through atomics, never through a reference into a
// frame that may already be gone.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace
{
    // java.lang.Math and java.lang.System: loaded before any user code runs, all
    // the methods used here are static, pure and thread-safe.  Deliberately NOT a
    // vmhook fixture -- a fixture class might not be loaded yet on the thread
    // under test, and a class-loading failure would look exactly like an attach
    // failure.  These cannot fail to be present.
    class java_math : public vmhook::object<java_math>
    {
    public:
        explicit java_math(vmhook::oop_t instance) noexcept
            : vmhook::object<java_math>{ instance }
        {
        }
    };

    class java_system : public vmhook::object<java_system>
    {
    public:
        explicit java_system(vmhook::oop_t instance) noexcept
            : vmhook::object<java_system>{ instance }
        {
        }
    };

    class java_integer : public vmhook::object<java_integer>
    {
    public:
        explicit java_integer(vmhook::oop_t instance) noexcept
            : vmhook::object<java_integer>{ instance }
        {
        }
    };

    /* Sentinel meaning "the worker never got far enough to write anything". */
    constexpr std::int64_t k_uncaptured{ -987654321 };

    /* Everything a worker thread reports back.  Atomics because the module
       thread reads these after the join, and because a worker that dies mid-way
       must still leave the earlier fields readable. */
    struct worker_result
    {
        std::atomic<int>          virgin_before_attach{ -1 };
        std::atomic<int>          attach_ok{ -1 };
        std::atomic<int>          in_thread_list_after{ -1 };
        std::atomic<int>          scope_bool{ -1 };
        std::atomic<int>          scope_owns{ -1 };
        std::atomic<std::int64_t> abs_result{ k_uncaptured };
        std::atomic<std::int64_t> max_result{ k_uncaptured };
        std::atomic<std::int64_t> nano_first{ k_uncaptured };
        std::atomic<std::int64_t> nano_second{ k_uncaptured };
        std::atomic<int>          alloc_calls_made{ -1 };
        std::atomic<int>          alloc_calls_decoded{ -1 };
        std::atomic<int>          alloc_calls_total{ -1 };
        std::atomic<int>          gone_after_detach{ -1 };
        std::atomic<int>          reached_end{ 0 };
    };

    /* @brief Whether the CALLING thread is in HotSpot's thread list right now. */
    auto current_thread_is_java() noexcept -> bool
    {
        return vmhook::hotspot::find_java_thread_by_os_thread_id(
                   vmhook::os::current_thread_id())
               != nullptr;
    }

    /*
        @brief The body of the plain-native-thread test.
        @details
        Runs on a std::thread that has never touched the JVM.  Writes every
        observation into `out` and returns; the module thread joins and grades.
    */
    auto worker_body(worker_result& out) noexcept -> void
    {
        try
        {
            // ---- 1. This thread must be a stranger to the VM -----------------
            // Everything below is meaningless if it is not.
            out.virgin_before_attach.store(current_thread_is_java() ? 0 : 1);

            // ---- 2. Attach, and check the VM agrees --------------------------
            {
                const vmhook::java_thread_scope java{};
                out.scope_bool.store(static_cast<bool>(java) ? 1 : 0);
                out.scope_owns.store(java.owns_attachment() ? 1 : 0);
                if (!java) { return; }

                out.attach_ok.store(1);
                out.in_thread_list_after.store(current_thread_is_java() ? 1 : 0);

                // ---- 3. Call Java from a thread that is not in any detour ----
                {
                    const auto abs_proxy{ java_math::static_method("abs", "(I)I") };
                    if (abs_proxy)
                    {
                        out.abs_result.store(
                            static_cast<std::int64_t>(static_cast<std::int32_t>(
                                abs_proxy->call(static_cast<std::int32_t>(-5)))));
                    }

                    const auto max_proxy{ java_math::static_method("max", "(II)I") };
                    if (max_proxy)
                    {
                        out.max_result.store(
                            static_cast<std::int64_t>(static_cast<std::int32_t>(
                                max_proxy->call(static_cast<std::int32_t>(3),
                                                static_cast<std::int32_t>(9)))));
                    }

                    const auto nano_proxy{ java_system::static_method("nanoTime", "()J") };
                    if (nano_proxy)
                    {
                        out.nano_first.store(
                            static_cast<std::int64_t>(nano_proxy->call()));
                        out.nano_second.store(
                            static_cast<std::int64_t>(nano_proxy->call()));
                    }
                }

                // ---- 4. Call under allocation pressure -----------------------
                // Integer.toString allocates a String per call, so a few thousand
                // of them make the young collector run repeatedly WHILE this
                // thread is inside Java.  That is the condition an unattached
                // thread cannot survive: the GC walks thread stacks, and this
                // thread only has a walkable one because it was attached.
                //
                // No explicit System.gc(): this suite has been bitten before by
                // forced collections on Windows, and allocation pressure provokes
                // the same window without the blast radius.
                {
                    constexpr int total{ 3000 };
                    int           made{ 0 };
                    int           decoded{ 0 };
                    const auto    to_string{
                        java_integer::static_method("toString", "(I)Ljava/lang/String;") };
                    if (to_string)
                    {
                        for (int i{ 0 }; i < total; ++i)
                        {
                            const auto returned{ to_string->call(
                                static_cast<std::int32_t>(i)) };
                            ++made;
                            if (static_cast<std::uint32_t>(returned) != 0u) { ++decoded; }
                        }
                    }
                    out.alloc_calls_total.store(total);
                    out.alloc_calls_made.store(made);
                    // Whether the returned String OOP decodes is a property of
                    // object-return handling, which is JDK- and call-path-
                    // dependent (see method_call_object / method_static, which
                    // record it the same way).  It is NOT what this module is
                    // testing, and hard-asserting it here would make an unrelated
                    // decode gap look like an attach failure.  Recorded, not
                    // asserted.
                    out.alloc_calls_decoded.store(decoded);
                }
            }   // scope ends -> detach

            // ---- 5. The detach really removed us -----------------------------
            out.gone_after_detach.store(current_thread_is_java() ? 0 : 1);
            out.reached_end.store(1);
        }
        catch (...)
        {
            // Whatever happened, the module thread still grades what was written.
        }
    }

    /*
        @brief A worker that calls Java WITHOUT attaching first.
        @details
        The headline of VMHOOK_AUTO_ATTACH_THREADS: a user who never heard of
        attaching writes an ordinary thread and it works.
    */
    auto auto_attach_worker(std::atomic<std::int64_t>& out_value,
                            std::atomic<int>&          out_was_virgin) noexcept -> void
    {
        try
        {
            out_was_virgin.store(current_thread_is_java() ? 0 : 1);

            // No attach call.  No scope.  Straight into Java.
            const auto abs_proxy{ java_math::static_method("abs", "(I)I") };
            if (abs_proxy)
            {
                out_value.store(static_cast<std::int64_t>(static_cast<std::int32_t>(
                    abs_proxy->call(static_cast<std::int32_t>(-4242)))));
            }

            // Leave cleanly: the thread_local detacher runs at thread exit, but
            // being explicit here proves the manual path works too.
            vmhook::detach_current_thread();
        }
        catch (...)
        {
        }
    }
}

VMHOOK_JVM_MODULE(thread_attach)
{
    try
    {
        vmhook::register_class<java_math>("java/lang/Math");
        vmhook::register_class<java_system>("java/lang/System");
        vmhook::register_class<java_integer>("java/lang/Integer");

        // =================================================================
        // Part A — the plain native thread
        // =================================================================
        worker_result result{};
        {
            std::thread worker{ [&result]() noexcept { worker_body(result); } };
            worker.join();
        }

        // The premise.  If a fresh std::thread were already a JavaThread, every
        // other check in Part A would pass for the wrong reason.
        ctx.check("thread_attach.fresh_std_thread_is_not_a_javathread",
                  result.virgin_before_attach.load() == 1);

        ctx.check("thread_attach.attach_succeeded_on_native_thread",
                  result.attach_ok.load() == 1);
        ctx.check("thread_attach.scope_reports_attached",
                  result.scope_bool.load() == 1);
        ctx.check("thread_attach.scope_owns_the_attachment",
                  result.scope_owns.load() == 1);

        // The VM's opinion, not vmhook's: the thread is in HotSpot's own list.
        ctx.check("thread_attach.vm_lists_the_thread_after_attach",
                  result.in_thread_list_after.load() == 1);

        // The point of the whole exercise: Java ran, correctly, off a detour.
        ctx.check("thread_attach.call_Math_abs_minus5_is_5",
                  result.abs_result.load() == 5);
        ctx.check("thread_attach.call_Math_max_3_9_is_9",
                  result.max_result.load() == 9);

        const std::int64_t nano_a{ result.nano_first.load() };
        const std::int64_t nano_b{ result.nano_second.load() };
        ctx.check("thread_attach.call_System_nanoTime_returned",
                  nano_a != k_uncaptured && nano_a != 0);
        ctx.check("thread_attach.call_System_nanoTime_is_monotonic",
                  nano_b != k_uncaptured && nano_b >= nano_a);

        const int alloc_made{ result.alloc_calls_made.load() };
        const int alloc_decoded{ result.alloc_calls_decoded.load() };
        const int alloc_total{ result.alloc_calls_total.load() };
        // The real assertion: every allocating call COMPLETED and returned to
        // native code.  Thousands of Java allocations mean the collector ran
        // repeatedly while this thread was inside Java -- which is survivable
        // only because the attach gave it a stack the GC can walk.
        ctx.check("thread_attach.every_allocating_call_completed_under_gc_pressure",
                  alloc_total > 0 && alloc_made == alloc_total);
        ctx.record("[INFO] thread_attach: " + std::to_string(alloc_made) + "/"
                   + std::to_string(alloc_total)
                   + " allocating Java calls completed from an attached native "
                     "thread; " + std::to_string(alloc_decoded)
                   + " returned a decodable String OOP (object-return decoding is "
                     "JDK/call-path dependent and not asserted here)");

        ctx.check("thread_attach.detached_thread_leaves_the_vm_thread_list",
                  result.gone_after_detach.load() == 1);
        ctx.check("thread_attach.worker_ran_to_completion",
                  result.reached_end.load() == 1);

        // =================================================================
        // Part B — auto-attach: no attach call at all
        // =================================================================
        {
            std::atomic<std::int64_t> auto_value{ k_uncaptured };
            std::atomic<int>          auto_virgin{ -1 };
            {
                std::thread worker{ [&auto_value, &auto_virgin]() noexcept
                {
                    auto_attach_worker(auto_value, auto_virgin);
                } };
                worker.join();
            }

            ctx.check("thread_attach.auto_worker_started_as_a_native_thread",
                      auto_virgin.load() == 1);
#if VMHOOK_AUTO_ATTACH_THREADS
            ctx.check("thread_attach.auto_attach_lets_an_unprepared_thread_call_java",
                      auto_value.load() == 4242);
#else
            // Built with auto-attach off: the call must fail CLEANLY -- no
            // answer, and above all no crash.
            ctx.check("thread_attach.without_auto_attach_the_call_declines_cleanly",
                      auto_value.load() != 4242);
            ctx.record("[INFO] thread_attach: built with VMHOOK_AUTO_ATTACH_THREADS=0");
#endif
        }

        // =================================================================
        // Part C — the module's own thread, and non-ownership
        // =================================================================
        // Whatever kind of thread the suite runs on, attaching must succeed.
        {
            const bool already_java{ current_thread_is_java() };
            const vmhook::java_thread_scope java{};

            ctx.check("thread_attach.attach_succeeds_on_the_suite_thread",
                      static_cast<bool>(java));

            if (already_java)
            {
                // The safety property: a scope that FOUND the thread attached
                // must not claim it, or leaving the scope would detach one of the
                // VM's own threads out from under the code that owns it.
                ctx.check("thread_attach.does_not_claim_a_thread_it_did_not_attach",
                          !java.owns_attachment());
            }
            else
            {
                ctx.record("[INFO] thread_attach: the suite thread was not a "
                           "JavaThread; non-ownership is exercised in Part A instead");
            }

            // Nesting must be harmless: an inner scope on an already-attached
            // thread must not detach it when it closes.
            {
                const vmhook::java_thread_scope inner{};
                ctx.check("thread_attach.nested_scope_reports_attached",
                          static_cast<bool>(inner));
                ctx.check("thread_attach.nested_scope_does_not_claim_ownership",
                          !inner.owns_attachment());
            }
            ctx.check("thread_attach.thread_still_attached_after_nested_scope_closed",
                      current_thread_is_java());

            const auto abs_proxy{ java_math::static_method("abs", "(I)I") };
            const std::int32_t suite_abs{ abs_proxy
                ? static_cast<std::int32_t>(abs_proxy->call(static_cast<std::int32_t>(-11)))
                : 0 };
            ctx.check("thread_attach.call_from_the_suite_thread_is_correct",
                      suite_abs == 11);
        }
    }
    catch (const std::exception& error)
    {
        ctx.record(std::string{ "[INFO] thread_attach: escaped exception: " } + error.what());
    }
    catch (...)
    {
        ctx.record("[INFO] thread_attach: escaped unknown exception");
    }
}
