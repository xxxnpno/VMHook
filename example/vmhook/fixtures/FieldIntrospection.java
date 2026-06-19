package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_introspection feature (area: fields).
 *
 * The native module field_introspection.cpp exhaustively exercises the four
 * field_proxy introspection accessors on a LIVE JVM:
 *
 *   - signature()        -> the exact JVM type descriptor for every field type
 *                           (Z B S I J F D C, Ljava/lang/String;, [I, [[I,
 *                           [Ljava/lang/Object;, an interface ref, a self ref).
 *   - is_static()        -> true for static fields, false for instance fields,
 *                           proven against BOTH the static_field(...) path and
 *                           the instance get_field(...) path, including a field
 *                           that is static but read through an instance wrapper.
 *   - is_reference()     -> true for L.../[... descriptors, false for primitives.
 *   - raw_address()      -> non-null for a resolved field, stable across repeated
 *                           lookups, equal to (mirror|oop)+offset recomputed
 *                           independently, and the exact address get()/
 *                           get_compressed_oop() read from.
 *   - get_compressed_oop() for a reference field decodes (decode_oop_pointer) to
 *                           the SAME oop that get() yields as void*, and that oop
 *                           is the REAL Java object (cross-checked here via
 *                           System.identityHashCode and array length / element).
 *
 * This fixture only PUBLISHES data and identity witnesses; almost every check is
 * side-effect free and runs before the probe.  The go/done handshake still fires
 * a real bytecode dispatch (so the harness' interpreter-hook contract holds and
 * so any runtime putfield/putstatic is observed post-dispatch) and, on mode 2,
 * forces a GC between two raw_address lookups to document the raw_address GC
 * staleness flaw.
 *
 * Java 8 syntax only (no var/records/switch-expr/text-blocks).
 */
public final class FieldIntrospection
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  The native module sets this and resets `done` BEFORE
     * raising `go` for each probe cycle.
     *   1 = touch(): plain bytecode dispatch (interpreter-hook contract) +
     *       republish identity witnesses so the post-dispatch reads are live.
     *   2 = forceGc(): run System.gc() hard, then touch(), so the native side
     *       can compare raw_address before/after a GC (staleness documentation).
     */
    public static volatile int mode;

    /** Observable side effect the native module asserts on (mode 1/2). */
    public static volatile int observed;

    // =====================================================================
    //  STATIC primitive fields — one per JVM descriptor.  Their VALUES are
    //  not the point (field_primitives_get covers values exhaustively); their
    //  SIGNATURES and is_static/is_reference classification are.
    // =====================================================================
    public static boolean sBool   = true;          // Z
    public static byte    sByte   = (byte) 0xAB;    // B
    public static short   sShort  = (short) 0xBEEF; // S
    public static int     sInt    = 0x0BADF00D;     // I
    public static long    sLong   = 0x1122334455667788L; // J
    public static float   sFloat  = 3.5f;           // F
    public static double  sDouble = 2.5d;           // D
    public static char    sChar   = 'Z';            // C

    // =====================================================================
    //  STATIC reference fields — every reference shape a descriptor can take.
    // =====================================================================
    /** Ljava/lang/String; */
    public static String sString = "introspect-me";

    /** [I — primitive array.  Known length + element-0 published below. */
    public static int[] sIntArray = new int[] { 11, 22, 33, 44, 55 };

    /** [[I — multidimensional array (descriptor starts with two '['). */
    public static int[][] sIntArray2D = new int[][] { { 1, 2 }, { 3, 4, 5 } };

    /** [Ljava/lang/Object; — reference-element array. */
    public static Object[] sObjArray = new Object[] { "a", "b", "c" };

    /** [Ljava/lang/String; — String[]. */
    public static String[] sStrArray = new String[] { "x", "y" };

    /** Ljava/lang/Object; — plain Object reference. */
    public static Object sObject = new Object();

    /** Ljava/lang/Runnable; — interface-typed reference (descriptor is L...;). */
    public static Runnable sRunnable = new Runnable()
    {
        public void run() { /* no-op */ }
    };

    /** Lvmhook/fixtures/FieldIntrospection; — a self-typed reference. */
    public static FieldIntrospection sSelfRef;

    /** [Ljava/lang/String; twin already above; add [[Ljava/lang/Object; — a
        reference-element-of-reference-element array (descriptor "[[Ljava/lang/Object;"). */
    public static Object[][] sObjArray2D = new Object[][] { { "p" }, { "q", "r" } };

    /** A null reference field — get_compressed_oop must read 0, decode to null. */
    public static String sNullString = null;

    /** A null array field. */
    public static int[] sNullArray = null;

    // =====================================================================
    //  FINAL fields — none of the five field_proxy accessors surface the
    //  JVM_ACC_FINAL flag (signature/is_static/is_reference/raw_address/
    //  get_compressed_oop are blind to finality), so the native side proves
    //  a `final` field behaves IDENTICALLY to a non-final twin of the same
    //  shape.  Distinct sentinel values keep the value asserts unambiguous.
    // =====================================================================
    /** static final I — same descriptor/static/non-ref as sInt. */
    public static final int sFinalInt = 0x12345678;

    /** static final Ljava/lang/String; — same descriptor/static/ref as sString. */
    public static final String sFinalString = "final-static";

    /** instance final I — same descriptor/non-static/non-ref as iInt. */
    public final int iFinalInt = 0x0DEFACED;

    // =====================================================================
    //  INSTANCE fields — mirror the primitive + reference set so is_static()
    //  can be proven false on a genuine putfield-backed instance slot, and so
    //  raw_address/get_compressed_oop can be exercised against instance OOPs.
    // =====================================================================
    public boolean iBool   = false;
    public byte    iByte   = (byte) 7;
    public short   iShort  = (short) 1234;
    public int     iInt    = 0x0BADCAFE;
    public long    iLong   = 0x0123456789ABCDEFL;
    public float   iFloat  = 1.25f;
    public double  iDouble = 9.5d;
    public char    iChar   = '#';
    public String  iString = "instance-string";
    public int[]   iIntArray = new int[] { 7, 8, 9 };
    public Object  iObject = new Object();
    public String  iNullString = null;

    // =====================================================================
    //  IDENTITY WITNESSES — published so the NATIVE side can prove a decoded
    //  compressed OOP is the *actual* Java object (not merely a non-null
    //  pointer).  identityHashCode is stable for the object's lifetime.
    // =====================================================================
    public static volatile int sStringIdentityHash;
    public static volatile int sObjectIdentityHash;
    public static volatile int sRunnableIdentityHash;
    public static volatile int sIntArrayIdentityHash;
    public static volatile int sIntArrayLength;
    public static volatile int sIntArrayElem0;
    public static volatile int sObjArrayLength;
    public static volatile int sStrArrayLength;
    public static volatile int sIntArray2DLength;
    public static volatile int sObjArray2DLength;
    public static volatile int sStrArrayElem0Length;
    public static volatile int sSelfRefIdentityHash;
    public static volatile int iStringIdentityHash;
    public static volatile int iObjectIdentityHash;
    public static volatile int iIntArrayIdentityHash;
    public static volatile int iIntArrayLength;

    /** Length of sString (so native can validate the decoded String). */
    public static volatile int sStringLength;

    /**
     * The single live instance the native side wraps for instance-field
     * introspection.  Kept reachable by this static reference.
     */
    public static volatile FieldIntrospection instance;

    /** Recomputes every identity witness from the current field references. */
    private static void publishWitnesses()
    {
        sStringIdentityHash   = System.identityHashCode(sString);
        sObjectIdentityHash   = System.identityHashCode(sObject);
        sRunnableIdentityHash = System.identityHashCode(sRunnable);
        sIntArrayIdentityHash = System.identityHashCode(sIntArray);
        sIntArrayLength       = (sIntArray == null) ? -1 : sIntArray.length;
        sIntArrayElem0        = (sIntArray == null || sIntArray.length == 0) ? -1 : sIntArray[0];
        sObjArrayLength       = (sObjArray == null) ? -1 : sObjArray.length;
        sStrArrayLength       = (sStrArray == null) ? -1 : sStrArray.length;
        sIntArray2DLength     = (sIntArray2D == null) ? -1 : sIntArray2D.length;
        sObjArray2DLength     = (sObjArray2D == null) ? -1 : sObjArray2D.length;
        sStrArrayElem0Length  = (sStrArray == null || sStrArray.length == 0
                                 || sStrArray[0] == null) ? -1 : sStrArray[0].length();
        sSelfRefIdentityHash  = System.identityHashCode(sSelfRef);
        sStringLength         = (sString == null) ? -1 : sString.length();

        if (instance != null)
        {
            iStringIdentityHash   = System.identityHashCode(instance.iString);
            iObjectIdentityHash   = System.identityHashCode(instance.iObject);
            iIntArrayIdentityHash = System.identityHashCode(instance.iIntArray);
            iIntArrayLength       = (instance.iIntArray == null) ? -1 : instance.iIntArray.length;
        }
    }

    /** Hookable instance method — proves the interpreter-hook-on-dispatch path. */
    public int touch(final int delta)
    {
        return this.iInt + delta;
    }

    static
    {
        sSelfRef = new FieldIntrospection();
        instance = new FieldIntrospection();
        publishWitnesses();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldIntrospection.go && !FieldIntrospection.done;
            }

            @Override
            public void run()
            {
                if (FieldIntrospection.mode == 2)
                {
                    // Force GC churn between the native side's two raw_address
                    // lookups.  Allocate garbage so a real collection happens
                    // even when -XX:+DisableExplicitGC neuters System.gc().
                    for (int n = 0; n < 64; ++n)
                    {
                        byte[] junk = new byte[64 * 1024];
                        junk[0] = (byte) n;
                    }
                    System.gc();
                    System.gc();
                }

                final FieldIntrospection self = FieldIntrospection.instance;
                FieldIntrospection.observed = self.touch(100);

                // Republish witnesses: if a moving GC relocated any target, the
                // identityHashCode is unchanged (it is cached in the header),
                // but the native side re-reads addresses post-probe.
                publishWitnesses();

                FieldIntrospection.done = true;
            }
        });
    }
}
