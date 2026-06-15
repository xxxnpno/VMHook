package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for {@code return_value::set_arg(index, primitive)} — the PRIMITIVE
 * argument-injection branch of set_arg (area: return_value / argument mutation).
 *
 * <p>{@code set_arg} mutates a Java method argument IN PLACE on the interpreter
 * local-variable array from inside a hook detour, BEFORE the original method body
 * runs, so the original body observes the replacement value.  The companion
 * native module ({@code tests/jvm/modules/return_set_arg.cpp}) installs an
 * interpreter hook on each method below; inside the hook it calls
 * {@code retval.set_arg(slot, value)} to overwrite a primitive argument slot, then
 * lets the body run.  Each {@code take*} method is a DUMB ACTOR: it copies exactly
 * the argument it received into an observable {@code static} field, so the field
 * reflects the POST-MUTATION value the body actually saw.</p>
 *
 * <p><b>This fixture deliberately covers PRIMITIVES ONLY.</b>  Object / String
 * argument mutation is exercised by the separate {@code ReturnSetWrapperNull}
 * fixture (and was the source of a no-SEH-suite crash class), so it is omitted
 * here entirely: every method below takes only {@code int / long / double / float
 * / boolean / byte / char / short} arguments, and the native module performs NO
 * in-detour object allocation and NO forced GC.</p>
 *
 * <p><b>Slot model</b> (HotSpot x64 interpreter; every local is an 8-byte slot,
 * and a {@code long}/{@code double} consumes TWO slots):</p>
 * <ul>
 *   <li>instance method: slot 0 = {@code this}, slot 1 = first arg, ...</li>
 *   <li>static method: slot 0 = first arg, ...</li>
 *   <li>{@code twoInts(int a, int b)}: this=0, a=slot1, b=slot2</li>
 *   <li>{@code mixLongInt(long a, int b)}: this=0, a=slots1+2, b=slot3
 *       (the {@code int} after a {@code long} proves a wide arg reserves two
 *       slots so the following arg's slot index is shifted)</li>
 *   <li>{@code intLong(int a, long b)}: this=0, a=slot1, b=slots2+3</li>
 *   <li>{@code doubleInt(double a, int b)}: this=0, a=slots1+2, b=slot3</li>
 * </ul>
 *
 * <p>The whole suite runs through a {@code go}/{@code done} handshake driven by
 * {@code mode}: the probe's {@code run()} dispatches the method family selected by
 * {@code mode} once each (one real bytecode dispatch per method — the only thing
 * that makes an interpreter hook fire), then raises {@code done}.  The native
 * module re-arms its hooks with fresh boundary values, sets {@code mode}, and
 * re-fires the probe for each value round.</p>
 *
 * <p>Target Java 8 syntax only (no var / records / switch-expressions).</p>
 */
public final class ReturnSetArg
{
    /** Native raises this to request the probe action; lowers it afterwards. */
    public static volatile boolean go;

    /** The probe action raises this when it has run; native polls it. */
    public static volatile boolean done;

    /** Selects which method family run() dispatches this round. */
    public static volatile int mode;

    /** How many times the probe action body ran (sanity for run_probe). */
    public static volatile int probeTicks;

    // Mode selectors.
    public static final int MODE_PRIMITIVES = 0; // every single-arg primitive + static int
    public static final int MODE_SLOTS      = 1; // twoInts / mixLongInt / intLong / doubleInt
    public static final int MODE_BOUNDS     = 2; // boundsTarget (out-of-range rejection)

    // The single instance the instance-method hooks dispatch through.
    public static final ReturnSetArg INSTANCE = new ReturnSetArg();

    // ── Observed argument values (what each method body actually received) ─────
    // Pre-seeded to a sentinel no test ever passes, so a method that never ran is
    // distinguishable from one that observed the sentinel.
    public static volatile int     obsInt     = 0x5A5A5A5A;
    public static volatile long    obsLong    = 0x5A5A5A5A5A5A5A5AL;
    public static volatile double  obsDouble  = 9876.54321;
    public static volatile float   obsFloat   = 1234.5678f;
    public static volatile boolean obsBool;
    public static volatile byte    obsByte    = (byte)  0x5A;
    public static volatile char    obsChar    = (char)  0x5A5A;
    public static volatile short   obsShort   = (short) 0x5AA5;

    // 32-bit WIDENING views of the byte/short argument: the body widens the
    // received byte/short parameter to int and stores it here, capturing the
    // 32-bit value the JVM observed BEFORE any narrowing back to the field's
    // declared width.  This is where a missing sign-extension in set_arg surfaces
    // (a byte/short of -1 injected as 0x...FF would widen to +255/+65535 instead
    // of -1).  The native side treats the observed widened value tolerantly
    // (PASS-or-[INFO]) because the widening result is JDK-implementation-sensitive.
    public static volatile int     obsByteWide  = 0x5A5A5A5A;
    public static volatile int     obsShortWide = 0x5A5A5A5A;

    public static volatile int     obsStaticInt = 0x5A5A5A5A;

    // Slot-model observations.
    public static volatile int  twoA;        // twoInts arg a
    public static volatile int  twoB;        // twoInts arg b
    public static volatile long mixLong;     // mixLongInt arg a (long)
    public static volatile int  mixInt;      // mixLongInt arg b (int, after the long)
    public static volatile int  ilInt;       // intLong arg a (int)
    public static volatile long ilLong;      // intLong arg b (long, after the int)
    public static volatile double diDouble;  // doubleInt arg a (double)
    public static volatile int    diInt;     // doubleInt arg b (int, after the double)

    // Bounds observation: the value the body saw after every out-of-range set_arg
    // was rejected.  Must remain the ORIGINAL passed value (no wild write).
    public static volatile int boundsObs = 0x5A5A5A5A;

    /** Set true by the action if any take* call threw (it must never throw). */
    public static volatile boolean sawException;

    // ── Dumb-actor methods (instance) — copy the received arg into a field ─────
    public void takeInt(int v)         { ReturnSetArg.obsInt = v; }
    public void takeLong(long v)       { ReturnSetArg.obsLong = v; }
    public void takeDouble(double v)   { ReturnSetArg.obsDouble = v; }
    public void takeFloat(float v)     { ReturnSetArg.obsFloat = v; }
    public void takeBoolean(boolean v) { ReturnSetArg.obsBool = v; }
    public void takeByte(byte v)       { ReturnSetArg.obsByte = v; ReturnSetArg.obsByteWide = v; }
    public void takeChar(char v)       { ReturnSetArg.obsChar = v; }
    public void takeShort(short v)     { ReturnSetArg.obsShort = v; ReturnSetArg.obsShortWide = v; }

    // ── Dumb-actor method (static, no 'this' — arg begins at slot 0) ───────────
    public static void takeStaticInt(int v) { ReturnSetArg.obsStaticInt = v; }

    // ── Slot-model methods ────────────────────────────────────────────────────
    public void twoInts(int a, int b)        { ReturnSetArg.twoA = a; ReturnSetArg.twoB = b; }
    public void mixLongInt(long a, int b)    { ReturnSetArg.mixLong = a; ReturnSetArg.mixInt = b; }
    public void intLong(int a, long b)       { ReturnSetArg.ilInt = a; ReturnSetArg.ilLong = b; }
    public void doubleInt(double a, int b)   { ReturnSetArg.diDouble = a; ReturnSetArg.diInt = b; }

    // ── Bounds method — body records the value it saw (must be the original) ───
    public void boundsTarget(int v) { ReturnSetArg.boundsObs = v; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnSetArg.go && !ReturnSetArg.done;
            }

            @Override
            public void run()
            {
                final ReturnSetArg self = ReturnSetArg.INSTANCE;
                boolean threw = false;
                try
                {
                    if (ReturnSetArg.mode == ReturnSetArg.MODE_PRIMITIVES)
                    {
                        // Original passed values are fixed; the native side
                        // overwrites them via set_arg, so the body sees the
                        // injected value, not these.  (The no-hook baseline run
                        // sees exactly these.)
                        self.takeInt(7);
                        self.takeLong(7L);
                        self.takeDouble(7.0);
                        self.takeFloat(7.0f);
                        self.takeBoolean(false);
                        self.takeByte((byte) 7);
                        self.takeChar((char) 7);
                        self.takeShort((short) 7);
                        ReturnSetArg.takeStaticInt(7);
                    }
                    else if (ReturnSetArg.mode == ReturnSetArg.MODE_SLOTS)
                    {
                        self.twoInts(7, 8);
                        self.mixLongInt(7L, 8);
                        self.intLong(7, 8L);
                        self.doubleInt(7.0, 8);
                    }
                    else // MODE_BOUNDS
                    {
                        self.boundsTarget(7);
                    }
                }
                catch (final Throwable t)
                {
                    threw = true;
                }
                ReturnSetArg.sawException = threw;
                ReturnSetArg.probeTicks = ReturnSetArg.probeTicks + 1;
                ReturnSetArg.done = true;
            }
        });
    }
}
