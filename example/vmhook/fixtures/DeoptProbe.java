package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the deoptimize feature (area: deoptimization).
 *
 * Drives vmhook::deoptimize_all_jit_compiled_methods() and
 * vmhook::deoptimize_methods_if(predicate) on a LIVE JVM.  The whole point of
 * the deopt API is to force a method whose body HotSpot has JIT-compiled (so the
 * compiled code bypasses the interpreter, and therefore bypasses an interpreter
 * hook) back to interpreted execution, so a freshly installed interpreter hook
 * takes effect.  To exercise that the native module needs methods it can:
 *
 *   1. WARM to a JIT-compiled state (Method::_code != null) with NO hook armed
 *      (a hook would set NO_COMPILE and keep the method interpreted forever),
 *   2. then deoptimise and observe Method::_code go null,
 *   3. then hook + invoke once and observe the interpreter hook finally FIRE.
 *
 * Two independent hot methods live in this ONE fixture class so the native side
 * can prove predicate SELECTIVITY: deoptimize_methods_if can pick exactly
 * `hotSelected` (by method name) and leave `hotUnselected` JIT-compiled.
 *
 * Both hot methods are deliberately tiny and side-effect-light so HotSpot is
 * eager to compile them once they go hot; the warm loops vary the argument so
 * the optimiser can't fold the whole loop to a constant (which would skip
 * compiling the callee).
 *
 * Canonical go/done handshake.  `done` LATCHES, so each scenario the native side
 * wants observed must complete inside ONE run() invocation; the module selects
 * the scenario via `mode` before raising `go`, then reads back the recorded
 * counters.  The hot-loop counts are large but bounded so a single run()
 * completes well within the native probe's ~5 s poll window.
 */
