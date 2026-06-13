package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the array_element_helpers feature (area: arrays / raw primitives).
 *
 * The feature under test is the trio of LOWEST-LEVEL raw array primitives in
 * vmhook.hpp that read/write a HotSpot primitive-array body with NO running JVM
 * call, by pure pointer arithmetic over the array layout (x64, compressed OOPs):
 *
 *     vmhook::array_length(void* array_oop)                 -> _length at +12
 *     vmhook::get_array_element&lt;T&gt;(void* array_oop, int idx)  -> +16 + idx*sizeof(T)
 *     vmhook::set_array_element&lt;T&gt;(void* array_oop, int idx, T) -> write at that addr
 *
 * The native module resolves each field below to its REAL array oop (via
 * vmhook::field_oop) and drives the three helpers directly against live HotSpot
 * heap memory.  The standalone no-JVM test (tests/test_array_element_helpers.cpp)
 * already pins the arithmetic on synthetic buffers; this fixture's job is to
 * prove the SAME helpers behave on genuine JVM arrays across JDK 8..26 and every
 * toolchain, and -- the headline safety surface -- that an out-of-bounds index
 * (== length, length+1, huge, -1, INT_MIN) reads a sentinel / no-ops WITHOUT
 * faulting on real adjacent heap.  On a synthetic buffer an OOB regression might
 * read slack bytes; on a live array it would read/write a neighbouring heap
 * object and crash, so this is where the bounds guard earns its keep.
 *
 * Coverage shapes (every primitive kind [Z [B [S [C [I [J [F [D + reference):
 *   - array_length read at lengths 0 / 1 / 2 / 1000,
 *   - canonical 3-element arrays with distinct recognisable values for
 *     get_array_element at index 0 / middle / last (right width per type),
 *   - boundary-value arrays (MIN / 0|-1 / MAX, plus NaN/Inf/-0.0/subnormal,
 *     true/false, and an astral-plane char encoded as a surrogate pair),
 *   - a reference Object[] / String[] with interleaved nulls,
 *   - a jagged int[][] for multidim outer-dimension element access,
 *   - a non-array object field (so a non-array oop fed to the helpers is shown
 *     to be handled gracefully),
 *   - null array references on two element types,
 *   - DEDICATED mutable scratch arrays (one per primitive + one Object[]) that
 *     the native side writes via set_array_element; a second probe action
 *     (verifyScratch) then re-reads them THROUGH Java and publishes a bitmask,
 *     proving each C++ write actually landed in the JVM heap and is visible to
 *     Java (the unique capability a live-JVM module has over the no-JVM test).
 *
 * Pure-ASCII source on purpose (no box-drawing glyphs; high/astral code units
 * written as (char) integer casts / explicit surrogate pairs, NOT \\uXXXX
 * literals) so it compiles under javac 8..26 with any -encoding and survives
 * source normalisation.  Java 8 syntax only: no var, no records, no
 * switch-expressions, no text blocks.
 */
public final class ArrayElementHelpers
{
    /** Native raises this to request a probe action; the action clears it. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect: probeChecksum proves the bytecode dispatch ran. */
    public static volatile long probeChecksum;

    /** Self-reference so the native side can wrap an instance for instance-field
     *  reads (read as a unique_ptr&lt;wrapper&gt;). */
    public static ArrayElementHelpers instance = new ArrayElementHelpers();

    // --- array_length shapes: lengths 0 / 1 / 2 / 1000 -----------------------
    public static final int BIG_LEN = 1000;
    public static int[]  len0Array    = new int[0];
    public static int[]  len1Array    = { 7 };
    public static int[]  len2Array    = { 11, 22 };
    public static int[]  len1000Array = new int[BIG_LEN];   // filled in <clinit>

    // --- Canonical 3-element arrays, one per primitive type -------------------
    // Distinct, ascending, trivially-recognisable values per element.
    public static boolean[] boolArray   = { true, false, true };
    public static byte[]    byteArray   = { (byte) 10, (byte) 20, (byte) 30 };
    public static short[]   shortArray  = { (short) 1000, (short) 2000, (short) 3000 };
    public static char[]    charArray   = { 'A', 'M', 'Z' };
    public static int[]     intArray    = { 100000, 200000, 300000 };
    public static long[]    longArray   = { 10000000000L, 20000000000L, 30000000000L };
    public static float[]   floatArray  = { 1.5f, 2.5f, 3.5f };
    public static double[]  doubleArray = { 1.25, 2.25, 3.25 };

    // --- Boundary-value arrays: MIN / (0 or -1) / MAX, plus specials ----------
    // Each 3-element array holds the type extremes so a sign / width / endianness
    // bug in get_array_element&lt;T&gt; on a REAL oop is caught.
    public static byte[]   boundaryByte   = { Byte.MIN_VALUE, (byte) -1, Byte.MAX_VALUE };
    public static short[]  boundaryShort  = { Short.MIN_VALUE, (short) -1, Short.MAX_VALUE };
    public static int[]    boundaryInt    = { Integer.MIN_VALUE, -1, Integer.MAX_VALUE };
    public static long[]   boundaryLong   = { Long.MIN_VALUE, -1L, Long.MAX_VALUE };
    public static char[]   boundaryChar   = { Character.MIN_VALUE, (char) 0x8000, Character.MAX_VALUE };

    // Float/double specials: NaN, +Inf, -Inf, -0.0, subnormal, MAX.
    public static float[]  specialFloat   =
        { Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY,
          -0.0f, Float.MIN_VALUE, Float.MAX_VALUE };
    public static double[] specialDouble  =
        { Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY,
          -0.0, Double.MIN_VALUE, Double.MAX_VALUE };

    // boolean true/false pattern (also length-2 boolean for the smallest stride).
    public static boolean[] boolPair      = { false, true };

    // An ASTRAL-plane code point (U+1F600) lives OUTSIDE the BMP, so Java stores
    // it as a UTF-16 SURROGATE PAIR: high 0xD83D, low 0xDE00.  As a char[] each
    // surrogate is a normal 16-bit code unit, so the raw uint16 helper reads each
    // half independently; this pins that the 2-byte stride reads surrogate halves
    // correctly (the astral-char angle from the task spec).
    public static char[]    astralChar    = { (char) 0xD83D, (char) 0xDE00 };

    // --- Reference arrays (compressed-OOP element stride) ---------------------
    // String[] with interleaved nulls: the native side decodes each slot as a
    // 4-byte compressed oop via get_array_element&lt;uint32&gt; and checks null-ness.
    public static String[]  refStrings    = { "alpha", null, "gamma" };
    // Object[] holding the same Element instances the wrapper can re-wrap.
    public static Object[]  refObjects    = new Object[3];   // filled in <clinit>
    public static Object[]  refNullsOnly  = new Object[] { null, null };

    // --- Jagged / multidim int[][] for outer-dimension element access ---------
    // Outer array has 3 rows of different lengths; row 1 is null.  The native
    // side reads the OUTER array's elements (each a compressed oop -> inner row
    // oop or null), then reads the length of each non-null inner row.
    public static int[][]   jagged        =
        { { 1, 2, 3 }, null, { 9 } };

    // --- A NON-array object field --------------------------------------------
    // Feeding this object's oop to array_length / get_array_element must be
    // graceful (no fault): array_length reads whatever int sits at +12 of an
    // ordinary object (its layout differs, so the value is arbitrary) and the
    // element helpers bounds-check against it.  The point is "no crash", not a
    // meaningful value -- the native side asserts only the no-fault contract.
    public static Object    notAnArray    = new Element(0xBEEF);

    // --- Null array REFERENCES (field holds null, not an array) ---------------
    public static int[]     nullIntArray  = null;
    public        long[]    instNullLong  = null;

    // --- Instance canonical arrays (instance-offset field path) ---------------
    public int[]    instIntArray   = { 4000, 5000, 6000 };
    public double[] instDoubleArray = { 4.25, 5.25, 6.25 };

    // =========================================================================
    //  DEDICATED MUTABLE SCRATCH ARRAYS.
    //  The native side WRITES these via set_array_element, then triggers the
    //  verifyScratch probe action, which re-reads them THROUGH Java and records
    //  a bitmask (scratchVerifyMask) of which scratch arrays now hold the exact
    //  expected post-write values.  This proves each C++ raw write reached the
    //  live JVM heap and is observable by Java -- impossible to show in the
    //  no-JVM standalone test.  Each scratch array starts zero/false so a
    //  "write didn't land" failure is unambiguous.
    // =========================================================================
    public static boolean[] scratchBool   = { false, false, false };
    public static byte[]    scratchByte   = { 0, 0, 0 };
    public static short[]   scratchShort  = { 0, 0, 0 };
    public static char[]    scratchChar   = { 0, 0, 0 };
    public static int[]     scratchInt    = { 0, 0, 0 };
    public static long[]    scratchLong   = { 0L, 0L, 0L };
    public static float[]   scratchFloat  = { 0f, 0f, 0f };
    public static double[]  scratchDouble = { 0.0, 0.0, 0.0 };

    /** Bitmask published by verifyScratch: bit i set => scratch array i holds
     *  exactly its expected post-write values (read back through Java). */
    public static volatile int scratchVerifyMask;

    /** Set true to ask the verifyScratch action to run (paired with go). */
    public static volatile boolean verifyScratchRequested;
    /** verifyScratch sets this once it has re-read the scratch arrays. */
    public static volatile boolean verifyScratchDone;

    // The exact post-write values the native side deposits into each scratch
    // array (index 0, 1, 2).  Kept here so BOTH sides agree on the contract.
    public static final boolean[] EXPECT_BOOL   = { true, false, true };
    public static final byte[]    EXPECT_BYTE   = { (byte) -128, (byte) 7, (byte) 127 };
    public static final short[]   EXPECT_SHORT  = { Short.MIN_VALUE, (short) 9, Short.MAX_VALUE };
    public static final char[]    EXPECT_CHAR   = { (char) 0x0041, (char) 0x4E2D, (char) 0xFFFF };
    public static final int[]     EXPECT_INT    = { Integer.MIN_VALUE, 1234567, Integer.MAX_VALUE };
    public static final long[]    EXPECT_LONG   = { Long.MIN_VALUE, 9876543210L, Long.MAX_VALUE };
    public static final float[]   EXPECT_FLOAT  = { -3.5f, 0.5f, 1234.5f };
    public static final double[]  EXPECT_DOUBLE = { -2.5, 0.25, 9.875 };

    /** A small object the native side can re-wrap from a reference-array slot. */
    public static final class Element
    {
        public final int tag;
        public Element(final int t) { this.tag = t; }
        public int getTag() { return this.tag; }
    }

    /**
     * Hookable instance method so the module can prove a real interpreter hook
     * fires on bytecode dispatch (mirrors Pilot.touch).  Returns intArray.length
     * + delta so the native side has a deterministic observed value.
     */
    public int touch(final int delta)
    {
        return ArrayElementHelpers.intArray.length + delta;
    }

    static
    {
        for (int i = 0; i < BIG_LEN; ++i)
        {
            len1000Array[i] = i * 3 + 1;       // deterministic; native recomputes
        }
        refObjects[0] = new Element(60);
        refObjects[1] = null;
        refObjects[2] = new Element(80);

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                // Two distinct actions share the go/done handshake: the primary
                // run (publishes probeChecksum) and the verifyScratch re-read.
                return ArrayElementHelpers.go
                    && !ArrayElementHelpers.done;
            }

            @Override
            public void run()
            {
                if (ArrayElementHelpers.verifyScratchRequested
                    && !ArrayElementHelpers.verifyScratchDone)
                {
                    ArrayElementHelpers.scratchVerifyMask = computeScratchMask();
                    ArrayElementHelpers.verifyScratchDone = true;
                }
                else
                {
                    // Touch one element of representative arrays through real
                    // bytecode dispatch so the run is observable.
                    long sum = 0L;
                    sum += boolArray[0] ? 1L : 0L;
                    sum += byteArray[0];
                    sum += shortArray[0];
                    sum += charArray[0];
                    sum += intArray[0];
                    sum += longArray[0];
                    sum += (long) floatArray[0];
                    sum += (long) doubleArray[0];
                    sum += len1000Array[BIG_LEN - 1];
                    sum += instance.touch(0);   // hookable dispatch
                    ArrayElementHelpers.probeChecksum = sum;
                }
                ArrayElementHelpers.done = true;
            }
        });
    }

    /** Re-read every scratch array through Java; bit i set iff array i matches
     *  its EXPECT_* contract exactly (bit-exact for float/double via raw bits). */
    private static int computeScratchMask()
    {
        int mask = 0;
        if (java.util.Arrays.equals(scratchBool,  EXPECT_BOOL))   { mask |= 1 << 0; }
        if (java.util.Arrays.equals(scratchByte,  EXPECT_BYTE))   { mask |= 1 << 1; }
        if (java.util.Arrays.equals(scratchShort, EXPECT_SHORT))  { mask |= 1 << 2; }
        if (java.util.Arrays.equals(scratchChar,  EXPECT_CHAR))   { mask |= 1 << 3; }
        if (java.util.Arrays.equals(scratchInt,   EXPECT_INT))    { mask |= 1 << 4; }
        if (java.util.Arrays.equals(scratchLong,  EXPECT_LONG))   { mask |= 1 << 5; }
        // float/double compared by raw bits so NaN / -0.0 are matched exactly.
        if (floatBitsEqual(scratchFloat, EXPECT_FLOAT))           { mask |= 1 << 6; }
        if (doubleBitsEqual(scratchDouble, EXPECT_DOUBLE))        { mask |= 1 << 7; }
        return mask;
    }

    private static boolean floatBitsEqual(final float[] a, final float[] b)
    {
        if (a == null || b == null || a.length != b.length) { return false; }
        for (int i = 0; i < a.length; ++i)
        {
            if (Float.floatToRawIntBits(a[i]) != Float.floatToRawIntBits(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    private static boolean doubleBitsEqual(final double[] a, final double[] b)
    {
        if (a == null || b == null || a.length != b.length) { return false; }
        for (int i = 0; i < a.length; ++i)
        {
            if (Double.doubleToRawLongBits(a[i]) != Double.doubleToRawLongBits(b[i]))
            {
                return false;
            }
        }
        return true;
    }
}
