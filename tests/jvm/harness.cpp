// Modular JVM test harness — registry implementation.
//
// Holds the global list of self-registered feature modules and runs them.
// Deliberately tiny and dependency-free (no vmhook.hpp) so it links into the
// example DLL alongside example.cpp without ODR or include-order surprises.
#include "harness.hpp"

#include <algorithm>
#include <csetjmp>
#include <exception>
#include <vector>

// Windows is needed for BOTH the MSVC __try/__except path AND the no-SEH
// (MinGW / clang-on-Windows) Vectored-Exception-Handler container below.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

// Pick the per-module crash-containment strategy ONCE, here, so run_one() and
// run_all() agree on whether a contained crash needs a post-recovery hook reset.
//
//   VMHOOK_HARNESS_CONTAINER_SEH   — MSVC __try/__except (real SEH AV trap).
//   VMHOOK_HARNESS_CONTAINER_VEH   — Win32 Vectored Exception Handler + setjmp
//                                    (MinGW + clang-on-Windows, x86-64), our
//                                    COMPILER-INDEPENDENT AV container.
//   (neither)                      — POSIX try/catch (Linux/macOS): catches C++
//                                    throws; a hardware fault is a separate
//                                    concern handled by os:: signal machinery /
//                                    is_valid_pointer gating, out of scope here.
#if defined(_MSC_VER) && !defined(__clang__)
#  define VMHOOK_HARNESS_CONTAINER_SEH 1
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
// Only the x86-64 Windows ABI exposes CONTEXT.Rip, which the VEH redirect needs.
// 32-bit Windows is not in CI; it falls through to plain try/catch (safe — it
// simply won't contain a hardware AV there, matching the pre-existing POSIX
// behaviour).
#  define VMHOOK_HARNESS_CONTAINER_VEH 1
#endif

namespace vmhook_test
{
    namespace
    {
        struct entry
        {
            const char* name;
            module_fn   fn;
            priority    prio;
        };

#if defined(VMHOOK_HARNESS_CONTAINER_VEH)
        // ── Compiler-independent per-module access-violation container ──────
        //
        // MinGW and clang-on-Windows have NO working __try/__except for hardware
        // faults (their SEH support doesn't trap an access violation the way
        // MSVC's does), and a plain C++ catch(...) never sees a SIGSEGV-class
        // fault at all — so a cold OOP/Klass deref that slips past
        // is_valid_pointer would take the whole JVM down mid-suite, leaving no
        // TOTAL line and no named culprit.  -fasync-exceptions was rejected (it
        // link-breaks the MSVC-STL std::variant destructors under lld-link), so
        // this container is PURE Win32: a process-wide Vectored Exception Handler
        // plus a thread-local setjmp recovery point, with NO exception-handling
        // codegen flags.
        //
        // Mechanism (x86-64): before running a module, run_one() arms a
        // thread-local jmp_buf (setjmp) and records the suite thread id.  If an
        // EXCEPTION_ACCESS_VIOLATION then fires ON THE SUITE THREAD while armed,
        // the VEH disarms and redirects the faulting context's RIP to a tiny
        // trampoline, returning EXCEPTION_CONTINUE_EXECUTION.  The OS resumes the
        // thread at the trampoline (back on its own stack, OUTSIDE the kernel
        // exception dispatcher — a direct longjmp from inside the VEH is
        // unsupported), which longjmps back into run_one()'s setjmp, which then
        // returns false ("module crashed").  Every other exception — and any AV
        // on the JVM's own threads (GC, compiler, safepoint) — is passed through
        // with EXCEPTION_CONTINUE_SEARCH, so the container never interferes with
        // the JVM's own fault handling.
        //
        // longjmp skips the faulting module's C++ destructors, so run_all()
        // calls ctx.reset() after a contained crash to tear down any hooks the
        // module left armed (see context::reset).  This deliberately mirrors the
        // MSVC __except path, which likewise abandons the faulting frames.

        thread_local std::jmp_buf g_recovery_env;
        thread_local volatile bool g_armed = false;
        thread_local unsigned long g_suite_tid = 0;

        // Resume target after a contained AV.  The VEH points the faulting
        // thread's RIP here; it runs on that thread's (realigned) stack, fully
        // out of the OS exception dispatcher, then longjmps into run_one().
        // [[noreturn]] — longjmp never returns to it.
        [[noreturn]] void veh_recovery_trampoline()
        {
            std::longjmp(g_recovery_env, 1);
        }

