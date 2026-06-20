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
    /** (byte)0x55 — a value with mixed-but-positive bits, so the decode landing
     *  in the wrong width (e.g. reading a stale neighbouring byte) changes it. */
    public byte retBytePattern()     { return (byte) 0x55; }            // 85
    /** (byte)0xAA == -86: the INVERSE alternating-bit pattern of 0x55, with bit 7
     *  (the sign bit) SET.  Together with retBytePattern (0x55, +85) it covers both
     *  alternating-bit phases at byte width and proves the sign bit decodes, instance
     *  + static.  A width error reading a stale neighbour byte would change it. */
    public byte retByteAlt()         { return (byte) 0xAA; }            // -86
    public static byte sRetByteAlt() { return (byte) 0xAA; }
    public static byte sRetBytePattern() { return (byte) 0x55; }
    public static byte sRetByteZero()    { return (byte) 0; }
    public static byte sRetByteOne()     { return (byte) 1; }
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
    /** (short)255 — bit 7 set, bit 15 CLEAR: stays POSITIVE 255.  Discriminates a
     *  correct 16-bit decode from one that narrows to int8 (which would read
     *  -1) — a value the byte-range boundaries above cannot expose. */
    public short retShort255()       { return (short) 0x00FF; }   // 255 (positive)
    /** (short)0xFF00 == -256: bit 15 set, low byte zero.  A decode that took only
     *  the low 8 bits would read 0; correct sign-extension reads -256. */
    public short retShortHighByte()  { return (short) 0xFF00; }   // -256
    /** (short)0x5555 == +21845: alternating-bit phase A, bit 15 CLEAR (positive). */
    public short retShortAltPos()    { return (short) 0x5555; }   // 21845
    /** (short)0xAAAA == -21846: alternating-bit phase B, bit 15 SET (negative).
     *  The pair (0x5555/0xAAAA) covers both phases at short width and proves bit 15
     *  sign-extends; reading only the low byte (0x55/0xAA) could not tell them
     *  apart from the byte-width values. */
    public short retShortAltNeg()    { return (short) 0xAAAA; }   // -21846
    public static short sRetShortNegOne() { return (short) -1; }
    public static short sRetShortMax()    { return Short.MAX_VALUE; }
    public static short sRetShortMin()    { return Short.MIN_VALUE; }
    public static short sRetShort255()    { return (short) 0x00FF; }
    public static short sRetShortZero()   { return (short) 0; }
    public static short sRetShortHighByte() { return (short) 0xFF00; }
    public static short sRetShortAltNeg() { return (short) 0xAAAA; }
    /** (S)S echo — proves a short ARGUMENT round-trips with sign intact. */
    public short echoShort(final short v)        { lastShortArg = v; return v; }
    public static short sEchoShort(final short v){ lastShortArg = v; return v; }
    /** (S)I — callee widens the short arg; proves it arrived SIGN-extended. */
    public int shortToInt(final short v)        { return (int) v; }
    public static int sShortToInt(final short v){ return (int) v; }
    /** (S)I used with arg 255 (0x00FF: bit 7 set, bit 15 clear) so the short arg
     *  is proven to arrive as POSITIVE 255 across the .i slot, discriminating a
     *  true 16-bit short pack from an int8 narrowing that would deliver -1.
     *  Deliberately does NOT write lastShortArg so it cannot disturb the
     *  "last short arg" witness the echoShort methods establish. */
    public int shortPosToInt(final short v)        { return (int) v; }
    public static int sShortPosToInt(final short v){ return (int) v; }

    // ----------------------------------------------------------------------
    //  char (C)   — UNSIGNED 16-bit, range 0..65535
    // ----------------------------------------------------------------------
    public char retCharZero()        { return (char) 0; }
    public char retCharA()           { return 'A'; }             // 65
    public char retCharMax()         { return (char) 0xFFFF; }   // 65535
    /** (char)0x00FF == 255: bit 7 set, bit 15 clear — discriminates the 16-bit
     *  ZERO-extend decode from an int8 narrowing (which would read -1). */
    public char retChar255()         { return (char) 0x00FF; }   // 255
    /** (char)0x8000 == 32768: ONLY bit 15 set.  The cleanest witness that a char
     *  return is ZERO-extended, not sign-extended — retCharMax (0xFFFF, all bits
     *  set) cannot distinguish "zero-extend" from several other failure modes,
     *  but 0x8000 read as a signed 16-bit value would be -32768. */
    public char retCharHighBit()     { return (char) 0x8000; }   // 32768
    /** (char)0xD83D == 55357: a lone UTF-16 HIGH-SURROGATE code unit.  Bit 15 is
     *  set (so it co-witnesses zero-extension) AND it carries a busy low byte, so
     *  it doubles as a non-byte-symmetric char-return pattern that a 16-bit decode
     *  reading a stale neighbouring byte would corrupt.  A lone surrogate is a
     *  perfectly legal Java `char`; it must round-trip as the unsigned value 55357,
     *  never -10243 (its signed-16 reading) nor 0x3D (a low-byte-only read). */
    public char retCharSurrogate()   { return (char) 0xD83D; }   // 55357
    /** (char)0x5555 == 21845 and (char)0xAAAA == 43690: the two alternating-bit
     *  phases at char width.  0xAAAA has bit 15 set, so it doubles as a second
     *  zero-extension witness (43690, never -21846) with a busy low byte. */
    public char retCharAltLo()       { return (char) 0x5555; }   // 21845
    public char retCharAltHi()       { return (char) 0xAAAA; }   // 43690
    public static char sRetCharA()      { return 'A'; }
    public static char sRetCharMax()    { return (char) 0xFFFF; }
    public static char sRetCharHighBit(){ return (char) 0x8000; }
    public static char sRetCharSurrogate(){ return (char) 0xD83D; }
    public static char sRetCharZero()   { return (char) 0; }
    public static char sRetCharAltHi()  { return (char) 0xAAAA; }
    public static char sRetChar255()    { return (char) 0x00FF; }
    /** (C)C echo — proves a char ARGUMENT round-trips across the full unsigned
     *  16-bit range (0xFFFF stays 0xFFFF). */
    public char echoChar(final char v)        { lastCharArg = v; return v; }
    public static char sEchoChar(final char v){ lastCharArg = v; return v; }
    /** (C)I — callee widens the char arg; proves it arrived ZERO-extended
     *  (arg 0xFFFF -> int 65535, never -1). */
    public int charToInt(final char v)        { return (int) v; }
    public static int sCharToInt(final char v){ return (int) v; }
    /** (C)I used with arg 0x8000 so a CLEAN bit-15 char arg (only the high bit
     *  set) is proven to arrive zero-extended (32768), not sign-extended (-32768).
     *  echoChar/charToInt with 0xFFFF cannot isolate bit 15 because every bit is
     *  set.  Deliberately does NOT write lastCharArg so it cannot disturb the
     *  "last char arg" witness the echoChar methods establish. */
    public int charHighBitToInt(final char v)        { return (int) v; }
    public static int sCharHighBitToInt(final char v){ return (int) v; }
    /** (C)I used with the lone surrogate 0xD83D so a char arg whose value lies in
     *  the surrogate range (bit 15 set + a busy low byte) is proven to arrive
     *  ZERO-extended (55357), never sign-extended (-10243).  Deliberately does NOT
     *  write lastCharArg so it cannot disturb the "last char arg" witness. */
    public int charSurrogateToInt(final char v)        { return (int) v; }
    public static int sCharSurrogateToInt(final char v){ return (int) v; }

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
    /** 0x12345678 — four DISTINCT non-zero bytes, so a byte-order error in the
     *  4-byte return decode reorders them into a different value.  The other int
     *  returns (0/-1/MAX/MIN/42) are all byte-symmetric or near-zero and cannot
     *  expose such a swap. */
    public int retIntPattern()       { return 0x12345678; }      // 305419896
    public static int sRetIntPattern() { return 0x12345678; }
    /** 0x55555555 == +1431655765 (bit 31 CLEAR) and 0xAAAAAAAA == -1431655766
     *  (bit 31 SET): the two alternating-bit phases at int width.  The 0xAA phase
     *  also pins the sign bit through the 4-byte int return decode; neither value
     *  is byte-symmetric, so a byte/word swap reorders it. */
    public int retIntAltPos()        { return 0x55555555; }      // 1431655765
    public int retIntAltNeg()        { return 0xAAAAAAAA; }      // -1431655766
    public static int sRetIntZero()    { return 0; }
    public static int sRetIntNegOne()  { return -1; }
    public static int sRetIntAltNeg()  { return 0xAAAAAAAA; }
    /** (I)F — the callee widens the int arg to float (exact for these magnitudes),
     *  letting the native side feed an int and read back a float RETURN whose value
     *  it can compare exactly: 16777216 == 2^24 (largest int exactly representable
     *  as a float, used as an arg) and the constant returners below. */
    public float intToFloat(final int v)        { return (float) v; }
    public static float sIntToFloat(final int v){ return (float) v; }
    /** (I)I echo — proves argument passthrough together with the return. */
    public int echoInt(final int v)  { lastEchoArg = v; return v; }
    public static int sEchoInt(final int v) { lastEchoArg = v; return v; }
    /** (I)I echo dedicated to the distinct-byte pattern 0x12345678 — does NOT
     *  write lastEchoArg, so it cannot disturb the "last echo arg" witness the
     *  ordinary echoInt establishes.  A byte-order error on the int ARG path
     *  (C++ -> .i slot -> body) would echo back a reordered value. */
    public int echoIntPattern(final int v)        { return v; }
    public static int sEchoIntPattern(final int v){ return v; }
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
    /** (ZBCSI)I — FIVE narrow args, one of EVERY narrow kind in a single frame
     *  (boolean, byte, char, short, int), each landing in its own one-slot local.
     *  Extends the arg-count axis past the 4-arg (IBCS)I above and proves a
     *  pure-narrow five-slot frame packs without cross-slot bleed: the boolean
     *  occupies its own slot ahead of the others (a common off-by-one when the .z
     *  slot is mis-sized) and the result mixes all five with asymmetric weights so
     *  any slot swap, drop, or width error changes it.  Java promotes boolean->int
     *  as 1/0, byte/short sign-extend, char zero-extends. */
    public int mixZBCSI(final boolean z, final byte b, final char c, final short s, final int i)
    {
        return ((z ? 1 : 0) * 5000011) + (b * 70001) + (c * 900007) + (s * 11) + i;
    }
    public static int sMixZBCSI(final boolean z, final byte b, final char c, final short s, final int i)
    {
        return ((z ? 1 : 0) * 5000011) + (b * 70001) + (c * 900007) + (s * 11) + i;
    }

    // ----------------------------------------------------------------------
    //  long (J)   — signed 64-bit (occupies TWO interpreter local slots)
    // ----------------------------------------------------------------------
    public long retLongZero()        { return 0L; }
    public long retLongNegOne()      { return -1L; }
    public long retLongMax()         { return Long.MAX_VALUE; }  // 9223372036854775807
    public long retLongMin()         { return Long.MIN_VALUE; }  // -9223372036854775808
    public long retLongBig()         { return 0x0123456789ABCDEFL; }
    /** High 32 bits set, low 32 bits zero.  A return decode that drops the high
     *  word (a 32-bit truncation) reads 0 instead of this large negative value. */
    public long retLongHighHalf()    { return 0xFFFFFFFF00000000L; } // -4294967296
    /** Low 32 bits set, high 32 bits zero == 4294967295 (POSITIVE).  A decode that
     *  sign-extends a 32-bit read of the low word would read -1; the correct
     *  64-bit decode keeps it positive.  Complements retLongHighHalf so a high/low
     *  word swap in the return path is caught from both directions. */
    public long retLongLowHalf()     { return 0x00000000FFFFFFFFL; } // 4294967295
    /** 0x5555555555555555 == +6148914691236517205 (bit 63 CLEAR) and
     *  0xAAAAAAAAAAAAAAAA == -6148914691236517206 (bit 63 SET): the two
     *  alternating-bit phases at 64-bit width.  Both halves of each value are busy
     *  and the two phases are bitwise inverses, so a high/low word swap or a 32-bit
     *  truncation in the 8-byte return decode changes them. */
    public long retLongAltPos()      { return 0x5555555555555555L; } // 6148914691236517205
    public long retLongAltNeg()      { return 0xAAAAAAAAAAAAAAAAL; } // -6148914691236517206
    public static long sRetLongMax()    { return Long.MAX_VALUE; }
    public static long sRetLongMin()    { return Long.MIN_VALUE; }
    public static long sRetLongBig()    { return 0x0123456789ABCDEFL; }
    public static long sRetLongHighHalf() { return 0xFFFFFFFF00000000L; }
    public static long sRetLongLowHalf()  { return 0x00000000FFFFFFFFL; }
    public static long sRetLongZero()     { return 0L; }
    public static long sRetLongNegOne()   { return -1L; }
    public static long sRetLongAltNeg()   { return 0xAAAAAAAAAAAAAAAAL; }

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
    /** A finite negative with a non-trivial mantissa, to exercise an ordinary
     *  (non-special) negative value through the 4-byte float return decode. */
    public float retFloatNegFiften(){ return -15.5f; }
    /** intBitsToFloat(0x12345678): a finite float whose 4-byte IEEE-754 pattern
     *  has FOUR distinct non-zero bytes.  Every other float return here is
     *  byte-sparse (0.5f == 0x3F000000 has three zero bytes; MAX/-0.0/Inf are
     *  similarly sparse), so a byte-order error in the 4-byte decode would slip
     *  past them — this value reorders into a different float if the words swap. */
    public float retFloatBusyBits(){ return Float.intBitsToFloat(0x12345678); }
    /** 2.0f — the simplest exact power-of-two above one, distinct from 0.5f/1.0f. */
    public float retFloatTwo()       { return 2.0f; }
    /** Float.MIN_NORMAL — the smallest POSITIVE NORMAL float (0x00800000).  Distinct
     *  from MIN_VALUE (the smallest subnormal, 0x00000001); together they bracket
     *  the subnormal/normal boundary in the float return decode. */
    public float retFloatMinNormal() { return Float.MIN_NORMAL; }
    /** intBitsToFloat(0x55555555) and (0xAAAAAAAA): two finite floats whose 4-byte
     *  patterns are alternating-bit inverses.  0xAAAAAAAA has bit 31 (the sign)
     *  set, so it is a finite NEGATIVE float; comparing RAW bits catches a byte
     *  swap the byte-sparse specials cannot.  (Both are ordinary finite normals,
     *  so std::isnan/std::isinf are false — checked via raw bits, not value.) */
    public float retFloatAltLo()     { return Float.intBitsToFloat(0x55555555); }
    public float retFloatAltHi()     { return Float.intBitsToFloat(0xAAAAAAAA); }
    /** 2.75f and -2.75f — finite values with a fractional part, exactly
     *  representable, used to drive the value_t conversion operator's float->int
     *  static_cast (truncation toward zero: 2.75f -> 2, -2.75f -> -2).  Both are
     *  well inside int range so the narrowing cast is well-defined (no UB). */
    public float retFloatTwoPoint75()    { return 2.75f; }
    public float retFloatNegTwoPoint75() { return -2.75f; }
    public static float sRetFloatHalf()   { return 0.5f; }
    public static float sRetFloatNaN()    { return Float.NaN; }
    public static float sRetFloatPosInf() { return Float.POSITIVE_INFINITY; }
    public static float sRetFloatNegZero(){ return -0.0f; }
    public static float sRetFloatBusyBits(){ return Float.intBitsToFloat(0x12345678); }
    public static float sRetFloatTwo()       { return 2.0f; }
    public static float sRetFloatMinNormal() { return Float.MIN_NORMAL; }
    public static float sRetFloatAltHi()     { return Float.intBitsToFloat(0xAAAAAAAA); }
    /** Static float boundary mirrors of the instance set, so the dedicated
     *  CallStaticFloatMethodA dispatch slot (JNI 137) is exercised across the full
     *  magnitude range — finite one, largest-finite MAX, smallest-positive
     *  subnormal MIN_VALUE, and -Inf — not just the NaN/Inf/-0.0 specials above. */
    public static float sRetFloatOne()      { return 1.0f; }
    public static float sRetFloatMax()      { return Float.MAX_VALUE; }
    public static float sRetFloatMinValue() { return Float.MIN_VALUE; }   // smallest positive subnormal
    public static float sRetFloatNegInf()   { return Float.NEGATIVE_INFINITY; }

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
    /** An ordinary finite negative (mirrors retFloatNegFiften for the 8-byte path). */
    public double retDoubleNegFifteen(){ return -15.0; }
    /** longBitsToDouble(0x123456789ABCDEF0): a finite double whose 8-byte IEEE-754
     *  pattern has its HIGH and LOW 32-bit words BOTH busy and distinct, so a
     *  high/low word swap in the 8-byte return decode produces a different double.
     *  Math.PI already has busy words, but this one's halves are deliberately
     *  asymmetric (no shared nibble run) for a sharper word-swap witness. */
    public double retDoubleBusyBits(){ return Double.longBitsToDouble(0x123456789ABCDEF0L); }
    /** 2.0 — simplest exact power-of-two above one. */
    public double retDoubleTwo()     { return 2.0; }
    /** Double.MIN_NORMAL — smallest POSITIVE NORMAL double (0x0010000000000000),
     *  distinct from MIN_VALUE (smallest subnormal); brackets the subnormal/normal
     *  boundary in the 8-byte return decode. */
    public double retDoubleMinNormal(){ return Double.MIN_NORMAL; }
    /** longBitsToDouble(0x5555555555555555) and (0xAAAA...): two finite doubles
     *  whose 8-byte patterns are alternating-bit inverses.  The 0xAA phase has bit
     *  63 (sign) set (finite negative); RAW-bit comparison catches a high/low word
     *  swap the byte-sparse specials cannot.  Both are ordinary finite normals. */
    public double retDoubleAltLo()   { return Double.longBitsToDouble(0x5555555555555555L); }
    public double retDoubleAltHi()   { return Double.longBitsToDouble(0xAAAAAAAAAAAAAAAAL); }
    /** 2.75 and -2.75 — finite, exactly representable, drive the value_t
     *  conversion operator's double->int64 static_cast (truncation toward zero:
     *  2.75 -> 2, -2.75 -> -2).  Well inside int64 range (no UB). */
    public double retDoubleTwoPoint75()    { return 2.75; }
    public double retDoubleNegTwoPoint75() { return -2.75; }
    public static double sRetDoublePi()     { return Math.PI; }
    public static double sRetDoubleNaN()    { return Double.NaN; }
    public static double sRetDoubleNegInf() { return Double.NEGATIVE_INFINITY; }
    public static double sRetDoubleNegZero(){ return -0.0; }
    public static double sRetDoubleBusyBits(){ return Double.longBitsToDouble(0x123456789ABCDEF0L); }
    public static double sRetDoubleTwo()       { return 2.0; }
    public static double sRetDoubleMinNormal() { return Double.MIN_NORMAL; }
    public static double sRetDoubleAltHi()     { return Double.longBitsToDouble(0xAAAAAAAAAAAAAAAAL); }
    /** Static double boundary mirrors of the instance set, so the dedicated
     *  CallStaticDoubleMethodA dispatch slot (JNI 140) is exercised across the full
     *  magnitude range — finite one, largest-finite MAX, smallest-positive
     *  subnormal MIN_VALUE, and +Inf — complementing the NaN/-Inf/-0.0 specials. */
    public static double sRetDoubleOne()      { return 1.0; }
    public static double sRetDoubleMax()      { return Double.MAX_VALUE; }
    public static double sRetDoubleMinValue() { return Double.MIN_VALUE; }  // smallest positive subnormal
    public static double sRetDoublePosInf()   { return Double.POSITIVE_INFINITY; }

    // ----------------------------------------------------------------------
    //  cross-kind value_t conversion drivers (batch-18 deepening)
    //  Finite values with a non-trivial MAGNITUDE and a fractional part, so the
    //  value_t conversion operator's float/double -> integer static_cast leg is
    //  exercised on more than the small +/-2.75 used elsewhere: a six-digit whole
    //  part proves the truncation keeps the full integer magnitude (not just the
    //  ones digit) and the .5 fraction proves it truncates toward zero, not rounds.
    // ----------------------------------------------------------------------
    public float  retFloatBigWhole()       { return 1000000.5f; }   // -> int 1000000
    public float  retFloatNegBigWhole()    { return -1000000.5f; }  // -> int -1000000
    public double retDoubleBigWhole()      { return 1234567.5; }    // -> long 1234567
    public double retDoubleNegBigWhole()   { return -1234567.5; }   // -> long -1234567
    /** 2^40 (1099511627776): exactly representable as BOTH a long and a double, so
     *  a long RETURN read into a double target is a lossless cross-kind widen whose
     *  exact value the native side can compare (not just "nonzero"). */
    public long   retLongPow2to40()        { return 1099511627776L; }
    /** 2^24 (16777216): the largest int whose successor is NOT exactly a float, used
     *  as an int RETURN read into a float target (exact widen) — the return-side twin
     *  of the (I)F intToFloat ARG path. */
    public int    retIntPow2to24()         { return 16777216; }
    /** (I)J — the callee widens the int arg to long.  Proves an int ARG and a long
     *  RETURN coexist in one dispatch and that the int arrived SIGN-extended into the
     *  64-bit result (arg -1 -> long -1, never 4294967295). */
    public long   intToLong(final int v)        { return (long) v; }
    public static long sIntToLong(final int v)  { return (long) v; }

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
