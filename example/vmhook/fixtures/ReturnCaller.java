package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the return_caller feature (area: hooks).
 *
 * The feature under test is, FROM INSIDE A HOOK on a leaf method:
 *   - {@code return_value::caller()} reports the IMMEDIATE calling method
 *     (class_name / method_name / signature) when that caller is an
 *     interpreted HotSpot frame, and {@code valid()==false} when the
 *     immediate caller is compiled (JIT) or native, and
 *   - {@code return_value::stack_trace()} returns the interpreted frames in
 *     order, with the immediate caller at index 0 and progressively older
 *     interpreter frames after it.
 *
 * Because {@code caller()} / {@code stack_trace()} walk the LIVE interpreter
 * saved-rbp chain at the moment the detour fires, the entire call chain must
 * execute as real Java bytecode on the Java thread.  The native side therefore
 * never builds frames itself: it hooks {@link #inner(int)} (always the same
 * leaf, signature {@code (I)I}) and then drives this fixture to build different
 * caller chains above that leaf via the {@code mode} selector.  The detour
 * records, for each invocation, what {@code caller()} and {@code stack_trace()}
 * reported; the native module reads those recordings back and asserts on them.
 *
 * The leaf {@code inner(int)} keeps a fixed name+descriptor on purpose: only
 * the CALLER varies between scenarios, so every assertion about caller-identity
 * is attributable to the walk and not to which method was hooked.
 *
 * Scenario selector (native sets {@code mode} + clears {@code done} on the
 * rising edge of {@code go}; each scenario is one probe cycle):
 *   1  = depth-2 chain  outerA(int) -> inner(int)
 *        (immediate caller is outerA, interpreted)
 *   2  = depth-3 chain  outerB(int) -> middle(int) -> inner(int)
 *        (immediate caller is middle; outerB sits one frame deeper)
 *   3  = deep self-recursion recurse(int) calling inner at the leaf, depth
 *        well beyond the default stack_trace cap (so truncation + the
 *        max_depth contract + "no infinite loop" can be exercised)
 *   4  = caller with a LONG reference-heavy descriptor longSig(...)->inner
 *        (proves signature is not truncated)
 *   5  = caller forced to JIT-compile: warmCaller(int) is invoked in a hot
 *        loop ABOVE the C2 threshold first, THEN once more to fire the leaf;
 *        the immediate caller frame is then (very likely) compiled, so
 *        caller().valid() must be false / the trace must omit it
 *   6  = TWO DIFFERENT interpreted callers in one cycle: alpha(int)->inner
 *        then beta(int)->inner, so the native side proves caller() reports
 *        the correct DISTINCT caller each fire (not a stale cache)
 *   7  = caller with a primitive-but-non-(I)I descriptor: longArgCaller(long)
 *        -> inner, so the reported signature is "(J)I" not "(I)I"
 *   8  = caller whose descriptor packs EVERY JVM primitive kind in declaration
 *        order: manyPrims(boolean,byte,char,short,int,float,double,long)->inner,
 *        descriptor "(ZBCSIFDJ)I" (each base-type letter exercised once)
 *   9  = caller whose descriptor carries ARRAY params: arrayArgs(int[],
 *        String[][])->inner, descriptor "([I[[Ljava/lang/String;)I" (single-dim
 *        primitive array + multi-dim reference array)
 *  10  = caller declared in a DISTINCT nested class Helper.bridge(int)->inner,
 *        so caller().class_name is "vmhook/fixtures/ReturnCaller$Helper" — the
 *        class-name analogue of mode 7's signature distinctness
 *  11  = caller that is a CONSTRUCTOR: a second ReturnCaller(int) <init> body
 *        calls inner, so caller().method_name is the angle-bracket name "<init>"
 *  12  = STATIC caller staticCaller(int)->inner (vs the otherwise all-instance
 *        callers): same interpreter layout, method_name "staticCaller"
 *  13  = the SAME interpreted caller stable(int)->inner fired THREE times in one
 *        cycle: every fire must report the identical class/method/signature AND
 *        the identical Method* — the stability dual of mode 6's distinctness,
 *        and caller() must be idempotent within a single detour
 *
 * Java 8 syntax only (no var / records / switch-expr / text-blocks).
 */
