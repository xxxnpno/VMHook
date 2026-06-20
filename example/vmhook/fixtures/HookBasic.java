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
     *
     *   --- Deepening (batch-16): arg-shape input classes ----------------------
     *   21 = manyArgs(...)        : instance, 8 args incl TWO longs + a double +
     *                               a String interleaved (slot alignment past the
     *                               8-arg boundary; every trailing slot correct)
     *   22 = staticManyArgs(...)  : static twin, WIDE-FIRST ordering (long at slot
     *                               0-1), no `this`
     *   23 = boundaryCombine(...) : combine driven with Integer.MIN / Long.MIN /
     *                               Integer.MAX (extreme primitive arg values)
     *   24 = boundaryCombine(...) : same method, driven with Integer.MAX / -1L
     *                               (all-ones long) / Integer.MIN
     *   25 = floatArg(float,int)  : instance, float arg decode + trailing int
     *   26 = nullRefArgs(String,HookBasic) : null String arg (-> empty std::string)
     *                               + null object arg (-> null unique_ptr)
     *   27 = objArg(HookBasic,int) : a REAL (non-this) object-wrapper arg whose
     *                               seed the detour reads (object arg != receiver)
     *   28 = coexist : touch() AND staticTouch() BOTH hooked at once, both called
     *                  in one dispatch (two distinct hooks coexisting, each decodes
     *                  its own arg)
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

    // ---- Deepening (batch-16) observations --------------------------------
    /** manyArgs() / staticManyArgs() allow-through results. */
    public static volatile long    manyArgsResult;
    public static volatile long    staticManyArgsResult;
    /** boundaryCombine() allow-through result. */
    public static volatile long    boundaryCombineResult;
    /** floatArg() allow-through result. */
    public static volatile float   floatArgResult;
    /** nullRefArgs() allow-through result (true iff both refs were null). */
    public static volatile boolean nullRefArgsBothNull;
    /** objArg() allow-through result (the seed of the passed-in object). */
    public static volatile int     objArgResult;
    /** coexist allow-through results for the two simultaneously-hooked methods. */
    public static volatile int     coexistInstanceResult;
    public static volatile int     coexistStaticResult;

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

    // ---- Deepening (batch-16) constants -----------------------------------
    /**
     * manyArgs(int,long,int,double,int,String,long,int): EIGHT declared args
     * with TWO longs + a double interleaved, so a correct decoder must keep
     * every trailing slot aligned well PAST the 8-arg / register-allocation
     * boundary.  Declaration-order values, chosen so each differs from the
     * others (a slot-shift would surface as a mismatched decode).
     */
    public static final int    MANY_A = 11;                       // slot (this=0) 1
    public static final long   MANY_B = 0x0A0B0C0D0E0F1011L;      // slots 2-3 (wide)
    public static final int    MANY_C = 22;                       // slot 4
    public static final double MANY_D = 6.5;                      // slots 5-6 (wide, exact)
    public static final int    MANY_E = 33;                       // slot 7
    public static final String MANY_F = "many";                   // slot 8 (ref)
    public static final long   MANY_G = -0x7FEEDDCCBBAA9988L;     // slots 9-10 (wide)
    public static final int    MANY_H = 44;                       // slot 11 (trailing int)

    /**
     * staticManyArgs(long,int,double,String,int,long): WIDE-FIRST (long at
     * slot 0-1, no `this`), exercising the offset table from index 0 with a
     * 2-slot leading type.
     */
    public static final long   SMANY_A = 0x1213141516171819L;    // slots 0-1 (wide, first)
    public static final int    SMANY_B = 55;                      // slot 2
    public static final double SMANY_C = -12.75;                  // slots 3-4 (wide, exact)
    public static final String SMANY_D = "swide";                 // slot 5 (ref)
    public static final int    SMANY_E = 66;                      // slot 6
    public static final long   SMANY_F = 0x2122232425262728L;    // slots 7-8 (wide, trailing)

    /** floatArg(float,int): a float arg (1-slot, distinct decode lane) + trailing int. */
    public static final float  FLOAT_ARG = -3.25f;                // exact in IEEE-754
    public static final int    FLOAT_TRAILING_I = 88;

    /** objArg(HookBasic,int): the passed-in object's seed (distinct from `this`). */
    public static final int    OBJ_ARG_SEED = 7777;
    public static final int    OBJ_ARG_DELTA = 9;

    /** coexist: the two arg values fed to the two simultaneously-hooked methods. */
    public static final int    COEXIST_INSTANCE_DELTA = 13;
    public static final int    COEXIST_STATIC_DELTA = 21;

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

    /**
     * Eight-argument instance method with TWO longs and a double interleaved.
     * A correct slot decoder must keep every trailing arg aligned past the
     * 8-arg / register boundary; the original body sums them so allow-through
     * proves the body ran with the unmodified args.
     */
    public long manyArgs(final int a, final long b, final int c, final double d,
                         final int e, final String f, final long g, final int h)
    {
        final int flen = (f == null) ? -1 : f.length();
        return this.seed + a + b + c + (long) d + e + flen + g + h;
    }

    /** Static twin of manyArgs with a WIDE leading arg (long at slot 0-1, no `this`). */
    public static long staticManyArgs(final long a, final int b, final double c,
                                      final String d, final int e, final long f)
    {
        final int dlen = (d == null) ? -1 : d.length();
        return a + b + (long) c + dlen + e + f;
    }

    /**
     * combine() variant driven with EXTREME primitive arg values (Integer.MIN,
     * Long.MIN, -1L all-ones, etc.).  Same (int,long,int) shape; the body sums
     * with a wide accumulator so overflow is well-defined two's-complement.
     */
    public long boundaryCombine(final int a, final long b, final int c)
    {
        return (long) a + b + c;
    }

    /** Float arg (1-slot, distinct lane) followed by a trailing int. */
    public double floatArg(final float f, final int i)
    {
        return (double) f + i;
    }

    /**
     * Two reference args, both passed null by the driver: a String (decodes to
     * an empty std::string in the detour) and a HookBasic object (decodes to a
     * null unique_ptr).  Body reports whether both were null.
     */
    public boolean nullRefArgs(final String s, final HookBasic obj)
    {
        return s == null && obj == null;
    }

    /**
     * A REAL object-wrapper arg distinct from `this`: the detour decodes `other`
     * and reads ITS seed, proving an object ARGUMENT (not the receiver) is
     * decoded to the correct instance.  Body returns the other's seed + delta.
     */
    public int objArg(final HookBasic other, final int delta)
    {
        return other.seed + delta;
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

    // ---- Deepening (batch-16) drivers -------------------------------------

    private static void runManyArgs()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        manyArgsResult = obj.manyArgs(MANY_A, MANY_B, MANY_C, MANY_D,
                                      MANY_E, MANY_F, MANY_G, MANY_H);
    }

    private static void runStaticManyArgs()
    {
        staticManyArgsResult = staticManyArgs(SMANY_A, SMANY_B, SMANY_C,
                                              SMANY_D, SMANY_E, SMANY_F);
    }

    private static void runBoundaryCombineExtreme()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        boundaryCombineResult =
            obj.boundaryCombine(Integer.MIN_VALUE, Long.MIN_VALUE, Integer.MAX_VALUE);
    }

    private static void runBoundaryCombineAllOnes()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        boundaryCombineResult =
            obj.boundaryCombine(Integer.MAX_VALUE, -1L, Integer.MIN_VALUE);
    }

    private static void runFloatArg()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 0;
        floatArgResult = (float) obj.floatArg(FLOAT_ARG, FLOAT_TRAILING_I);
    }

    private static void runNullRefArgs()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 0;
        nullRefArgsBothNull = obj.nullRefArgs(null, null);
    }

    private static void runObjArg()
    {
        final HookBasic receiver = new HookBasic();
        receiver.seed = 0;
        final HookBasic other = new HookBasic();
        other.seed = OBJ_ARG_SEED;
        objArgResult = receiver.objArg(other, OBJ_ARG_DELTA);
    }

    private static void runCoexist()
    {
        // Both touch() (instance) and staticTouch() (static) are hooked at once.
        // One dispatch calls each exactly once so both detours fire in the same
        // probe cycle, each decoding its own arg.
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        coexistInstanceResult = obj.touch(COEXIST_INSTANCE_DELTA);
        coexistStaticResult = staticTouch(COEXIST_STATIC_DELTA);
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
                    case 21:
                        runManyArgs();
                        break;
                    case 22:
                        runStaticManyArgs();
                        break;
                    case 23:
                        runBoundaryCombineExtreme();
                        break;
                    case 24:
                        runBoundaryCombineAllOnes();
                        break;
                    case 25:
                        runFloatArg();
                        break;
                    case 26:
                        runNullRefArgs();
                        break;
                    case 27:
                        runObjArg();
                        break;
                    case 28:
                        runCoexist();
                        break;
                    default:
                        break;
                }
                HookBasic.done = true;
            }
        });
    }
}
