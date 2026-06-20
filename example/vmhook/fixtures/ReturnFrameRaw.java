package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the return_value_frame_raw_access feature (area: hooks).
 *
 * The feature under test is, FROM INSIDE A HOOK detour, raw access to the live
 * HotSpot interpreter frame the trampoline stashed in the {@code return_value}:
 *   - {@code ret.frame()} returns a non-null {@code hotspot::frame*} for a real
 *     interpreted dispatch;
 *   - {@code ret.frame()->get_method()} is the SAME method the hook was installed
 *     on (name + descriptor match);
 *   - {@code ret.frame()->get_locals()} is the live local-variable array — its
 *     slot 0 holds {@code this} (the receiver oop) for an instance method, and
 *     holds the FIRST primitive arg (NOT an oop) for a static method;
 *   - reading raw primitive arg slots off {@code get_locals()} reproduces the
 *     Java argument values, respecting the HotSpot slot model: a {@code long} /
 *     {@code double} occupies TWO slots and its 64-bit value is stored at the
 *     LOWER address {@code locals[-(slot+1)]}, while the NEXT argument shifts by
 *     two slot offsets;
 *   - the same locals array that {@code frame()} exposes is the one
 *     {@code ret.set_arg(...)} mutates (raw read and typed write agree);
 *   - out-of-range slot reads return a default and never crash the JVM.
 *
 * Because the raw frame is the live {@code rbp} of the intercepted interpreter
 * activation, every method below must execute as genuine Java bytecode on the
 * Java thread (the only thing that makes the interpreter hook fire).  Each method
 * is hooked INDEPENDENTLY on the native side, so the single {@code go}/{@code done}
 * probe runs every method exactly once and the native module gathers all of its
 * raw-frame observations from that one cycle.
 *
 * Every method just echoes the argument(s) it actually received into observable
 * {@code static} fields so the native side can cross-check its raw-frame reads
 * against ground truth (and so allow-through is provable: the body still ran).
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks).
 */
public final class ReturnFrameRaw
{
    /** Native raises this to request the probe action; lowers it afterwards. */
    public static volatile boolean go;

    /** The probe action raises this when it has run; native polls it. */
    public static volatile boolean done;

    /** How many times the probe body has run (handshake proof). */
    public static volatile int probeTicks = 0;

    // A singleton receiver the probe uses for the instance-method dispatches.
    // `tag` is a per-instance fingerprint the native side reads THROUGH the oop
    // it recovers from local slot 0 — proving slot 0 really is this receiver.
    public static final int INSTANCE_TAG = 0x5A11AB1E;   // arbitrary recognisable
    public final int tag = INSTANCE_TAG;
    public static final ReturnFrameRaw instance = new ReturnFrameRaw();

    // ---- Constant args (mirrored on the native side) -----------------------
    // Chosen so each fits its width and no two collide, and so the long/double
    // halves are non-trivial (catch a halves-swap on the two-slot read).
    public static final int    SIMPLE_X = 0x1234;
    public static final int    WIDE_A   = 0x0A0B0C0D;            // int  -> slot 1
    public static final long   WIDE_B   = 0x1122334455667788L;   // long -> slots 2..3
    public static final double WIDE_C   = 3.141592653589793;     // double -> slots 4..5
    public static final int    WIDE_D   = -0x0708090A;           // int  -> slot 6 (after two wides)
    public static final int    STATIC_A = 0x00C0FFEE;            // static int -> slot 0
    public static final long   STATIC_B = 0x7EDCBA9876543210L;   // static long -> slots 1..2
    public static final int    STATIC_C = 0x1BADD00D;            // static int -> slot 3 (after long)

