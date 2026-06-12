package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_primitives_get feature: field_proxy::get() for EVERY
 * JVM primitive descriptor (Z B S C I J F D) at regular AND boundary values.
 *
 * The native module reads each field two ways -- via the static accessor
 * static_field("name")->get() and via an instance wrapper's
 * get_field("name")->get() -- and compares the decoded C++ value (and the
 * variant alternative index) against the Java-side value baked in here.
 *
 * Coverage strategy:
 *  - STATIC fields hold every boundary value of every primitive so the native
 *    side can read them with static_field(...).
 *  - INSTANCE fields mirror a representative subset so the instance-dispatch
 *    path of get() is exercised too (the audit notes get() must ignore the
 *    static/instance flag and produce identical values either way).
 *  - RUNTIME fields are written fresh by the probe's run() on a real Java
 *    bytecode dispatch (putstatic / putfield), proving get() reads live
 *    post-dispatch JVM state rather than just class-initializer constants.
 *
 * Bit patterns matter for F/D: the floats/doubles are reconstructed from raw
 * int/long bits via Float.intBitsToFloat / Double.longBitsToDouble so the
 * native side can assert bit-exact round-trips (NaN payload, signaling-NaN
 * bit, denormal mantissa, +/-0.0 sign bit).
 *
 * ENCODING NOTE: every char value uses a numeric/\\uXXXX form (processed by the
 * Java lexer regardless of the source-file encoding), so this fixture compiles
 * identically under javac on Windows (Cp1252) and Linux/macOS (UTF-8) -- the CI
 * invokes javac without an explicit -encoding flag.
 */
