package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_static feature (area: methods).
 *
 * Exercises {@code static_method("name")->call(args)} for STATIC Java methods
 * that return every JVM primitive (Z B S C I J F D), {@code java.lang.String},
 * an object reference, and array references, and proves on a real JVM (genuine
 * bytecode dispatch via the Harness go/done handshake) that:
 *
 *   - the returned value_t decodes to the EXACT Java return value for each
 *     primitive width and boundary (min/max/-1/NaN/Inf/-0.0/65535 ...),
 *   - the String return decodes to the exact UTF-8 text (eager std::string
 *     alternative on both call paths),
 *   - the object / array return routes through the uint32/oop alternative (its
 *     full usability is JDK/call-path dependent and recorded as INFO, mirroring
 *     method_call_object),
 *   - NO RECEIVER is passed to a static method: the first declared argument
 *     occupies slot 0 (a {@code this} would have pushed it to slot 1), the
 *     proxy's receiver OOP is null, and a static method observably cannot and
 *     does not see any instance state,
 *   - EVERY primitive (Z B S C I J F D incl. the two-slot wide long/double),
 *     String, and object ARGUMENT round-trips: the static method echoes / records
 *     exactly the value it received, proving the C++ arg-packer maps each type to
 *     the right descriptor slot with no receiver shift,
 *   - OVERLOADED static methods (same name, distinct signatures) resolve to the
 *     RIGHT overload both by C++ argument TYPE and by an explicit
 *     static_method(name, signature) pin (distinct return sentinels per overload),
 *   - a static method that MUTATES a static field has its side effect observable,
 *   - {@code method_proxy::is_static()} returns TRUE for every static method and
 *     FALSE for every instance method (the accessor that reads JVM_ACC_STATIC
 *     straight from the live Method's _access_flags),
 *   - static_method() REJECTS an instance method of the same surface (the
 *     JVM_ACC_STATIC gate on the static get_method path), and an instance wrapper's
 *     get_method() of a STATIC method still dispatches statically (no receiver),
 *   - a STATIC method on an INTERFACE (Java 8+), on an ABSTRACT class, on an ENUM,
 *     and an INHERITED static (declared in a super, named through the sub — statics
 *     are NOT polymorphic, so the sub sees the super's declaration) all resolve and
 *     dispatch,
 *   - first touch of a class through a static call triggers its {@code <clinit>}
 *     (lazy class initialization) — characterized.
 *
 * Java 8 syntax only (no var / records / switch-expression / text blocks).
 * Only java.* + vmhook.Harness are referenced.
 *
 * Everything the native module asserts happens inside ONE run() invocation,
 * because method_proxy::call() requires the current JavaThread to be set, which
 * only holds on the Java thread while it executes inside the interpreter detour.
 * The native module hooks {@link #trigger(int)} and performs every static call
 * from within that detour.
 */
public final class MethodStatic
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ---- "no receiver" instrumentation -----------------------------------

    /**
     * A static method body cannot reference {@code this}.  To give the native
     * side a hard, observable proof that no receiver was injected, the static
     * recorder methods below stamp the arguments they ACTUALLY received into
     * these fields.  If a stray receiver had been pushed into slot 0, the
     * interpreter would have shifted every declared argument by one slot and
     * the recorded values would be wrong (or the call would mis-dispatch).
     */
    public static volatile int   recordedIntArg;
    public static volatile long  recordedLongArg;
    public static volatile int   recordedFirstOfThree;
    public static volatile long  recordedSecondOfThree;
    public static volatile int   recordedThirdOfThree;

    /** Counts how many times the static recorder ran (allow-through proof). */
    public static volatile int   staticRecorderHits;

    // ---- full-width single-argument echo recorders -----------------------
    // One static field per primitive + String + object, written by the matching
    // sEcho*/sRecord* method below so the native side can prove EACH argument
    // type landed intact at parameter slot 0 with no receiver shift.
    public static volatile boolean recordedBoolArg;
    public static volatile byte    recordedByteArg;
    public static volatile short   recordedShortArg;
    public static volatile char    recordedCharArg;
    public static volatile long    recordedEchoLong;   // sEchoLong only (distinct from recordedLongArg)
    public static volatile float   recordedFloatArg;
    public static volatile double  recordedDoubleArg;
    public static volatile String  recordedStringArg;
    /** identityHashCode of the last object argument sArgIdentity received. */
    public static volatile int     recordedObjectIdentity;

    /** A mutable static field a static method flips; the side effect is asserted. */
    public static volatile int     mutableState;

    /**
     * An INSTANCE field with a poison value.  A correctly-dispatched STATIC
     * method has no access to it; this exists only so the wrapper's
     * static-vs-instance story is concrete and to seed instance scenarios.
     */
    private int instancePoison = 0xBADF00D;

    /** Seed used by the instance returners (parity / is_static()==false set). */
    private int seed = 4242;

    // ---- Trigger (hooked; the detour runs every native static call) -------

    /** Hookable instance method; the native module hooks this to get a frame. */
    public int trigger(final int delta)
    {
        return this.seed + delta;
    }

    // ======================================================================
    //  STATIC primitive returners — one per type, at representative
    //  boundaries.  Names are unique so static_method("name") is unambiguous.
    // ======================================================================

    public static boolean sBoolTrue()   { return true; }
    public static boolean sBoolFalse()  { return false; }

    public static byte sByteMax()       { return Byte.MAX_VALUE; }   // 127
    public static byte sByteMin()       { return Byte.MIN_VALUE; }   // -128
    public static byte sByteNegOne()    { return (byte) -1; }

    public static short sShortMax()     { return Short.MAX_VALUE; }  // 32767
    public static short sShortMin()     { return Short.MIN_VALUE; }  // -32768
    public static short sShortNegOne()  { return (short) -1; }

    public static char sCharA()         { return 'A'; }             // 65
    public static char sCharMax()       { return Character.MAX_VALUE; } // 65535

    public static int sIntMax()         { return Integer.MAX_VALUE; }
    public static int sIntMin()         { return Integer.MIN_VALUE; }
    public static int sIntFortyTwo()    { return 42; }
    public static int sIntNegOne()      { return -1; }

    public static long sLongMax()       { return Long.MAX_VALUE; }
    public static long sLongMin()       { return Long.MIN_VALUE; }
    public static long sLongBig()       { return 0x0123456789ABCDEFL; }

    public static float sFloatHalf()    { return 0.5f; }
    public static float sFloatNegZero() { return -0.0f; }
    public static float sFloatNaN()     { return Float.NaN; }
    public static float sFloatPosInf()  { return Float.POSITIVE_INFINITY; }
    public static float sFloatMax()     { return Float.MAX_VALUE; }

    public static double sDoublePi()    { return 3.141592653589793; }
    public static double sDoubleNegZero() { return -0.0d; }
    public static double sDoubleNaN()   { return Double.NaN; }
    public static double sDoubleNegInf() { return Double.NEGATIVE_INFINITY; }
    public static double sDoubleMax()   { return Double.MAX_VALUE; }

    public static void sVoidBump()      { ++staticRecorderHits; }

    // ======================================================================
    //  STATIC String + object returners.
    // ======================================================================

    /** Static String return; exact ASCII payload the native side pins. */
    public static String sStringHello() { return "hello-static"; }

    /** Static String return with non-ASCII payload (UTF-8: café). */
    public static String sStringUnicode() { return "café"; }

    /** Static String return that is empty (distinct from null / void). */
    public static String sStringEmpty() { return ""; }

    /** Static String return that is null (decodes to empty, never crashes). */
    public static String sStringNull()  { return null; }

    /** Static object return: a fresh tagged child. */
    public static MethodStatic sMakeChild()
    {
        final MethodStatic child = new MethodStatic();
        child.seed = STATIC_CHILD_SEED;
        return child;
    }

    /** Static object return that is null (must yield a null unique_ptr). */
    public static MethodStatic sNullChild() { return null; }

    public static final int STATIC_CHILD_SEED = 9090;

    // ======================================================================
    //  STATIC ARRAY returners — the '[I' / '[Ljava/lang/String;' /
    //  '[Ljava/lang/Object;' reference-return branches of value_t.
    // ======================================================================

    /** Length + element sentinels for the int[] returner (native mirrors them). */
    public static final int  ARR_INT_LEN  = 4;
    public static final int  ARR_INT_0    = 10;
    public static final int  ARR_INT_1    = 20;
    public static final int  ARR_INT_2    = 30;
    public static final int  ARR_INT_3    = 40;

    /** Static int[] return ('[I'). */
    public static int[] sIntArray()
    {
        return new int[] { ARR_INT_0, ARR_INT_1, ARR_INT_2, ARR_INT_3 };
    }

    /** Static String[] return ('[Ljava/lang/String;'). */
    public static String[] sStringArray()
    {
        return new String[] { "a0", "a1", "a2" };
    }

    /** Static Object[] return ('[Ljava/lang/Object;') holding tagged children. */
    public static Object[] sObjectArray()
    {
        return new Object[] { sMakeChild(), sMakeChild() };
    }

    /** Static array return that is null (must yield a null unique_ptr). */
    public static int[] sNullArray() { return null; }

    // ======================================================================
    //  STATIC argument-passing methods — these PROVE no receiver is passed
    //  AND that EVERY argument type round-trips through the C++ arg-packer.
    // ======================================================================

    /**
     * Echoes its only argument.  If a {@code this} had been injected into slot
     * 0, the interpreter would read the receiver as the int and return garbage
     * (or the call_stub param_count would be off by one).  A correct static
     * dispatch returns exactly {@code v}.
     */
    public static int sEchoInt(final int v) { return v; }

    // One echo returner per remaining primitive type + String.  Each RETURNS its
    // argument so the native side proves the value survived the C++->JVM packing
    // for that descriptor (Z B S C J F D, Ljava/lang/String;).
    public static boolean sEchoBool(final boolean v)   { recordedBoolArg = v;   return v; }
    public static byte    sEchoByte(final byte v)      { recordedByteArg = v;   return v; }
    public static short   sEchoShort(final short v)    { recordedShortArg = v;  return v; }
    public static char    sEchoChar(final char v)      { recordedCharArg = v;   return v; }
    public static long    sEchoLong(final long v)      { recordedEchoLong = v;  return v; }
    public static float   sEchoFloat(final float v)    { recordedFloatArg = v;  return v; }
    public static double  sEchoDouble(final double v)  { recordedDoubleArg = v; return v; }
    public static String  sEchoString(final String v)  { recordedStringArg = v; return v; }

    /**
     * Records its single int argument into a static field, so the native side
     * confirms the value arrived intact at parameter slot 0 (no leading
     * receiver slot).  Also bumps the allow-through hit counter.
     */
    public static void sRecordInt(final int v)
    {
        recordedIntArg = v;
        ++staticRecorderHits;
    }

    /** Records a long arg (2 interpreter slots) at slot 0 — no receiver shift. */
    public static long sRecordLong(final long v)
    {
        recordedLongArg = v;
        ++staticRecorderHits;
        return v;
    }

    /**
     * Multi-arg static recorder: first int at slot 0, long across slots 1-2,
     * trailing int at slot 3.  With a phantom receiver every one of these would
     * be shifted by a slot.  Returns the sum so the native side double-checks.
     */
    public static long sRecordThree(final int a, final long b, final int c)
    {
        recordedFirstOfThree  = a;
        recordedSecondOfThree = b;
        recordedThirdOfThree  = c;
        ++staticRecorderHits;
        return (long) a + b + c;
    }

    /**
     * Returns the identity hash of the single OBJECT argument it receives, and
     * also stamps it into recordedObjectIdentity.  Used so the native side can
     * confirm the object argument (not a receiver) reached the static method as
     * parameter zero — a returned 0 means the arg was null.
     */
    public static int sArgIdentity(final MethodStatic arg)
    {
        final int id = (arg == null) ? 0 : System.identityHashCode(arg);
        recordedObjectIdentity = id;
        return id;
    }

    /** Returns the length of an int[] argument (-1 for null) — array arg echo. */
    public static int sArrayLen(final int[] arg)
    {
        return (arg == null) ? -1 : arg.length;
    }

    /** Sets mutableState to v and returns the PRIOR value — observable mutation. */
    public static int sMutateState(final int v)
    {
        final int prior = mutableState;
        mutableState = v;
        ++staticRecorderHits;
        return prior;
    }

    // ======================================================================
    //  MULTI-ARGUMENT slot-shape methods.  Each computes a DETERMINISTIC
    //  digest of all its arguments and RETURNS it, so the native side proves
    //  the C++ arg-packer maps every argument to the right slot (and, for
    //  long/double, the right WIDE pair) with no receiver shift — and does so
    //  PATH-INDEPENDENTLY (a primitive return is bit-identical on the call_stub
    //  and call_jni paths, so these are all hard-asserted on every JDK).
    // ======================================================================

    /** Two ints; returns a*1000003 + b so order/slot swaps are detectable. */
    public static int  sSumII(final int a, final int b)            { return a * 1000003 + b; }

    /** Two longs; mixes both so a transposition changes the result. */
    public static long sSumJJ(final long a, final long b)          { return a * 1000003L + b; }

    /** Two doubles; returns their raw-mixed sum (bit-checked by the caller). */
    public static double sSumDD(final double a, final double b)    { return a * 4.0 + b; }

    /** Two floats; returns a mix that is order-sensitive. */
    public static float sSumFF(final float a, final float b)       { return a * 4.0f + b; }

    /** Two booleans; returns their XOR (slot-0 vs slot-1 packing of Z Z). */
    public static boolean sBoolXor(final boolean a, final boolean b) { return a ^ b; }

    /**
     * Mixed float / int / double in declaration order — exercises the packer
     * interleaving a 1-slot float, a 1-slot int, and a 2-slot double.  Returns a
     * double digest that depends on each argument's value AND position.
     */
    public static double sMixFID(final float f, final int i, final double d)
    {
        return (double) f * 1000.0 + (double) i * 7.0 + d;
    }

    /**
     * Every narrow primitive plus a WIDE long, in one call: Z B S C I J.  Returns
     * a long digest where each argument occupies disjoint bit/scale ranges so any
     * mis-slotting (especially the long's two-slot pair after the narrow args) is
     * caught.
     */
    public static long sPackPrims(final boolean z, final byte b, final short s,
                                  final char c, final int i, final long j)
    {
        long acc = 0L;
        acc += z ? 1L : 0L;
        acc += ((long) b) << 1;        // -7 -> contributes a signed term
        acc += ((long) s) << 9;        // signed short
        acc += ((long) c) << 25;       // unsigned char (zero-extended)
        acc += ((long) i) << 41;       // int term
        acc ^= j;                       // long term mixed in via xor
        return acc;
    }

    /** Seven ints — the maximum C++ arg fan-out the packer slot array allows for
     *  a static call (no receiver).  Returns a position-weighted sum. */
    public static int s7Ints(final int a, final int b, final int c, final int d,
                             final int e, final int f, final int g)
    {
        return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7;
    }

    // ======================================================================
    //  BATCH-18 DEEPENING — additional slot-shape / boundary methods.
    //  Every method here RETURNS a deterministic digest of its arguments so the
    //  native side proves the C++ arg-packer mapped each value to the right
    //  slot (and, for long/double, the right WIDE pair) with no receiver shift.
    //  All RETURNS are primitive, hence bit-identical on the call_stub and
    //  call_jni paths => hard-asserted on every JDK.
    // ======================================================================

    /**
     * Eight ints — EXACTLY the maximum argument fan-out both dispatch paths
     * support for a static call (the call-stub params[8] slot array and the
     * call_jni jvalue[8] array are each capped at 8, enforced at compile time;
     * a static call has no receiver so all eight slots carry real arguments).
     * Returns a position-weighted sum so any single mis-slot is detectable.
     */
    public static int s8Ints(final int a, final int b, final int c, final int d,
                             final int e, final int f, final int g, final int h)
    {
        return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
    }

    /**
     * Wide/narrow interleave: long, int, long, int — four PACKED arg values, two
     * of them WIDE (each long occupies two interpreter locals but one C++ pack
     * slot).  Returns a digest where each argument lands in a disjoint range so a
     * transposition of either wide pair, or of either narrow int, changes the
     * result.  Complements sPackPrims (which trails its single long).
     */
    public static long sWideShape(final long p, final int q, final long r, final int s)
    {
        return p * 7L + ((long) q << 3) + r * 31L + ((long) s << 50);
    }

    /**
     * Three doubles — three consecutive WIDE pairs, no narrow slot between them.
     * Returns a position-weighted mix (all terms exactly representable) so the
     * packer's handling of back-to-back two-slot arguments is bit-checked.
     */
    public static double sThreeD(final double a, final double b, final double c)
    {
        return a * 64.0 + b * 8.0 + c;
    }

    /**
     * Reuse-the-proxy probe: a plain int echo with a DIFFERENT name from
     * sEchoInt so a single resolved proxy can be call()'d twice with distinct
     * arguments and each result checked — proving a method_proxy is not
     * single-shot.  Returns its argument unchanged.
     */
    public static int sEchoInt2(final int v) { return v; }

    /**
     * Records the identity hash of an object argument exactly like sArgIdentity,
     * but is a SECOND method so a child returned by one static call can be fed
     * back in as the argument of another (proving an oop the library itself
     * decoded round-trips back through the arg-packer).  Returns the identity
     * (0 for null).
     */
    public static int sArgIdentity2(final Object arg)
    {
        return (arg == null) ? 0 : System.identityHashCode(arg);
    }

    /** Length of a String argument (-1 for null) — distinct null-String-ARG path
     *  from the null-String-RETURN cases.  A null arg must yield -1, NOT crash. */
    public static int sStringArgLen(final String arg)
    {
        return (arg == null) ? -1 : arg.length();
    }

    // ======================================================================
    //  OVERLOADED static methods — all named "sPoly", told apart by signature.
    //  Each returns a DISTINCT sentinel so the native side proves WHICH overload
    //  resolved, both by C++ argument type and by an explicit signature pin.
    //  Declaration order is scrambled relative to descriptor sort order so the
    //  resolver cannot depend on source order.
    // ======================================================================

    public static final int POLY_INT    = 7001;  // sPoly(int)      -> (I)I
    public static final int POLY_LONG   = 7002;  // sPoly(long)     -> (J)I
    public static final int POLY_DOUBLE = 7003;  // sPoly(double)   -> (D)I
    public static final int POLY_STRING = 7004;  // sPoly(String)   -> (Ljava/lang/String;)I
    public static final int POLY_INT2   = 7005;  // sPoly(int,int)  -> (II)I
    public static final int POLY_FLOAT  = 7006;  // sPoly(float)    -> (F)I
    public static final int POLY_INTLONG = 7007; // sPoly(int,long) -> (IJ)I

    public static int sPoly(final int a)               { return POLY_INT; }
    public static int sPoly(final String a)            { return POLY_STRING; }
    public static int sPoly(final long a)              { return POLY_LONG; }
    public static int sPoly(final double a)            { return POLY_DOUBLE; }
    public static int sPoly(final float a)             { return POLY_FLOAT; }
    public static int sPoly(final int a, final int b)  { return POLY_INT2; }
    public static int sPoly(final int a, final long b) { return POLY_INTLONG; }

    // ======================================================================
    //  INSTANCE methods — used to prove is_static() == FALSE for non-static,
    //  and that static_method() REJECTS an instance method.  Names are
    //  deliberately distinct from the static ones so the native side can target
    //  each precisely.
    // ======================================================================

    /** Instance int returner (is_static() must be false). */
    public int iGetSeed() { return this.seed; }

    /** Instance String returner (is_static() must be false). */
    public String iLabel() { return "instance-label"; }

    /** Instance method that takes an int (is_static() must be false). */
    public int iEcho(final int v) { return this.seed + v; }

    /** Instance void method (is_static() must be false). */
    public void iTouch() { this.instancePoison = 0; }

    // ======================================================================
    //  INHERITED-STATIC: a base class declares a static method; this class
    //  does NOT redeclare it.  Statics are NOT polymorphic — naming it through
    //  the SUB resolves the SUPER's single declaration (the superclass-chain
    //  walk in the static get_method path).  StaticSub extends StaticBase so the
    //  native side can register a wrapper for the SUB and still reach the
    //  inherited static.
    // ======================================================================

    public static class StaticBase
    {
        public static final int BASE_STATIC_VALUE = 0x5151;  // 20817

        /** Declared ONLY here; reachable through StaticSub via inheritance. */
        public static int baseStaticValue() { return BASE_STATIC_VALUE; }

        /** An instance method, so a StaticSub wrapper has a non-static to check. */
        public int baseInstanceSeed() { return 1717; }
    }

    /** Subclass that adds NO static of its own — inherits baseStaticValue(). */
    public static class StaticSub extends StaticBase
    {
        public static final int SUB_STATIC_VALUE = 0x6262;   // 25186

        /** A static declared on the SUB itself (distinct from the inherited one). */
        public static int subStaticValue() { return SUB_STATIC_VALUE; }
    }

    // ======================================================================
    //  ABSTRACT class with a STATIC method (statics are independent of the
    //  abstract-ness; the static dispatches without any instance).
    // ======================================================================

    public abstract static class AbstractWithStatic
    {
        public static final int ABSTRACT_STATIC_VALUE = 0x7373;  // 29555

        public static int abstractStaticValue() { return ABSTRACT_STATIC_VALUE; }

        /** The abstract instance method — never called by the native side. */
        public abstract int mustImplement();
    }

    // ======================================================================
    //  INTERFACE with a STATIC method (Java 8+).  A static interface method
    //  lives on the interface's own _methods array; the native side registers a
    //  wrapper for the INTERFACE itself and resolves it directly (it is NOT
    //  inherited by implementors, by JLS, so resolving it through the interface
    //  klass is the only correct path).
    // ======================================================================

    public interface StaticIface
    {
        int IFACE_STATIC_VALUE = 0x8484;  // 33924  (interface fields are implicitly static final)

        static int ifaceStaticValue() { return IFACE_STATIC_VALUE; }

        /**
         * An interface DEFAULT method (Java 8+).  A default method is a NON-static
         * INSTANCE method that lives on the interface's own _methods array, so it
         * is exactly the trap the static-resolution gate must reject:
         * static_method("ifaceDefaultValue") on the interface wrapper must return
         * nullopt (the JVM_ACC_STATIC filter skips it), even though a name-only
         * scan of the interface klass would otherwise match it.
         */
        default int ifaceDefaultValue() { return 0x4242; }
    }

    // ======================================================================
    //  ENUM with a STATIC method (in addition to the implicit values()/valueOf).
    // ======================================================================

    public enum StaticEnum
    {
        ALPHA, BETA, GAMMA;

        public static final int ENUM_STATIC_VALUE = 0x9595;  // 38293

        public static int enumStaticValue() { return ENUM_STATIC_VALUE; }
    }

    // ======================================================================
    //  <clinit> characterization: a class whose static initializer flips a
    //  sentinel.  The probe LOADS it without initializing (Class.forName with
    //  initialize=false) so the native wrapper can resolve the klass, but its
    //  <clinit> stays pending until the FIRST static call touches a static
    //  member — at which point clinitValue() returns the post-<clinit> sentinel.
    // ======================================================================

    public static final class ClinitProbe
    {
        /** Set to true only by the static initializer below. */
        public static volatile boolean initialized;

        /** The value <clinit> stamps; clinitValue() returns it. */
        public static final int CLINIT_VALUE = 0xC11C;  // 49436

        static
        {
            initialized = true;
        }

        /** First call triggers <clinit>; returns the post-init sentinel. */
        public static int clinitValue() { return CLINIT_VALUE; }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodStatic.go && !MethodStatic.done;
            }

            @Override
            public void run()
            {
                // Reset allow-through counters so the per-cycle assertions are
                // deterministic, THEN drive trigger() through normal bytecode
                // dispatch.  That fires the native interpreter detour, and the
                // detour performs every static_method(...).call() the module
                // asserts on (the only context where current_java_thread is set,
                // which method_proxy::call() requires).
                staticRecorderHits = 0;
                recordedIntArg = 0;
                recordedLongArg = 0L;
                recordedFirstOfThree = 0;
                recordedSecondOfThree = 0L;
                recordedThirdOfThree = 0;
                mutableState = 0;

                // Make sure the auxiliary klasses the native side wraps are
                // LOADED (so register_class<>'s ClassLoaderDataGraph walk finds
                // them) before the detour resolves them.  Referencing a static
                // field both loads AND initializes these — that is intentional
                // for the inherited / abstract / interface / enum cases (their
                // static methods are about RESOLUTION + DISPATCH, not <clinit>
                // timing).
                int warm = 0;
                warm += StaticSub.SUB_STATIC_VALUE;
                warm += StaticBase.BASE_STATIC_VALUE;
                warm += AbstractWithStatic.ABSTRACT_STATIC_VALUE;
                warm += StaticIface.IFACE_STATIC_VALUE;
                warm += StaticEnum.ENUM_STATIC_VALUE;
                if (warm == Integer.MIN_VALUE) { throw new IllegalStateException("unreachable"); }

                // The <clinit> probe is handled differently: LOAD it without
                // INITIALIZING (initialize=false) so its <clinit> stays pending
                // for the native side's first static call to trigger.  The class
                // must be loaded so register_class<> can resolve the klass.
                try
                {
                    Class.forName("vmhook.fixtures.MethodStatic$ClinitProbe",
                                  false, MethodStatic.class.getClassLoader());
                }
                catch (final ClassNotFoundException ex)
                {
                    System.err.println("ClinitProbe load failed: " + ex);
                }

                final MethodStatic instance = new MethodStatic();
                instance.trigger(7);
                MethodStatic.done = true;
            }
        });
    }
}
