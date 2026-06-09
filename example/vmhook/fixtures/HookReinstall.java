package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_reinstall_after_shutdown feature (area: hooks /
 * lifecycle / REVIVE).
 *
 * This fixture is the live-bytecode counterpart to the native module that
 * proves vmhook::shutdown_hooks() is REVERSIBLE in the strongest sense: a hook
 * installed, torn down by shutdown_hooks(), and then installed AGAIN must come
 * back to life and fire on real dispatch.  Historically this was a flaw --
 * shutdown_hooks() latched vmhook::hotspot::g_shutdown_requested (and
 * detail::auto_repair::g_started, and the high-level watcher install latches)
 * true forever, so after one teardown every fresh install reported success but
 * its detour was silently dead (common_detour early-returns on the flag, and
 * the auto-repair watchdog refused to respawn).  The fix resets all three on
 * the way out of shutdown_hooks() (vmhook.hpp:~8867-8880).  This fixture lets
 * the native side prove the REVIVE happens, over and over, on instance AND
 * static methods, and via BOTH teardown surfaces (scoped_hook RAII drop AND
 * the bulk shutdown_hooks()).
 *
 * Every hookable method body is a PURE, deterministic function of its argument
 * and the instance `seed` (no hidden state), so the native side can assert the
 * result is byte-exact the original after every teardown -- there is nothing a
 * hook could perturb except by being genuinely in the dispatch path.
 *
 * Canonical go/done handshake: the native module sets `mode`, raises `go`, the
 * Harness loop runs run() on the Java thread (genuine bytecode dispatch, which
 * is what makes an interpreter hook fire), then the module polls the latched
 * `done`.  Because `done` latches, every Java call a single assertion needs
 * must happen in one run() invocation; the module selects the scenario via
 * `mode` and clears `done` on the rising edge of `go`.
 *
 * JAVA 8 SOURCE: no var / records / switch-expressions / text-blocks / lambdas
 * in fields; only an anonymous Harness.Probe.  Compiles identically under javac
 * 8 and javac 25+ (verified with javac --release 8 and --release 21; the CI
 * compiles fixtures with `javac -encoding UTF-8`, and this file is pure ASCII so
 * the encoding is irrelevant either way).
 */
public final class HookReinstall
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Set by the native module BEFORE
     * it raises `go` so a single probe cycle drives exactly the calls it
     * asserts on.
     *   1 = instance ping(int) called PING_CALLS times  (revive on an INSTANCE
     *       method, the headline cycle target)
     *   2 = static  sping(int) called SPING_CALLS times  (revive on a STATIC
     *       method -- no `this`, arg at slot 0)
     *   3 = ping(int) once AND sping(int) once in the SAME run()  (multi-method
     *       revive: one teardown must silence both, one re-arm must revive both)
     */
    public static volatile int mode;

    // ---- Instance scenario data -------------------------------------------

    /** Seed for the instance; ping(delta) returns seed + delta. */
    private int seed = SEED;

    /** Last value the original ping() body computed (byte-exact-original proof). */
    public static volatile int lastPingResult;

    /** Last value the original sping() body computed. */
    public static volatile int lastSpingResult;

    /** Number of times run() actually invoked the instance ping(). */
    public static volatile int pingCallsMade;

    /** Number of times run() actually invoked the static sping(). */
    public static volatile int spingCallsMade;

    /**
     * Total number of run() invocations across the whole module.  Monotonic; a
     * cheap liveness witness the native side can watch increase across the many
     * shutdown->reinstall cycles to confirm the Java probe really ran each time
     * (independent of whether the detour fired).
     */
    public static volatile int runEpoch;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; ping folds this into its result. */
    public static final int SEED = 9001;

    /** How many times each repeat-mode drives its method. */
    public static final int PING_CALLS = 3;
    public static final int SPING_CALLS = 2;

    /** The exact delta ping() is fed (same every call so the result is stable). */
    public static final int PING_DELTA = 41;

    /** The exact delta sping() is fed. */
    public static final int SPING_DELTA = 13;

    /** Pre-computed original (un-hooked) results, mirrored natively. */
    public static final int PING_ORIGINAL = SEED + PING_DELTA;     // 9042
    public static final int SPING_ORIGINAL = SPING_DELTA * 7;      // 91

    // ---- Hookable methods (pure functions; no side effects in the body) ----

    /** Hookable instance method.  Returns seed + delta. */
    public int ping(final int delta)
    {
        return this.seed + delta;
    }

    /** Hookable static method.  Returns delta * 7. */
    public static int sping(final int delta)
    {
        return delta * 7;
    }

    private static void runPing()
    {
        final HookReinstall obj = new HookReinstall();
        obj.seed = SEED;
        int made = 0;
        int last = 0;
        for (int n = 0; n < PING_CALLS; ++n)
        {
            last = obj.ping(PING_DELTA);
            ++made;
        }
        lastPingResult = last;
        pingCallsMade = made;
    }

    private static void runSping()
    {
        int made = 0;
        int last = 0;
        for (int n = 0; n < SPING_CALLS; ++n)
        {
            last = sping(SPING_DELTA);
            ++made;
        }
        lastSpingResult = last;
        spingCallsMade = made;
    }

    private static void runBoth()
    {
        final HookReinstall obj = new HookReinstall();
        obj.seed = SEED;
        lastPingResult = obj.ping(PING_DELTA);
        pingCallsMade = 1;
        lastSpingResult = sping(SPING_DELTA);
        spingCallsMade = 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookReinstall.go && !HookReinstall.done;
            }

            @Override
            public void run()
            {
                // Reset the per-cycle counters so each probe cycle's fire-count
                // assertions are independent of prior cycles.
                HookReinstall.pingCallsMade = 0;
                HookReinstall.spingCallsMade = 0;

                switch (HookReinstall.mode)
                {
                    case 1:
                        runPing();
                        break;
                    case 2:
                        runSping();
                        break;
                    case 3:
                        runBoth();
                        break;
                    default:
                        break;
                }

                HookReinstall.runEpoch = HookReinstall.runEpoch + 1;
                HookReinstall.done = true;
            }
        });
    }
}