public final class FieldPrimitivesGet
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    // =====================================================================
    //  boolean  ("Z")  -- variant alternative: bool (index 0)
    // =====================================================================
    public static boolean sBoolTrue  = true;
    public static boolean sBoolFalse = false;

    // =====================================================================
    //  byte  ("B")  -- variant alternative: int8_t (index 1)
    // =====================================================================
    public static byte sByteZero   = 0;
    public static byte sByteOne    = 1;
    public static byte sByteNegOne = -1;
    public static byte sByteMin    = Byte.MIN_VALUE;   // -128
    public static byte sByteMax    = Byte.MAX_VALUE;   //  127
    public static byte sByte0x7F   = (byte) 0x7F;      //  127
    public static byte sByte0x80   = (byte) 0x80;      // -128
    public static byte sByte0xFF   = (byte) 0xFF;      //   -1
    public static byte sByte0xAB   = (byte) 0xAB;      //  -85

    // =====================================================================
    //  short  ("S")  -- variant alternative: int16_t (index 2)
    // =====================================================================
    public static short sShortZero   = 0;
    public static short sShortOne    = 1;
    public static short sShortNegOne = -1;
    public static short sShortMin    = Short.MIN_VALUE; // -32768
    public static short sShortMax    = Short.MAX_VALUE; //  32767
    public static short sShort0x8000 = (short) 0x8000;  // -32768
    public static short sShort0x7FFF = (short) 0x7FFF;  //  32767
    public static short sShortBeef   = (short) 0xBEEF;  //  -16657

    // =====================================================================
    //  int  ("I")  -- variant alternative: int32_t (index 3)
    // =====================================================================
    public static int sIntZero       = 0;
    public static int sIntOne        = 1;
    public static int sIntNegOne     = -1;
    public static int sIntMin        = Integer.MIN_VALUE;
    public static int sIntMax        = Integer.MAX_VALUE;
    public static int sIntDeadBeef   = 0xDEADBEEF;       // -559038737
    public static int sInt0x7FFFFFFF = 0x7FFFFFFF;       //  2147483647
    public static int sInt0x80000000 = 0x80000000;       // -2147483648

    // =====================================================================
    //  long  ("J")  -- variant alternative: int64_t (index 4)
    // =====================================================================
    public static long sLongZero               = 0L;
    public static long sLongOne                = 1L;
    public static long sLongNegOne             = -1L;
    public static long sLongMin                = Long.MIN_VALUE;
    public static long sLongMax                = Long.MAX_VALUE;
    public static long sLongDeadBeef           = 0xDEADBEEFCAFEBABEL;
    public static long sLong0x7FFFFFFFFFFFFFFF = 0x7FFFFFFFFFFFFFFFL;
    public static long sLong0x8000000000000000 = 0x8000000000000000L;
    public static long sLongHighBits           = 0x00000000FFFFFFFFL; // 4294967295

    // =====================================================================
    //  char  ("C")  -- variant alternative: uint16_t (index 7), UTF-16 unit.
    //  All values written as integer/\\uXXXX escapes (encoding-independent).
    // =====================================================================
    public static char sCharNull    = 0x0000; // '\0' (NUL, char MIN)
    public static char sCharSpace   = 0x0020; // ' '
    public static char sCharA       = 0x0041; // 'A'
    public static char sCharZeroDig = 0x0030; // '0' (mid ASCII)
    public static char sChar0x7F    = 0x007F; // DEL, last single-byte ASCII
    public static char sChar0x80    = 0x0080; // first value with high bit set
    public static char sChar0xFF    = 0x00FF; // last value that fits in 8 bits
    public static char sChar0x0100  = 0x0100; // first value needing the high byte
    public static char sCharMax     = 0xFFFF; // Character.MAX_VALUE
    public static char sCharHighBit = 0x00E9; // 'e-acute' (>0x7F: char-narrowing witness)
    public static char sCharBmp     = 0x4E2D; // CJK char (>0xFF: high-byte-drop witness)
    public static char sCharHiSurr  = 0xD83D; // high surrogate of U+1F600
    public static char sCharLoSurr  = 0xDE00; // low surrogate of U+1F600
    public static char sCharMinSurr = 0xD800; // first high surrogate
    public static char sCharMaxSurr = 0xDFFF; // last low surrogate

    // =====================================================================
    //  float  ("F")  -- variant alternative: float (index 5).
    //  Built from raw bits so the native side asserts bit-exact patterns.
    // =====================================================================
    public static float sFloatPosZero = Float.intBitsToFloat(0x00000000); // +0.0
    public static float sFloatNegZero = Float.intBitsToFloat(0x80000000); // -0.0
    public static float sFloatOne     = Float.intBitsToFloat(0x3F800000); // +1.0
    public static float sFloatNegOne  = Float.intBitsToFloat(0xBF800000); // -1.0
    public static float sFloatMin     = Float.MIN_VALUE;                  // 0x00000001 (denormal)
    public static float sFloatMax     = Float.MAX_VALUE;                  // 0x7F7FFFFF
    public static float sFloatMinNorm = Float.MIN_NORMAL;                 // 0x00800000
    public static float sFloatPosInf  = Float.POSITIVE_INFINITY;          // 0x7F800000
    public static float sFloatNegInf  = Float.NEGATIVE_INFINITY;          // 0xFF800000
    public static float sFloatNaN     = Float.NaN;                        // canonical qNaN 0x7FC00000
    public static float sFloatSNaN    = Float.intBitsToFloat(0x7F800001); // signaling NaN
    public static float sFloatNaNPay  = Float.intBitsToFloat(0x7FA55555); // qNaN with payload
    public static float sFloatPi      = 3.14159265358979F;                // ordinary value
    public static float sFloatDenorm  = Float.intBitsToFloat(0x00000001); // 1.4e-45 (== MIN_VALUE)
    // Exact-representable fractions: asserted BIT-EXACT (no epsilon needed).
    public static float sFloatHalf    = 0.5F;                             // 0x3F000000
    public static float sFloatQuarter = 0.25F;                            // 0x3E800000
    public static float sFloatNegHalf = -0.5F;                            // 0xBF000000
    public static float sFloatThreeQ  = 0.75F;                            // 0x3F400000
    public static float sFloatTwo     = 2.0F;                             // 0x40000000

    // =====================================================================
    //  double  ("D")  -- variant alternative: double (index 6)
    // =====================================================================
    public static double sDoublePosZero = Double.longBitsToDouble(0x0000000000000000L); // +0.0
    public static double sDoubleNegZero = Double.longBitsToDouble(0x8000000000000000L); // -0.0
    public static double sDoubleOne     = Double.longBitsToDouble(0x3FF0000000000000L); // +1.0
    public static double sDoubleNegOne  = Double.longBitsToDouble(0xBFF0000000000000L); // -1.0
    public static double sDoubleMin     = Double.MIN_VALUE;                              // 0x0...01 denormal
    public static double sDoubleMax     = Double.MAX_VALUE;                              // 0x7FEFFFFFFFFFFFFF
    public static double sDoubleMinNorm = Double.MIN_NORMAL;                             // 0x0010000000000000
    public static double sDoublePosInf  = Double.POSITIVE_INFINITY;                      // 0x7FF0000000000000
    public static double sDoubleNegInf  = Double.NEGATIVE_INFINITY;                      // 0xFFF0000000000000
    public static double sDoubleNaN     = Double.NaN;                                    // 0x7FF8000000000000
    public static double sDoubleSNaN    = Double.longBitsToDouble(0x7FF0000000000001L);  // signaling NaN
    public static double sDoubleNaNPay  = Double.longBitsToDouble(0x7FFAAAAAAAAAAAAAL);  // qNaN with payload
    public static double sDoublePi      = 3.141592653589793;                             // ordinary value
    public static double sDoubleDenorm  = Double.longBitsToDouble(0x0000000000000001L);  // == MIN_VALUE
    // Exact-representable fractions: asserted BIT-EXACT (no epsilon needed).
    public static double sDoubleHalf    = 0.5;                                            // 0x3FE0000000000000
    public static double sDoubleQuarter = 0.25;                                           // 0x3FD0000000000000
    public static double sDoubleNegHalf = -0.5;                                           // 0xBFE0000000000000
    public static double sDoubleThreeQ  = 0.75;                                           // 0x3FE8000000000000
    public static double sDoubleTwo     = 2.0;                                            // 0x4000000000000000

    // =====================================================================
    //  INSTANCE fields -- representative subset of each primitive so the
    //  instance-dispatch path of get() is exercised.  Values intentionally
    //  DIFFER from the static ones so a static/instance mix-up is caught.
    // =====================================================================
    public boolean iBool   = true;
    public byte    iByte   = (byte) 0xFE;                       //   -2
    public short   iShort  = (short) 0xCAFE;                    //  -13570
    public int     iInt    = 0x0BADF00D;                       //  195948557
    public long    iLong   = 0x0123456789ABCDEFL;
    public char    iChar   = 0x20AC;                            // euro sign
    public float   iFloat  = Float.intBitsToFloat(0xC0490FDB);  // -pi bit pattern
    public double  iDouble = Double.longBitsToDouble(0x400921FB54442D18L); // pi

    // Instance BOUNDARY fields: the instance-dispatch path of get() exercised at
    // the extremes of every primitive (not just one representative value), so a
    // static/instance offset-basis mix-up at a boundary is caught too.
    public boolean iBoolFalse  = false;
    public byte    iByteMin    = Byte.MIN_VALUE;                //  -128
    public byte    iByteMax    = Byte.MAX_VALUE;                //   127
    public byte    iByteZero   = 0;
    public short   iShortMin   = Short.MIN_VALUE;              // -32768
    public short   iShortMax   = Short.MAX_VALUE;              //  32767
    public int     iIntMin     = Integer.MIN_VALUE;
    public int     iIntMax     = Integer.MAX_VALUE;
    public int     iIntNegOne  = -1;
    public long    iLongMin    = Long.MIN_VALUE;
    public long    iLongMax    = Long.MAX_VALUE;
    public char    iCharNull   = 0x0000;                       // '\0'
    public char    iCharMax    = 0xFFFF;
    public float   iFloatPosInf = Float.POSITIVE_INFINITY;     // 0x7F800000
    public float   iFloatNaN    = Float.NaN;                   // 0x7FC00000
    public float   iFloatNegZero = Float.intBitsToFloat(0x80000000); // -0.0
    public double  iDoubleMax   = Double.MAX_VALUE;            // 0x7FEFFFFFFFFFFFFF
    public double  iDoubleNegInf = Double.NEGATIVE_INFINITY;   // 0xFFF0000000000000
    public double  iDoubleNegZero = Double.longBitsToDouble(0x8000000000000000L); // -0.0

    // =====================================================================
    //  EXHAUSTIVE INSTANCE fields -- FULL boundary parity with the static set
    //  for EVERY primitive, so the instance-dispatch path of get() (offset basis
    //  = decoded oop + offset) is exercised at EVERY value the static path is, not
    //  just a representative subset.  Names mirror the static sXxx fields with an
    //  iXxx prefix.  All char/float/double values are encoding-independent numeric
    //  or raw-bit forms (see the ENCODING NOTE above).
    // =====================================================================

    // byte ("B") -- remaining boundaries (Min/Max/Zero already declared above).
    public byte iByteOne    = 1;
    public byte iByteNegOne = -1;
    public byte iByte0x7F   = (byte) 0x7F;   //  127
    public byte iByte0x80   = (byte) 0x80;   // -128
    public byte iByte0xFF   = (byte) 0xFF;   //   -1 (sign-extension witness)
    public byte iByte0xAB   = (byte) 0xAB;   //  -85

    // short ("S") -- remaining boundaries (Min/Max already declared above).
    public short iShortZero   = 0;
    public short iShortNegOne = -1;
    public short iShort0x8000 = (short) 0x8000; // -32768
    public short iShort0x7FFF = (short) 0x7FFF; //  32767
    public short iShortBeef   = (short) 0xBEEF; //  -16657

    // int ("I") -- remaining boundaries (Min/Max/NegOne already declared above).
    public int iIntZero       = 0;
    public int iIntOne         = 1;
    public int iIntDeadBeef    = 0xDEADBEEF;     // -559038737
    public int iInt0x7FFFFFFF  = 0x7FFFFFFF;     //  2147483647
    public int iInt0x80000000  = 0x80000000;     // -2147483648

    // long ("J") -- remaining boundaries (Min/Max already declared above).
    public long iLongZero               = 0L;
    public long iLongOne                = 1L;
    public long iLongNegOne             = -1L;
    public long iLongDeadBeef           = 0xDEADBEEFCAFEBABEL;
    public long iLong0x7FFFFFFFFFFFFFFF = 0x7FFFFFFFFFFFFFFFL;
    public long iLong0x8000000000000000 = 0x8000000000000000L;
    public long iLongHighBits           = 0x00000000FFFFFFFFL; // 4294967295

    // char ("C") -- full BMP + surrogate sweep (Null/Max already declared above).
    public char iCharSpace   = 0x0020; // ' '
    public char iCharA       = 0x0041; // 'A'
    public char iCharZeroDig = 0x0030; // '0'
    public char iChar0x7F    = 0x007F; // DEL, last single-byte ASCII
    public char iChar0x80    = 0x0080; // first value with high bit set
    public char iChar0xFF    = 0x00FF; // last value that fits in 8 bits
    public char iChar0x0100  = 0x0100; // first value needing the high byte
    public char iCharHighBit = 0x00E9; // 'e-acute' (>0x7F: char-narrowing witness)
    public char iCharBmp     = 0x4E2D; // CJK char (>0xFF: high-byte-drop witness)
    public char iCharHiSurr  = 0xD83D; // high surrogate of U+1F600
    public char iCharLoSurr  = 0xDE00; // low surrogate of U+1F600
    public char iCharMinSurr = 0xD800; // first high surrogate
    public char iCharMaxSurr = 0xDFFF; // last low surrogate

    // float ("F") -- full IEEE-754 special set + exact fractions
    // (PosInf/NaN/NegZero already declared above).  Raw bits so the native side
    // asserts bit-exact patterns through the instance path.
    public float iFloatPosZero = Float.intBitsToFloat(0x00000000); // +0.0
    public float iFloatOne     = Float.intBitsToFloat(0x3F800000); // +1.0
    public float iFloatNegOne  = Float.intBitsToFloat(0xBF800000); // -1.0
    public float iFloatMin     = Float.MIN_VALUE;                  // 0x00000001 denormal
    public float iFloatMax     = Float.MAX_VALUE;                  // 0x7F7FFFFF
    public float iFloatMinNorm = Float.MIN_NORMAL;                 // 0x00800000
    public float iFloatNegInf  = Float.NEGATIVE_INFINITY;          // 0xFF800000
    public float iFloatSNaN    = Float.intBitsToFloat(0x7F800001); // signaling NaN
    public float iFloatNaNPay  = Float.intBitsToFloat(0x7FA55555); // qNaN with payload
    public float iFloatDenorm  = Float.intBitsToFloat(0x00000001); // == MIN_VALUE
    public float iFloatHalf    = 0.5F;                             // 0x3F000000
    public float iFloatQuarter = 0.25F;                            // 0x3E800000
    public float iFloatNegHalf = -0.5F;                            // 0xBF000000
    public float iFloatThreeQ  = 0.75F;                            // 0x3F400000
    public float iFloatTwo     = 2.0F;                             // 0x40000000

    // double ("D") -- full IEEE-754 special set + exact fractions
    // (Max/NegInf/NegZero already declared above).
    public double iDoublePosZero = Double.longBitsToDouble(0x0000000000000000L); // +0.0
    public double iDoubleOne     = Double.longBitsToDouble(0x3FF0000000000000L); // +1.0
    public double iDoubleNegOne  = Double.longBitsToDouble(0xBFF0000000000000L); // -1.0
    public double iDoubleMin     = Double.MIN_VALUE;                             // 0x0...01 denormal
    public double iDoubleMinNorm = Double.MIN_NORMAL;                            // 0x0010000000000000
    public double iDoublePosInf  = Double.POSITIVE_INFINITY;                     // 0x7FF0000000000000
    public double iDoubleNaN     = Double.NaN;                                   // 0x7FF8000000000000
    public double iDoubleSNaN    = Double.longBitsToDouble(0x7FF0000000000001L); // signaling NaN
    public double iDoubleNaNPay  = Double.longBitsToDouble(0x7FFAAAAAAAAAAAAAL); // qNaN with payload
    public double iDoubleDenorm  = Double.longBitsToDouble(0x0000000000000001L); // == MIN_VALUE
    public double iDoubleHalf    = 0.5;                                          // 0x3FE0000000000000
    public double iDoubleQuarter = 0.25;                                         // 0x3FD0000000000000
    public double iDoubleNegHalf = -0.5;                                         // 0xBFE0000000000000
    public double iDoubleThreeQ  = 0.75;                                         // 0x3FE8000000000000
    public double iDoubleTwo     = 2.0;                                          // 0x4000000000000000

    // =====================================================================
    //  FINAL instance primitive fields -- read through the SAME get() path.
    //  `final` changes nothing about the storage layout (still a regular
    //  instance slot at oop + offset), so get() must read them identically; this
    //  pins that the read path does not special-case (or choke on) final fields.
    // =====================================================================
    public final boolean fBool   = true;
    public final byte    fByte   = (byte) 0x80;                       // -128
    public final short   fShort  = (short) 0x7FFF;                    //  32767
    public final int     fInt    = 0x7FFFFFFF;                        //  INT32_MAX
    public final long    fLong   = Long.MIN_VALUE;
    public final char    fChar   = 0xFFFF;                            //  CHAR MAX
    public final float   fFloat  = Float.intBitsToFloat(0x7F800000);  // +Inf
    public final double  fDouble = Double.longBitsToDouble(0x7FF8000000000000L); // canonical NaN

    // =====================================================================
    //  MULTI-OFFSET cluster -- six int fields declared back-to-back with
    //  DISTINCT values.  Reading all six and getting six distinct correct values
    //  proves get() applies each field's OWN offset (oop + entry->offset); an
    //  off-by-one offset bug would read a neighbour and surface as a wrong value.
    // =====================================================================
    public int off0 = 0x1000_0001;
    public int off1 = 0x2000_0002;
    public int off2 = 0x3000_0003;
    public int off3 = 0x4000_0004;
    public int off4 = 0x5000_0005;
    public int off5 = 0x6000_0006;

    // Mixed-width neighbours: a byte wedged between two ints, so the int reads
    // must skip the (possibly-padded) 1-byte slot and still land correctly --
    // another offset-correctness witness across heterogeneous widths.
    public int  mixBeforeInt = 0x0AAAAAA0;
    public byte mixByte      = (byte) 0x5A;   //  90
    public int  mixAfterInt  = 0x0BBBBBB0;

    // =====================================================================
    //  MUTATE-BETWEEN-READS witness -- pre-seeded to a known NON-default value,
    //  then OVERWRITTEN by writeRuntime() (putfield) to a DIFFERENT value.  The
    //  native side reads it BEFORE and AFTER the probe; before != after proves
    //  get() reads LIVE storage on every call (no caching of the first read).
    // =====================================================================
    public int  mutInt  = 0x11111111;   // pre-probe sentinel
    public long mutLong  = 0x2222222222222222L;

    // =====================================================================
    //  RUNTIME fields -- written fresh by the probe on a real bytecode
    //  dispatch (putstatic / putfield).  The native side reads them AFTER
    //  the probe completes, proving get() reflects live JVM state.
    // =====================================================================
    public static boolean rBool;
    public static byte    rByte;
    public static short   rShort;
    public static int     rInt;
    public static long    rLong;
    public static char    rChar;
    public static float   rFloat;
    public static double  rDouble;

    public boolean rIBool;
    public int     rIInt;
    public long    rILong;
    public double  rIDouble;

    /** Held so the native side can build an instance wrapper for instance reads. */
    public static FieldPrimitivesGet instance = new FieldPrimitivesGet();

    /** Side effect the probe performs through real bytecode, observed natively. */
    private int seed = 7;

    /**
     * Writes the runtime fields via genuine putstatic / putfield bytecode and
     * returns a witness value.  The field writes here are what the native side
     * reads back, so get() is tested against live, post-dispatch state.
     */
    public int writeRuntime(final int salt)
    {
        // Static runtime fields (putstatic).
        rBool   = true;
        rByte   = (byte) 0x80;            // -128, INT8_MIN
        rShort  = Short.MIN_VALUE;        // -32768
        rInt    = Integer.MIN_VALUE;
        rLong   = Long.MAX_VALUE;
        rChar   = 0xFFFF;
        rFloat  = Float.NEGATIVE_INFINITY;
        rDouble = Double.longBitsToDouble(0x7FF8000000000000L); // canonical NaN

        // Instance runtime fields (putfield) on the held instance.
        instance.rIBool   = true;
        instance.rIInt    = 0x7FFFFFFF;   // INT32_MAX
        instance.rILong   = Long.MIN_VALUE;
        instance.rIDouble = Math.PI;

        // Mutate-between-reads witnesses: overwrite the pre-seeded sentinels with
        // DIFFERENT values so the native side's post-probe re-read differs from its
        // pre-probe read (proves get() is not caching the first value).
        instance.mutInt  = 0x33333333; // was 0x11111111
        instance.mutLong = 0x4444444444444444L; // was 0x2222...

        return this.seed + salt;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldPrimitivesGet.go && !FieldPrimitivesGet.done;
            }

            @Override
            public void run()
            {
                // Real bytecode dispatch: invokevirtual writeRuntime(...) which
                // executes putstatic / putfield on the runtime fields the native
                // side then reads back through field_proxy::get().
                FieldPrimitivesGet.instance.writeRuntime(35);
                FieldPrimitivesGet.done = true;
            }
        });
    }
}