public final class DeoptProbe
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Native sets this BEFORE raising
     * `go` so one probe cycle drives exactly the calls about to be asserted on.
     *   1 = warm hotSelected()   WARM_CALLS times (drive it to JIT compilation)
     *   2 = warm hotUnselected() WARM_CALLS times (drive it to JIT compilation)
     *   3 = call hotSelected(DELTA)   ONCE   (post-deopt single dispatch / hook-fire probe)
     *   4 = call hotUnselected(DELTA) ONCE   (post-deopt single dispatch / hook-fire probe)
     *   5 = warm BOTH hot methods WARM_CALLS times each (one-shot warm of the pair)
     *   6 = warm hotStatic()   WARM_CALLS times (drive the STATIC hot method to JIT)
     *   7 = call hotStatic(DELTA)   ONCE   (post-deopt single dispatch / hook-fire probe)
     */
    public static volatile int mode;

    /** Seed for the hot instance methods; hotX(delta) returns seed + delta. */
    private int seed = SEED;

    // ---- Recorded observations (allow-through proofs) ---------------------

    /** Last value the original hotSelected() body computed. */
    public static volatile int lastSelectedResult;

    /** Last value the original hotUnselected() body computed. */
    public static volatile int lastUnselectedResult;

    /** XOR accumulator of every hotSelected() return inside one probe (defeats DCE). */
    public static volatile long selectedXor;

    /** XOR accumulator of every hotUnselected() return inside one probe (defeats DCE). */
    public static volatile long unselectedXor;

    /** Number of times run() actually invoked hotSelected() in the last cycle. */
    public static volatile int selectedCallsMade;

    /** Number of times run() actually invoked hotUnselected() in the last cycle. */
    public static volatile int unselectedCallsMade;

    /** Last value the original hotStatic() body computed. */
    public static volatile int lastStaticResult;

    /** XOR accumulator of every hotStatic() return inside one probe (defeats DCE). */
    public static volatile long staticXor;

    /** Number of times run() actually invoked hotStatic() in the last cycle. */
    public static volatile int staticCallsMade;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; hotX(delta) returns seed + delta. */
    public static final int SEED = 1000;

    /** The delta fed to the hot methods on the single-call dispatch scenarios. */
    public static final int DELTA = 7;

    /** hotSelected(DELTA) / hotUnselected(DELTA) body result on a single call. */
    public static final int SINGLE_RESULT = SEED + DELTA;

    /** Seed baked into the STATIC hot method (no `this`, so a class constant). */
    public static final int STATIC_SEED = 2000;

    /** hotStatic(DELTA) body result on a single call. */
    public static final int STATIC_SINGLE_RESULT = STATIC_SEED + DELTA;

    /**
     * Iterations for the JIT-warming hot loops.  Comfortably above the default
     * C1 CompileThreshold (~1500-10000 depending on JDK / tiered) so HotSpot
     * *wants* to compile the hot methods; because NO hook is armed on them
     * during warming, HotSpot is free to populate Method::_code.
     */
    public static final int WARM_CALLS = 200000;

    // ---- Hookable / warmable hot methods ----------------------------------

    /**
     * Hot method the predicate SELECTS (and the all-sweep deoptimises).
     * Deliberately tiny so HotSpot is eager to compile it once it goes hot.
     */
    public int hotSelected(final int delta)
    {
        return this.seed + delta;
    }

    /**
     * Hot method the by-name predicate must LEAVE alone.  Same shape as
     * hotSelected so warming behaviour matches; only the name differs.
     */
    public int hotUnselected(final int delta)
    {
        return this.seed + delta;
    }

    /**
     * STATIC hot method, same (I)I descriptor as the instance pair but with no
     * receiver.  Proves the deopt dance (and a post-deopt interpreter hook) work
     * identically whether or not the Method has a `this` slot.
     */
    public static int hotStatic(final int delta)
    {
        return STATIC_SEED + delta;
    }

    private static void runWarmStatic(final int iterations)
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
        staticXor = acc;
        staticCallsMade = made;
    }

    private static void runStaticOnce(final int delta)
    {
        final int r = hotStatic(delta);
        lastStaticResult = r;
        staticXor = r;
        staticCallsMade = 1;
    }

    private static void runWarmSelected(final int iterations)
    {
        final DeoptProbe obj = new DeoptProbe();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            final int d = i & 0xFF;
            last = obj.hotSelected(d);
            acc ^= last;
            ++made;
        }
        lastSelectedResult = last;
        selectedXor = acc;
        selectedCallsMade = made;
    }

    private static void runWarmUnselected(final int iterations)
    {
        final DeoptProbe obj = new DeoptProbe();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            final int d = i & 0xFF;
            last = obj.hotUnselected(d);
            acc ^= last;
            ++made;
        }
        lastUnselectedResult = last;
        unselectedXor = acc;
        unselectedCallsMade = made;
    }

    private static void runSelectedOnce(final int delta)
    {
        final DeoptProbe obj = new DeoptProbe();
        obj.seed = SEED;
        final int r = obj.hotSelected(delta);
        lastSelectedResult = r;
        selectedXor = r;
        selectedCallsMade = 1;
    }

    private static void runUnselectedOnce(final int delta)
    {
        final DeoptProbe obj = new DeoptProbe();
        obj.seed = SEED;
        final int r = obj.hotUnselected(delta);
        lastUnselectedResult = r;
        unselectedXor = r;
        unselectedCallsMade = 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return DeoptProbe.go && !DeoptProbe.done;
            }

            @Override
            public void run()
            {
                switch (DeoptProbe.mode)
                {
                    case 1:
                        runWarmSelected(WARM_CALLS);
                        break;
                    case 2:
                        runWarmUnselected(WARM_CALLS);
                        break;
                    case 3:
                        runSelectedOnce(DELTA);
                        break;
                    case 4:
                        runUnselectedOnce(DELTA);
                        break;
                    case 5:
                        runWarmSelected(WARM_CALLS);
                        runWarmUnselected(WARM_CALLS);
                        break;
                    case 6:
                        runWarmStatic(WARM_CALLS);
                        break;
                    case 7:
                        runStaticOnce(DELTA);
                        break;
                    default:
                        break;
                }
                DeoptProbe.done = true;
            }
        });
    }
}
