package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_is_reference feature (area: methods).
 *
 * The ONE thing under test: {@code vmhook::method_proxy::is_reference()} — the
 * O(1) introspection accessor that reports whether a method's RETURN type is a
 * Java reference (object / array, descriptor char 'L' or '[') as opposed to a
 * primitive (Z B S C I J F D) or void (V).  It reads the character that follows
 * the closing ')' of the JVM descriptor; it requires NO live bytecode dispatch
 * (it never calls the method), so the native module asserts everything straight
 * from the resolved proxies — no call() and therefore no current_java_thread is
 * needed for the core coverage.
 *
 * This fixture supplies a method per return KIND, in INSTANCE and STATIC flavours
 * so the native side exercises both the instance {@code get_method("name")} path
 * and the static {@code static_method("name")} path:
 *
 *   primitive returns (is_reference() must be FALSE):
 *     retBool   ()Z      retByte ()B      retShort()S      retChar ()C
 *     retInt    ()I      retLong ()J      retFloat()F      retDouble()D
 *     retVoid   ()V      (V is "not a reference" — the close+1 == 'V' branch)
 *
 *   reference returns (is_reference() must be TRUE):
 *     retString ()Ljava/lang/String;   retObject ()Ljava/lang/Object;
 *     retIntArray ()[I                 retStringArray ()[Ljava/lang/String;
 *
 * Each of the above has an {@code s}-prefixed STATIC twin with the IDENTICAL
 * return descriptor, so is_reference() is proven independent of static-ness.
 *
 * Overload disambiguation: {@code dual(...)} is a single NAME carrying two
 * overloads that differ ONLY in return type-class, selectable purely by explicit
 * JVM descriptor:
 *     dual(I)I                            -> primitive return  (is_reference FALSE)
 *     dual(Ljava/lang/String;)Ljava/lang/Object;  -> reference return (TRUE)
 * (Java forbids overloading by return type alone, so the two overloads also
 * differ in their parameter list; the native side resolves each by its EXACT
 * descriptor and proves is_reference() tracks the RESOLVED overload, not the bare
 * name.)  A static twin {@code sdual(...)} mirrors this for the static path.
 *
 * NOTE: the method bodies here are never executed by the test — is_reference() is
 * pure metadata.  They return harmless constants only so the class verifies and
 * loads.  The {@code SINGLETON} field exists so the native side can fetch an
 * instance (the standard "get an instance to reach the instance get_method path"
 * trick) without driving any Java code.  The Harness.Probe below is a trivial
 * handshake required by the modular-harness contract; the module does not need it
 * for is_reference(), so its run() is a pure no-op flag flip.
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks); only
 * java.* + vmhook.Harness are referenced.
 */
public final class IsReference
{
    /** Native sets this true to request the (no-op) action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ======================================================================
    //  INSTANCE primitive returns — is_reference() must be FALSE for each.
    // ======================================================================
    public boolean retBool()   { return true; }
    public byte    retByte()   { return (byte) 1; }
    public short   retShort()  { return (short) 2; }
    public char    retChar()   { return 'A'; }
    public int     retInt()    { return 3; }
    public long    retLong()   { return 4L; }
    public float   retFloat()  { return 0.5f; }
    public double  retDouble() { return 6.0; }
    public void    retVoid()   { /* V: not a reference */ }

    // ======================================================================
    //  INSTANCE reference returns — is_reference() must be TRUE for each.
    // ======================================================================
    public String   retString()      { return "ref"; }
    public Object   retObject()       { return this; }
    public int[]    retIntArray()     { return INT_ARRAY; }
    public String[] retStringArray()  { return STRING_ARRAY; }

    // ======================================================================
    //  STATIC primitive returns (identical descriptors to the instance ones).
    // ======================================================================
    public static boolean sRetBool()   { return false; }
    public static byte     sRetByte()   { return (byte) -1; }
    public static short    sRetShort()  { return (short) -2; }
    public static char     sRetChar()   { return 'B'; }
    public static int      sRetInt()    { return -3; }
    public static long     sRetLong()   { return -4L; }
    public static float    sRetFloat()  { return -0.5f; }
    public static double   sRetDouble() { return -6.0; }
    public static void     sRetVoid()   { /* V: not a reference */ }

    // ======================================================================
    //  STATIC reference returns (identical descriptors to the instance ones).
    // ======================================================================
    public static String   sRetString()     { return "sref"; }
    public static Object    sRetObject()      { return STATIC_SELF; }
    public static int[]     sRetIntArray()    { return INT_ARRAY; }
    public static String[]  sRetStringArray() { return STRING_ARRAY; }

    // ======================================================================
    //  INSTANCE + STATIC: BOXED wrapper-type returns.  Every boxed type is a
    //  Java reference ('L...;'), so is_reference() is TRUE — including the
    //  boxed java.lang.Void ('()Ljava/lang/Void;'), which is the sharp
    //  contrast against the PRIMITIVE void '()V' (retVoid above, FALSE).  The
    //  boxed-vs-primitive void pair is the headline edge of this block.
    // ======================================================================
    public Boolean   retBoxedBool()   { return Boolean.TRUE; }
    public Byte      retBoxedByte()   { return Byte.valueOf((byte) 1); }
    public Short     retBoxedShort()  { return Short.valueOf((short) 2); }
    public Character retBoxedChar()   { return Character.valueOf('A'); }
    public Integer   retBoxedInt()    { return Integer.valueOf(3); }
    public Long      retBoxedLong()   { return Long.valueOf(4L); }
    public Float     retBoxedFloat()  { return Float.valueOf(0.5f); }
    public Double    retBoxedDouble() { return Double.valueOf(6.0); }
    public Void      retBoxedVoid()   { return null; }

    public static Boolean   sRetBoxedBool()   { return Boolean.FALSE; }
    public static Integer   sRetBoxedInt()    { return Integer.valueOf(-3); }
    public static Long      sRetBoxedLong()   { return Long.valueOf(-4L); }
    public static Double    sRetBoxedDouble() { return Double.valueOf(-6.0); }
    public static Character sRetBoxedChar()   { return Character.valueOf('B'); }
    public static Void      sRetBoxedVoid()   { return null; }

    // ======================================================================
    //  INSTANCE + STATIC: USER-defined reference returns (a nested type, by
    //  both the concrete class and an implemented interface).  Descriptor is
    //  'Lvmhook/fixtures/IsReference$Box;' / '$Tag;' — still 'L...;', so
    //  is_reference() is TRUE.  Proves the verdict does NOT depend on the
    //  type being a JDK type.
    // ======================================================================
    public Box retBox()        { return BOX; }
    public Tag retTagIface()   { return BOX; }
    public static Box sRetBox()      { return BOX; }
    public static Tag sRetTagIface() { return BOX; }

    // ======================================================================
    //  INSTANCE: every PRIMITIVE-ELEMENT array kind ([Z [B [S [C [I [J [F [D).
    //  is_reference() keys on the leading '[', so ALL of these are TRUE even
    //  though the ELEMENT type is a primitive — an array is a Java reference.
    // ======================================================================
    public boolean[] retBoolArray()   { return new boolean[] { true }; }
    public byte[]    retByteArray()   { return new byte[] { 1 }; }
    public short[]   retShortArray()  { return new short[] { 2 }; }
    public char[]    retCharArray()   { return new char[] { 'A' }; }
    public long[]    retLongArray()   { return new long[] { 4L }; }
    public float[]   retFloatArray()  { return new float[] { 0.5f }; }
    public double[]  retDoubleArray() { return new double[] { 6.0 }; }
    public Object[]  retObjectArray() { return new Object[] { this }; }

    // ======================================================================
    //  INSTANCE: MULTI-dimensional arrays ([[I [[Ljava/lang/String; [[[B).
    //  The return descriptor still LEADS with '[', so is_reference() is TRUE.
    // ======================================================================
    public int[][]      retInt2DArray()     { return new int[][] { { 1 } }; }
    public String[][]   retString2DArray()  { return new String[][] { { "x" } }; }
    public byte[][][]   retByte3DArray()    { return new byte[][][] { { { 1 } } }; }

    // ======================================================================
    //  INSTANCE: reference returns that are NOT String/Object — a JDK
    //  collection type and an INTERFACE type.  Both descriptors are 'L...;',
    //  so is_reference() is TRUE.  (Descriptor text is JDK-version-stable.)
    // ======================================================================
    public java.util.List<String> retList()      { return EMPTY_LIST; }
    public CharSequence            retInterface() { return "iface"; }

    // A return whose element type is a USER reference type — '[Lvmhook/.../$Box;'.
    public Box[]     retBoxArray()    { return new Box[] { BOX }; }
    // A very deep primitive array — '[[[[[I' — still a reference (leading '[').
    public int[][][][][] retInt5DArray() { return new int[1][0][0][0][0]; }

    // ======================================================================
    //  INSTANCE: MORE reference return kinds whose descriptor combines '[' with
    //  a NESTED reference element ('L...;') or a boxed/interface/user element —
    //  every one still LEADS with '[' (or 'L'), so is_reference() is TRUE.  These
    //  exercise the live resolution path over '[' + nested-'L' descriptors the
    //  earlier sweeps only touched for String[][] and Box[].
    // ======================================================================
    // Interface-element array — '[Ljava/lang/CharSequence;'.
    public CharSequence[] retInterfaceArray() { return new CharSequence[] { "iface" }; }
    // Boxed-element array — '[Ljava/lang/Integer;'.
    public Integer[]      retBoxedIntArray()  { return new Integer[] { Integer.valueOf(3) }; }
    // Boxed-Void element array — '[Ljava/lang/Void;' (reference element + array).
    public Void[]         retBoxedVoidArray() { return new Void[] { null }; }
    // 2-D user-type array — '[[Lvmhook/fixtures/IsReference$Box;'.
    public Box[][]        retBox2DArray()     { return new Box[][] { { BOX } }; }
    // 3-D String array — '[[[Ljava/lang/String;' (deep '[' + nested 'L').
    public String[][][]   retString3DArray()  { return new String[][][] { { { "x" } } }; }

    // A reference PARAM with a reference RETURN — '(Ljava/lang/String;)Ljava/lang/Object;'.
    // The RED-HERRING inverse: here the return IS a reference, so is_reference() is
    // TRUE; pairing it with takesString (reference param, primitive return) proves
    // is_reference() reads ONLY the return slot, independent of the param 'L'.
    public Object refParamRefReturn(final String s) { return s; }

    // ======================================================================
    //  STATIC twins of the array / collection / interface returns, so the
    //  static_method() path sees the SAME '[' / 'L' return descriptors as the
    //  instance path.  Identical descriptors prove is_reference() independent
    //  of static-ness across EVERY reference return kind, not just scalars.
    // ======================================================================
    public static boolean[] sRetBoolArray()   { return new boolean[] { false }; }
    public static byte[]    sRetByteArray()   { return new byte[] { -1 }; }
    public static short[]   sRetShortArray()  { return new short[] { -2 }; }
    public static char[]    sRetCharArray()   { return new char[] { 'B' }; }
    public static long[]    sRetLongArray()   { return new long[] { -4L }; }
    public static float[]   sRetFloatArray()  { return new float[] { -0.5f }; }
    public static double[]  sRetDoubleArray() { return new double[] { -6.0 }; }
    public static Object[]  sRetObjectArray() { return new Object[] { STATIC_SELF }; }
    public static int[][]   sRetInt2DArray()  { return new int[][] { { 1 } }; }
    public static byte[][][] sRetByte3DArray() { return new byte[][][] { { { 1 } } }; }
    public static java.util.List<String> sRetList()      { return EMPTY_LIST; }
    public static CharSequence            sRetInterface() { return "siface"; }
    // STATIC twins of the NEW nested-reference array kinds — same '[' + 'L'
    // descriptors as their instance twins, so is_reference() is TRUE and the
    // static_method() path is proven over them too.
    public static CharSequence[] sRetInterfaceArray() { return new CharSequence[] { "siface" }; }
    public static Integer[]      sRetBoxedIntArray()  { return new Integer[] { Integer.valueOf(-3) }; }
    public static Box[][]        sRetBox2DArray()     { return new Box[][] { { BOX } }; }
    public static String[][][]   sRetString3DArray()  { return new String[][][] { { { "s" } } }; }

    // ======================================================================
    //  PARAM-LIST RED HERRING: methods whose PARAMETER list contains 'L' / '['
    //  (reference / array params) but whose RETURN is a primitive or void.
    //  is_reference() must look ONLY after the ')' — the param 'L'/'[' must
    //  NOT leak into the verdict.  These are the cases that catch a parser
    //  that scanned the whole descriptor instead of the return slot.
    // ======================================================================

    /** takesString(Ljava/lang/String;)I — reference PARAM, primitive return. */
    public int  takesString(final String s)   { return s == null ? 0 : s.length(); }

    /** takesIntArray([I)I — array PARAM, primitive return. */
    public int  takesIntArray(final int[] a)   { return a == null ? 0 : a.length; }

    /** takesObjectArray([Ljava/lang/Object;)V — array-of-ref PARAM, void return. */
    public void takesObjectArray(final Object[] a) { /* V: not a reference */ }

    /** takesMixed(Ljava/lang/String;[IJ)Z — mixed ref+array+long params, boolean. */
    public boolean takesMixed(final String s, final int[] a, final long n) { return false; }

    /** sTakesString(Ljava/lang/String;)I — static red-herring twin. */
    public static int sTakesString(final String s) { return s == null ? 0 : s.length(); }

    // ======================================================================
    //  Overloaded pair: ONE name, primitive vs reference return, told apart
    //  ONLY by explicit JVM descriptor.  Proves is_reference() tracks the
    //  specific RESOLVED overload, not merely the method name.
    // ======================================================================

    /** dual(I)I — primitive return. */
    public int dual(final int a) { return a; }

    /** dual(Ljava/lang/String;)Ljava/lang/Object; — reference return. */
    public Object dual(final String s) { return s; }

    /** sdual(I)I — static primitive return. */
    public static int sdual(final int a) { return a; }

    /** sdual(Ljava/lang/String;)Ljava/lang/Object; — static reference return. */
    public static Object sdual(final String s) { return s; }

    // ======================================================================
    //  Fixed reference payloads (so the array returners are well-defined).
    // ======================================================================
    private static final int[]    INT_ARRAY    = { 10, 20, 30 };
    private static final String[] STRING_ARRAY = { "x", "y" };

    /** A stable empty List the reference-but-not-String returner hands back. */
    private static final java.util.List<String> EMPTY_LIST =
        java.util.Collections.emptyList();

    /** A stable object the static Object returner hands back. */
    private static final Object STATIC_SELF = new Object();

    // ======================================================================
    //  USER reference types returned above.  Box is a concrete nested class
    //  (descriptor 'Lvmhook/fixtures/IsReference$Box;'); Tag is the interface
    //  it implements (descriptor 'Lvmhook/fixtures/IsReference$Tag;').  Both
    //  are reference returns regardless of being user-defined.
    // ======================================================================
    public interface Tag
    {
        int kind();
    }

    public static final class Box implements Tag
    {
        @Override
        public int kind() { return 7; }
    }

    /** A stable user-type payload the Box / Tag returners hand back. */
    private static final Box BOX = new Box();

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return IsReference.go && !IsReference.done;
            }

            @Override
            public void run()
            {
                // is_reference() is pure metadata — nothing to dispatch.  The
                // handshake exists only to satisfy the harness contract; flip
                // done so any native run_probe() resolves cleanly.
                IsReference.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps so it can drive the INSTANCE
     * {@code get_method("name")} resolution path.  Created eagerly so the native
     * side can fetch it via the {@code SINGLETON} static reference field without
     * executing any Java code.
     */
    public static final IsReference SINGLETON = new IsReference();
}
