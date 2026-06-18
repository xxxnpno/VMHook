package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_arrays_primitive feature (area: fields).
 *
 * Exercises reading Java primitive arrays
 *   [Z [B [S [C [I [J [F [D
 * out of object/static fields into std::vector&lt;T&gt; via the C++
 * get_field("a")-&gt;get() implicit-conversion path (the primitive-array read
 * path inside field_proxy::value_t::read_array_value / append_array_value).
 *
 * The native module reads every field declared here and asserts the size and
 * each element against the values frozen below.  Coverage angles per type:
 *   - a canonical 3-element array with distinct, easily-recognised values,
 *   - an empty array (length 0),
 *   - a single-element array,
 *   - a large array (256 elements) populated by a deterministic formula,
 *   - a boundary array holding the type's MIN / MAX / special values,
 *   - a null array reference (the field holds null, not an array).
 * Both the STATIC and the INSTANCE variants of every shape exist so the native
 * side covers the static-mirror read path and the instance-offset read path
 * independently (canonical / empty / single / boundary, plus a null reference).
 *
 * Shape mirrors the canonical Pilot fixture: a public-static-volatile go/done
 * handshake the native side drives through run_probe, a self-reference
 * 'instance' the native side wraps for instance-field reads, and a static-block
 * self-registration of a Harness.Probe.  The probe touches one element of every
 * array through a real Java bytecode dispatch (publishing a checksum) before it
 * raises 'done'; the native reads happen out-of-probe through direct field
 * access, which is the real-world usage pattern for field_proxy array reads.
 *
 * Pure-ASCII source on purpose (no box-drawing comment glyphs; high char code
 * units written as (char) integer casts, NOT \\uXXXX literals) so it compiles
 * under javac 8..25 with any -encoding and survives source-normalisation.
 * Java 8 syntax only: no var, no records, no switch-expressions.
 */
public final class FieldArraysPrimitive
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /** Self-reference so the native side can wrap an instance for the
     *  instance-field read path (read as a unique_ptr&lt;wrapper&gt;). */
    public static FieldArraysPrimitive instance = new FieldArraysPrimitive();

    /** Touched by the probe so the native side can confirm the bytecode
     *  dispatch actually ran (a non-zero checksum proves run() executed). */
    public static volatile long probeChecksum;

    // --- Canonical 3-element STATIC arrays -----------------------------------
    // Values chosen so each element is distinct and trivially recognisable on
    // the C++ side (ascending, well inside each type's range).
    public static boolean[] staticBoolArray   = { true, false, true };
    public static byte[]    staticByteArray   = { (byte) 1, (byte) 2, (byte) 3 };
    public static short[]   staticShortArray  = { (short) 100, (short) 200, (short) 300 };
    public static char[]    staticCharArray   = { 'A', 'B', 'C' };
    public static int[]     staticIntArray    = { 1000, 2000, 3000 };
    public static long[]    staticLongArray   = { 1000000000L, 2000000000L, 3000000000L };
    public static float[]   staticFloatArray  = { 1.5f, 2.5f, 3.5f };
    public static double[]  staticDoubleArray = { 1.25, 2.25, 3.25 };

    // --- Canonical 3-element INSTANCE arrays ---------------------------------
    public boolean[] instBoolArray   = { false, true, false };
    public byte[]    instByteArray   = { (byte) 4, (byte) 5, (byte) 6 };
    public short[]   instShortArray  = { (short) 400, (short) 500, (short) 600 };
    public char[]    instCharArray   = { 'X', 'Y', 'Z' };
    public int[]     instIntArray    = { 4000, 5000, 6000 };
    public long[]    instLongArray   = { 4000000000L, 5000000000L, 6000000000L };
    public float[]   instFloatArray  = { 4.5f, 5.5f, 6.5f };
    public double[]  instDoubleArray = { 4.25, 5.25, 6.25 };

    // --- Empty arrays (length 0) for every type ------------------------------
    public static boolean[] emptyBoolArray   = new boolean[0];
    public static byte[]    emptyByteArray   = new byte[0];
    public static short[]   emptyShortArray  = new short[0];
    public static char[]    emptyCharArray   = new char[0];
    public static int[]     emptyIntArray    = new int[0];
    public static long[]    emptyLongArray   = new long[0];
    public static float[]   emptyFloatArray  = new float[0];
    public static double[]  emptyDoubleArray = new double[0];

    // --- Empty arrays (length 0), INSTANCE variant ---------------------------
    public boolean[] instEmptyBoolArray   = new boolean[0];
    public byte[]    instEmptyByteArray   = new byte[0];
    public short[]   instEmptyShortArray  = new short[0];
    public char[]    instEmptyCharArray   = new char[0];
    public int[]     instEmptyIntArray    = new int[0];
    public long[]    instEmptyLongArray   = new long[0];
    public float[]   instEmptyFloatArray  = new float[0];
    public double[]  instEmptyDoubleArray = new double[0];

    // --- Single-element arrays for every type --------------------------------
    public static boolean[] singleBoolArray   = { true };
    public static byte[]    singleByteArray   = { (byte) 42 };
    public static short[]   singleShortArray  = { (short) 12345 };
    public static char[]    singleCharArray   = { 'Q' };
    public static int[]     singleIntArray    = { 1234567 };
    public static long[]    singleLongArray   = { 1234567890123L };
    public static float[]   singleFloatArray  = { 3.14159f };
    public static double[]  singleDoubleArray = { 2.718281828 };

    // --- Single-element arrays, INSTANCE variant -----------------------------
    public boolean[] instSingleBoolArray   = { false };
    public byte[]    instSingleByteArray   = { (byte) -7 };
    public short[]   instSingleShortArray  = { (short) -321 };
    public char[]    instSingleCharArray   = { 'q' };
    public int[]     instSingleIntArray    = { -7654321 };
    public long[]    instSingleLongArray   = { -9876543210987L };
    public float[]   instSingleFloatArray  = { -1.5f };
    public double[]  instSingleDoubleArray = { -0.0078125 };

    // --- Large arrays (256 elements) by a deterministic formula --------------
    // Filled in the static / instance initialisers below so the C++ side can
    // recompute the expected value at each index without a second data copy.
    public static final int LARGE_LEN = 256;
    public static boolean[] largeBoolArray   = new boolean[LARGE_LEN];
    public static byte[]    largeByteArray   = new byte[LARGE_LEN];
    public static short[]   largeShortArray  = new short[LARGE_LEN];
    public static char[]    largeCharArray   = new char[LARGE_LEN];
    public static int[]     largeIntArray    = new int[LARGE_LEN];
    public static long[]    largeLongArray   = new long[LARGE_LEN];
    public static float[]   largeFloatArray  = new float[LARGE_LEN];
    public static double[]  largeDoubleArray = new double[LARGE_LEN];

    // --- Boundary-value arrays (MIN / special / MAX per type) ----------------
    // boolean has no numeric boundary; we encode a {false,true,true} pattern.
    public static boolean[] boundaryBoolArray   = { false, true, true };
    public static byte[]    boundaryByteArray   = { Byte.MIN_VALUE, (byte) 0, Byte.MAX_VALUE };
    public static short[]   boundaryShortArray  = { Short.MIN_VALUE, (short) 0, Short.MAX_VALUE };
    public static char[]    boundaryCharArray   = { Character.MIN_VALUE, (char) 0x41, (char) 0x7F };
    public static int[]     boundaryIntArray    = { Integer.MIN_VALUE, 0, Integer.MAX_VALUE };
    public static long[]    boundaryLongArray   = { Long.MIN_VALUE, 0L, Long.MAX_VALUE };
    public static float[]   boundaryFloatArray  =
        { -Float.MAX_VALUE, 0.0f, Float.MAX_VALUE };
    public static double[]  boundaryDoubleArray =
        { -Double.MAX_VALUE, 0.0, Double.MAX_VALUE };

    // --- Boundary-value arrays, INSTANCE variant -----------------------------
    public boolean[] instBoundaryBoolArray   = { true, false, false };
    public byte[]    instBoundaryByteArray   = { Byte.MIN_VALUE, (byte) -1, Byte.MAX_VALUE };
    public short[]   instBoundaryShortArray  = { Short.MIN_VALUE, (short) -1, Short.MAX_VALUE };
    public char[]    instBoundaryCharArray   = { Character.MIN_VALUE, (char) 0x01, Character.MAX_VALUE };
    public int[]     instBoundaryIntArray    = { Integer.MIN_VALUE, -1, Integer.MAX_VALUE };
    public long[]    instBoundaryLongArray   = { Long.MIN_VALUE, -1L, Long.MAX_VALUE };
    public float[]   instBoundaryFloatArray  =
        { -Float.MAX_VALUE, Float.MIN_VALUE, Float.MAX_VALUE };
    public double[]  instBoundaryDoubleArray =
        { -Double.MAX_VALUE, Double.MIN_VALUE, Double.MAX_VALUE };

    // Extra float/double special-value arrays (NaN / +Inf / -Inf / subnormal).
    public static float[]   specialFloatArray  =
        { Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.MIN_VALUE };
    public static double[]  specialDoubleArray =
        { Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, Double.MIN_VALUE };

    // Null array REFERENCES (the field holds null, not an empty array).  Reading
    // these must decode_array_oop(0) -> nullptr -> empty vector, never a crash.
    // One static + one instance, two distinct element types, prove the
    // null-reference short-circuit on both read paths and that it is element-
    // type independent.
    public static int[]  nullIntArray = null;
    public long[]        instNullLongArray = null;

    // A long[] whose elements hold values that do NOT fit in 32 bits, so a
    // (buggy) narrow read into vector<int32_t> would be detectable as a 4-byte
    // stride over the 8-byte data.  Used by the width-mismatch documentation
    // check on the native side.  High word differs from low word per element.
    public static long[]    wideLongArray =
        { 0x1122334455667788L, 0x7FFFFFFF00000001L, -1L };

    // A char[] holding high (>0xFF) code units, written as (char) integer
    // casts so the source stays pure ASCII (compiles under any javac -encoding,
    // and survives source-normalisation that would otherwise un-escape a
    // literal).  Elements: 'a'(0x61), 0x00FF, 0x0100, 0x20AC (euro sign).
    // Lets the native side document the lossy char[] -> vector<char> narrowing
    // (each 16-bit code unit truncated to the low 8 bits).
    public static char[]    unicodeCharArray =
        { 'a', (char) 0x00FF, (char) 0x0100, (char) 0x20AC };

    // A char[] holding the printable-range boundary code units the JLS calls
    // out: ' ' (0x20, the first printable ASCII) and Character.MAX_VALUE
    // (0xFFFF, the largest char), with an ordinary letter in between.  ' '
    // and 'A' survive the vector<char> narrowing (both <= 0x7F); 0xFFFF
    // truncates to 0xFF, documenting the same lossy narrowing at the char
    // boundary.  The raw uint16 read recovers all three exactly.
    public static char[]    spaceCharArray = { ' ', (char) 0xFFFF, 'A' };

    // --- Large arrays (1024 elements) ----------------------------------------
    // The task asks for a 1000+ element shape on top of the 256 case above;
    // these stress reserve() + the per-element loop at a four-figure length.
    // Same deterministic-formula approach so the C++ side recomputes each
    // expected element by index without a second data copy.  Three element
    // widths (4 / 8 / 8 bytes) cover the int / long / double append paths.
    public static final int LARGE_K_LEN = 1024;
    public static int[]    largeKIntArray    = new int[LARGE_K_LEN];
    public static long[]   largeKLongArray   = new long[LARGE_K_LEN];
    public static double[] largeKDoubleArray = new double[LARGE_K_LEN];

    // --- Multi-dimensional primitive arrays ----------------------------------
    // A multi-dim primitive array field holds a reference to an OUTER object
    // array whose elements are themselves (compressed-OOP) references to inner
    // primitive arrays.  The native side decodes the outer reference, walks
    // each slot to the inner primitive array, and reads the inner elements --
    // proving the read path composes across a dimension boundary.
    //
    // Rectangular 2-D int[][] (2 rows x 3 cols).
    public static int[][]    staticIntGrid =
        { { 10, 20, 30 }, { 40, 50, 60 } };
    // Rectangular 2-D double[][] (2 rows x 2 cols), exact halves.
    public static double[][] staticDoubleGrid =
        { { 1.5, 2.5 }, { 3.5, 4.5 } };
    // 3-D byte[][][]: two outer planes of differing shape (jagged in the
    // middle/inner dimensions) so depth-length and element value are both
    // exercised at three levels of nesting.
    public static byte[][][] staticByteCube =
        { { { (byte) 1, (byte) 2 }, { (byte) 3 } },
          { { (byte) 4, (byte) 5, (byte) 6 } } };
    // 2-D int[][] whose MIDDLE row is null -- the native walk must read the
    // null inner slot as an empty row (no crash), then continue.
    public static int[][]    staticIntGridNullRow =
        { { 1, 2 }, null, { 3 } };

    // --- Jagged primitive arrays (rows of differing length) ------------------
    // int[][] with rows of length 1 / 2 / 3, plus a deliberately EMPTY row.
    public static int[][]    staticJaggedIntArray =
        { { 7 }, { 8, 9 }, { }, { 10, 11, 12 } };

    // --- INSTANCE multi-dim / jagged variants --------------------------------
    public int[][]    instIntGrid =
        { { 70, 80 }, { 90, 100 }, { 110, 120 } };           // 3 x 2 rectangular
    public short[][]  instJaggedShortArray =
        { { (short) 1 }, { (short) -2, (short) 3 }, { (short) 4, (short) 5, (short) 6 } };

    // --- TWO-element arrays (the smallest "many" boundary, length 2) ----------
    // The flat shapes above cover length 0 / 1 / 3 / 256 / 1024 but never 2.
    // A length-2 read exercises the loop's first->last transition with no mid.
    public static byte[]    twoByteArray  = { (byte) -100, (byte) 100 };
    public static int[]     twoIntArray   = { -2000000000, 2000000000 };
    public static long[]    twoLongArray  = { Long.MIN_VALUE + 1L, Long.MAX_VALUE - 1L };
    public static double[]  twoDoubleArray = { -123.456, 789.0625 };

    // --- ALL-SAME-VALUE arrays (degenerate content, distinct from ascending) --
    // Every element identical -- a read that accidentally reused one slot's
    // address for all indices would still "pass" an ascending array's spot
    // checks at index 0; an all-same array catches a stuck-stride only via the
    // SIZE, while an all-distinct large array (above) catches the stride.  Pair
    // both: here we assert each element equals the constant AND the size.
    public static int[]     sameIntArray   = { 777, 777, 777, 777, 777 };
    public static boolean[] allTrueArray   = { true, true, true, true };
    public static boolean[] allFalseArray  = { false, false, false, false };

    // --- UNSIGNED-reinterpretation source arrays ------------------------------
    // A [B and a [S holding values whose sign bit is set, so reading them into
    // an UNSIGNED C++ element type (uint8_t / uint16_t) through the generic
    // vector<T> path exercises get_array_element<T>'s raw memcpy with a distinct
    // instantiation and proves the bytes are reinterpreted, not sign-mangled.
    // signedByteArray:  { -1, -128, 127 } -> as uint8_t { 255, 128, 127 }.
    // signedShortArray: { -1, -32768, 32767 } -> as uint16_t { 65535, 32768, 32767 }.
    public static byte[]    signedByteArray  = { (byte) -1, Byte.MIN_VALUE, Byte.MAX_VALUE };
    public static short[]   signedShortArray = { (short) -1, Short.MIN_VALUE, Short.MAX_VALUE };
    // A [I holding negatives, read into uint32_t -> { 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF }.
    public static int[]     signedIntArray   = { -1, Integer.MIN_VALUE, Integer.MAX_VALUE };

    // --- INSTANCE large + special variants ------------------------------------
    // The static path has a 256-element large array and a NaN/Inf/subnormal
    // special array; the INSTANCE-offset read path had neither.  A modest
    // 64-element instance large int array (deterministic formula) plus an
    // instance special float array close that gap at a small heap cost.
    public static final int INST_LARGE_LEN = 64;
    public int[]   instLargeIntArray = new int[INST_LARGE_LEN];
    public float[] instSpecialFloatArray =
        { Float.NaN, Float.NEGATIVE_INFINITY, Float.POSITIVE_INFINITY, -0.0f };

    // Fill instLargeIntArray via a deterministic formula the native side can
    // recompute by index (instance initialiser: runs for every constructed
    // instance, including the static 'instance' self-reference above).
    {
        for (int i = 0; i < INST_LARGE_LEN; ++i)
        {
            instLargeIntArray[i] = i * 11 - 333;
        }
    }

    // --- char[] with NUL (0x00) embedded and a surrogate-range code unit ------
    // Java char[] stores raw 16-bit code units with no NUL terminator; a value
    // of 0x0000 in the middle is a legal element, and a lone surrogate (0xD800)
    // is a legal char (not a valid String, but a valid char[] element).  The raw
    // uint16 read must recover all four exactly; into vector<char> each narrows
    // to its low byte.  Pure ASCII source (all via (char) casts).
    public static char[]    nulCharArray = { (char) 0x0000, 'Z', (char) 0xD800, (char) 0x0041 };

    static
    {
        for (int i = 0; i < LARGE_LEN; ++i)
        {
            largeBoolArray[i]   = (i % 2) == 0;
            largeByteArray[i]   = (byte) (i - 128);          // spans -128..127
            largeShortArray[i]  = (short) (i * 7 - 900);
            largeCharArray[i]   = (char) (i + 32);           // printable-ish
            largeIntArray[i]    = i * 3 + 1;
            largeLongArray[i]   = (long) i * 1000000007L + 5L;
            largeFloatArray[i]  = i + 0.5f;
            largeDoubleArray[i] = i + 0.25;
        }

        for (int i = 0; i < LARGE_K_LEN; ++i)
        {
            largeKIntArray[i]    = i * 5 - 2000;
            largeKLongArray[i]   = (long) i * 1000000009L - 7L;
            largeKDoubleArray[i] = i * 0.5 - 256.0;
        }

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldArraysPrimitive.go && !FieldArraysPrimitive.done;
            }

            @Override
            public void run()
            {
                // Touch one element of every array through real bytecode
                // dispatch so the run is observable, then publish a checksum.
                long sum = 0L;
                sum += staticBoolArray[0] ? 1L : 0L;
                sum += staticByteArray[0];
                sum += staticShortArray[0];
                sum += staticCharArray[0];
                sum += staticIntArray[0];
                sum += staticLongArray[0];
                sum += (long) staticFloatArray[0];
                sum += (long) staticDoubleArray[0];
                sum += instance.instBoolArray[1] ? 1L : 0L;
                sum += instance.instByteArray[0];
                sum += instance.instIntArray[0];
                sum += instance.instLongArray[0];
                sum += largeIntArray[LARGE_LEN - 1];
                sum += singleIntArray[0];
                // Touch the 1000+ and multi-dim shapes too so a class-init /
                // classpath failure that nulls any of them surfaces as a probe
                // checksum mismatch rather than a silent skip.
                sum += largeKIntArray[LARGE_K_LEN - 1];
                sum += staticIntGrid[1][2];
                sum += staticByteCube[1][0][2];
                sum += staticJaggedIntArray[3][2];
                sum += instance.instIntGrid[2][1];
                // Touch the new flat / unsigned-source / instance-large shapes
                // so a class-init failure nulling any of them is observable.
                sum += twoIntArray[1];
                sum += sameIntArray[0];
                sum += signedIntArray[2];
                sum += instance.instLargeIntArray[INST_LARGE_LEN - 1];
                FieldArraysPrimitive.probeChecksum = sum;
                FieldArraysPrimitive.done = true;
            }
        });
    }
}