    // Narrow-primitive arg constants for the all-widths instance method.  Each
    // value is chosen near a width boundary so a sign-extend / truncate bug on
    // the raw slot read is caught.  All five (boolean, byte, char, short, int)
    // occupy ONE slot apiece, so they sit at consecutive base slots 1..5.
    public static final boolean NARROW_Z = true;                 // boolean -> slot 1
    public static final byte    NARROW_B = (byte) 0x80;          // byte (=-128) -> slot 2
    public static final char    NARROW_C = (char) 0xBEEF;        // char (unsigned 16) -> slot 3
    public static final short   NARROW_S = (short) 0x8001;       // short (=-32767) -> slot 4
    public static final int     NARROW_I = Integer.MIN_VALUE;    // int -> slot 5

    // float occupies ONE slot, so the trailing int must NOT shift by two — the
    // single-slot-float rule mirror of the two-slot long/double rule.
    public static final float   FLOAT_F  = Float.intBitsToFloat(0x40490FDB); // ~PI, slot 1
    public static final int     FLOAT_TAIL = 0x6C0FFEE5;         // int right after the float -> slot 2

    // Boundary long/double for the extreme-wide instance method.  MIN long has
    // its high bit set; a NaN double has every exponent bit set + a payload —
    // both maximally stress the two-slot 64-bit read.
    public static final long   EDGE_LMIN = Long.MIN_VALUE;       // long -> slots 1..2
    public static final double EDGE_DNAN = Double.longBitsToDouble(0x7FF8000ABCDEF123L); // double -> slots 3..4

    // Static method whose FIRST arg is a long: slot 0 holds the 64-bit value at
    // the lower slot even with NO `this` — the two-slot rule at the very front.
    public static final long   SLF_L0   = 0x0102030405060708L;   // long -> slots 0..1 (value@slot 0)
    public static final int    SLF_TAIL = 0x55AA55AA;            // int -> slot 2 (after the leading long)

    // Wide set_arg round-trip: the native hook overwrites the long arg via
    // set_arg, the body echoes what it finally observed.
    public static final long   WIDE_RT_INJECT = 0x0BADF00DDEADBEEFL;

    // Eight one-slot ints in a row (this@0, args@1..8).  Each value is a
    // recognisable, non-colliding pattern so a one-off slot-walk drift deep into
    // the array is caught (the existing instanceWide only reaches slot 6).  M5
    // and M8 also serve as the in-method-range set_arg targets (a HIGH but VALID
    // logical slot) the native side round-trips, and the body echoes M8 so the
    // allow-through / set_arg readback is provable.
    public static final int    MANY_1 = 0x11111111;
    public static final int    MANY_2 = 0x22222222;
    public static final int    MANY_3 = 0x33333333;
    public static final int    MANY_4 = 0x44444444;
    public static final int    MANY_5 = 0x55555555;
    public static final int    MANY_6 = 0x66666666;
    public static final int    MANY_7 = 0x77777777;
    public static final int    MANY_8 = 0x7EEEEEEE;
    // Native set_arg(5, this) overwrites M5 in-place; the body echoes the M5 slot
    // AFTER any mutation so the high-slot write is provable through the body too.
    public static final int    MANY_5_INJECT = 0x0AB1CD05;

    // Static method with THREE consecutive two-slot args and NO this: the
    // accumulator must add +2 each time from slot 0 onward (long@0(value@0),
    // double@2(value@2), long@4(value@4), int@6).  The static counterpart of the
    // instance two-slot model, which the instance-only instanceWide does not pin.
    public static final long   SMW_L0 = 0x0A0A0A0A0B0B0B0BL;       // long  -> slots 0..1
    public static final double SMW_D  = Double.longBitsToDouble(0x4005BF0995AAF790L); // double -> slots 2..3
    public static final long   SMW_L1 = 0x0C0C0C0C0D0D0D0DL;       // long  -> slots 4..5
    public static final int    SMW_T  = 0x0E0E0E0E;                // int   -> slot 6

