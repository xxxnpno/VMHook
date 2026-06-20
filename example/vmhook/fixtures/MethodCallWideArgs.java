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
    // addD(double,double) — TWO doubles adjacent (the double analogue of addL;
    // proves four contiguous wide slots stay distinct, no half-bleed between).
    public static volatile double wAddDa = Double.NaN;
    public static volatile double wAddDb = Double.NaN;
    // jd(long,double) — long IMMEDIATELY followed by a double (two wide kinds
    // back-to-back, four contiguous slots, no narrow between them).
    public static volatile long   wJdA = SENTINEL;
    public static volatile double wJdB = Double.NaN;
    // dj(double,long) — double IMMEDIATELY followed by a long (the mirror).
    public static volatile double wDjA = Double.NaN;
    public static volatile long   wDjB = SENTINEL;
    // hexA(int,long,double,int,long,double) — a 6-arg frame interleaving every
    // kind; ten interpreter slots.  Each narrow flanked by / following a wide.
    public static volatile int    wHexAa = (int) SENTINEL;
    public static volatile long   wHexAb = SENTINEL;
    public static volatile double wHexAc = Double.NaN;
    public static volatile int    wHexAd = (int) SENTINEL;
    public static volatile long   wHexAe = SENTINEL;
    public static volatile double wHexAf = Double.NaN;
    // hexB(long,int,double,long,int,double) — a DIFFERENT 6-arg interleave so a
    // single fixed mis-alignment cannot pass both hexA and hexB.
    public static volatile long   wHexBa = SENTINEL;
    public static volatile int    wHexBb = (int) SENTINEL;
    public static volatile double wHexBc = Double.NaN;
    public static volatile long   wHexBd = SENTINEL;
    public static volatile int    wHexBe = (int) SENTINEL;
    public static volatile double wHexBf = Double.NaN;
    // mixF(float,long,float) — wide long in the MIDDLE flanked by FLOATS.  A
    // float occupies ONE slot (like int) but has its own 'F' descriptor; this is
    // distinct from mixA(int,long,int) because a packer that mishandles 'F'
    // adjacency to a wide slot (rather than 'I') would corrupt a flanking float.
    public static volatile float wMixFa = (float) SENTINEL;
    public static volatile long  wMixFb = SENTINEL;
    public static volatile float wMixFc = (float) SENTINEL;
    // fld(float,long,double) — float, long, double: a narrow 'F' immediately
    // BEFORE two back-to-back wide kinds (J then D), five interpreter slots.
    public static volatile float  wFldA = (float) SENTINEL;
    public static volatile long   wFldB = SENTINEL;
    public static volatile double wFldC = Double.NaN;
    // mixS(String,long,String) — wide long in the MIDDLE flanked by OBJECT
    // references (each one slot).  Proves a wide arg neither truncates nor
    // shifts when its neighbours are 'L' reference slots, AND that the two
    // references do not swap (distinct lengths make a swap change the return).
    public static volatile String wMixSa = null;
    public static volatile long   wMixSb = SENTINEL;
    public static volatile String wMixSc = null;
    // objLong(String,long) — reference THEN wide long (wide arg starts at the
    // slot right after a one-slot reference).
    public static volatile String wOlObj  = null;
    public static volatile long   wOlLong = SENTINEL;
    // longObj(long,String) — wide long THEN reference (the reference slot must
    // start exactly two slots after the long's start, not one).
    public static volatile long   wLoLong = SENTINEL;
    public static volatile String wLoObj  = null;

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
    // static adjacent-wide witnesses (no receiver; first wide arg at slot 0).
    public static volatile double sWAddDa = Double.NaN;
    public static volatile double sWAddDb = Double.NaN;
    public static volatile long   sWJdA = SENTINEL;
    public static volatile double sWJdB = Double.NaN;
    public static volatile double sWDjA = Double.NaN;
    public static volatile long   sWDjB = SENTINEL;
    // static float/object-interleave witnesses (no receiver; first arg slot 0).
    public static volatile float  sWFldA = (float) SENTINEL;
    public static volatile long   sWFldB = SENTINEL;
    public static volatile double sWFldC = Double.NaN;
    public static volatile String sWMixSa = null;
    public static volatile long   sWMixSb = SENTINEL;
    public static volatile String sWMixSc = null;

    // mixE(float,double,float) — a DOUBLE in the MIDDLE flanked by FLOATS (the
    // 'F' analogue of mixC's int flanks).  A packer that mis-expands a wide
    // DOUBLE against an 'F' neighbour (vs the 'I' in mixC) corrupts a flank.
    public static volatile float  wMixEa = (float) SENTINEL;
    public static volatile double wMixEb = Double.NaN;
    public static volatile float  wMixEc = (float) SENTINEL;
    // jidi(long,int,double,int) — the exact (JIDI) shape: a long, then a narrow
    // int squeezed between the long and a double, then a trailing int after the
    // double.  Seven interpreter slots (2+1+2+1+1); two narrows each adjacent to
    // a different wide kind, so a one-slot drift anywhere fails a witness.
    public static volatile long   wJidiA = SENTINEL;
    public static volatile int    wJidiB = (int) SENTINEL;
    public static volatile double wJidiC = Double.NaN;
    public static volatile int    wJidiD = (int) SENTINEL;
    // idj(int,double,long) — the (ID J) shape: a narrow int, then a double, then
    // a long.  The long must START exactly two slots after the double's start
    // (slot 3 for instance: int@1, double@2-3, long@4-5).
    public static volatile int    wIdjA = (int) SENTINEL;
    public static volatile double wIdjB = Double.NaN;
    public static volatile long   wIdjC = SENTINEL;
    // widePent(long,int,double,int,float) — the explicit five-arg "every shape"
    // tail: long, int, double, int, float (eight interpreter slots).  A TRAILING
    // float after a wide-heavy frame is the witness that the float lands on the
    // right single slot once two wides + two narrows precede it.
    public static volatile long   wPentA = SENTINEL;
    public static volatile int    wPentB = (int) SENTINEL;
    public static volatile double wPentC = Double.NaN;
    public static volatile int    wPentD = (int) SENTINEL;
    public static volatile float  wPentE = (float) SENTINEL;
    // sixL(long x6) — SIX adjacent longs: twelve contiguous interpreter slots,
    // the deep-packing witness for the long kind (every slot pair must stay
    // distinct, no half-bleed between any neighbour).  All six stamped.
    public static volatile long wSixLa = SENTINEL;
    public static volatile long wSixLb = SENTINEL;
    public static volatile long wSixLc = SENTINEL;
    public static volatile long wSixLd = SENTINEL;
    public static volatile long wSixLe = SENTINEL;
    public static volatile long wSixLf = SENTINEL;
    // sixD(double x6) — SIX adjacent doubles: twelve contiguous slots, the
    // deep-packing witness for the double kind.  Stamped as RAW bits-equivalent
    // doubles (the native side reads each back bit-exact).
    public static volatile double wSixDa = Double.NaN;
    public static volatile double wSixDb = Double.NaN;
    public static volatile double wSixDc = Double.NaN;
    public static volatile double wSixDd = Double.NaN;
    public static volatile double wSixDe = Double.NaN;
    public static volatile double wSixDf = Double.NaN;
    // mixSD(String,double,String) — a wide DOUBLE between two OBJECT references
    // (the double analogue of mixS's long-between-references).  Proves a wide
    // double neither truncates nor shifts beside 'L' slots and the two distinct-
    // length references do not swap.
    public static volatile String wMixSDa = null;
    public static volatile double wMixSDb = Double.NaN;
    public static volatile String wMixSDc = null;

    // STATIC deep-packing witnesses (no receiver; first wide arg at slot 0).
    public static volatile long sWSixLa = SENTINEL;
    public static volatile long sWSixLb = SENTINEL;
    public static volatile long sWSixLc = SENTINEL;
    public static volatile long sWSixLd = SENTINEL;
    public static volatile long sWSixLe = SENTINEL;
    public static volatile long sWSixLf = SENTINEL;
    public static volatile double sWSixDa = Double.NaN;
    public static volatile double sWSixDb = Double.NaN;
    public static volatile double sWSixDc = Double.NaN;
    public static volatile double sWSixDd = Double.NaN;
    public static volatile double sWSixDe = Double.NaN;
    public static volatile double sWSixDf = Double.NaN;
    // static (JIDIF) five-arg "every shape" witnesses (first arg at slot 0).
    public static volatile long   sWPentA = SENTINEL;
    public static volatile int    sWPentB = (int) SENTINEL;
    public static volatile double sWPentC = Double.NaN;
    public static volatile int    sWPentD = (int) SENTINEL;
    public static volatile float  sWPentE = (float) SENTINEL;

    // septa(int,int,int,int,int,int,long) — SEVEN args, six leading narrow ints
    // then a TRAILING wide long.  For an INSTANCE call the receiver takes slot 0,
    // the six ints take slots 1..6, and the long takes slots 7..8 — so the long's
    // FIRST word lands in the last writable call-stub word (params[7]) and the
    // long must NOT be truncated, dropped, or shifted at that boundary.  This is
    // the deepest instance frame the call-stub params[8] can hold without the wide
    // arg's leading word spilling past the array.  All seven operands stamped.
    public static volatile int  wSeptaA = (int) SENTINEL;
    public static volatile int  wSeptaB = (int) SENTINEL;
    public static volatile int  wSeptaC = (int) SENTINEL;
    public static volatile int  wSeptaD = (int) SENTINEL;
    public static volatile int  wSeptaE = (int) SENTINEL;
    public static volatile int  wSeptaF = (int) SENTINEL;
    public static volatile long wSeptaG = SENTINEL;
    // octa(int,int,int,int,int,int,int,long) — EIGHT args, seven leading narrow
    // ints then a TRAILING wide long, dispatched STATICALLY so the no-receiver
    // frame puts the seven ints in slots 0..6 and the long in slots 7..8 (its
    // leading word in the last call-stub word params[7]).  This is the maximum
    // arg count call() packs (param_idx caps at 8); a wide value as the 8th arg
    // is the explicit boundary witness for "the last argument is wide and must
    // survive".  All eight operands stamped.
    public static volatile int  sWOctaA = (int) SENTINEL;
    public static volatile int  sWOctaB = (int) SENTINEL;
    public static volatile int  sWOctaC = (int) SENTINEL;
    public static volatile int  sWOctaD = (int) SENTINEL;
    public static volatile int  sWOctaE = (int) SENTINEL;
    public static volatile int  sWOctaF = (int) SENTINEL;
    public static volatile int  sWOctaG = (int) SENTINEL;
    public static volatile long sWOctaH = SENTINEL;
    // octaD(int,int,int,int,int,int,int,double) — the DOUBLE analogue of octa:
    // seven narrow ints then a TRAILING wide double as the 8th arg, dispatched
    // statically.  Proves the 'D'-kind eighth arg lands bit-exact at the boundary
    // (NaN payload / sign survive the deepest pack).  All eight operands stamped.
    public static volatile int    sWOctaDa = (int) SENTINEL;
    public static volatile int    sWOctaDb = (int) SENTINEL;
    public static volatile int    sWOctaDc = (int) SENTINEL;
    public static volatile int    sWOctaDd = (int) SENTINEL;
    public static volatile int    sWOctaDe = (int) SENTINEL;
    public static volatile int    sWOctaDf = (int) SENTINEL;
    public static volatile int    sWOctaDg = (int) SENTINEL;
    public static volatile double sWOctaDh = Double.NaN;

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

    /** TWO doubles, adjacent: the double analogue of addL(long,long).  Four
     *  contiguous wide slots.  The return scales `a` by an EXACT power of two in a
     *  SEPARATE statement before adding `b`, so (1) an a<->b swap changes the
     *  result, and (2) Java's no-FMA rule and the C++ side's split expression both
     *  round `a*8.0` to a double FIRST, then add `b` — bit-identical on every
     *  target with no fused-multiply-add contraction to diverge over. */
    public double addD(final double a, final double b)
    {
        wAddDa = a;
        wAddDb = b;
        final double sa = a * 8.0; // exact exponent bump for finite a; no rounding
        return sa + b;             // pure add of two already-rounded doubles
    }

    /** Two wide kinds back-to-back, long THEN double: (long, double).  No narrow
     *  between them — proves the double starts exactly two slots after the long's
     *  start.  Return is a pure sum of both contributions; the witnesses pin each
     *  exactly (so a swap-into-the-wrong-kind is caught by the witnesses, and any
     *  truncation/zeroing of either by the return). */
    public double jd(final long a, final double b)
    {
        wJdA = a;
        wJdB = b;
        return (double) a + b;
    }

    /** The mirror: double THEN long, (double, long). */
    public double dj(final double a, final long b)
    {
        wDjA = a;
        wDjB = b;
        return a + (double) b;
    }

    /** Six args interleaving EVERY kind: int, long, double, int, long, double —
     *  ten interpreter slots.  Each narrow (a, d) sits next to wide neighbours, so
     *  a one-slot shift anywhere corrupts at least one witness AND the return.
     *  Return is a pure sum (no FMA-contractible mul+add); every operand is
     *  stamped to a witness so an exact-value check is independent of the sum. */
    public double hexA(final int a, final long b, final double c,
                       final int d, final long e, final double f)
    {
        wHexAa = a;
        wHexAb = b;
        wHexAc = c;
        wHexAd = d;
        wHexAe = e;
        wHexAf = f;
        return (double) a + (double) b + c + (double) d + (double) e + f;
    }

    /** A DIFFERENT six-arg interleave: long, int, double, long, int, double, so a
     *  single fixed mis-alignment cannot satisfy both hexA and hexB. */
    public double hexB(final long a, final int b, final double c,
                       final long d, final int e, final double f)
    {
        wHexBa = a;
        wHexBb = b;
        wHexBc = c;
        wHexBd = d;
        wHexBe = e;
        wHexBf = f;
        return (double) a + (double) b + c + (double) d + (double) e + f;
    }

    /** Wide long in the MIDDLE flanked by FLOATS: float, long, float.  Floats
     *  occupy one slot each (like int) but carry the 'F' descriptor; a packer
     *  that mis-expands the wide long against an 'F' neighbour would corrupt a
     *  flanking float.  Both floats are EXACT-representable so the return and the
     *  witnesses compare bit-exact.  The multipliers are exact small powers of two
     *  so a*256.0f and c*4.0f introduce no rounding, and each product is summed in
     *  its own already-rounded step (no FMA contraction). */
    public float mixF(final float a, final long b, final float c)
    {
        wMixFa = a;
        wMixFb = b;
        wMixFc = c;
        final float sa = a * 256.0f;     // exact for the small integral floats used
        final float sc = c * 4.0f;       // exact
        final float lo = (float) b;      // long contribution, rounded once
        return sa + lo + sc;
    }

    /** Narrow FLOAT immediately before two back-to-back wide kinds: float, long,
     *  double — five interpreter slots (1 + 2 + 2).  The float must not be
     *  widened into the long's slots, and the double must start exactly two slots
     *  past the long.  Each operand is stamped; the return is a pure sum. */
    public double fld(final float a, final long b, final double c)
    {
        wFldA = a;
        wFldB = b;
        wFldC = c;
        return (double) a + (double) b + c;
    }

    /** Wide long in the MIDDLE flanked by OBJECT references: String, long, String.
     *  Each reference is one slot.  Returns the full long plus a length-weighted
     *  mix of the two Strings (distinct multipliers so an a<->c swap of unequal
     *  lengths changes the result); each arg is stamped to a witness so the long
     *  is provably intact and the references provably did not swap.  Null-guarded
     *  so a wrong-arity abuse call (missing String -> null slot) cannot NPE. */
    public long mixS(final String a, final long b, final String c)
    {
        wMixSa = a;
        wMixSb = b;
        wMixSc = c;
        final long la = (a == null) ? 0L : (long) a.length();
        final long lc = (c == null) ? 0L : (long) c.length();
        return b + la * 1000003L + lc * 97L;
    }

    /** Object reference THEN wide long: String, long.  The long starts at the slot
     *  immediately after the one-slot reference (slot 1 for instance). */
    public long objLong(final String o, final long v)
    {
        wOlObj  = o;
        wOlLong = v;
        return v;
    }

    /** Wide long THEN object reference: long, String.  The reference slot must
     *  start exactly TWO slots after the long's start, not one. */
    public long longObj(final long v, final String o)
    {
        wLoLong = v;
        wLoObj  = o;
        return v;
    }

    /** A DOUBLE in the MIDDLE flanked by FLOATS: float, double, float.  This is
     *  the 'F'-neighbour analogue of mixC(int,double,int): a packer that mis-
     *  expands the wide double against an 'F' slot (rather than 'I') would corrupt
     *  a flanking float.  Both floats are exact small powers-of-two-scaled values
     *  so the return and witnesses compare bit-exact; the products are computed in
     *  their own already-rounded steps so no FMA contraction can diverge. */
    public float mixE(final float a, final double b, final float c)
    {
        wMixEa = a;
        wMixEb = b;
        wMixEc = c;
        final float sa = a * 256.0f;     // exact for the small integral floats used
        final float sc = c * 4.0f;       // exact
        final float bf = (float) b;      // double contribution, rounded once
        return sa + bf + sc;
    }

    /** The exact (JIDI) shape: long, int, double, int.  Seven interpreter slots.
     *  The first int sits between a long and a double; the second int follows the
     *  double.  Each operand is stamped; the return is a pure left-to-right sum
     *  (no FMA-contractible mul+add) recomputed identically on the native side. */
    public double jidi(final long a, final int b, final double c, final int d)
    {
        wJidiA = a;
        wJidiB = b;
        wJidiC = c;
        wJidiD = d;
        return (double) a + (double) b + c + (double) d;
    }

    /** The (ID J) shape: int, double, long.  The long must start exactly two
     *  slots after the double's start.  Pure sum; every operand stamped. */
    public double idj(final int a, final double b, final long c)
    {
        wIdjA = a;
        wIdjB = b;
        wIdjC = c;
        return (double) a + b + (double) c;
    }

    /** The explicit five-arg "every shape" tail: long, int, double, int, float —
     *  eight interpreter slots (2+1+2+1+1).  A TRAILING float after a wide-heavy
     *  frame proves the float lands on the correct single slot.  Pure left-to-
     *  right sum; the float widens to double once.  Every operand stamped. */
    public double widePent(final long a, final int b, final double c,
                           final int d, final float e)
    {
        wPentA = a;
        wPentB = b;
        wPentC = c;
        wPentD = d;
        wPentE = e;
        return (double) a + (double) b + c + (double) d + (double) e;
    }

    /** SIX adjacent longs: twelve contiguous interpreter slots, the deep-packing
     *  witness for the long kind.  Distinct small prime-ish multipliers (each a
     *  SEPARATE term) make any neighbour swap change the result; the full 64-bit
     *  contribution of each operand makes any truncation change it.  Each operand
     *  is also stamped to its own witness.  All arithmetic is Java two's-complement
     *  wraparound, mirrored on the native side via unsigned-wrap helpers. */
    public long sixL(final long a, final long b, final long c,
                     final long d, final long e, final long f)
    {
        wSixLa = a;
        wSixLb = b;
        wSixLc = c;
        wSixLd = d;
        wSixLe = e;
        wSixLf = f;
        return a * 1000003L + b * 31L + c * 131L + d * 524287L + e * 8191L + f;
    }

    /** SIX adjacent doubles: twelve contiguous slots, the deep-packing witness for
     *  the double kind.  Each operand is scaled by a DISTINCT exact power of two in
     *  its own rounded step (so the multiply introduces no rounding), then summed
     *  strictly left-to-right.  Java has no FMA for '+' and evaluates left-to-
     *  right, and the native side recomputes the identical operation order, so the
     *  bits match exactly.  Every operand stamped (read back bit-exact). */
    public double sixD(final double a, final double b, final double c,
                       final double d, final double e, final double f)
    {
        wSixDa = a;
        wSixDb = b;
        wSixDc = c;
        wSixDd = d;
        wSixDe = e;
        wSixDf = f;
        final double ta = a * 2.0;
        final double tb = b * 4.0;
        final double tc = c * 8.0;
        final double td = d * 16.0;
        final double te = e * 32.0;
        final double tf = f * 64.0;
        return ta + tb + tc + td + te + tf;
    }

    /** A wide DOUBLE between two OBJECT references: String, double, String — the
     *  double analogue of mixS(String,long,String).  Returns the double plus a
     *  length-weighted mix of the two Strings (distinct multipliers so an a<->c
     *  swap of unequal lengths changes the result).  Each operand stamped so the
     *  double is provably intact and the references provably did not swap.  Null-
     *  guarded so a wrong-arity abuse call cannot NPE.  The String lengths are
     *  added as doubles in their own rounded steps to stay FMA-safe. */
    public double mixSD(final String a, final double b, final String c)
    {
        wMixSDa = a;
        wMixSDb = b;
        wMixSDc = c;
        final double la = (a == null) ? 0.0 : (double) a.length();
        final double lc = (c == null) ? 0.0 : (double) c.length();
        final double wa = la * 1024.0;   // exact scale
        final double wc = lc * 16.0;     // exact scale
        return b + wa + wc;
    }

    /** SEVEN args: six narrow ints then a TRAILING wide long.  For an instance
     *  call the receiver is slot 0, the ints fill slots 1..6 and the long fills
     *  slots 7..8, so the long's leading word lands in the last writable call-stub
     *  word.  Distinct small multipliers (each a separate term) catch any neighbour
     *  swap; the full 64-bit contribution of the trailing long catches a truncation
     *  or a drop of the boundary arg.  Every operand stamped.  Java two's-complement
     *  wraparound throughout (mirrored on the native side via unsigned-wrap helpers). */
    public long septa(final int a, final int b, final int c,
                      final int d, final int e, final int f, final long g)
    {
        wSeptaA = a;
        wSeptaB = b;
        wSeptaC = c;
        wSeptaD = d;
        wSeptaE = e;
        wSeptaF = f;
        wSeptaG = g;
        return ((long) a) * 3L + ((long) b) * 31L + ((long) c) * 131L
             + ((long) d) * 524287L + ((long) e) * 8191L + ((long) f) * 17L
             + g * 1000003L;
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

    /** Static TWO doubles (no receiver; first double at slot 0).  Same FMA-safe
     *  split-multiply pattern as the instance addD. */
    public static double sAddD(final double a, final double b)
    {
        sWAddDa = a;
        sWAddDb = b;
        final double sa = a * 8.0;
        return sa + b;
    }

    /** Static long-then-double adjacency (first wide kind at slot 0). */
    public static double sJd(final long a, final double b)
    {
        sWJdA = a;
        sWJdB = b;
        return (double) a + b;
    }

    /** Static double-then-long adjacency. */
    public static double sDj(final double a, final long b)
    {
        sWDjA = a;
        sWDjB = b;
        return a + (double) b;
    }

    /** Static float,long,double interleave (no receiver; float at slot 0). */
    public static double sFld(final float a, final long b, final double c)
    {
        sWFldA = a;
        sWFldB = b;
        sWFldC = c;
        return (double) a + (double) b + c;
    }

    /** Static String,long,String — wide long between two references, no receiver
     *  (first reference at slot 0).  Same length-weighted return as the instance
     *  mixS, null-guarded for abuse safety. */
    public static long sMixS(final String a, final long b, final String c)
    {
        sWMixSa = a;
        sWMixSb = b;
        sWMixSc = c;
        final long la = (a == null) ? 0L : (long) a.length();
        final long lc = (c == null) ? 0L : (long) c.length();
        return b + la * 1000003L + lc * 97L;
    }

    /** Static SIX longs — twelve contiguous slots at the no-receiver frame (first
     *  long at slot 0).  Same deep-packing formula as the instance sixL. */
    public static long sSixL(final long a, final long b, final long c,
                             final long d, final long e, final long f)
    {
        sWSixLa = a;
        sWSixLb = b;
        sWSixLc = c;
        sWSixLd = d;
        sWSixLe = e;
        sWSixLf = f;
        return a * 1000003L + b * 31L + c * 131L + d * 524287L + e * 8191L + f;
    }

    /** Static SIX doubles — twelve contiguous slots at the no-receiver frame.
     *  Same FMA-safe split-scale-then-left-to-right-sum as the instance sixD. */
    public static double sSixD(final double a, final double b, final double c,
                               final double d, final double e, final double f)
    {
        sWSixDa = a;
        sWSixDb = b;
        sWSixDc = c;
        sWSixDd = d;
        sWSixDe = e;
        sWSixDf = f;
        final double ta = a * 2.0;
        final double tb = b * 4.0;
        final double tc = c * 8.0;
        final double td = d * 16.0;
        final double te = e * 32.0;
        final double tf = f * 64.0;
        return ta + tb + tc + td + te + tf;
    }

    /** Static (JIDIF) five-arg "every shape" tail (first arg at slot 0).  Same
     *  pure left-to-right sum as the instance widePent. */
    public static double sWidePent(final long a, final int b, final double c,
                                   final int d, final float e)
    {
        sWPentA = a;
        sWPentB = b;
        sWPentC = c;
        sWPentD = d;
        sWPentE = e;
        return (double) a + (double) b + c + (double) d + (double) e;
    }

    /** EIGHT args: seven narrow ints then a TRAILING wide long, STATIC (no
     *  receiver) so the ints fill slots 0..6 and the long fills slots 7..8 — its
     *  leading word in the last call-stub word params[7].  This is the maximum
     *  arg count call() packs; a wide value as the 8th arg is the boundary witness
     *  that the last argument is wide and survives.  Distinct multipliers catch a
     *  swap; the full 64-bit trailing long catches a truncation/drop.  All stamped.
     *  Java two's-complement wraparound (mirrored via unsigned-wrap helpers). */
    public static long sOcta(final int a, final int b, final int c, final int d,
                             final int e, final int f, final int g, final long h)
    {
        sWOctaA = a;
        sWOctaB = b;
        sWOctaC = c;
        sWOctaD = d;
        sWOctaE = e;
        sWOctaF = f;
        sWOctaG = g;
        sWOctaH = h;
        return ((long) a) * 3L + ((long) b) * 31L + ((long) c) * 131L
             + ((long) d) * 524287L + ((long) e) * 8191L + ((long) f) * 17L
             + ((long) g) * 41L + h * 1000003L;
    }

    /** The DOUBLE analogue of sOcta: seven narrow ints then a TRAILING wide
     *  double as the 8th arg, STATIC.  Proves the 'D'-kind eighth argument lands
     *  bit-exact at the boundary.  The seven ints are summed (widened to double in
     *  their own step), then the trailing double is added last; the native side
     *  recomputes the identical left-to-right order so the bits match with no FMA
     *  contraction.  All eight operands stamped (the double read back bit-exact). */
    public static double sOctaD(final int a, final int b, final int c, final int d,
                                final int e, final int f, final int g, final double h)
    {
        sWOctaDa = a;
        sWOctaDb = b;
        sWOctaDc = c;
        sWOctaDd = d;
        sWOctaDe = e;
        sWOctaDf = f;
        sWOctaDg = g;
        sWOctaDh = h;
        return (double) a + (double) b + (double) c + (double) d
             + (double) e + (double) f + (double) g + h;
    }

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