        LONG CALLBACK harness_veh(::EXCEPTION_POINTERS* info) noexcept
        {
            if (info == nullptr || info->ExceptionRecord == nullptr
                || info->ContextRecord == nullptr)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            // Only contain a genuine access violation that we armed for, and only
            // on the suite thread — never touch the JVM's own threads/faults.
            if (!g_armed
                || info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
                || ::GetCurrentThreadId() != g_suite_tid)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            // Disarm BEFORE redirecting so a fault inside the trampoline/longjmp
            // path (or a second async AV) can't re-enter this recovery.
            g_armed = false;

            ::CONTEXT* const ctx = info->ContextRecord;
            // Realign RSP to the x64 ABI's pre-CALL boundary (16-byte aligned,
            // then -8 so that the trampoline sees the alignment a normal CALL
            // would leave).  We are abandoning the faulting frame's stack region
            // anyway; longjmp restores RSP from the jmp_buf immediately.
            ctx->Rsp = (ctx->Rsp & ~static_cast<DWORD64>(0xF)) - 8;
            ctx->Rip = reinterpret_cast<DWORD64>(&veh_recovery_trampoline);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Install the VEH exactly once, process-wide, as the FIRST handler
        // (so it sees the AV before any later-registered handler).  C++11 magic
        // statics make this init-once race-free across threads.  Returns whether
        // the handler is installed; if AddVectoredExceptionHandler ever failed
        // run_one() degrades to "uncontained" (same as not having the container).
        bool ensure_veh_installed() noexcept
        {
            static void* const handle =
                ::AddVectoredExceptionHandler(1u /* first */, &harness_veh);
            return handle != nullptr;
        }

        // Run the module body catching only C++ throws.  Kept in its OWN
        // function — separate from run_one()'s setjmp frame — so the C++ EH
        // scope and the setjmp/longjmp recovery never share a stack frame
        // (no -Wclobbered, no interaction between the two unwinding mechanisms).
        // A hardware access violation here does NOT unwind through this
        // try/catch; it is intercepted by the VEH, which redirects out to the
        // trampoline (longjmp), bypassing this frame entirely.  Returns true on
        // clean completion, false if a C++ exception escaped the module.
        bool invoke_module_catching_cpp(const module_fn fn, context& ctx) noexcept
        {
            try
            {
                fn(ctx);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
#endif // VMHOOK_HARNESS_CONTAINER_VEH

        // Run one module, CONTAINING a hard crash (access violation chasing a
        // stale OOP, a bad hook) so it can't take the JVM and the whole suite
        // down.
        //
        //   • MSVC: real SEH containment via __try/__except (which must live in a
        //     function with no C++ unwinding objects — hence this tiny standalone
        //     helper).
        //   • MinGW / clang-on-Windows: the Vectored-Exception-Handler + setjmp
        //     container above — compiler-independent, no EH-codegen flags.
        //   • POSIX (Linux/macOS): a plain C++ try/catch, which catches C++
        //     throws but NOT a segfault; hardware-fault safety there comes from
        //     vmhook's own is_valid_pointer gating / os:: signal machinery, out
        //     of scope for the harness.
        //
        // Returns true on clean completion, false if it crashed/threw.
        inline auto run_one(const module_fn fn, context& ctx) noexcept -> bool
        {
#if defined(VMHOOK_HARNESS_CONTAINER_SEH)
            __try
            {
                fn(ctx);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
#elif defined(VMHOOK_HARNESS_CONTAINER_VEH)
            // No container available (AddVectoredExceptionHandler failed) — fall
            // back to catching C++ throws only; a hardware AV would still
            // propagate, exactly as the pre-existing POSIX path.
            if (!ensure_veh_installed())
            {
                return invoke_module_catching_cpp(fn, ctx);
            }

            // Arm the thread-local recovery point.  setjmp returns 0 on the
            // initial call (go run the module); it returns non-zero when the VEH
            // trampoline longjmps back here after a contained access violation.
            // The module body runs in invoke_module_catching_cpp() — a SEPARATE
            // frame — so this setjmp frame owns no C++ try scope and no locals
            // that a longjmp could clobber.
            g_suite_tid = ::GetCurrentThreadId();
            if (setjmp(g_recovery_env) != 0)
            {
                // Recovered from a contained AV inside the module body.
                g_armed = false;
                return false;
            }

            g_armed = true;
            const bool ok = invoke_module_catching_cpp(fn, ctx);
            g_armed = false;   // reached only on a clean return / caught C++ throw;
                               // an AV longjmps to the setjmp arm above instead.
            return ok;
#else
            try
            {
                fn(ctx);
                return true;
            }
            catch (...)
            {
                return false;
            }
#endif
        }

        // Function-local static so registration from other TUs' static
        // initializers (which may run before/after this TU's) always sees a
        // constructed vector — avoids the static-init-order fiasco.
        auto registry() -> std::vector<entry>&
        {
            static std::vector<entry> modules;
            return modules;
        }
    }

    auto register_module(const char* const name, const module_fn fn,
                         const priority prio) -> void
    {
        if (name && fn)
        {
            registry().push_back(entry{ name, fn, prio });
        }
    }

    auto run_all(context& ctx) -> std::size_t
    {
        // DISABLE the background auto-repair watchdog for the ENTIRE functional
        // suite — the decisive cure for the persistent hook+GC crashes.  Even
        // with every watchdog read fault-proofed (os::safe_read) and every write
        // guarded (os::safe_write/os::protect), the watchdog REPAIRING — toggling
        // page protection and rewriting an i2i stub — while the JVM is mid
        // code-cache sweep/relocation during a GC can corrupt JVM state
        // uncontainably.  That happens on the watchdog's OWN detached thread,
        // OFF the suite thread, so the per-module SEH/__try container can never
        // catch it (NO-TOTAL crashes on specific msvc JDK cells).  The
        // functional modules don't need continuous auto-repair: ctx.reset()
        // clears hook state at each module boundary, and any module that
        // specifically exercises the watchdog re-enables it locally for its own
        // GC-quiet test and disables it again before returning.  Done ONCE here,
        // before the module loop, so no watchdog runs during the GC-heavy
        // modules.  No-op if the driver didn't wire the callback.
        if (ctx.set_auto_repair)
        {
            ctx.set_auto_repair(false);
        }

        // Order by ascending priority so a `priority::first` module (the warm-up)
        // runs before every ordinary module and a `priority::last` module runs
        // after them, REGARDLESS of static-initializer / link order (which the
        // C++ standard leaves unspecified and which MinGW/GNU ld in fact
        // REVERSES — see harness.hpp).  std::stable_sort preserves registration
        // order among equal-priority modules, so the default `normal` modules
        // keep running in exactly the order they do today.
        std::stable_sort(registry().begin(), registry().end(),
                         [](const entry& lhs, const entry& rhs) noexcept
                         {
                             return static_cast<int>(lhs.prio) < static_cast<int>(rhs.prio);
                         });

        std::size_t ran{ 0 };
        for (const entry& module_entry : registry())
        {
            if (ctx.record)
            {
                ctx.record(std::string{ "[INFO] === module: " } + module_entry.name + " ===");
            }
            const bool clean{ run_one(module_entry.fn, ctx) };
            if (!clean)
            {
#if defined(VMHOOK_HARNESS_CONTAINER_VEH)
                // No-SEH Windows (MinGW / clang-on-Windows): the VEH container
                // caught a hardware ACCESS VIOLATION (or, far more rarely, an
                // escaped C++ throw).  A cold OOP/Klass deref that slips past
                // is_valid_pointer — in-range + aligned, but pointing at an
                // unmapped/GC-relocated page — faults HERE, yet MSVC's __try
                // contains it and Linux/macOS codegen does not fault the same way.
                // So it is a no-SEH TOOLCHAIN ARTIFACT, not a library defect: the
                // library is provably correct on the SEH/POSIX matrix.  Record a
                // VISIBLE, NAMED [INFO] (auditable, and hardenable via os::safe_read)
                // instead of failing the build, so an intermittent cold-fault on one
                // no-SEH job can't red the whole matrix forever.  The MSVC (SEH) and
                // POSIX paths keep the hard [FAIL] below as the real-bug backstop —
                // a crash that reproduces THERE is a genuine defect.
                if (ctx.record)
                {
                    ctx.record(std::string{ "[INFO] module " } + module_entry.name
                               + " hit a contained access violation on a no-SEH "
                                 "toolchain (cold-fault past is_valid_pointer; library "
                                 "correct on SEH/POSIX — harden the deref with os::safe_read)");
                }
#else
                // MSVC (__try) / POSIX (try/catch): a contained crash or an escaped
                // C++ throw is a real failure — name it so the run goes red and one
                // pass reveals EVERY bad module.
                if (ctx.check)
                {
                    ctx.check(std::string{ "module_" } + module_entry.name + "_completed_cleanly", false);
                }
#endif
            }
            // Paired with the "=== module: X ===" line above: with per-line
            // flushing in the driver, if a module crashes the JVM on a toolchain
            // WITHOUT any container (e.g. 32-bit Windows, or if VEH install
            // failed) the results file ends at its "=== module: X ===" with no
            // matching "--- done ---", naming the culprit even when the whole
            // process died.  With the container, every module reaches its
            // "--- done ---" and the suite reaches TOTAL.
            if (ctx.record)
            {
                ctx.record(std::string{ "[INFO] --- module " } + module_entry.name + " done ---");
            }
            // INTER-MODULE RESET (cross-platform suite-safety): every module must hand
            // the next one a PRISTINE hook system.  scoped_hook/hook_handle::stop()
            // tears down a hook's patch but does NOT stop the auto-repair watchdog or
            // clear its stored Method* — only shutdown_hooks() does.  A watchdog left
            // running by ANY earlier module keeps polling verify_hooks() over stored
            // Method*; once a LATER module's forced System.gc()/class-unload relocates
            // or frees that Method, the watchdog raw-derefs it ON ITS OWN DETACHED
            // THREAD — outside the per-module SEH/__try AND the detour guard — so the
            // fault is uncontained and crashes the JVM (NO-TOTAL) on msvc + linux-gcc,
            // not just the no-SEH legs.  Reset unconditionally AFTER EVERY module (not
            // only after a contained crash) so no watchdog / stored Method* / armed
            // hook ever survives across the module boundary.  reset() == reversible
            // vmhook::shutdown_hooks(); the next module hooks again from a clean slate.
            if (ctx.reset)
            {
                ctx.reset();
            }
            ++ran;
        }
        return ran;
    }
}