    // Instance method INTERLEAVING wide/narrow in a DIFFERENT order than
    // instanceWide (which is int,long,double,int): here long,int,double,int.
    // this@0, l@1(value@1->lower slot 2), i@3, d@4(value@4->lower slot 5),
    // tail@6.  Catches an accumulator that only happens to work for the existing
    // ordering.
    public static final long   WTN_L    = 0x1314151617181920L;     // long   -> slots 1..2
    public static final int    WTN_I    = 0x21222324;              // int    -> slot 3
    public static final double WTN_D    = Double.longBitsToDouble(0x400921FB54442D18L); // double (2*PI) -> slots 4..5
    public static final int    WTN_TAIL = 0x31323334;              // int    -> slot 6

    // Narrow set_arg ZERO injection: writing 0 into a one-slot int local exercises
    // the all-zero-slot write path (the store comment flags zero-slot writes as a
    // historical corruption class).  The body echoes what it finally saw.
    public static final int    ZERO_RT_INPUT = 0x1357ACE0;         // non-zero original

    // ---- Echoed observations (what each body actually received) -------------
    public static volatile int    simpleSeen   = 0;
    public static volatile int    wideASeen    = 0;
    public static volatile long   wideBSeen    = 0L;
    public static volatile long   wideCBitsSeen = 0L;            // raw IEEE-754 bits
    public static volatile int    wideDSeen    = 0;
    public static volatile int    staticASeen  = 0;
    public static volatile long   staticBSeen  = 0L;
    public static volatile int    staticCSeen  = 0;

    // set_arg round-trip target: the body echoes what it actually observed AFTER
    // any in-hook mutation, so the native side can confirm frame()'s locals
    // alias the array set_arg writes.
    public static volatile int    roundTripSeen = 0;

    // ---- Echoed observations for the deepening methods ---------------------
    public static volatile int    narrowZSeen = -1;              // boolean as 0/1
    public static volatile int    narrowBSeen = 0;               // byte (sign-extended)
    public static volatile int    narrowCSeen = 0;               // char (zero-extended)
    public static volatile int    narrowSSeen = 0;               // short (sign-extended)
    public static volatile int    narrowISeen = 0;               // int
    public static volatile long   floatFBitsSeen = 0L;           // raw IEEE-754 bits of the float
    public static volatile int    floatTailSeen = 0;             // the int right after the float
    public static volatile long   edgeLSeen = 0L;                // Long.MIN_VALUE
    public static volatile long   edgeDBitsSeen = 0L;            // NaN double raw bits
    public static volatile long   slfL0Seen = 0L;                // leading static long
    public static volatile int    slfTailSeen = 0;               // trailing static int
    public static volatile long   wideRoundTripSeen = 0L;        // long observed after set_arg

    // Deepening batch-20 echoes.
    public static volatile int    many5Seen = 0;                 // slot 5 observed AFTER set_arg(5,...)
    public static volatile int    many8Seen = 0;                 // slot 8 (deepest one-slot arg)
    public static volatile int    manySumSeen = 0;               // XOR of all eight, allow-through proof
    public static volatile long   smwL0Seen = 0L;                // static wide arg0
    public static volatile long   smwDBitsSeen = 0L;             // static wide double raw bits
    public static volatile long   smwL1Seen = 0L;                // static wide arg2 (long)
    public static volatile int    smwTSeen = 0;                  // static wide trailing int
    public static volatile long   wtnLSeen = 0L;                 // interleaved long
    public static volatile int    wtnISeen = 0;                  // interleaved int between the wides
    public static volatile long   wtnDBitsSeen = 0L;             // interleaved double raw bits
    public static volatile int    wtnTailSeen = 0;               // interleaved trailing int
    public static volatile int    zeroRtSeen = 0;                // int observed after set_arg(1, 0)

    // ---- Hookable methods ---------------------------------------------------

    /**
     * Minimal instance method.  Used for receiver / method-identity / bounds:
     *   slot 0 = this, slot 1 = x.
     */
    public int instanceSimple(final int x)
    {
        simpleSeen = x;
        return this.tag + x;
    }

