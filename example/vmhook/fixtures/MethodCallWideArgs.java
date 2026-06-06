package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_call_wide_args" feature (area: methods).
 *
 * The feature under test is {@code vmhook::method_proxy::call(args...)} passing
 * {@code long} / {@code double} arguments correctly.  Each of those occupies TWO
 * interpreter local slots, while every other primitive (and an object reference)
 * occupies ONE.  The classic bug class is a wide argument whose upper 32 bits
 * leak into — or whose presence shifts — the FOLLOWING parameter slot, silently
 * corrupting the next {@code int} (or mis-aligning everything after it).  vmhook's
 * call_stub fast path packs one {@code intptr_t} slot per C++ argument and lets
 * the hand-written stub expand wide values into interpreter locals; the call_jni
 * fallback packs one {@code jvalue} per argument ({@code value.j} / {@code value.d}
 * for the wide kinds).  BOTH paths must deliver the wide value bit-exact AND leave
 * every neighbouring argument untouched.
 *
 * To catch any slot-shift this fixture's methods combine their arguments into a
 * DETERMINISTIC return value (distinct prime-ish multipliers per parameter, so an
 * a<->b swap, a truncation, or a neighbour-corruption all change the result), and
 * ADDITIONALLY stamp the exact value each parameter actually received into a
 * dedicated static field.  The native side reads BOTH: the combined return AND the
 * per-parameter witness fields.  A wide arg corrupting the next {@code int} would
 * leave the witness field holding the wrong value even if (by coincidence) the
 * combined return happened to match.
 *
 * Coverage of WIDE VALUES is exhaustive at the boundaries:
 *   long   : 0, 1, -1, Long.MIN_VALUE, Long.MAX_VALUE, a 0x0123... bit pattern,
 *            a high-bits-only pattern (0xFFFFFFFF00000000) and a low-bits-only one
 *            (0x00000000FFFFFFFF) — the two halves that a 32-bit truncation bug
 *            would confuse.
 *   double : +0.0, -0.0, +1.0, -1.0, Math.PI, a negative, +Inf, -Inf, canonical
 *            NaN, a signaling NaN, a payload qNaN, smallest subnormal (MIN_VALUE),
 *            MIN_NORMAL and MAX_VALUE — reconstructed from raw bits via
 *            Double.longBitsToDouble so the native side asserts bit-exact survival
 *            (NaN payload, signaling bit, denormal mantissa, sign of zero).
 *
 * WIDE POSITION is covered leading / middle / trailing, long+double mixed in one
 * call, two longs, two doubles, and an all-wide four-arg frame.
 *
 * The native module hooks {@code trigger(int)} (the probe calls it on a real
 * bytecode dispatch); inside that detour {@code current_java_thread} is live, the
 * ONLY context in which {@code method_proxy::call()} may dispatch.  Static AND
 * instance variants are present so both the receiver-in-slot-0 and the
 * no-receiver frame layouts are exercised across the two-slot boundary.
 *
 * ENCODING NOTE: this fixture is pure ASCII; any incidental non-ASCII would use a
 * \\uXXXX escape so it compiles identically under javac on Windows (Cp1252) and
 * Linux/macOS (UTF-8) with no -encoding flag.
 *
 * Java 8 syntax only (no var / records / switch-expressions / text blocks /
 * List.of); only java.* + vmhook.Harness are referenced.
 */
