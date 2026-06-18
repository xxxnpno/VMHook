package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_basic feature (area: hooks).
 *
 * Exercises vmhook::hook<T> / scoped_hook installed on BOTH an instance method
 * and a static method, and proves on a real JVM bytecode dispatch that:
 *   - the detour fires exactly once per Java call (the probe makes a known,
 *     deterministic number of calls; the native module asserts the fire count),
 *   - the detour sees the correct receiver (`self`) — verified by reading the
 *     instance's own `seed` field, and by calling on two DIFFERENT instances
 *     with different seeds,
 *   - the detour decodes every argument correctly across primitive widths and
 *     across the J/D 2-slot boundary (int, long, double, boolean, String),
 *   - the ORIGINAL method body still runs after a non-cancelling detour
 *     (allow-through): the returned/observed values are the unmodified results.
 *
 * The fixture follows the canonical go/done handshake.  The native module
 * raises `go`, the Harness loop runs `run()` on the Java thread (so the
 * interpreter hook fires on genuine bytecode dispatch), then the module polls
 * `done`.  Because `done` latches, every Java call the module wants observed
 * must happen inside a SINGLE `run()` invocation; the module selects which
 * scenario to run via the `mode` selector below and reads back the recorded
 * observations from the public fields.
 */
public final class HookBasic
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  The native module sets this
     * BEFORE raising `go` so a single probe cycle drives exactly the calls the
     * module is about to assert on.
     *   1 = instance touch() called INSTANCE_CALLS times (exactly-once + self + arg + allow-through)
     *   2 = static staticTouch() called STATIC_CALLS times
     *   3 = instance combine(int,long,int)        (multi-slot arg decode + self)
     *   4 = static  staticCombine(int,long,int)   (multi-slot arg decode, no self)
     *   5 = instance two-different-instances touch (self is the CORRECT instance)
     *   6 = instance wideArgs(boolean,double,String,int) (boolean/double/String decode + self)
     *   7 = instance touch() ONE call, used AFTER the module dropped its handle
     *       (proves the scoped_hook uninstalled — detour must NOT fire)
     *
     *   --- Return-value interception + arg mutation + teardown-cycle scenarios ---
     *   8  = retInt(int)      : hook overrides the int return         (set<int>)
     *   9  = retLong(int)     : hook overrides the long return        (set<long>)
     *   10 = retDouble(int)   : hook overrides the double return      (set<double>, xmm0)
     *   11 = retFloat(int)    : hook overrides the float return       (set<float>, xmm0/32)
     *   12 = retBoolean(int)  : hook overrides the boolean return     (set<bool>)
     *   13 = retByte(int)     : hook overrides a NEGATIVE byte return (sign-extension)
     *   14 = retShort(int)    : hook overrides a NEGATIVE short return (sign-extension)
     *   15 = retChar(int)     : hook overrides a char return          (zero-extension)
     *   16 = getName()        : hook returns null reference           (set<wrapper>(nullptr))
     *   17 = retInt(int)      : hook calls cancel() WITHOUT set -> Java observes 0
     *   18 = touch(int)       : hook mutates the delta arg via set_arg -> body sees it
     *   19 = touch(int)       : re-install probe (one call; counted like mode 1 but x1)
     *   20 = retInt(int)      : plain (non-scoped) hook firing probe (one call)
     */
    public static volatile int mode;

    // ---- Instance scenario data -------------------------------------------

    /** Seed for the primary instance; touch(delta) returns seed + delta. */
    private int seed = 1000;

    /** Last value the original touch() body computed (allow-through proof). */
    public static volatile int lastTouchResult;

    /** Running sum of every touch() return across one probe cycle. */
    public static volatile long touchResultSum;

    /** Number of times run() actually invoked the instance touch(). */
    public static volatile int instanceCallsMade;

    /** Number of times run() actually invoked the static staticTouch(). */
    public static volatile int staticCallsMade;

    /** combine()'s last original return (a + b + c). */
    public static volatile long combineResult;

    /** staticCombine()'s last original return. */
    public static volatile long staticCombineResult;

    /** wideArgs()'s last original return. */
    public static volatile double wideResult;

    /** Seeds of the two instances used by mode 5 (so native can cross-check self). */
    public static volatile int instanceASeed;
    public static volatile int instanceBSeed;

    /** Original results from the two-instance scenario. */
    public static volatile int twoInstanceResultA;
    public static volatile int twoInstanceResultB;

    // ---- Return-value interception observations (modes 8-20) ---------------
    /** What Java actually observed as the return of each typed ret* method. */
    public static volatile int     retIntObserved;
    public static volatile long    retLongObserved;
    public static volatile double  retDoubleObserved;
    public static volatile float   retFloatObserved;
    public static volatile boolean retBooleanObserved;
    public static volatile byte    retByteObserved;
    public static volatile short   retShortObserved;
    public static volatile char    retCharObserved;
    /** Whether getName() came back null (reference-null override proof). */
    public static volatile boolean nameWasNull;
    /** touch() result after the hook mutated its delta arg via set_arg. */
    public static volatile int     mutatedTouchResult;
    /** touch() result for the re-install / plain-hook firing probes. */
    public static volatile int     reinstallTouchResult;

    /** The UN-hooked natural return of retInt() (sanity baseline). */
    public static final int  RET_INT_NATURAL = 1;

    /** How many calls each mode is expected to drive (mirrored on native side). */
    public static final int INSTANCE_CALLS = 3;
    public static final int STATIC_CALLS = 4;

    /** The exact delta values mode 1 feeds touch(), in order. */
    public static final int TOUCH_DELTA_0 = 7;
    public static final int TOUCH_DELTA_1 = 11;
    public static final int TOUCH_DELTA_2 = 42;

    /** The exact arg mode 2 feeds staticTouch(). */
    public static final int STATIC_DELTA = 99;

    /** Multi-slot scenario constants (modes 3/4). */
    public static final int COMBINE_A = 5;
    public static final long COMBINE_B = 0x1122334455667788L;
    public static final int COMBINE_C = -13;

    /** Two-instance scenario constants (mode 5). */
    public static final int SEED_A = 2000;
    public static final int SEED_B = 30000;
    public static final int DELTA_A = 3;
    public static final int DELTA_B = 4;

    /** Wide-arg scenario constants (mode 6). */
    public static final boolean WIDE_FLAG = true;
    public static final double WIDE_D = 2.5;
    public static final String WIDE_S = "vmhook";
    public static final int WIDE_I = 77;

    // ---- Hookable methods -------------------------------------------------

    /** Hookable instance method.  Returns seed + delta. */
    public int touch(final int delta)
    {
        return this.seed + delta;
    }

    /** Hookable static method.  Returns delta * 2. */
    public static int staticTouch(final int delta)
    {
        return delta * 2;
    }

    /**
     * Multi-slot instance method: a long sits between two ints, so a correct
     * decoder must widen the long across two interpreter slots and still read
     * the trailing int from the right slot.
     */
    public long combine(final int a, final long b, final int c)
    {
        return this.seed + a + b + c;
    }

    /** Static twin of combine(): no `this`, so the first int is at slot 0. */
    public static long staticCombine(final int a, final long b, final int c)
    {
        return a + b + c;
    }

    /**
     * Wide-argument instance method exercising boolean / double / String /
     * int decode together (double consumes two slots; String is a reference).
     */
    public double wideArgs(final boolean flag, final double d, final String s, final int i)
    {
        final int slen = (s == null) ? -1 : s.length();
        return (flag ? 1.0 : 0.0) + d + slen + i;
    }

    // ---- Typed-return hookable methods (return-value interception) ---------
    // Each naturally returns a known value; a hook may override it via set<T>.
    // The native module proves Java observes the OVERRIDDEN value (set path),
    // the natural value (allow-through), or 0/null (cancel / null override).

    /** Natural return: x + 1.  Used for int set + cancel + plain-hook probes. */
    public int retInt(final int x)        { return x + 1; }

    /** Natural return: x + 1 as a long. */
    public long retLong(final int x)      { return (long) x + 1L; }

    /** Natural return: x + 0.5 as a double. */
    public double retDouble(final int x)  { return x + 0.5; }

    /** Natural return: x + 0.25 as a float. */
    public float retFloat(final int x)    { return x + 0.25f; }

    /** Natural return: x is even. */
    public boolean retBoolean(final int x) { return (x & 1) == 0; }

    /** Natural return: low byte of x. */
    public byte retByte(final int x)      { return (byte) x; }

    /** Natural return: low short of x. */
    public short retShort(final int x)    { return (short) x; }

    /** Natural return: x as a char. */
    public char retChar(final int x)      { return (char) x; }

    /** Natural return: a non-null String (hook may force null). */
    public String getName()              { return "HookBasic"; }

    private static void runInstanceTouch()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        int made = 0;
        long sum = 0;
        int r0 = obj.touch(TOUCH_DELTA_0);
        sum += r0; ++made;
        int r1 = obj.touch(TOUCH_DELTA_1);
        sum += r1; ++made;
        int r2 = obj.touch(TOUCH_DELTA_2);
        sum += r2; ++made;
        lastTouchResult = r2;
        touchResultSum = sum;
        instanceCallsMade = made;
    }

    private static void runStaticTouch()
    {
        int made = 0;
        int last = 0;
        for (int n = 0; n < STATIC_CALLS; ++n)
        {
            last = staticTouch(STATIC_DELTA);
            ++made;
        }
        lastTouchResult = last;
        staticCallsMade = made;
    }

    private static void runCombine()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        combineResult = obj.combine(COMBINE_A, COMBINE_B, COMBINE_C);
    }

    private static void runStaticCombine()
    {
        staticCombineResult = staticCombine(COMBINE_A, COMBINE_B, COMBINE_C);
    }

    private static void runTwoInstances()
    {
        final HookBasic a = new HookBasic();
        a.seed = SEED_A;
        final HookBasic b = new HookBasic();
        b.seed = SEED_B;
        instanceASeed = a.seed;
        instanceBSeed = b.seed;
        twoInstanceResultA = a.touch(DELTA_A);
        twoInstanceResultB = b.touch(DELTA_B);
    }

    private static void runWideArgs()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 0;
        wideResult = obj.wideArgs(WIDE_FLAG, WIDE_D, WIDE_S, WIDE_I);
    }

    private static void runUninstallProbe()
    {
        // One plain touch() call.  By the time the module raises mode 7 it has
        // already dropped the scoped_hook handle, so the detour must NOT fire,
        // yet the original body must still run normally.
        final HookBasic obj = new HookBasic();
        obj.seed = 500;
        lastTouchResult = obj.touch(1);
        instanceCallsMade = 1;
    }

    // ---- Return-value interception drivers --------------------------------
    // Each performs exactly ONE call to the hooked method and records what
    // Java observes as the return.  The native module sets the hook BEFORE
    // raising go, so the observed value reflects any set<T>/cancel override.

    private static void runRetInt()     { retIntObserved     = new HookBasic().retInt(RET_INT_NATURAL); }
    private static void runRetLong()    { retLongObserved    = new HookBasic().retLong(10); }
    private static void runRetDouble()  { retDoubleObserved  = new HookBasic().retDouble(10); }
    private static void runRetFloat()   { retFloatObserved   = new HookBasic().retFloat(10); }
    private static void runRetBoolean() { retBooleanObserved = new HookBasic().retBoolean(3); }
    private static void runRetByte()    { retByteObserved    = new HookBasic().retByte(0); }
    private static void runRetShort()   { retShortObserved   = new HookBasic().retShort(0); }
    private static void runRetChar()    { retCharObserved    = new HookBasic().retChar(0); }

    private static void runGetName()
    {
        final String n = new HookBasic().getName();
        nameWasNull = (n == null);
    }

    private static void runMutateArg()
    {
        // touch(int): the hook overwrites the delta arg via set_arg, then allows
        // the body through, so the result reflects the MUTATED delta.
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        mutatedTouchResult = obj.touch(1);   // hook rewrites delta -> body sees it
    }

    private static void runReinstallTouch()
    {
        // One touch() call used by the re-install + plain-hook firing probes.
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        reinstallTouchResult = obj.touch(TOUCH_DELTA_0);
    }

    private static void runPlainRetInt()
    {
        // One retInt() call for the plain (non-scoped) hook firing probe.
        retIntObserved = new HookBasic().retInt(RET_INT_NATURAL);
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookBasic.go && !HookBasic.done;
            }

            @Override
            public void run()
            {
                switch (HookBasic.mode)
                {
                    case 1:
                        runInstanceTouch();
                        break;
                    case 2:
                        runStaticTouch();
                        break;
                    case 3:
                        runCombine();
                        break;
                    case 4:
                        runStaticCombine();
                        break;
                    case 5:
                        runTwoInstances();
                        break;
                    case 6:
                        runWideArgs();
                        break;
                    case 7:
                        runUninstallProbe();
                        break;
                    case 8:
                        runRetInt();
                        break;
                    case 9:
                        runRetLong();
                        break;
                    case 10:
                        runRetDouble();
                        break;
                    case 11:
                        runRetFloat();
                        break;
                    case 12:
                        runRetBoolean();
                        break;
                    case 13:
                        runRetByte();
                        break;
                    case 14:
                        runRetShort();
                        break;
                    case 15:
                        runRetChar();
                        break;
                    case 16:
                        runGetName();
                        break;
                    case 17:
                        runRetInt();
                        break;
                    case 18:
                        runMutateArg();
                        break;
                    case 19:
                        runReinstallTouch();
                        break;
                    case 20:
                        runPlainRetInt();
                        break;
                    default:
                        break;
                }
                HookBasic.done = true;
            }
        });
    }
}