    /**
     * Wide instance method exercising the full slot model:
     *   slot 0 = this, slot 1 = a (int), slots 2..3 = b (long, value at base
     *   slot 2), slots 4..5 = c (double, value at base slot 4), slot 6 = d (int).
     */
    public long instanceWide(final int a, final long b, final double c, final int d)
    {
        wideASeen     = a;
        wideBSeen     = b;
        wideCBitsSeen = Double.doubleToRawLongBits(c);
        wideDSeen     = d;
        return this.tag + a + b + (long) c + d;
    }

    /**
     * Static twin: NO {@code this}, so the first primitive arg sits at slot 0.
     *   slot 0 = a (int), slots 1..2 = b (long, value at base slot 1),
     *   slot 3 = c (int).
     */
    public static long staticWide(final int a, final long b, final int c)
    {
        staticASeen = a;
        staticBSeen = b;
        staticCSeen = c;
        return (long) a + b + c;
    }

    /**
     * set_arg round-trip target: slot 0 = this, slot 1 = value.  The native hook
     * mutates slot 1 via set_arg, then reads it back through frame()->get_locals()
     * to prove both reach the same array; the body records what it finally saw.
     */
    public int roundTrip(final int value)
    {
        roundTripSeen = value;
        return value;
    }

    /**
     * All five one-slot narrow primitives in a row (boolean, byte, char, short,
     * int).  None is wide, so they sit at consecutive base slots 1..5 with NO
     * two-slot gaps — the single-slot half of the slot model, with each value
     * pushed to a width boundary to catch a sign/zero-extend bug on the raw read.
     *   slot 0 = this, slot 1 = z, slot 2 = b, slot 3 = c, slot 4 = s, slot 5 = i.
     */
    public int instanceNarrow(final boolean z, final byte b, final char c,
                              final short s, final int i)
    {
        narrowZSeen = z ? 1 : 0;
        narrowBSeen = b;
        narrowCSeen = c;
        narrowSSeen = s;
        narrowISeen = i;
        return i;
    }

    /**
     * A float occupies exactly ONE slot, so the trailing int does NOT shift by
     * two (unlike after a long/double).  Proves the single-slot-float rule.
     *   slot 0 = this, slot 1 = f (float), slot 2 = tail (int).
     */
    public int instanceFloat(final float f, final int tail)
    {
        floatFBitsSeen = Float.floatToRawIntBits(f) & 0xFFFFFFFFL;
        floatTailSeen  = tail;
        return tail;
    }

    /**
     * Boundary wide values: Long.MIN_VALUE then a NaN double.  Maximally
     * stresses the two consecutive two-slot reads.
     *   slot 0 = this, slots 1..2 = l (long, value@slot 1), slots 3..4 = d
     *   (double, value@slot 3).
     */
    public long instanceEdgeWide(final long l, final double d)
    {
        edgeLSeen     = l;
        edgeDBitsSeen = Double.doubleToRawLongBits(d);
        return l;
    }

    /**
     * Static method whose FIRST arg is a long: slot 0 holds the 64-bit value at
     * the lower slot even with NO {@code this} — the two-slot rule at the front.
     *   slots 0..1 = l (long, value@slot 0), slot 2 = tail (int).
     */
    public static long staticLeadingLong(final long l, final int tail)
    {
        slfL0Seen   = l;
        slfTailSeen = tail;
        return l;
    }

    /**
     * Wide set_arg round-trip: slot 0 = this, slots 1..2 = value (long).  The
     * native hook overwrites the long via set_arg(1, ...), which lands at the
     * lower slot locals[-2]; the body records what it finally observed.
     */
    public long wideRoundTrip(final long value)
    {
        wideRoundTripSeen = value;
        return value;
    }

