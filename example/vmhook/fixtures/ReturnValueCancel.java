package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code return_value::cancel()} / force-cancel feature
 * (area: hooks).
 *
 * <p>{@code cancel()} flips {@code return_slot::cancel = true} WITHOUT writing
 * {@code retval}.  When the trampoline takes the cancel path it unconditionally
 * loads the (zero-initialised) retval cell into both {@code rax} and {@code xmm0}
 * regardless of the Java method's return descriptor, so calling {@code cancel()}
 * on a non-void method forces the Java caller to receive {@code 0 / null / +0.0}.
 * The original method body is skipped in every case.  This fixture lets the
 * native module prove every one of those facts on a LIVE JVM:</p>
 *
 * <ul>
 *   <li>void cancel: the original body's side effect never happens
 *       (instance AND static dispatch),</li>
 *   <li>cancel on an {@code int} returner: caller observes {@code 0},</li>
 *   <li>cancel on a {@code long} returner: caller observes {@code 0L} (full
 *       64-bit cell, not a stale high dword),</li>
 *   <li>cancel on a {@code double} returner: caller observes {@code +0.0}
 *       (exercises the {@code movq xmm0} epilogue), and it is NOT {@code NaN},</li>
 *   <li>cancel on a {@code boolean} returner: caller observes {@code false},</li>
 *   <li>cancel on a {@code char} returner: caller observes U+0000,</li>
 *   <li>cancel on a reference ({@code Object}/{@code String}) returner: caller
 *       observes {@code null},</li>
 *   <li>cancel-vs-allow-through: with the hook installed but NOT cancelling, the
 *       original value flows; with cancel it is suppressed,</li>
 *   <li>cancel + set combined (both orders) - asserted on the native side via
 *       the forced observed value,</li>
 *   <li>per-invocation cancel state: a hook that cancels only every OTHER call
 *       proves the cancel flag lives in the per-call trampoline slot and does
 *       not stick across invocations.</li>
 * </ul>
 *
 * <p>Strategy mirrors {@link ReturnSetPrimitives} / {@link Pilot}: a dumb actor
 * with a {@code go}/{@code done} handshake and a static-block self-registration.
 * The native module selects WHAT the probe does via the {@code mode} field, then
 * runs one {@code run_probe} handshake per round.  Every {@code orig*} method
 * returns a fixed NON-ZERO / NON-NULL value the native side never forces, so an
 * observed {@code 0 / null / +0.0} can only mean the cancel path delivered it.</p>
 *
 * <p>Pure-ASCII source so it compiles identically under javac 8..25 on any host
 * (the lone unicode char is written as a {@code \\u0000} escape).</p>
 */
