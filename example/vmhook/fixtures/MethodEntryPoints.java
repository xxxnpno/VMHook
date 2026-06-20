package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_entry_points_i2i_i2c feature (area: HotSpot Method
 * entry-point accessor layer).
 *
 * The native module drives the live Method* entry-point accessors directly:
 *   - method::get_i2i_entry() / get_from_interpreted_entry() / get_from_compiled_entry()
 *   - method::set_from_interpreted_entry() / set_from_compiled_entry() / set_code()
 *   - method::get_adapter() and get_c2i_entry_from_adapter() (c2i recovery)
 *
 * To exercise every entry-point shape it needs methods of several kinds:
 *
 *   warm(I)I   - a tiny INSTANCE method warmed to a JIT-compiled state so the
 *                native side can read a NON-null _from_compiled_entry and recover
 *                the c2i adapter on a method that really has a compiled body.
 *   touched(I)I- a second INSTANCE method dispatched at least once (LINKED) but
 *                NOT warmed; proves the get_adapter() offset is process-wide by
 *                resolving the adapter on a DIFFERENT method than `warm`.
 *   sset(I)I   - a STATIC method, dispatched once, used for the setter round-trip
 *                so the native side can mutate entry points on a method it never
 *                hooks (a clean throwaway target, distinct from the hooked ones).
 *   quiet(I)I  - a NEVER-DISPATCHED boundary method; its entry points still
 *                resolve (offsets are class-wide) but interpreted dispatch has
 *                never linked it, characterised on the native side.
 *
 * Canonical go/done handshake; `done` LATCHES, so each native-observed scenario
 * completes inside ONE run() invocation.  Native selects the scenario via `mode`
 * before raising `go`, then reads back the recorded counters.
 */
