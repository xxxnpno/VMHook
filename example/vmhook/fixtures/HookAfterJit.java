package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_install_after_jit feature (area: hooks / deopt-on-install).
 *
 * Proves the headline behaviour the README promises for the
 * "install a hook on an ALREADY-JIT-compiled method" path on a LIVE JVM:
 *
 *   - a method (hot(int)) is first warmed to JIT compilation BEFORE any hook is
 *     installed (a tight hot loop drives HotSpot to publish Method::_code != null),
 *   - the native module then installs a vmhook::hook<T> on that warm method.  At
 *     install time vmhook observes _code != null ("is JIT-compiled") and must
 *     DEOPTIMISE the method back to the interpreter — clear Method::_code and
 *     redirect the entry points to the c2i adapter / i2i stub — so the patched
 *     i2i stub actually takes effect,
 *   - driving hot() once more afterward FIRES the detour (the deopt routed the
 *     freshly-resolving dispatch through the interpreter and our patch), the
 *     detour sees the correct receiver + decoded arg, and (non-cancelling)
 *     allow-through leaves the original body result intact,
 *   - a CANCELLING detour can force a return value even on the formerly-compiled
 *     method (proves we own the dispatch, not just observe it),
 *   - after the hook is removed (shutdown_hooks), hot() runs normally again and
 *     the detour does NOT fire.
 *
 * Canonical go/done handshake.  `done` LATCHES, so each scenario the native side
 * wants observed must complete inside ONE run() invocation; the module selects
 * the scenario via `mode` before raising `go`, then reads back the recorded
 * counters.  The native side ORDERS the phases: warm (mode 1/3) happens with NO
 * hook armed, install happens on the native thread BETWEEN probe cycles, then the
 * single-call probes (mode 2/4) drive the post-install / post-removal dispatch.
 *
 * The hot-loop count is intentionally large but bounded so a single run()
 * completes well within the native probe's poll window, and so HotSpot has ample
 * budget to compile hot() before the native side installs the hook.
 */
