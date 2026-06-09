package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_call_primitives" feature: exercises
 * vmhook::method_proxy::call() returning every JVM primitive (Z B S C I J F D)
 * and void, at boundary values, through a real bytecode dispatch.
 *
 * How the native module drives it:
 *   - The native side hooks {@link #trigger(int)}.  Inside that detour
 *     vmhook::hotspot::current_java_thread is set, which is the ONLY context in
 *     which method_proxy::call() may invoke the interpreter / JNI call gate.
 *   - The probe's run() simply calls trigger(7) on the shared instance.  That
 *     fires the detour, and the detour calls every return-typed method below via
 *     method_proxy::call(), capturing the converted C++ value_t into atomics.
 *
 * Each primitive has an INSTANCE returner and a STATIC returner so the native
 * module exercises BOTH JNI dispatch slot sets (instance Call<T>MethodA and
 * static CallStatic<T>MethodA), as well as the call_stub fast path when the JDK
 * exposes StubRoutines::_call_stub_entry.
 *
 * Boundary values are deliberate: signed min/max for B/S/I/J, the full unsigned
 * range for C, and the IEEE-754 special values (NaN, +/-Inf, -0.0, MIN_VALUE,
 * MAX_VALUE) for F/D, so the C++ side proves sign-extension, zero-extension and
 * bit-cast fidelity rather than just "some number came back".
 *
 * Java 8 syntax only (no var / records / switch-expressions).
 */
public final class MethodPrimitives
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Incremented by every void returner so the native side can prove the
     *  void dispatch actually reached a real Java body (side effect). */
    public static volatile int voidInstanceHits;
    public static volatile int voidStaticHits;

    /** Records the argument the last (I)I echo method received, so the native
     *  side can prove arguments are passed through alongside the return. */
    public static volatile int lastEchoArg;

    /** Witness fields recording the argument each narrow-primitive echo method
     *  last received.  Stored at the DECLARED width so a native side-effect check
     *  proves the argument arrived at a real Java body unmangled (not just that
     *  the return value happened to match).  The mix* fields capture each slot of
     *  the heterogeneous (int,byte,char,short) frame so a per-slot read confirms
     *  every narrow arg landed in its own descriptor-typed slot. */
    public static volatile boolean lastBoolArg;
    public static volatile byte    lastByteArg;
    public static volatile short   lastShortArg;
    public static volatile char    lastCharArg;
    public static volatile int     mixIArg;
    public static volatile byte    mixBArg;
    public static volatile char    mixCArg;
    public static volatile short   mixSArg;

    // ----- the method the native module hooks to obtain a live thread -----

    /** Hookable instance method.  The native detour on this method calls every
     *  returner below via method_proxy::call(). */
    public int trigger(final int delta)
    {
        return delta + 1;
    }

    // ----------------------------------------------------------------------
    //  boolean (Z)
    // ----------------------------------------------------------------------
    public boolean retBoolTrue()         { return true; }
    public boolean retBoolFalse()        { return false; }
    public static boolean sRetBoolTrue() { return true; }
    public static boolean sRetBoolFalse(){ return false; }
    /** (Z)Z echo + (Z)Z logical NOT — proves a boolean ARGUMENT round-trips
     *  through the .z jvalue slot (0/1) and reaches a real body (witness). */
    public boolean echoBool(final boolean v)        { lastBoolArg = v; return v; }
    public static boolean sEchoBool(final boolean v){ lastBoolArg = v; return v; }
    public boolean notBool(final boolean v)         { return !v; }

    // ----------------------------------------------------------------------
    //  byte (B)   — signed 8-bit, range -128..127
    // ----------------------------------------------------------------------
    public byte retByteZero()        { return (byte) 0; }
    public byte retByteOne()         { return (byte) 1; }
    public byte retByteNegOne()      { return (byte) -1; }
    public byte retByteMax()         { return Byte.MAX_VALUE; }   // 127
    public byte retByteMin()         { return Byte.MIN_VALUE; }   // -128
    public static byte sRetByteNegOne() { return (byte) -1; }
    public static byte sRetByteMax()    { return Byte.MAX_VALUE; }
    public static byte sRetByteMin()    { return Byte.MIN_VALUE; }
    /** (B)B echo — proves a byte ARGUMENT round-trips through the .i jvalue
     *  slot with its sign intact (-1 stays -1, not 255). */
    public byte echoByte(final byte v)        { lastByteArg = v; return v; }
    public static byte sEchoByte(final byte v){ lastByteArg = v; return v; }
    /** (B)I — the callee widens the byte arg to int.  Proves the byte reached
     *  the body SIGN-extended (arg -1 -> int -1), isolating the sign path from
     *  the return-decode path tested by the (B)B echo above. */
    public int byteToInt(final byte v)        { return (int) v; }
    public static int sByteToInt(final byte v){ return (int) v; }

    // ----------------------------------------------------------------------
    //  short (S)  — signed 16-bit, range -32768..32767
    // ----------------------------------------------------------------------
    public short retShortZero()      { return (short) 0; }
    public short retShortNegOne()    { return (short) -1; }
    public short retShortMax()       { return Short.MAX_VALUE; }  // 32767
    public short retShortMin()       { return Short.MIN_VALUE; }  // -32768
    public static short sRetShortNegOne() { return (short) -1; }
    public static short sRetShortMax()    { return Short.MAX_VALUE; }
    public static short sRetShortMin()    { return Short.MIN_VALUE; }
    /** (S)S echo — proves a short ARGUMENT round-trips with sign intact. */
    public short echoShort(final short v)        { lastShortArg = v; return v; }
    public static short sEchoShort(final short v){ lastShortArg = v; return v; }
    /** (S)I — callee widens the short arg; proves it arrived SIGN-extended. */
    public int shortToInt(final short v)        { return (int) v; }
    public static int sShortToInt(final short v){ return (int) v; }

    // ----------------------------------------------------------------------
    //  char (C)   — UNSIGNED 16-bit, range 0..65535
    // ----------------------------------------------------------------------
    public char retCharZero()        { return (char) 0; }
    public char retCharA()           { return 'A'; }             // 65
    public char retCharMax()         { return (char) 0xFFFF; }   // 65535
    public static char sRetCharA()      { return 'A'; }
    public static char sRetCharMax()    { return (char) 0xFFFF; }
    /** (C)C echo — proves a char ARGUMENT round-trips across the full unsigned
     *  16-bit range (0xFFFF stays 0xFFFF). */
    public char echoChar(final char v)        { lastCharArg = v; return v; }
    public static char sEchoChar(final char v){ lastCharArg = v; return v; }
    /** (C)I — callee widens the char arg; proves it arrived ZERO-extended
     *  (arg 0xFFFF -> int 65535, never -1). */
    public int charToInt(final char v)        { return (int) v; }
    public static int sCharToInt(final char v){ return (int) v; }

    // ----------------------------------------------------------------------
    //  int (I)    — signed 32-bit
    // ----------------------------------------------------------------------
    public int retIntZero()          { return 0; }
    public int retIntNegOne()        { return -1; }
    public int retIntMax()           { return Integer.MAX_VALUE; }   // 2147483647
    public int retIntMin()           { return Integer.MIN_VALUE; }   // -2147483648
    public int retIntFortyTwo()      { return 42; }
    public static int sRetIntMax()      { return Integer.MAX_VALUE; }
    public static int sRetIntMin()      { return Integer.MIN_VALUE; }
    public static int sRetIntFortyTwo() { return 42; }
    /** (I)I echo — proves argument passthrough together with the return. */
    public int echoInt(final int v)  { lastEchoArg = v; return v; }
    public static int sEchoInt(final int v) { lastEchoArg = v; return v; }
    /** (II)I addition — exercises TWO int args AND int two's-complement overflow
     *  wrap (Integer.MAX_VALUE + 1 == Integer.MIN_VALUE). */
    public int addInt(final int a, final int b)        { return a + b; }
    public static int sAddInt(final int a, final int b){ return a + b; }
    /** (III)I — three int args; proves narrow args pack in declaration order
     *  (an a/c swap or a dropped middle arg changes the asymmetric result). */
    public int sumThreeInts(final int a, final int b, final int c)        { return (a * 100) + (b * 10) + c; }
    public static int sSumThreeInts(final int a, final int b, final int c){ return (a * 100) + (b * 10) + c; }
    /** (IBCS)I — heterogeneous narrow args in ONE frame.  Each arg is captured
     *  to its own witness field so the native side can confirm every narrow
     *  primitive landed in its own descriptor-typed slot (int, byte, char,
     *  short) rather than colliding.  The returned combination is asymmetric so
     *  any slot swap is also caught by the return value alone. */
    public int mixIBCS(final int i, final byte b, final char c, final short s)
    {
        mixIArg = i;
        mixBArg = b;
        mixCArg = c;
        mixSArg = s;
        return (i * 1000003) + (b * 7) + (c * 13) + s;
    }
    public static int sMixIBCS(final int i, final byte b, final char c, final short s)
    {
        mixIArg = i;
        mixBArg = b;
        mixCArg = c;
        mixSArg = s;
        return (i * 1000003) + (b * 7) + (c * 13) + s;
    }

    // ----------------------------------------------------------------------
    //  long (J)   — signed 64-bit (occupies TWO interpreter local slots)
    // ----------------------------------------------------------------------
    public long retLongZero()        { return 0L; }
    public long retLongNegOne()      { return -1L; }
    public long retLongMax()         { return Long.MAX_VALUE; }  // 9223372036854775807
    public long retLongMin()         { return Long.MIN_VALUE; }  // -9223372036854775808
    public long retLongBig()         { return 0x0123456789ABCDEFL; }
    public static long sRetLongMax()    { return Long.MAX_VALUE; }
    public static long sRetLongMin()    { return Long.MIN_VALUE; }
    public static long sRetLongBig()    { return 0x0123456789ABCDEFL; }

    // ----------------------------------------------------------------------
    //  float (F)
    // ----------------------------------------------------------------------
    public float retFloatZero()      { return 0.0f; }
    public float retFloatOne()       { return 1.0f; }
    public float retFloatNegOne()    { return -1.0f; }
    public float retFloatHalf()      { return 0.5f; }
    public float retFloatMax()       { return Float.MAX_VALUE; }
    public float retFloatMinValue()  { return Float.MIN_VALUE; }   // smallest positive subnormal
    public float retFloatNegZero()   { return -0.0f; }
    public float retFloatNaN()       { return Float.NaN; }
    public float retFloatPosInf()    { return Float.POSITIVE_INFINITY; }
    public float retFloatNegInf()    { return Float.NEGATIVE_INFINITY; }
    public static float sRetFloatHalf()   { return 0.5f; }
    public static float sRetFloatNaN()    { return Float.NaN; }
    public static float sRetFloatPosInf() { return Float.POSITIVE_INFINITY; }
    public static float sRetFloatNegZero(){ return -0.0f; }

    // ----------------------------------------------------------------------
    //  double (D)
    // ----------------------------------------------------------------------
    public double retDoubleZero()    { return 0.0; }
    public double retDoubleOne()     { return 1.0; }
    public double retDoubleNegOne()  { return -1.0; }
    public double retDoublePi()      { return Math.PI; }
    public double retDoubleMax()     { return Double.MAX_VALUE; }
    public double retDoubleMinValue(){ return Double.MIN_VALUE; }  // smallest positive subnormal
    public double retDoubleNegZero() { return -0.0; }
    public double retDoubleNaN()     { return Double.NaN; }
    public double retDoublePosInf()  { return Double.POSITIVE_INFINITY; }
    public double retDoubleNegInf()  { return Double.NEGATIVE_INFINITY; }
    public static double sRetDoublePi()     { return Math.PI; }
    public static double sRetDoubleNaN()    { return Double.NaN; }
    public static double sRetDoubleNegInf() { return Double.NEGATIVE_INFINITY; }
    public static double sRetDoubleNegZero(){ return -0.0; }

    // ----------------------------------------------------------------------
    //  void (V)
    // ----------------------------------------------------------------------
    public void retVoidBump()        { voidInstanceHits++; }
    public static void sRetVoidBump(){ voidStaticHits++; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodPrimitives.go && !MethodPrimitives.done;
            }

            @Override
            public void run()
            {
                final MethodPrimitives instance = new MethodPrimitives();
                // Driving trigger() through normal bytecode dispatch fires the
                // native interpreter hook; that detour performs every
                // method_proxy::call() the test asserts on.
                instance.trigger(7);
                MethodPrimitives.done = true;
            }
        });
    }
}
