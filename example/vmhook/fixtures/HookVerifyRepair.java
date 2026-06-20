package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_verify_repair feature (area: hooks / verify + repair).
 *
 * Drives vmhook::verify_hooks() (manual + auto-repair watchdog) and the JIT
 * drift path on a LIVE JVM.  The headline behaviours the native module proves:
 *
 *   - a freshly installed hook is reported INTACT (verify_hooks() == 0) and
 *     FIRES on a real bytecode dispatch,
 *   - driving the hooked method through a HOT LOOP (tens/hundreds of thousands
 *     of calls) — enough to make HotSpot want to JIT-compile it — does NOT make
 *     the interpreter hook stop firing (vmhook holds NO_COMPILE on the hooked
 *     method, so every call still routes through the patched i2i stub),
 *   - if the JVM ever does repopulate Method::_code behind vmhook's back
 *     (mode-3 JIT drift), verify_hooks() detects + re-arms it and the hook keeps
 *     firing afterward,
 *   - a deterministically-forced drift (native clears NO_COMPILE and warms the
 *     method so HotSpot compiles it) is detected by verify_hooks() as a repair
 *     event and re-armed, after which the hook fires again.
 *
 * Canonical go/done handshake.  `done` LATCHES, so each scenario the native side
 * wants observed must complete inside ONE run() invocation; the module selects
 * the scenario via `mode` before raising `go`, then reads back the recorded
 * counters.  The hot-loop count is intentionally large but bounded so a single
 * run() completes well within the native probe's ~5 s poll window.
 */
public final class HookVerifyRepair
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Native sets this BEFORE raising
     * `go` so one probe cycle drives exactly the calls about to be asserted on.
     *   1 = call hot() ONCE  (intact-hook smoke: detour must fire exactly once)
     *   2 = call hot() HOT_CALLS times in a tight loop (drive JIT compilation)
     *   3 = call hot() WARM_CALLS times — used AFTER native clears NO_COMPILE,
     *       to give HotSpot a window to actually compile the method
     *   4 = call hot() ONCE — post-repair re-check (detour must fire again)
     *   5 = call cold() ONCE — second-method smoke (multi-hook verify scenarios)
     *   6 = call BOTH hot() and cold() ONCE each — drives both hooked methods in
     *       one probe so the native side can assert a multi-hook verify count
     */
    public static volatile int mode;

    /** Seed for the hot() instance method; hot(delta) returns seed + delta. */
    private int seed = SEED;

    /** Last value the original hot() body computed (allow-through proof). */
    public static volatile int lastHotResult;

    /** XOR accumulator of every hot() return inside one probe (defeats DCE). */
    public static volatile long hotResultXor;

    /** Number of times run() actually invoked hot() in the last probe cycle. */
    public static volatile int hotCallsMade;

    /** Last value the original cold() body computed (allow-through proof). */
    public static volatile int lastColdResult;

    /** Number of times run() actually invoked cold() in the last probe cycle. */
    public static volatile int coldCallsMade;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; hot(delta) returns seed + delta. */
    public static final int SEED = 1000;

    /** The delta fed to hot() on the single-call smoke / re-check scenarios. */
    public static final int HOT_DELTA = 7;

    /**
     * Iterations for the JIT-warming hot loop (mode 2).  Comfortably above the
     * default C1 CompileThreshold (~1500-10000 depending on JDK / tiered) so
     * HotSpot *wants* to compile hot(); vmhook's NO_COMPILE is what keeps it
     * from doing so while the hook is healthy.
     */
    public static final int HOT_CALLS = 200000;

    /** Iterations for the post-NO_COMPILE-clear warm loop (mode 3). */
    public static final int WARM_CALLS = 200000;

    // ---- Hookable method --------------------------------------------------

    /**
     * Hookable instance method.  Deliberately tiny and side-effect-light so
     * HotSpot is eager to compile it once it goes hot.  Returns seed + delta.
     */
    public int hot(final int delta)
    {
        return this.seed + delta;
    }

    /**
     * Second hookable instance method, distinct Method* from hot() but the same
     * tiny shape.  Used by the multi-hook verify scenarios so the native side can
     * arm two independent hooks and assert verify_hooks() counts drift per hook.
     * Returns seed + delta, like hot().
     */
    public int cold(final int delta)
    {
        return this.seed + delta;
    }

    private static void runColdOnce(final int delta)
    {
        final HookVerifyRepair obj = new HookVerifyRepair();
        obj.seed = SEED;
        final int r = obj.cold(delta);
        lastColdResult = r;
        coldCallsMade = 1;
    }

    private static void runBothOnce(final int delta)
    {
        final HookVerifyRepair obj = new HookVerifyRepair();
        obj.seed = SEED;
        final int rh = obj.hot(delta);
        final int rc = obj.cold(delta);
        lastHotResult = rh;
        hotResultXor = rh;
        hotCallsMade = 1;
        lastColdResult = rc;
        coldCallsMade = 1;
    }

    private static void runHotOnce(final int delta)
    {
        final HookVerifyRepair obj = new HookVerifyRepair();
        obj.seed = SEED;
        final int r = obj.hot(delta);
        lastHotResult = r;
        hotResultXor = r;
        hotCallsMade = 1;
    }

    private static void runHotLoop(final int iterations)
    {
        final HookVerifyRepair obj = new HookVerifyRepair();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            // Vary the delta a little so the JIT can't fold the whole loop to a
            // constant, but keep it cheap.  The low 8 bits keep the result
            // bounded and the XOR meaningful.
            final int d = i & 0xFF;
            last = obj.hot(d);
            acc ^= last;
            ++made;
        }
        lastHotResult = last;
        hotResultXor = acc;
        hotCallsMade = made;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookVerifyRepair.go && !HookVerifyRepair.done;
            }

            @Override
            public void run()
            {
                switch (HookVerifyRepair.mode)
                {
                    case 1:
                        runHotOnce(HOT_DELTA);
                        break;
                    case 2:
                        runHotLoop(HOT_CALLS);
                        break;
                    case 3:
                        runHotLoop(WARM_CALLS);
                        break;
                    case 4:
                        runHotOnce(HOT_DELTA);
                        break;
                    case 5:
                        runColdOnce(HOT_DELTA);
                        break;
                    case 6:
                        runBothOnce(HOT_DELTA);
                        break;
                    default:
                        break;
                }
                HookVerifyRepair.done = true;
            }
        });
    }
}