public final class MethodCallWideArgs
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** trigger() invocation count (handshake proof). */
    public static volatile int triggerCount;

    // ----------------------------------------------------------------------
    //  Per-parameter WITNESS fields.  Each method stamps the EXACT value it
    //  received into the matching field, so the native side can prove the wide
    //  argument neither truncated nor corrupted a neighbouring slot, INDEPENDENT
    //  of the combined return value.  All long witnesses default to a sentinel
    //  that no test argument uses, so "did the method run?" is unambiguous.
    // ----------------------------------------------------------------------
    public static final long SENTINEL = 0x5A5A5A5A5A5A5A5AL;

    // idL(long) / idD(double) single-arg echoes (dedicated, never shared)
    public static volatile long   wIdL = SENTINEL;
    public static volatile double wIdD = Double.NaN;
    // addL(long,long)
    public static volatile long wAddLa = SENTINEL;
    public static volatile long wAddLb = SENTINEL;
    // mixA(int,long,int)  — wide in the MIDDLE; the two ints flank it
    public static volatile int  wMixAa = (int) SENTINEL;
    public static volatile long wMixAb = SENTINEL;
    public static volatile int  wMixAc = (int) SENTINEL;
    // mixB(long,int,long) — wide leading AND trailing, int squeezed between
    public static volatile long wMixBa = SENTINEL;
    public static volatile int  wMixBb = (int) SENTINEL;
    public static volatile long wMixBc = SENTINEL;
    // scaleD(double,int)  — wide LEADING, int trailing (the canonical "double
    // must not corrupt the following int" witness)
    public static volatile double wScaleDx = Double.NaN;
    public static volatile int    wScaleDn = (int) SENTINEL;
    // mixC(int,double,int) — double in the MIDDLE
    public static volatile int    wMixCa = (int) SENTINEL;
    public static volatile double wMixCb = Double.NaN;
    public static volatile int    wMixCc = (int) SENTINEL;
    // mixD(long,double,long,double) — ALL FOUR wide
    public static volatile long   wMixDa = SENTINEL;
    public static volatile double wMixDb = Double.NaN;
    public static volatile long   wMixDc = SENTINEL;
    public static volatile double wMixDd = Double.NaN;
    // intAfterLong(long,int) / intAfterDouble(double,int) — the minimal
    // two-slot widening-bug witnesses: the int immediately after one wide arg.
    public static volatile long wIalLong   = SENTINEL;
    public static volatile int  wIalInt    = (int) SENTINEL;
    public static volatile double wIadDouble = Double.NaN;
    public static volatile int    wIadInt    = (int) SENTINEL;
    // longAfterInt(int,long) / doubleAfterInt(int,double) — wide arg AFTER a
    // narrow one (the wide value must start on the correct slot).
    public static volatile int  wLaiInt    = (int) SENTINEL;
    public static volatile long wLaiLong   = SENTINEL;
    public static volatile int    wDaiInt    = (int) SENTINEL;
    public static volatile double wDaiDouble = Double.NaN;

    // STATIC-variant witnesses (no receiver; first arg starts at slot 0).
    public static volatile long   sWAddLa = SENTINEL;
    public static volatile long   sWAddLb = SENTINEL;
    public static volatile int    sWMixAa = (int) SENTINEL;
    public static volatile long   sWMixAb = SENTINEL;
    public static volatile int    sWMixAc = (int) SENTINEL;
    public static volatile double sWScaleDx = Double.NaN;
    public static volatile int    sWScaleDn = (int) SENTINEL;
    public static volatile long   sWMixDa = SENTINEL;
    public static volatile double sWMixDb = Double.NaN;
    public static volatile long   sWMixDc = SENTINEL;
    public static volatile double sWMixDd = Double.NaN;

    /** Held so the native side can build an instance wrapper for instance calls. */
    public static MethodCallWideArgs instance = new MethodCallWideArgs();

    // ======================================================================
    //  Hook target.
    // ======================================================================

    /** Hookable instance method; the native module hooks this to get a live
     *  JavaThread, then performs every method_proxy::call() inside the detour. */
    public int trigger(final int delta)
    {
        triggerCount++;
        return delta + 1;
    }

    // ======================================================================
    //  INSTANCE wide-arg methods.  Each combines its args deterministically
    //  AND records each arg into a witness field.
    // ======================================================================

    /** Two longs.  Distinct multipliers so a<->b swap is caught; the sum keeps
     *  the full 64-bit contribution of each so truncation is caught too. */
    public long addL(final long a, final long b)
    {
        wAddLa = a;
        wAddLb = b;
        return a * 1000003L + b;
    }

    /** Single long echo (bit-exact round trip of the whole 64-bit value). */
    public long idL(final long a)
    {
        wIdL = a;
        return a;
    }

    /** Single double echo (bit-exact round trip incl. NaN payload / -0.0). */
    public double idD(final double d)
    {
        wIdD = d;
        return d;
    }

    /** Wide arg in the MIDDLE: int, long, int.  The two ints must survive the
     *  two-slot long between them.  Return mixes all three with the long's full
     *  width, and each arg is stamped to a witness. */
    public long mixA(final int a, final long b, final int c)
    {
        wMixAa = a;
        wMixAb = b;
        wMixAc = c;
        return ((long) a) * 7L + b * 1000003L + ((long) c) * 13L;
    }

    /** Wide LEADING and TRAILING, narrow squeezed between: long, int, long. */
    public long mixB(final long a, final int b, final long c)
    {
        wMixBa = a;
        wMixBb = b;
        wMixBc = c;
        return a * 1000003L + ((long) b) * 7L + c * 97L;
    }

    /** Wide LEADING, narrow TRAILING: double, int.  Returns x * n; both stamped.
     *  This is the canonical "a double arg must not corrupt the following int"
     *  case — wScaleDn must equal exactly the int passed. */
    public double scaleD(final double x, final int n)
    {
        wScaleDx = x;
        wScaleDn = n;
        return x * n;
    }

    /** Double in the MIDDLE: int, double, int.  Both ints must survive. */
    public double mixC(final int a, final double b, final int c)
    {
        wMixCa = a;
        wMixCb = b;
        wMixCc = c;
        return b + (double) a + (double) c;
    }

    /** ALL FOUR wide: long, double, long, double.  Returns a deterministic mix
     *  and stamps each; any cross-slot bleed changes a witness and the return. */
    public double mixD(final long a, final double b, final long c, final double d)
    {
        wMixDa = a;
        wMixDb = b;
        wMixDc = c;
        wMixDd = d;
        return (double) a + b + (double) c + d;
    }

    /** Minimal witness: int immediately AFTER a long.  wIalInt must equal the
     *  int passed even though the long ahead of it spans two slots. */
    public int intAfterLong(final long a, final int b)
    {
        wIalLong = a;
        wIalInt  = b;
        return b;
    }

    /** Minimal witness: int immediately AFTER a double. */
    public int intAfterDouble(final double a, final int b)
    {
        wIadDouble = a;
        wIadInt    = b;
        return b;
    }

    /** Wide arg AFTER a narrow one: int, long.  The long must start at the right
     *  slot (slot 1 for instance after the int at slot 0). */
    public long longAfterInt(final int a, final long b)
    {
        wLaiInt  = a;
        wLaiLong = b;
        return b;
    }

    /** Wide arg AFTER a narrow one: int, double. */
    public double doubleAfterInt(final int a, final double b)
    {
        wDaiInt    = a;
        wDaiDouble = b;
        return b;
    }

    // ======================================================================
    //  Overload pair that ONLY differs by a wide-vs-narrow parameter kind, so
    //  resolve_compatible_method() must pick the long overload for a C++ int64
    //  arg and the int overload for a C++ int32 arg.  Distinct returns prove it.
    // ======================================================================
    public static final int WIDTH_TAG_INT  = 111;
    public static final int WIDTH_TAG_LONG = 222;

    /** widthTag(int) -> WIDTH_TAG_INT. */
    public int widthTag(final int v)  { return WIDTH_TAG_INT; }
    /** widthTag(long) -> WIDTH_TAG_LONG. */
    public int widthTag(final long v) { return WIDTH_TAG_LONG; }

    public static final int FD_TAG_FLOAT  = 333;
    public static final int FD_TAG_DOUBLE = 444;

    /** fdTag(float) -> FD_TAG_FLOAT. */
    public int fdTag(final float v)  { return FD_TAG_FLOAT; }
    /** fdTag(double) -> FD_TAG_DOUBLE. */
    public int fdTag(final double v) { return FD_TAG_DOUBLE; }

    // ======================================================================
    //  STATIC wide-arg methods — exercise the no-receiver frame (first arg at
    //  slot 0) across the two-slot boundary.
    // ======================================================================

    public static long sAddL(final long a, final long b)
    {
        sWAddLa = a;
        sWAddLb = b;
        return a * 1000003L + b;
    }

    public static long sMixA(final int a, final long b, final int c)
    {
        sWMixAa = a;
        sWMixAb = b;
        sWMixAc = c;
        return ((long) a) * 7L + b * 1000003L + ((long) c) * 13L;
    }

    public static double sScaleD(final double x, final int n)
    {
        sWScaleDx = x;
        sWScaleDn = n;
        return x * n;
    }

    public static double sMixD(final long a, final double b, final long c, final double d)
    {
        sWMixDa = a;
        sWMixDb = b;
        sWMixDc = c;
        sWMixDd = d;
        return (double) a + b + (double) c + d;
    }

    /** Static single-long echo. */
    public static long sIdL(final long a) { return a; }
    /** Static single-double echo. */
    public static double sIdD(final double d) { return d; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodCallWideArgs.go && !MethodCallWideArgs.done;
            }

            @Override
            public void run()
            {
                // A single real bytecode dispatch into trigger() fires the native
                // interpreter hook; ALL method_proxy::call() work happens inside
                // that detour where current_java_thread is set.
                MethodCallWideArgs.instance.trigger(7);
                MethodCallWideArgs.done = true;
            }
        });
    }
}