public final class MethodEntryPoints
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Native sets this BEFORE raising
     * `go` so one probe cycle drives exactly the calls about to be asserted on.
     *   1 = call warm(DELTA)    ONCE  (single dispatch / hook-fire probe)
     *   2 = warm warm()         WARM_CALLS times (drive it to JIT compilation)
     *   3 = call touched(DELTA) ONCE  (link the second instance method)
     *   4 = call sset(DELTA)    ONCE  (link the static throwaway method)
     *   5 = call iset(DELTA)    ONCE  (link the SECOND static throwaway method)
     *   6 = call touched(DELTA) WARM_CALLS times (drive the second instance
     *                           method to its own JIT-compiled state, so the
     *                           native side can prove the adapter offset latches
     *                           independently of warm() ever compiling)
     */
    public static volatile int mode;

    /** Seed for the hot instance methods; warm(delta) returns seed + delta. */
    private int seed = SEED;

    // ---- Recorded observations (allow-through proofs) ---------------------

    /** Last value the original warm() body computed. */
    public static volatile int lastWarmResult;

    /** Last value the original touched() body computed. */
    public static volatile int lastTouchedResult;

    /** Last value the original sset() body computed. */
    public static volatile int lastSsetResult;

    /** Last value the original iset() body computed. */
    public static volatile int lastIsetResult;

    /** XOR accumulator of every warm() return inside one probe (defeats DCE). */
    public static volatile long warmXor;

    /** XOR accumulator of every touched() return inside one probe (defeats DCE). */
    public static volatile long touchedXor;

    /** Number of times run() actually invoked warm() in the last cycle. */
    public static volatile int warmCallsMade;

    /** Number of times run() actually invoked touched() in the last cycle. */
    public static volatile int touchedCallsMade;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; warm(delta) returns seed + delta. */
    public static final int SEED = 2000;

    /** The delta fed to the hot methods on the single-call dispatch scenarios. */
    public static final int DELTA = 11;

    /** warm(DELTA) / touched(DELTA) body result on a single call. */
    public static final int SINGLE_RESULT = SEED + DELTA;

    /** sset(DELTA) body result (static; no seed). */
    public static final int SSET_RESULT = DELTA + 100;

    /** iset(DELTA) body result (static; no seed). */
    public static final int ISET_RESULT = DELTA + 200;

    /**
     * Iterations for the JIT-warming hot loop.  Comfortably above the default
     * C1 CompileThreshold so HotSpot wants to compile warm(); because NO hook is
     * armed on it during warming, HotSpot is free to populate Method::_code.
     */
    public static final int WARM_CALLS = 200000;

    // ---- Hookable / warmable methods --------------------------------------

    /**
     * The headline INSTANCE method.  Warmed to a JIT-compiled state so the
     * native side can read a non-null _from_compiled_entry and recover c2i.
     */
    public int warm(final int delta)
    {
        return this.seed + delta;
    }

    /**
     * A SECOND instance method, dispatched once (linked) but not warmed.  Lets
     * the native side resolve get_adapter() on a method other than warm() to
     * prove the detected _adapter offset is process-wide.
     */
    public int touched(final int delta)
    {
        return this.seed + delta;
    }

    /**
     * A STATIC method, dispatched once.  The native side uses it as a clean,
     * never-hooked throwaway target for the entry-point setter round-trip.
     */
    public static int sset(final int delta)
    {
        return delta + 100;
    }

    /**
     * A SECOND STATIC method, dispatched once.  Gives the native side a second,
     * independent clean throwaway target for the entry-point setter round-trip
     * so it can prove setter/getter offset agreement on a method distinct from
     * sset() (and never hooked) -- the FIX-C dance must land identically on any
     * Method, not just the first one probed.
     */
    public static int iset(final int delta)
    {
        return delta + 200;
    }

    /**
     * A method that run() NEVER dispatches.  Its entry points still resolve
     * (offsets are class-wide) but interpreted dispatch has never linked it;
     * the native side characterises that boundary case.
     */
    public int quiet(final int delta)
    {
        return this.seed - delta;
    }

    private static void runCallWarmOnce(final int delta)
    {
        final MethodEntryPoints obj = new MethodEntryPoints();
        obj.seed = SEED;
        final int r = obj.warm(delta);
        lastWarmResult = r;
        warmXor = r;
        warmCallsMade = 1;
    }

    private static void runWarm(final int iterations)
    {
        final MethodEntryPoints obj = new MethodEntryPoints();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            final int d = i & 0xFF;
            last = obj.warm(d);
            acc ^= last;
            ++made;
        }
        lastWarmResult = last;
        warmXor = acc;
        warmCallsMade = made;
    }

    private static void runCallTouchedOnce(final int delta)
    {
        final MethodEntryPoints obj = new MethodEntryPoints();
        obj.seed = SEED;
        lastTouchedResult = obj.touched(delta);
    }

    private static void runWarmTouched(final int iterations)
    {
        final MethodEntryPoints obj = new MethodEntryPoints();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            final int d = i & 0xFF;
            last = obj.touched(d);
            acc ^= last;
            ++made;
        }
        lastTouchedResult = last;
        touchedXor = acc;
        touchedCallsMade = made;
    }

    private static void runCallSsetOnce(final int delta)
    {
        lastSsetResult = sset(delta);
    }

    private static void runCallISetOnce(final int delta)
    {
        lastIsetResult = iset(delta);
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodEntryPoints.go && !MethodEntryPoints.done;
            }

            @Override
            public void run()
            {
                switch (MethodEntryPoints.mode)
                {
                    case 1:
                        runCallWarmOnce(DELTA);
                        break;
                    case 2:
                        runWarm(WARM_CALLS);
                        break;
                    case 3:
                        runCallTouchedOnce(DELTA);
                        break;
                    case 4:
                        runCallSsetOnce(DELTA);
                        break;
                    case 5:
                        runCallISetOnce(DELTA);
                        break;
                    case 6:
                        runWarmTouched(WARM_CALLS);
                        break;
                    default:
                        break;
                }
                MethodEntryPoints.done = true;
            }
        });
    }
}