public final class ReturnCaller
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector (native programs this before raising go). */
    public static volatile int mode;

    /** Observable leaf side effect (sum of inner() arguments this cycle). */
    public static volatile int observed;

    /** How many times inner() actually ran this cycle (handshake proof). */
    public static volatile int innerCalls;

    /**
     * Iteration count for the JIT-warmup scenario (mode 5).  Large enough to
     * push warmCaller well past the default tiered C1/C2 compile thresholds
     * (C2 default ~10000 invocations) so the method is compiled by the time
     * the LAST call drives the leaf.  The native side does NOT depend on the
     * exact value — only that it is "hot enough".
     */
    public static final int WARMUP_ITERATIONS = 200000;

    /** Expected number of distinct leaf calls for the recursion scenario. */
    public static final int RECURSION_DEPTH = 90;

    /** Constant args used by the various callers (mirrored on native side). */
    public static final int  ARG_OUTER_A   = 11;
    public static final int  ARG_OUTER_B   = 12;
    public static final int  ARG_LONGSIG   = 13;
    public static final int  ARG_WARM      = 14;
    public static final int  ARG_ALPHA     = 15;
    public static final int  ARG_BETA      = 16;
    public static final long ARG_LONG      = 17L;
    public static final int  ARG_MANYPRIMS = 18;
    public static final int  ARG_ARRAYARGS = 19;
    public static final int  ARG_HELPER    = 20;
    public static final int  ARG_CTOR      = 21;
    public static final int  ARG_STATIC    = 22;
    public static final int  ARG_STABLE    = 23;

    /** How many times the stable() caller fires the leaf this cycle (mode 13). */
    public static final int STABLE_FIRES = 3;

    // ---- Constructors -------------------------------------------------------

    /** Default constructor (used by the scenario runners). */
    public ReturnCaller()
    {
    }

    /**
     * Int-arg constructor whose {@code <init>} body is the immediate caller of
     * inner for mode 11, so caller().method_name is the angle-bracket "<init>".
     */
    public ReturnCaller(final int x)
    {
        this.inner(x);
    }

    // ---- The fixed leaf -----------------------------------------------------

    /**
     * The single hooked leaf.  Name + descriptor are FIXED at {@code (I)I} so
     * only the caller above it varies between scenarios.  Kept tiny so HotSpot
     * leaves it interpreted (the native detour replaces its entry anyway).
     */
    public int inner(final int x)
    {
        innerCalls++;
        observed += x;
        return x * 2;
    }

    // ---- mode 1: depth-2 interpreted caller ---------------------------------

    /** Immediate interpreted caller for mode 1. */
    public int outerA(final int x)
    {
        return this.inner(x + 1) + 1;
    }

    // ---- mode 2: depth-3 interpreted chain ----------------------------------

    /** Outer frame for mode 2 (one level deeper than the immediate caller). */
    public int outerB(final int x)
    {
        return this.middle(x + 1) + 1;
    }

    /** Immediate interpreted caller for mode 2 (sits between outerB and inner). */
    public int middle(final int x)
    {
        return this.inner(x + 1) + 1;
    }

    // ---- mode 3: deep recursion ---------------------------------------------

    /**
     * Self-recursion: descends {@code depth} frames, then calls inner at the
     * bottom.  Every recursive frame has the SAME name+descriptor, so the
     * native side can assert that the deep portion of the trace is uniform and
     * that the walk stops cleanly at max_depth without spinning.
     */
    public int recurse(final int depth)
    {
        if (depth <= 0)
        {
            return this.inner(1);
        }
        return this.recurse(depth - 1) + 1;
    }

    // ---- mode 4: long reference-heavy descriptor ----------------------------

    /**
     * Caller whose JVM descriptor is deliberately long: eight Object params
     * plus a trailing int yields
     *   (Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;
     *    Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;I)I
     * which is > 140 chars — far past anything a fixed buffer would hold.  The
     * native side mirrors the exact expected descriptor string.
     */
    public int longSig(final Object a, final Object b, final Object c, final Object d,
                       final Object e, final Object f, final Object g, final Object h,
                       final int x)
    {
        // touch every ref so javac cannot elide the params
        final int n = (a != null ? 1 : 0) + (b != null ? 1 : 0) + (c != null ? 1 : 0)
                    + (d != null ? 1 : 0) + (e != null ? 1 : 0) + (f != null ? 1 : 0)
                    + (g != null ? 1 : 0) + (h != null ? 1 : 0);
        return this.inner(x + n);
    }

    // ---- mode 5: JIT-compiled immediate caller ------------------------------

    /**
     * Caller that the probe drives in a hot loop so HotSpot compiles it.  The
     * loop body returns a value the JIT cannot constant-fold away (depends on
     * the loop counter), and the FINAL call (the one that actually reaches the
     * leaf) happens after the method is hot — so the immediate caller frame on
     * the stack at the moment the detour fires is a COMPILED frame.  The
     * feature contract: caller() is invalid for a compiled immediate caller.
     */
    public int warmCaller(final int x)
    {
        // Only the sentinel value drives the leaf; all warmup iterations do
        // pure arithmetic so they stay cheap but still trip the counters.
        if (x < 0)
        {
            return this.inner(ARG_WARM);
        }
        return (x * 31) ^ (x + 7);
    }

    // ---- mode 6: two distinct interpreted callers ---------------------------

    /** First distinct interpreted caller for mode 6. */
    public int alpha(final int x)
    {
        return this.inner(x) + 1;
    }

    /** Second distinct interpreted caller for mode 6. */
    public int beta(final int x)
    {
        return this.inner(x) + 2;
    }

    // ---- mode 7: caller with a (J)I descriptor ------------------------------

    /** Caller whose descriptor is (J)I, so caller().signature must reflect J. */
    public int longArgCaller(final long v)
    {
        return this.inner((int) v);
    }

    // ---- mode 8: every JVM primitive kind in one descriptor -----------------

    /**
     * Caller packing all eight base-type letters in JVM declaration order:
     * boolean Z, byte B, char C, short S, int I, float F, double D, long J,
     * giving descriptor "(ZBCSIFDJ)I".  Each param is touched so javac keeps it.
     */
    public int manyPrims(final boolean a, final byte b, final char c,
                         final short d, final int e, final float f,
                         final double g, final long h)
    {
        final int n = (a ? 1 : 0) + (b & 1) + (c & 1) + (d & 1) + (e & 1)
                    + ((int) f & 1) + ((int) g & 1) + ((int) h & 1);
        return this.inner(ARG_MANYPRIMS + (n & 0));
    }

    // ---- mode 9: array-typed descriptor -------------------------------------

    /**
     * Caller whose descriptor carries a single-dim primitive array and a
     * two-dim reference array, yielding "([I[[Ljava/lang/String;)I".
     */
    public int arrayArgs(final int[] xs, final String[][] ss)
    {
        final int n = (xs != null ? xs.length : 0) + (ss != null ? ss.length : 0);
        return this.inner(ARG_ARRAYARGS + (n & 0));
    }

    // ---- mode 10: caller in a distinct nested class -------------------------

    /**
     * Distinct nested class whose method is the immediate caller, so the
     * reported class_name is "vmhook/fixtures/ReturnCaller$Helper" rather than
     * the leaf's own class.  Holds the outer instance to reach inner().
     */
    public static final class Helper
    {
        private final ReturnCaller owner;

        Helper(final ReturnCaller o)
        {
            this.owner = o;
        }

        int bridge(final int x)
        {
            return this.owner.inner(x) + 1;
        }
    }

    // ---- mode 12: static caller ---------------------------------------------

    /**
     * Static immediate caller (the other callers are instance methods); the
     * interpreter frame layout is the same, so caller() must still resolve it.
     */
    public static int staticCaller(final ReturnCaller owner, final int x)
    {
        return owner.inner(x) + 1;
    }

    // ---- mode 13: a stable caller fired repeatedly --------------------------

    /** Single interpreted caller fired multiple times in one cycle (mode 13). */
    public int stable(final int x)
    {
        return this.inner(x) + 1;
    }

    // ---- scenario runners ---------------------------------------------------

    private void runOuterA()
    {
        observed = 0;
        innerCalls = 0;
        this.outerA(ARG_OUTER_A);
    }

    private void runOuterBMiddle()
    {
        observed = 0;
        innerCalls = 0;
        this.outerB(ARG_OUTER_B);
    }

    private void runRecursion()
    {
        observed = 0;
        innerCalls = 0;
        this.recurse(RECURSION_DEPTH);
    }

    private void runLongSig()
    {
        observed = 0;
        innerCalls = 0;
        final Object o = new Object();
        this.longSig(o, o, o, o, o, o, o, o, ARG_LONGSIG);
    }

    private void runWarmCaller()
    {
        observed = 0;
        innerCalls = 0;
        // Heat warmCaller well past the C2 threshold with non-folding inputs.
        int sink = 0;
        for (int i = 0; i < WARMUP_ITERATIONS; i++)
        {
            sink += this.warmCaller(i);
        }
        // Publish the sink so the JIT cannot dead-code the whole loop.
        ReturnCaller.warmSink = sink;
        // Now the one call that actually reaches the leaf, from the (now
        // compiled) warmCaller frame.
        this.warmCaller(-1);
    }

    private void runTwoCallers()
    {
        observed = 0;
        innerCalls = 0;
        this.alpha(ARG_ALPHA);
        this.beta(ARG_BETA);
    }

    private void runLongArgCaller()
    {
        observed = 0;
        innerCalls = 0;
        this.longArgCaller(ARG_LONG);
    }

    private void runManyPrims()
    {
        observed = 0;
        innerCalls = 0;
        this.manyPrims(true, (byte) 1, 'x', (short) 2, 3, 4.0f, 5.0, 6L);
    }

    private void runArrayArgs()
    {
        observed = 0;
        innerCalls = 0;
        final int[] xs = new int[] { 1, 2 };
        final String[][] ss = new String[][] { { "a" } };
        this.arrayArgs(xs, ss);
    }

    private void runHelper()
    {
        observed = 0;
        innerCalls = 0;
        new Helper(this).bridge(ARG_HELPER);
    }

    private void runConstructorCaller()
    {
        observed = 0;
        innerCalls = 0;
        // The int-arg constructor's <init> body is the immediate caller of inner.
        new ReturnCaller(ARG_CTOR);
    }

    private void runStaticCaller()
    {
        observed = 0;
        innerCalls = 0;
        ReturnCaller.staticCaller(this, ARG_STATIC);
    }

    private void runStableCaller()
    {
        observed = 0;
        innerCalls = 0;
        for (int i = 0; i < STABLE_FIRES; i++)
        {
            this.stable(ARG_STABLE);
        }
    }

    /** Keeps the warmup loop from being optimised away. */
    public static volatile int warmSink;

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnCaller.go && !ReturnCaller.done;
            }

            @Override
            public void run()
            {
                final ReturnCaller probe = new ReturnCaller();
                switch (ReturnCaller.mode)
                {
                    case 1:
                        probe.runOuterA();
                        break;
                    case 2:
                        probe.runOuterBMiddle();
                        break;
                    case 3:
                        probe.runRecursion();
                        break;
                    case 4:
                        probe.runLongSig();
                        break;
                    case 5:
                        probe.runWarmCaller();
                        break;
                    case 6:
                        probe.runTwoCallers();
                        break;
                    case 7:
                        probe.runLongArgCaller();
                        break;
                    case 8:
                        probe.runManyPrims();
                        break;
                    case 9:
                        probe.runArrayArgs();
                        break;
                    case 10:
                        probe.runHelper();
                        break;
                    case 11:
                        probe.runConstructorCaller();
                        break;
                    case 12:
                        probe.runStaticCaller();
                        break;
                    case 13:
                        probe.runStableCaller();
                        break;
                    default:
                        break;
                }
                ReturnCaller.done = true;
            }
        });
    }
}
