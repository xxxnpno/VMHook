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
 *       write path), so the array becomes reachable from real Java bytecode.</li>
 * </ol>
 * After the detour returns, the probe calls {@link #captureAll()} — pure Java
 * bytecode — which reads, for every {@code recv*} that was filled, the array's
 * {@code .length} and its {@code getClass().getName()} into the {@code obsLen*} /
 * {@code obsType*} witness fields the native side then reads back.  This proves
 * the made array is a REAL, usable Java array (the klass stamp and the _length
 * field are both Java-correct), not merely a native byte blob.
 *
 * <p>The bulk of the exhaustive coverage (every descriptor at length 0/1/3/256,
 * negative-length guard, malformed descriptors) is asserted NATIVELY inside the
 * detour via make_java_array + array_length + element round-trips; this fixture's
 * job is the Java-visible witness for one representative length per descriptor.</p>
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
 * javac 8 and javac 25 with any -encoding.</p>
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

    // =====================================================================
    //  Hooked method — the native detour anchor.
    // =====================================================================

    /**
     * No-arg trigger the native module hooks.  Calling it on a real bytecode
     * dispatch fires the interpreter hook; inside that detour the native side
     * allocates every representative array via make_java_array and stores each
     * into the matching {@code recv*} field.  Just bumps a counter so the
     * native side can confirm the hook fired.
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
                //     native detour allocates every representative array and
                //     stores each into the matching recv* field.
                self.cycle();

                // (2) Snapshot what Java sees in each recv* field (pure Java
                //     bytecode: getfield + arraylength + getClass).
                captureAll();

                MakeJavaArray.done = true;
            }
        });
    }
}