    /**
     * Eight one-slot ints in a row: this@0, then args@1..8 with NO two-slot
     * gaps.  Reaches deeper into the local array than any other method here, so
     * a one-off slot-walk drift past slot 6 is caught.  The native hook also
     * overwrites slot 5 via set_arg(5, ...) (a HIGH but in-range logical slot),
     * so the body's m5 echo proves the high-slot write landed (allow-through).
     *   slot 0 = this, slots 1..8 = a1..a8.
     */
    public int instanceManyLocals(final int a1, final int a2, final int a3,
                                  final int a4, final int a5, final int a6,
                                  final int a7, final int a8)
    {
        many5Seen   = a5;   // may be the injected value if the native hook wrote it
        many8Seen   = a8;
        manySumSeen = a1 ^ a2 ^ a3 ^ a4 ^ a6 ^ a7 ^ a8; // excludes a5 (mutated)
        return a8;
    }

    /**
     * Static method, NO this, THREE consecutive two-slot args then a tail int:
     * long@slots0..1(value@0), double@slots2..3(value@2), long@slots4..5
     * (value@4), int@slot6.  The slot accumulator must add +2 each time starting
     * from slot 0 — the static multi-wide counterpart instanceWide lacks.
     */
    public static long staticManyWide(final long l0, final double d,
                                      final long l1, final int tail)
    {
        smwL0Seen    = l0;
        smwDBitsSeen = Double.doubleToRawLongBits(d);
        smwL1Seen    = l1;
        smwTSeen     = tail;
        return l0 + (long) d + l1 + tail;
    }

    /**
     * Instance method interleaving wide/narrow in a DIFFERENT order than
     * instanceWide: long, int, double, int.  this@0, l@slots1..2(value@1), i@3,
     * d@slots4..5(value@4), tail@6.  Catches a slot accumulator that only
     * happens to work for the int,long,double,int ordering.
     */
    public long instanceWideThenNarrow(final long l, final int i,
                                       final double d, final int tail)
    {
        wtnLSeen     = l;
        wtnISeen     = i;
        wtnDBitsSeen = Double.doubleToRawLongBits(d);
        wtnTailSeen  = tail;
        return l + i + (long) d + tail;
    }

    /**
     * Narrow set_arg ZERO injection target: this@0, value@slot1.  The native
     * hook writes 0 into slot 1 (the all-zero-slot write path) and the body
     * echoes what it finally observed, proving a zero write neither corrupts an
     * adjacent slot nor is silently dropped.
     */
    public int zeroRoundTrip(final int value)
    {
        zeroRtSeen = value;
        return value;
    }

    private void runAll()
    {
        // Each call is one real bytecode dispatch -> the matching interpreter
        // hook fires and reads (and for roundTrip, mutates) the live frame.
        this.instanceSimple(SIMPLE_X);
        this.instanceWide(WIDE_A, WIDE_B, WIDE_C, WIDE_D);
        staticWide(STATIC_A, STATIC_B, STATIC_C);
        this.roundTrip(7);
        this.instanceNarrow(NARROW_Z, NARROW_B, NARROW_C, NARROW_S, NARROW_I);
        this.instanceFloat(FLOAT_F, FLOAT_TAIL);
        this.instanceEdgeWide(EDGE_LMIN, EDGE_DNAN);
        staticLeadingLong(SLF_L0, SLF_TAIL);
        this.wideRoundTrip(11L);
        this.instanceManyLocals(MANY_1, MANY_2, MANY_3, MANY_4,
                                MANY_5, MANY_6, MANY_7, MANY_8);
        staticManyWide(SMW_L0, SMW_D, SMW_L1, SMW_T);
        this.instanceWideThenNarrow(WTN_L, WTN_I, WTN_D, WTN_TAIL);
        this.zeroRoundTrip(ZERO_RT_INPUT);
        probeTicks++;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnFrameRaw.go && !ReturnFrameRaw.done;
            }

            @Override
            public void run()
            {
                ReturnFrameRaw.instance.runAll();
                ReturnFrameRaw.done = true;
            }
        });
    }
}
