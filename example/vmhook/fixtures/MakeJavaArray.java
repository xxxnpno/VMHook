package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code vmhook::make_java_array(class_name, length, element_size)}
 * feature (area: heap allocation / arrays).  make_java_array is the low-level
 * primitive that allocates a brand-new Java ARRAY object straight from C++ with
 * NO JNI NewTypeArray — it finds the array klass (with a JDK-8 JNI FindClass
 * fallback, "FIX D"), allocates {@code header + length*element_size} bytes via
 * make_java_object, and writes the Java array length into the arrayOop _length
 * slot.  It is what make_java_string builds on (it allocates the backing
 * {@code [B} / {@code [C}).
 *
 * <p>The native module ({@code tests/jvm/modules/make_java_array.cpp}) does ALL
 * of its make_java_array work from INSIDE an interpreter detour on {@link #cycle()}
 * (where HotSpot's current_java_thread — the precondition for heap allocation — is
 * established).  Inside that detour, for ONE representative array per element
 * descriptor, it:</p>
 * <ol>
 *   <li>allocates the array with make_java_array(descriptor, length, elemSize),</li>
 *   <li>stores the freshly-made array OOP into the matching {@code recv*} static
 *       field below via field_proxy::set (the object-reference / compressed-OOP
 *       write path), so the array becomes reachable from real Java bytecode,</li>
 *   <li>BUILDS a primitive array FROM A C++ std::vector, writes every element with
 *       set_array_element, then PASSES the made array INTO one of the
 *       {@code sum*Array} / {@code check*Array} methods below via
 *       {@code method_proxy::call}.  Those methods run genuine Java bytecode
 *       ({@code arraylength} + {@code Xaload} over the whole array) and return a
 *       value that depends on EVERY element, so a wrong element width, a bad
 *       length, a slot/stride mistake, or element corruption is caught Java-side
 *       — the array is proven to be a well-formed, JVM-usable array, not just a
 *       native byte blob.</li>
 *   <li>for the REFERENCE arrays, allocates an EMPTY {@code Object[]} /
 *       {@code String[]} with make_java_array and passes it into
 *       {@link #fillCheckObjectArray(Object[])} / {@link #fillCheckStringArray(String[])},
 *       which FILL it with refs + interspersed nulls using real {@code aastore}
 *       bytecode and read them back with {@code aaload} — proving the made
 *       reference array supports element storage/retrieval.</li>
 * </ol>
 * After the detour returns, the probe calls {@link #captureAll()} — pure Java
 * bytecode — which reads, for every {@code recv*} that was filled, the array's
 * {@code .length} and its {@code getClass().getName()} into the {@code obsLen*} /
 * {@code obsType*} witness fields the native side then reads back.
 *
 * <h3>Element descriptors covered</h3>
 * {@code [Z [B [S [C [I [J [F [D} (primitive) and
 * {@code [Ljava/lang/Object;} / {@code [Ljava/lang/String;} (reference).  The
 * primitive {@code [B} and {@code [C} paths are HARD invariants on every JDK
 * because make_java_string depends on them; the reference-array paths are
 * gated best-effort on JDK 8 by the native module.
 *
 * <p>Pure-ASCII source, Java-8 syntax only (anonymous Harness.Probe, no var /
 * records / switch-expressions / text blocks), so it compiles identically under
 * javac 8 and javac 26 with any -encoding.</p>
 */
public final class MakeJavaArray
{
    // ── go / done handshake driven by the native module via run_probe ───────
    public static volatile boolean go;
    public static volatile boolean done;

    /** Liveness counter: bumped each time the hooked {@link #cycle()} body runs. */
    public static volatile int cycleCount;

    /**
     * The representative length the native side allocates for each {@code recv*}
     * field (must match the constant the C++ module uses for its Java-visible
     * arrays).  Distinct from 0/1/256 so a stale/zero witness is unmistakable.
     */
    public static final int WITNESS_LEN = 3;

    /**
     * The length of the arrays the native side builds from a C++ vector and feeds
     * into the {@code sum*Array} / {@code check*Array} verifiers below.  Small and
     * fixed so each verifier can hard-code the exact per-position expectation the
     * native side wrote.  Must equal the C++ module's k_feed_len.
     */
    public static final int FEED_LEN = 5;

    // =====================================================================
    //  recv* — receiver slots the native side overwrites with a made array.
    //  Declared as Object so field_proxy::set's object-reference write path is
    //  exercised uniformly; captureAll() casts each back to its real array type
    //  to read .length.  Initialised to a recognisable NON-array sentinel so a
    //  skipped write is caught (the witness would reflect the sentinel's class
    //  name / the null marker, never a real array type).
    // =====================================================================
    public static Object recvZ   = "<<unwritten-Z>>";
    public static Object recvB   = "<<unwritten-B>>";
    public static Object recvS   = "<<unwritten-S>>";
    public static Object recvC   = "<<unwritten-C>>";
    public static Object recvI   = "<<unwritten-I>>";
    public static Object recvJ   = "<<unwritten-J>>";
    public static Object recvF   = "<<unwritten-F>>";
    public static Object recvD   = "<<unwritten-D>>";
    public static Object recvObj = "<<unwritten-Obj>>";
    public static Object recvStr = "<<unwritten-Str>>";
    /** Receiver for the multi-dimensional ("[[I") made array. */
    public static Object recvMD  = "<<unwritten-MD>>";

    // =====================================================================
    //  Witnesses captured by captureAll() with genuine Java bytecode.
    //   obsLen*   = the array's .length      (-1 if the slot was null,
    //                                          -2 if it was not an array at all)
    //   obsType*  = getClass().getName()     ("" if null)
    //   obsNull*  = true if the slot was still null after the native write
    //  obsType uses the JVM's binary array-class name, e.g. "[I",
    //  "[Ljava.lang.Object;" (dotted), so the native side asserts the exact
    //  klass the made array carries.
    // =====================================================================
    public static int     obsLenZ;   public static String obsTypeZ   = "";  public static boolean obsNullZ;
    public static int     obsLenB;   public static String obsTypeB   = "";  public static boolean obsNullB;
    public static int     obsLenS;   public static String obsTypeS   = "";  public static boolean obsNullS;
    public static int     obsLenC;   public static String obsTypeC   = "";  public static boolean obsNullC;
    public static int     obsLenI;   public static String obsTypeI   = "";  public static boolean obsNullI;
    public static int     obsLenJ;   public static String obsTypeJ   = "";  public static boolean obsNullJ;
    public static int     obsLenF;   public static String obsTypeF   = "";  public static boolean obsNullF;
    public static int     obsLenD;   public static String obsTypeD   = "";  public static boolean obsNullD;
    public static int     obsLenObj; public static String obsTypeObj = "";  public static boolean obsNullObj;
    public static int     obsLenStr; public static String obsTypeStr = "";  public static boolean obsNullStr;
    public static int     obsLenMD;  public static String obsTypeMD  = "";  public static boolean obsNullMD;

    // =====================================================================
    //  PASS-INTO-JAVA witnesses.  The native side builds a primitive array from a
    //  C++ vector and passes it into the matching sum*/check* method; each method
    //  records, with genuine bytecode, what it OBSERVED: the array's .length, its
    //  getClass().getName(), and its first/last element — so the native side can
    //  prove Java saw exactly the array it built, element-for-element.  The METHOD
    //  RETURN (a long that depends on every element) is the primary checksum the
    //  native side compares against its own C++ computation; these fields are the
    //  secondary, per-element corroboration.
    //   fed*Called : the verifier body actually ran (call dispatched);
    //   fed*Len    : the .length the verifier saw (-1 if the arg was null);
    //   fed*Type   : getClass().getName() the verifier saw ("" if null);
    //   fed*First  : element [0] as a long/bits (0 if length 0 / null);
    //   fed*Last   : element [length-1] as a long/bits (0 if length 0 / null).
    // =====================================================================
    public static boolean fedZCalled;   public static int fedZLen = -3;   public static String fedZType = "";   public static long fedZFirst;   public static long fedZLast;
    public static boolean fedBCalled;   public static int fedBLen = -3;   public static String fedBType = "";   public static long fedBFirst;   public static long fedBLast;
    public static boolean fedSCalled;   public static int fedSLen = -3;   public static String fedSType = "";   public static long fedSFirst;   public static long fedSLast;
    public static boolean fedCCalled;   public static int fedCLen = -3;   public static String fedCType = "";   public static long fedCFirst;   public static long fedCLast;
    public static boolean fedICalled;   public static int fedILen = -3;   public static String fedIType = "";   public static long fedIFirst;   public static long fedILast;
    public static boolean fedJCalled;   public static int fedJLen = -3;   public static String fedJType = "";   public static long fedJFirst;   public static long fedJLast;
    public static boolean fedFCalled;   public static int fedFLen = -3;   public static String fedFType = "";   public static long fedFFirst;   public static long fedFLast;
    public static boolean fedDCalled;   public static int fedDLen = -3;   public static String fedDType = "";   public static long fedDFirst;   public static long fedDLast;

    // ── Reference-array fill/check witnesses (set by fillCheck*Array). ──
    //   ref*Called    : the body ran;
    //   ref*Len       : the .length the body saw (-1 if null);
    //   ref*NonNull   : how many elements were non-null AFTER the body filled it;
    //   ref*Roundtrip : true if every element the body stored read back identical
    //                   (aastore then aaload by ==), proving real element storage.
    public static boolean refObjCalled;   public static int refObjLen = -3;   public static int refObjNonNull = -1;   public static boolean refObjRoundtrip;
    public static boolean refStrCalled;   public static int refStrLen = -3;   public static int refStrNonNull = -1;   public static boolean refStrRoundtrip;

    // =====================================================================
    //  Hooked method — the native detour anchor.
    // =====================================================================

    /**
     * No-arg trigger the native module hooks.  Calling it on a real bytecode
     * dispatch fires the interpreter hook; inside that detour the native side
     * allocates every representative array via make_java_array, stores each into
     * the matching {@code recv*} field, builds primitive arrays from C++ vectors
     * and passes them into the {@code sum*Array} / {@code check*Array} verifiers,
     * and feeds the reference arrays into the {@code fillCheck*Array} fillers.
     * Just bumps a counter so the native side can confirm the hook fired.
     */
    public void cycle()
    {
        cycleCount++;
    }

    // ── Per-slot capture helpers (genuine getfield + arraylength + getClass
    //    bytecode).  length() returns -1 for null and -2 for a non-array, so a
    //    skipped/garbage write is distinguishable from a real 0/1/3/256 array. ──

    private static int lengthOf(final Object o)
    {
        if (o == null)
        {
            return -1;
        }
        if (!o.getClass().isArray())
        {
            return -2;
        }
        return java.lang.reflect.Array.getLength(o);
    }

    private static String typeOf(final Object o)
    {
        return (o == null) ? "" : o.getClass().getName();
    }

    /**
     * Snapshots — with real Java bytecode — what the JVM observes in each
     * {@code recv*} field after the native detour wrote a made array there.
     * Captures .length, the binary class name, and the null marker into the
     * {@code obs*} witnesses the native side reads back.
     */
    private static void captureAll()
    {
        obsNullZ   = (recvZ   == null); obsLenZ   = lengthOf(recvZ);   obsTypeZ   = typeOf(recvZ);
        obsNullB   = (recvB   == null); obsLenB   = lengthOf(recvB);   obsTypeB   = typeOf(recvB);
        obsNullS   = (recvS   == null); obsLenS   = lengthOf(recvS);   obsTypeS   = typeOf(recvS);
        obsNullC   = (recvC   == null); obsLenC   = lengthOf(recvC);   obsTypeC   = typeOf(recvC);
        obsNullI   = (recvI   == null); obsLenI   = lengthOf(recvI);   obsTypeI   = typeOf(recvI);
        obsNullJ   = (recvJ   == null); obsLenJ   = lengthOf(recvJ);   obsTypeJ   = typeOf(recvJ);
        obsNullF   = (recvF   == null); obsLenF   = lengthOf(recvF);   obsTypeF   = typeOf(recvF);
        obsNullD   = (recvD   == null); obsLenD   = lengthOf(recvD);   obsTypeD   = typeOf(recvD);
        obsNullObj = (recvObj == null); obsLenObj = lengthOf(recvObj); obsTypeObj = typeOf(recvObj);
        obsNullStr = (recvStr == null); obsLenStr = lengthOf(recvStr); obsTypeStr = typeOf(recvStr);
        obsNullMD  = (recvMD  == null); obsLenMD  = lengthOf(recvMD);  obsTypeMD  = typeOf(recvMD);
    }

    // =====================================================================
    //  PASS-INTO-JAVA verifiers.  Each takes a made array the native side
    //  populated from a C++ vector and walks EVERY element with genuine bytecode,
    //  returning a position-weighted value that depends on each element (so a
    //  wrong width / stride / length / corruption changes the return).  Each also
    //  records the .length, class name, and first/last element it saw into the
    //  fed* witnesses.  All are written to be null-safe: a null arg returns a
    //  distinguished NULL_RESULT and records fed*Len = -1.
    // =====================================================================

    /** Distinguished return when a verifier is handed a null array. */
    public static final long NULL_RESULT = -999L;

    private static long weight(final int i)
    {
        // 1-based odd weight per position so ordering matters and no two
        // positions cancel (sum of element*weight is order-sensitive).
        return (long) (2 * i + 1);
    }

    /**
     * boolean[]: returns sum over i of (value[i] ? 1 : 0) * weight(i).  Proves
     * each boolean slot is the 1-byte value the native side wrote (true/false),
     * in order.
     */
    public static long sumBoolArray(final boolean[] a)
    {
        fedZCalled = true;
        if (a == null) { fedZLen = -1; return NULL_RESULT; }
        fedZLen = a.length; fedZType = a.getClass().getName();
        if (a.length > 0) { fedZFirst = a[0] ? 1L : 0L; fedZLast = a[a.length - 1] ? 1L : 0L; }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            if (a[i]) { acc += weight(i); }
        }
        return acc;
    }

    /**
     * byte[]: returns sum over i of value[i] * weight(i), with value[i] the SIGNED
     * Java byte.  A 0xFF byte therefore contributes -1 (not 255), so a sign /
     * width mistake in the native write is caught.
     */
    public static long sumByteArray(final byte[] a)
    {
        fedBCalled = true;
        if (a == null) { fedBLen = -1; return NULL_RESULT; }
        fedBLen = a.length; fedBType = a.getClass().getName();
        if (a.length > 0) { fedBFirst = a[0]; fedBLast = a[a.length - 1]; }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            acc += (long) a[i] * weight(i);
        }
        return acc;
    }

    /** short[]: position-weighted sum of the signed Java shorts. */
    public static long sumShortArray(final short[] a)
    {
        fedSCalled = true;
        if (a == null) { fedSLen = -1; return NULL_RESULT; }
        fedSLen = a.length; fedSType = a.getClass().getName();
        if (a.length > 0) { fedSFirst = a[0]; fedSLast = a[a.length - 1]; }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            acc += (long) a[i] * weight(i);
        }
        return acc;
    }

    /**
     * char[]: position-weighted sum of the UNSIGNED char code units (0..65535).
     * Exercises the full UTF-16 code-unit range, incl. 0xFFFF, with the unsigned
     * semantics a char carries (so a sign-extension mistake would change the sum).
     */
    public static long sumCharArray(final char[] a)
    {
        fedCCalled = true;
        if (a == null) { fedCLen = -1; return NULL_RESULT; }
        fedCLen = a.length; fedCType = a.getClass().getName();
        if (a.length > 0) { fedCFirst = a[0]; fedCLast = a[a.length - 1]; }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            acc += (long) a[i] * weight(i);   // a[i] is 0..65535
        }
        return acc;
    }

    /** int[]: position-weighted sum (widened to long so MIN/MAX don't overflow). */
    public static long sumIntArray(final int[] a)
    {
        fedICalled = true;
        if (a == null) { fedILen = -1; return NULL_RESULT; }
        fedILen = a.length; fedIType = a.getClass().getName();
        if (a.length > 0) { fedIFirst = a[0]; fedILast = a[a.length - 1]; }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            acc += (long) a[i] * weight(i);
        }
        return acc;
    }

    /**
     * long[]: position-weighted XOR-fold of the 64-bit values.  XOR (not sum) so
     * the FULL 64 bits of every wide element matter — a high/low 32-bit swap or a
     * truncated wide write changes the fold.  Proves no slot/width mistake in the
     * 8-byte element write.
     */
    public static long sumLongArray(final long[] a)
    {
        fedJCalled = true;
        if (a == null) { fedJLen = -1; return NULL_RESULT; }
        fedJLen = a.length; fedJType = a.getClass().getName();
        if (a.length > 0) { fedJFirst = a[0]; fedJLast = a[a.length - 1]; }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            acc ^= a[i] + weight(i);   // +weight keeps it order-sensitive
        }
        return acc;
    }

    /**
     * float[]: XOR-fold of the raw IEEE-754 bits (Float.floatToRawIntBits) of each
     * element.  Raw bits (not the float value) so canonical NaN, signalling NaN,
     * +/-Inf and -0.0 are all distinguished — proving the native side's bit-exact
     * float write survives into a real Java float[].
     */
    public static long checkFloatArray(final float[] a)
    {
        fedFCalled = true;
        if (a == null) { fedFLen = -1; return NULL_RESULT; }
        fedFLen = a.length; fedFType = a.getClass().getName();
        if (a.length > 0)
        {
            fedFFirst = Float.floatToRawIntBits(a[0]) & 0xFFFFFFFFL;
            fedFLast  = Float.floatToRawIntBits(a[a.length - 1]) & 0xFFFFFFFFL;
        }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            acc ^= (Float.floatToRawIntBits(a[i]) & 0xFFFFFFFFL) + weight(i);
        }
        return acc;
    }

    /**
     * double[]: XOR-fold of the raw IEEE-754 bits (Double.doubleToRawLongBits) of
     * each element — the 64-bit wide-element analogue of checkFloatArray.  Proves
     * the bit-exact double write (NaN / Inf / -0.0) survives into a Java double[]
     * with no high/low word swap.
     */
    public static long checkDoubleArray(final double[] a)
    {
        fedDCalled = true;
        if (a == null) { fedDLen = -1; return NULL_RESULT; }
        fedDLen = a.length; fedDType = a.getClass().getName();
        if (a.length > 0)
        {
            fedDFirst = Double.doubleToRawLongBits(a[0]);
            fedDLast  = Double.doubleToRawLongBits(a[a.length - 1]);
        }
        long acc = 0L;
        for (int i = 0; i < a.length; i++)
        {
            acc ^= Double.doubleToRawLongBits(a[i]) + weight(i);
        }
        return acc;
    }

    // ── Reference-array fillers: prove the made [L... array supports real
    //    element storage.  The native side hands in an EMPTY (default-null) array;
    //    the body FILLS it with refs + interspersed nulls (aastore), reads them
    //    back (aaload), and reports what it saw.  Returns nonNullCount so the
    //    native side has a value-bearing result too. ──

    /**
     * Object[]: store [0]="obj-zero" (String), [1]=null, [2]=Integer(42), then any
     * remaining slots left null.  Read each back by == identity to prove aaload
     * returns exactly what aastore wrote.  Records length / non-null count /
     * round-trip flag; returns the non-null count.
     */
    public static long fillCheckObjectArray(final Object[] a)
    {
        refObjCalled = true;
        if (a == null) { refObjLen = -1; return NULL_RESULT; }
        refObjLen = a.length;
        final Object e0 = "obj-zero";
        final Object e2 = Integer.valueOf(42);
        boolean roundtrip = true;
        if (a.length > 0) { a[0] = e0; if (a[0] != e0) { roundtrip = false; } }
        if (a.length > 1) { a[1] = null; if (a[1] != null) { roundtrip = false; } }
        if (a.length > 2) { a[2] = e2; if (a[2] != e2) { roundtrip = false; } }
        int nonNull = 0;
        for (int i = 0; i < a.length; i++)
        {
            if (a[i] != null) { nonNull++; }
        }
        refObjNonNull = nonNull;
        refObjRoundtrip = roundtrip;
        return (long) nonNull;
    }

    /**
     * String[]: store [0]="alpha", [1]=null, [2]="gamma" (refs + interspersed
     * null), read back by == identity (interned literals), and report.  Proves the
     * made String[] is a usable reference array with the correct element klass
     * (a non-String store would throw ArrayStoreException, which would fail the
     * probe — so a clean return is itself proof the element type is String).
     */
    public static long fillCheckStringArray(final String[] a)
    {
        refStrCalled = true;
        if (a == null) { refStrLen = -1; return NULL_RESULT; }
        refStrLen = a.length;
        final String e0 = "alpha";
        final String e2 = "gamma";
        boolean roundtrip = true;
        if (a.length > 0) { a[0] = e0; if (a[0] != e0) { roundtrip = false; } }
        if (a.length > 1) { a[1] = null; if (a[1] != null) { roundtrip = false; } }
        if (a.length > 2) { a[2] = e2; if (a[2] != e2) { roundtrip = false; } }
        int nonNull = 0;
        for (int i = 0; i < a.length; i++)
        {
            if (a[i] != null) { nonNull++; }
        }
        refStrNonNull = nonNull;
        refStrRoundtrip = roundtrip;
        return (long) nonNull;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MakeJavaArray.go && !MakeJavaArray.done;
            }

            @Override
            public void run()
            {
                final MakeJavaArray self = new MakeJavaArray();

                // (1) Fire the cycle hook once: a real bytecode dispatch so the
                //     native detour allocates every representative array, stores
                //     each into the matching recv* field, builds primitive arrays
                //     from C++ vectors and passes them into the sum*/check*
                //     verifiers, and feeds the reference arrays into the
                //     fillCheck* fillers.
                self.cycle();

                // (2) Snapshot what Java sees in each recv* field (pure Java
                //     bytecode: getfield + arraylength + getClass).
                captureAll();

                MakeJavaArray.done = true;
            }
        });
    }
}