public final class HookAfterJit
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Native sets this BEFORE raising
     * `go` so one probe cycle drives exactly the calls about to be asserted on.
     *   1 = WARM hot() WARM_CALLS times in a tight loop, NO hook installed yet
     *       (drives HotSpot to JIT-compile hot() so Method::_code becomes non-null
     *        BEFORE the native side installs its hook),
     *   2 = call hot() ONCE with HOT_DELTA (post-install: detour must fire once,
     *       correct self + arg; allow-through OR forced-return per native control),
     *   3 = WARM hot() WARM_CALLS times again (a second JIT window; used by native
     *       to re-establish _code != null if the first warm did not stick, still
     *       BEFORE the hook is installed),
     *   4 = call hot() ONCE with HOT_DELTA (post-removal: detour must NOT fire,
     *       original body still runs).
     *   5 = WARM hotStatic() WARM_CALLS times, NO hook installed yet (drives the
     *       JIT on a STATIC method so the native side can install after it is hot).
     *   6 = call hotStatic() ONCE with HOT_DELTA (post-install on a static method:
     *       detour must fire once, first arg at slot 0, allow-through OR forced).
     *   7 = WARM both overloads over(int) and over(int,int) WARM_CALLS times,
     *       NO hook installed yet (so the native side can hook ONE overload on an
     *       already-hot pair and prove only the selected descriptor is detoured).
     *   8 = call over(int) ONCE and over(int,int) ONCE with HOT_DELTA-based args
     *       (post-install: exactly the hooked overload fires; the sibling runs raw).
     *   9 = run the compiled caller loop callerLoop(CALLER_CALLS): a SEPARATE
     *       method that repeatedly dispatches hot() so HotSpot can compile the
     *       CALLER (and inline hot() into it) BEFORE the native install — the
     *       README "catches direct callers" case.  Records how many times the
     *       caller body actually executed (callerIterations) so native can compare
     *       against the detour fire-count to characterise the stale-IC window.
     *  10 = run callerLoop(CALLER_CALLS) AGAIN (post-install probe for the compiled
     *       caller: native compares fires-vs-iterations; the deopt sweep is the
     *       documented fix for stale inline caches).
     *  11 = call hot() N_REPEAT times in one probe (post-install: the detour must
     *       fire on EVERY dispatch — proves a hooked method does not silently
     *       re-JIT past the hook within a probe; native asserts fires == N_REPEAT).
     *  12 = call hotStatic() N_REPEAT times in one probe (same exact-count contract
     *       on the static path).
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

    /** Last value the original hotStatic() body computed (allow-through proof). */
    public static volatile int lastStaticResult;

    /** Last value over(int) returned in the last probe (allow-through proof). */
    public static volatile int lastOver1Result;

    /** Last value over(int,int) returned in the last probe (allow-through proof). */
    public static volatile int lastOver2Result;

    /** XOR of every value the compiled caller loop observed (defeats DCE). */
    public static volatile long callerResultXor;

    /** Number of times the compiled caller loop body actually ran hot(). */
    public static volatile int callerIterations;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; hot(delta) returns seed + delta. */
    public static final int SEED = 1000;

    /** The delta fed to hot() on the single-call probes (modes 2 / 4). */
    public static final int HOT_DELTA = 7;

    /**
     * Iterations for the JIT-warming hot loop (modes 1 / 3).  Comfortably above
     * the default C1/C2 thresholds (~1500-10000 depending on JDK / tiered) so
     * HotSpot compiles hot() and publishes Method::_code — the precondition for
     * the "install on an already-JIT'd method" scenario.  Because NO hook is
     * armed during these warm loops, nothing inhibits the compilation.
     */
    public static final int WARM_CALLS = 200000;

    /** Base added by the STATIC hookable method; hotStatic(delta) = STATIC_BASE + delta. */
    public static final int STATIC_BASE = 2000;

    /** Constant the single-int overload over(int) adds; over(a) = a + OVER1_ADD. */
    public static final int OVER1_ADD = 30;

    /** Constant the two-int overload over(int,int) adds; over(a,b) = a + b + OVER2_ADD. */
    public static final int OVER2_ADD = 40;

    /**
     * Iterations of the compiled-caller loop (mode 9 / 10).  Comfortably above the
     * tiered thresholds so HotSpot compiles callerLoop itself (and is free to
     * inline hot() into it) BEFORE the native side installs its hook on hot().
     */
    public static final int CALLER_CALLS = 200000;

    /**
     * Repeat count for the "detour fires on EVERY dispatch" probes (mode 11 / 12).
     * Small and bounded: this runs AFTER the hook is installed, so each iteration
     * is an interpreter dispatch through the detour; we only need enough to prove
     * the count is exact, not to re-warm the JIT.
     */
    public static final int N_REPEAT = 64;

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
     * Hookable STATIC method.  Tiny and side-effect-light so HotSpot compiles it
     * once it goes hot.  Has NO 'this', so its first (and only) argument lives at
     * interpreter slot 0 — the static-method slot model the native detour relies
     * on.  Returns STATIC_BASE + delta.
     */
    public static int hotStatic(final int delta)
    {
        return STATIC_BASE + delta;
    }

    /**
     * Single-int overload of over().  Shares the name over with the two-int
     * overload below; the two are distinguished ONLY by JVM descriptor, so hooking
     * exactly one of them proves overload selection survives the after-JIT deopt.
     * Returns a + OVER1_ADD.
     */
    public int over(final int a)
    {
        return a + OVER1_ADD;
    }

    /**
     * Two-int overload of over().  Returns a + b + OVER2_ADD.
     */
    public int over(final int a, final int b)
    {
        return a + b + OVER2_ADD;
    }

    private static void runHotOnce(final int delta)
    {
        final HookAfterJit obj = new HookAfterJit();
        obj.seed = SEED;
        final int r = obj.hot(delta);
        lastHotResult = r;
        hotResultXor = r;
        hotCallsMade = 1;
    }

    private static void runHotLoop(final int iterations)
    {
        final HookAfterJit obj = new HookAfterJit();
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

    private static void runStaticLoop(final int iterations)
    {
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            final int d = i & 0xFF;
            last = hotStatic(d);
            acc ^= last;
            ++made;
        }
        lastStaticResult = last;
        hotResultXor = acc;
        hotCallsMade = made;
    }

    private static void runStaticOnce(final int delta)
    {
        final int r = hotStatic(delta);
        lastStaticResult = r;
        hotResultXor = r;
        hotCallsMade = 1;
    }

    private static void runStaticRepeat(final int times)
    {
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < times; ++i)
        {
            last = hotStatic(HOT_DELTA);
            acc ^= last;
            ++made;
        }
        lastStaticResult = last;
        hotResultXor = acc;
        hotCallsMade = made;
    }

    private static void runHotRepeat(final int times)
    {
        final HookAfterJit obj = new HookAfterJit();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < times; ++i)
        {
            last = obj.hot(HOT_DELTA);
            acc ^= last;
            ++made;
        }
        lastHotResult = last;
        hotResultXor = acc;
        hotCallsMade = made;
    }

    private static void runOverloadLoop(final int iterations)
    {
        final HookAfterJit obj = new HookAfterJit();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int r1 = 0;
        int r2 = 0;
        for (int i = 0; i < iterations; ++i)
        {
            final int a = i & 0xFF;
            r1 = obj.over(a);
            r2 = obj.over(a, a);
            acc ^= (r1 ^ r2);
            ++made;
        }
        lastOver1Result = r1;
        lastOver2Result = r2;
        hotResultXor = acc;
        hotCallsMade = made;
    }

    private static void runOverloadOnce()
    {
        final HookAfterJit obj = new HookAfterJit();
        obj.seed = SEED;
        lastOver1Result = obj.over(HOT_DELTA);
        lastOver2Result = obj.over(HOT_DELTA, HOT_DELTA);
        hotCallsMade = 2;
    }

    private static void runCallerLoop(final int iterations)
    {
        // A SEPARATE caller body so HotSpot can compile this loop (and is free to
        // inline hot() into it) independently of hot() itself.  Returns through a
        // field so the JIT cannot dead-code-eliminate the whole loop.
        final HookAfterJit obj = new HookAfterJit();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        for (int i = 0; i < iterations; ++i)
        {
            acc ^= obj.hot(HOT_DELTA);
            ++made;
        }
        callerResultXor = acc;
        callerIterations = made;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookAfterJit.go && !HookAfterJit.done;
            }

            @Override
            public void run()
            {
                switch (HookAfterJit.mode)
                {
                    case 1:
                        runHotLoop(WARM_CALLS);
                        break;
                    case 2:
                        runHotOnce(HOT_DELTA);
                        break;
                    case 3:
                        runHotLoop(WARM_CALLS);
                        break;
                    case 4:
                        runHotOnce(HOT_DELTA);
                        break;
                    case 5:
                        runStaticLoop(WARM_CALLS);
                        break;
                    case 6:
                        runStaticOnce(HOT_DELTA);
                        break;
                    case 7:
                        runOverloadLoop(WARM_CALLS);
                        break;
                    case 8:
                        runOverloadOnce();
                        break;
                    case 9:
                        runCallerLoop(CALLER_CALLS);
                        break;
                    case 10:
                        runCallerLoop(CALLER_CALLS);
                        break;
                    case 11:
                        runHotRepeat(N_REPEAT);
                        break;
                    case 12:
                        runStaticRepeat(N_REPEAT);
                        break;
                    default:
                        break;
                }
                HookAfterJit.done = true;
            }
        });
    }
}