public final class ReturnValueCancel
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which probe body the action runs this round.  The native module
     * writes it before raising {@code go}.  See the MODE_* constants.
     */
    public static volatile int mode;

    // ---- Probe modes -------------------------------------------------------
    /** Call every orig* method once and record what the caller observed. */
    public static final int MODE_OBSERVE_ALL = 0;
    /**
     * Call the void side-effect method twice and record the side-effect counter
     * after each call (for the "cancel only every other call" angle).
     */
    public static final int MODE_VOID_TWICE = 1;
    /**
     * Call origVoid() three times, snapshotting the side-effect counter after
     * each call.  Lets the native side prove the per-invocation cancel slot
     * resets independently across MORE than two dispatches (cancel call 2 only,
     * cancel calls 0+2, etc.) — every snapshot is captured so the native side
     * can pin which calls ran.
     */
    public static final int MODE_VOID_THRICE = 2;

    // =======================================================================
    //  Observed return values (what the Java caller actually received).
    //  Pre-seeded to a sentinel no test forces, so "hook never fired" is
    //  distinguishable from "hook forced the sentinel".
    // =======================================================================
    public static volatile int     obsInt        = 0x5A5A5A5A;
    public static volatile long    obsLong       = 0x5A5A5A5A5A5A5A5AL;
    public static volatile double  obsDouble     = 9876.54321;
    public static volatile boolean obsBool       = true;
    public static volatile char    obsChar       = (char) 0x5A5A;
    public static volatile byte    obsByte       = (byte) 0x5A;
    public static volatile short   obsShort      = (short) 0x5A5A;
    public static volatile float   obsFloat      = 9876.5f;
    /** True iff the reference-returning method handed the caller {@code null}. */
    public static volatile boolean obsRefIsNull;
    /** Identity hash of the returned reference (0 when null) - breadcrumb. */
    public static volatile int     obsRefIdentity;

    public static volatile int     obsStaticInt    = 0x5A5A5A5A;
    public static volatile double  obsStaticDouble = 9876.54321;
    public static volatile long    obsStaticLong   = 0x5A5A5A5A5A5A5A5AL;
    public static volatile boolean obsStaticBool   = true;
    public static volatile char    obsStaticChar   = (char) 0x5A5A;
    public static volatile byte    obsStaticByte   = (byte) 0x5A;
    public static volatile short   obsStaticShort  = (short) 0x5A5A;
    public static volatile float   obsStaticFloat  = 9876.5f;

    /** True iff the observed double was NaN (cancel must yield +0.0, not NaN). */
    public static volatile boolean obsDoubleWasNaN;
    /** True iff the observed double carried a sign bit (cancel must be +0.0). */
    public static volatile boolean obsDoubleWasNegZero;
    /** True iff the observed float was NaN (cancel must yield +0.0f, not NaN). */
    public static volatile boolean obsFloatWasNaN;
    /** True iff the observed float carried a sign bit (cancel must be +0.0f). */
    public static volatile boolean obsFloatWasNegZero;
    /** True iff the observed STATIC float carried a sign bit. */
    public static volatile boolean obsStaticFloatWasNegZero;

    // ---- Void side-effect witnesses ---------------------------------------
    /** Bumped by the void instance body; cancel must leave it untouched. */
    public static volatile int sideEffect;
    /** Bumped by the void static body; cancel must leave it untouched. */
    public static volatile int staticSideEffect;
    /** sideEffect snapshot after the 1st VOID_TWICE/VOID_THRICE call. */
    public static volatile int sideEffectAfterCall1;
    /** sideEffect snapshot after the 2nd VOID_TWICE/VOID_THRICE call. */
    public static volatile int sideEffectAfterCall2;
    /** sideEffect snapshot after the 3rd VOID_THRICE call. */
    public static volatile int sideEffectAfterCall3;

    // ---- Control observations ---------------------------------------------
    /** Set true by the action if any orig* call threw (it must never throw). */
    public static volatile boolean sawException;
    /** How many times an action body ran (sanity for run_probe). */
    public static volatile int     roundCount;

    // The single instance the instance-method hooks dispatch through.
    public static final ReturnValueCancel INSTANCE = new ReturnValueCancel();

    // =======================================================================
    //  Original-return methods (instance).  Each returns a NON-ZERO / NON-NULL
    //  value the native side never forces.  A void method records a side effect.
    // =======================================================================

    /** Void body with an observable side effect (instance dispatch). */
    public void origVoid()
    {
        sideEffect = sideEffect + 7;
    }

    /** Returns a non-zero int (1111). */
    public int origInt()
    {
        return 1111;
    }

    /** Returns a non-zero long whose HIGH dword is set (catches half-cell bugs). */
    public long origLong()
    {
        return 0x7FFFFFFF00000001L;
    }

    /** Returns a non-zero double (11.25). */
    public double origDouble()
    {
        return 11.25;
    }

    /** Returns {@code true} (cancel must flip the observation to false). */
    public boolean origBool()
    {
        return true;
    }

    /** Returns 'A' (cancel must flip the observation to U+0000). */
    public char origChar()
    {
        return 'A';
    }

    /** Returns a fresh non-null reference (cancel must hand the caller null). */
    public Object origRef()
    {
        return new Object();
    }

    /** Returns a non-zero byte (88); cancel must flip the observation to 0. */
    public byte origByte()
    {
        return (byte) 88;
    }

    /** Returns a non-zero short (7777); cancel must flip the observation to 0. */
    public short origShort()
    {
        return (short) 7777;
    }

    /**
     * Returns a non-zero float (33.5); cancel must flip the observation to
     * +0.0f (the 32-bit twin of the double case: exercises the SAME movq xmm0
     * epilogue but a {@code freturn} that reads only the low 32 bits of the SSE
     * register).
     */
    public float origFloat()
    {
        return 33.5f;
    }

    // =======================================================================
    //  Original-return methods (static).
    // =======================================================================

    /** Void body with an observable side effect (static dispatch). */
    public static void origStaticVoid()
    {
        staticSideEffect = staticSideEffect + 13;
    }

    /** Returns a non-zero int (2222). */
    public static int origStaticInt()
    {
        return 2222;
    }

    /** Returns a non-zero double (22.25). */
    public static double origStaticDouble()
    {
        return 22.25;
    }

    /** Returns a non-zero long with the HIGH dword set (static dispatch). */
    public static long origStaticLong()
    {
        return 0x7FFFFFFF00000002L;
    }

    /** Returns {@code true} (static); cancel must flip the observation to false. */
    public static boolean origStaticBool()
    {
        return true;
    }

    /** Returns 'Z' (static); cancel must flip the observation to U+0000. */
    public static char origStaticChar()
    {
        return 'Z';
    }

    /** Returns a non-zero byte (66) (static); cancel must flip it to 0. */
    public static byte origStaticByte()
    {
        return (byte) 66;
    }

    /** Returns a non-zero short (6666) (static); cancel must flip it to 0. */
    public static short origStaticShort()
    {
        return (short) 6666;
    }

    /** Returns a non-zero float (44.5) (static); cancel must flip it to +0.0f. */
    public static float origStaticFloat()
    {
        return 44.5f;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            public boolean pending()
            {
                return ReturnValueCancel.go && !ReturnValueCancel.done;
            }

            public void run()
            {
                final ReturnValueCancel self = ReturnValueCancel.INSTANCE;
                boolean threw = false;
                try
                {
                    if (ReturnValueCancel.mode == MODE_VOID_TWICE)
                    {
                        // Two back-to-back void calls; snapshot the side-effect
                        // counter after each so the native side can prove the
                        // cancel flag is per-invocation (cancel call 1, allow
                        // call 2 -> only the 2nd bumps the counter).
                        self.origVoid();
                        ReturnValueCancel.sideEffectAfterCall1 = ReturnValueCancel.sideEffect;
                        self.origVoid();
                        ReturnValueCancel.sideEffectAfterCall2 = ReturnValueCancel.sideEffect;
                    }
                    else if (ReturnValueCancel.mode == MODE_VOID_THRICE)
                    {
                        // Three back-to-back void calls; snapshot after each so
                        // the native side can pin EXACTLY which of the three
                        // dispatches the per-call cancel slot suppressed.
                        self.origVoid();
                        ReturnValueCancel.sideEffectAfterCall1 = ReturnValueCancel.sideEffect;
                        self.origVoid();
                        ReturnValueCancel.sideEffectAfterCall2 = ReturnValueCancel.sideEffect;
                        self.origVoid();
                        ReturnValueCancel.sideEffectAfterCall3 = ReturnValueCancel.sideEffect;
                    }
                    else
                    {
                        // MODE_OBSERVE_ALL: call everything once, record what
                        // the Java caller observed.

                        // Void side effects (instance + static).
                        self.origVoid();
                        origStaticVoid();

                        // Primitive returns (instance dispatch).
                        ReturnValueCancel.obsInt    = self.origInt();
                        ReturnValueCancel.obsLong   = self.origLong();
                        final double d              = self.origDouble();
                        ReturnValueCancel.obsDouble = d;
                        ReturnValueCancel.obsDoubleWasNaN     = (d != d);
                        // +0.0 == -0.0 is true, but 1/+0.0 == +Inf while
                        // 1/-0.0 == -Inf - that distinguishes the sign bit
                        // without needing Double.doubleToRawLongBits (still
                        // pure JDK-8 API).
                        ReturnValueCancel.obsDoubleWasNegZero =
                            (d == 0.0) && ((1.0 / d) < 0.0);
                        ReturnValueCancel.obsBool   = self.origBool();
                        ReturnValueCancel.obsChar   = self.origChar();
                        ReturnValueCancel.obsByte   = self.origByte();
                        ReturnValueCancel.obsShort  = self.origShort();
                        final float f               = self.origFloat();
                        ReturnValueCancel.obsFloat  = f;
                        ReturnValueCancel.obsFloatWasNaN     = (f != f);
                        // Same +0.0f vs -0.0f sign probe as the double case:
                        // 1.0f/+0.0f == +Inf, 1.0f/-0.0f == -Inf.
                        ReturnValueCancel.obsFloatWasNegZero =
                            (f == 0.0f) && ((1.0f / f) < 0.0f);

                        // Reference return (instance dispatch).
                        final Object ref = self.origRef();
                        ReturnValueCancel.obsRefIsNull   = (ref == null);
                        ReturnValueCancel.obsRefIdentity =
                            (ref == null) ? 0 : System.identityHashCode(ref);

                        // Primitive returns (static dispatch).
                        ReturnValueCancel.obsStaticInt    = origStaticInt();
                        ReturnValueCancel.obsStaticDouble = origStaticDouble();
                        ReturnValueCancel.obsStaticLong   = origStaticLong();
                        ReturnValueCancel.obsStaticBool   = origStaticBool();
                        ReturnValueCancel.obsStaticChar   = origStaticChar();
                        ReturnValueCancel.obsStaticByte   = origStaticByte();
                        ReturnValueCancel.obsStaticShort  = origStaticShort();
                        final float sf                    = origStaticFloat();
                        ReturnValueCancel.obsStaticFloat  = sf;
                        ReturnValueCancel.obsStaticFloatWasNegZero =
                            (sf == 0.0f) && ((1.0f / sf) < 0.0f);
                    }
                }
                catch (final Throwable t)
                {
                    threw = true;
                }
                ReturnValueCancel.sawException = threw;
                ReturnValueCancel.roundCount = ReturnValueCancel.roundCount + 1;
                ReturnValueCancel.done = true;
            }
        });
    }
}
