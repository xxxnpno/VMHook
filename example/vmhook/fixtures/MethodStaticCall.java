package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_proxy_static_method_portability" feature (area: methods).
 *
 * Where the sibling {@code MethodStatic} fixture pins the static-CALL value
 * contract through ONE wrapper, this fixture is laser-focused on the
 * PORTABILITY of the static-dispatch path itself: the portable
 * {@code static_method("name")} / {@code static_method("name","sig")} factories
 * (available on every compiler, no deducing-this required) feeding the
 * no-receiver call gate (call_stub fast path on JDK 8-20, CallStatic&lt;T&gt;MethodA
 * call_jni fallback on JDK 21+).  It proves, on a real JVM via genuine bytecode
 * dispatch (the Harness go/done handshake), that for EVERY return type and EVERY
 * argument shape the static path:
 *
 *   - returns / side-effects the EXACT Java value for void, Z B C S I J F D,
 *     java.lang.String (ASCII, UTF-8, empty, null) and an object reference (null
 *     yields a null unique_ptr on every path; full usability is call-path
 *     dependent and recorded as INFO, mirroring method_call_object),
 *   - passes NO receiver: the first declared argument lands at parameter slot 0
 *     (a phantom {@code this} would shift every arg by a slot), proven by static
 *     recorder methods that stamp the args they actually saw, and across the
 *     long+double TWO-SLOT boundary,
 *   - resolves OVERLOADED static methods by the C++ argument type even when the
 *     name alone is ambiguous.  This is the recently-FIXED crash: before
 *     resolve_compatible_method() learned to derive a static method's declaring
 *     klass from the Method's ConstantPool _pool_holder, a primitive blasted into
 *     a reference parameter slot (or vice-versa) tore the JVM down with an AV.
 *     The {@code sOver(...)} family below has a primitive-vs-reference overload
 *     set so a mis-resolution would be exactly that AV — a correct sentinel
 *     proves the fix,
 *   - and the portable {@code static_method(name,sig)} explicit-signature path
 *     dispatches the pinned overload verbatim.
 *
 * A deliberately primitive-ONLY overload set ({@code sNum(int|long|double)}) lets
 * the native side probe "call with an arg matching NO overload" (a float, which
 * maps to descriptor F) without risking a reference-slot AV: the fallback lands
 * in a primitive slot, so the worst case is a wrong value, never a process tear
 * down.  The native module characterizes that outcome rather than asserting it.
 *
 * Java 8 syntax only (no var / records / switch-expressions / text blocks).
 * Only java.* + vmhook.Harness are referenced.
 *
 * Everything the native module asserts happens inside ONE trigger(int) detour,
 * because method_proxy::call() requires the current JavaThread to be set, which
 * only holds on the Java thread while it executes inside the interpreter detour.
 */
public final class MethodStaticCall
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ---- "no receiver" instrumentation -----------------------------------
    //
    // A static method body cannot reference `this`.  The recorder methods below
    // stamp the arguments they ACTUALLY received into these fields; if a stray
    // receiver had been pushed into slot 0 the interpreter would have shifted
    // every declared argument by one slot and the recorded values would be wrong
    // (or the call would mis-dispatch entirely).
    public static volatile long   recordedLongArg;
    public static volatile double recordedDoubleArg;
    public static volatile int    recordedFirstOfFour;     // int   at slot 0
    public static volatile long   recordedSecondOfFour;    // long  across slots 1-2
    public static volatile double recordedThirdOfFour;     // double across slots 3-4
    public static volatile int    recordedFourthOfFour;    // int   at slot 5
    public static volatile long   recordedTwoSlotLong;     // long  arg of (long,double)
    public static volatile double recordedTwoSlotDouble;   // double arg of (long,double)
    public static volatile int    recordedObjSeed;         // seed read off an object arg
    public static volatile boolean recordedBoolArg;        // bool arg (1 slot, written as int 0/1)
    public static volatile float   recordedFloatArg;       // float arg (1 slot, IEEE-exact bits)

    /** Counts how many times any static recorder ran (allow-through proof). */
    public static volatile int    staticRecorderHits;

    /** Seed used by instance returners and by object args/returns.  The native
     *  side reads it via get_field("seed") to prove a returned wrapper is usable. */
    private int seed = 4242;

    public static final int STATIC_CHILD_SEED = 7373;

    // ---- Trigger (hooked; the detour runs every native static call) -------

    /** Hookable instance method; the native module hooks this to get a frame. */
    public int trigger(final int delta)
    {
        return this.seed + delta;
    }

    // ======================================================================
    //  STATIC returners — one per JVM return type, at representative
    //  boundaries.  Names are unique so static_method("name") is unambiguous;
    //  each also has a known, fixed JVM descriptor for the (name,sig) path.
    // ======================================================================

    public static void    sVoid()       { ++staticRecorderHits; }   // ()V

    public static boolean sBoolTrue()   { return true; }
    public static boolean sBoolFalse()  { return false; }

    public static byte    sByteMax()    { return Byte.MAX_VALUE; }   // 127
    public static byte    sByteMin()    { return Byte.MIN_VALUE; }   // -128
    public static byte    sByteNegOne() { return (byte) -1; }

    public static char    sCharA()      { return 'A'; }             // 65
    public static char    sCharMax()    { return Character.MAX_VALUE; } // 65535

    public static short   sShortMax()   { return Short.MAX_VALUE; }  // 32767
    public static short   sShortMin()   { return Short.MIN_VALUE; }  // -32768
    public static short   sShortNegOne(){ return (short) -1; }

    public static int     sIntMax()     { return Integer.MAX_VALUE; }
    public static int     sIntMin()     { return Integer.MIN_VALUE; }
    public static int     sIntFortyTwo(){ return 42; }
    public static int     sIntNegOne()  { return -1; }

    public static long    sLongMax()    { return Long.MAX_VALUE; }
    public static long    sLongMin()    { return Long.MIN_VALUE; }
    public static long    sLongBig()    { return 0x0123456789ABCDEFL; }

    public static float   sFloatHalf()  { return 0.5f; }
    public static float   sFloatNegZero() { return -0.0f; }
    public static float   sFloatNaN()   { return Float.NaN; }
    public static float   sFloatPosInf(){ return Float.POSITIVE_INFINITY; }
    public static float   sFloatMax()   { return Float.MAX_VALUE; }

    public static double  sDoublePi()   { return 3.141592653589793; }
    public static double  sDoubleNegZero() { return -0.0d; }
    public static double  sDoubleNaN()  { return Double.NaN; }
    public static double  sDoubleNegInf() { return Double.NEGATIVE_INFINITY; }
    public static double  sDoubleMax()  { return Double.MAX_VALUE; }

    // ---- String + object returners ---------------------------------------

    public static String  sStringHello()   { return "hello-portable"; }
    public static String  sStringUnicode() { return "café"; }          // UTF-8
    public static String  sStringEmpty()   { return ""; }
    public static String  sStringNull()    { return null; }

    /** Static object return: a fresh tagged child (seed = STATIC_CHILD_SEED). */
    public static MethodStaticCall sMakeChild()
    {
        final MethodStaticCall child = new MethodStaticCall();
        child.seed = STATIC_CHILD_SEED;
        return child;
    }

    /** Static object return that is null (must yield a null unique_ptr). */
    public static MethodStaticCall sNullChild() { return null; }

    // ======================================================================
    //  ARGUMENT-SHAPE methods — these PROVE no receiver is passed AND that
    //  every argument kind round-trips at the right slot.
    // ======================================================================

    /** No-arg returner already covered by sIntFortyTwo(); add a no-arg long. */
    public static long sZeroArgLong() { return 0x7766554433221100L; }

    /**
     * Echoes its only int argument.  A phantom receiver in slot 0 would make the
     * interpreter read the receiver as the int and return garbage; a correct
     * static dispatch returns exactly {@code v}.
     */
    public static int  sEchoInt(final int v) { return v; }

    /** Echoes a long (two interpreter slots) — proves slot-0 long alignment. */
    public static long sEchoLong(final long v) { return v; }

    /** Echoes a double (two interpreter slots). */
    public static double sEchoDouble(final double v) { return v; }

    /** Echoes a String argument straight back (object/reference arg shape). */
    public static String sEchoString(final String v) { return v; }

    /**
     * Records a boolean arg (one interpreter slot, the JVM writes the value as a
     * full int 0/1) at slot 0 — no receiver shift.  Returns it so the native side
     * can assert both the return value and the recorded field.
     */
    public static boolean sEchoBool(final boolean v)
    {
        recordedBoolArg = v;
        ++staticRecorderHits;
        return v;
    }

    /**
     * Records a float arg (one interpreter slot) at slot 0 — no receiver shift.
     * Returns it so the native side can assert IEEE-exact bits (not a value that
     * may differ after a float-double-float widening round trip).
     */
    public static float sEchoFloat(final float v)
    {
        recordedFloatArg = v;
        ++staticRecorderHits;
        return v;
    }

    /** Records a long arg (2 slots) at slot 0 — no receiver shift. Returns it. */
    public static long sRecordLong(final long v)
    {
        recordedLongArg = v;
        ++staticRecorderHits;
        return v;
    }

    /** Records a double arg (2 slots) at slot 0 — no receiver shift. Returns it. */
    public static double sRecordDouble(final double v)
    {
        recordedDoubleArg = v;
        ++staticRecorderHits;
        return v;
    }

    /**
     * Multi-arg recorder spanning BOTH two-slot kinds: int at slot 0, long
     * across slots 1-2, double across slots 3-4, trailing int at slot 5.  With a
     * phantom receiver every one of these would be shifted by a slot.  Returns a
     * mixing of all four so the native side double-checks the whole frame.
     */
    public static long sRecordFour(final int a, final long b, final double c, final int d)
    {
        recordedFirstOfFour  = a;
        recordedSecondOfFour = b;
        recordedThirdOfFour  = c;
        recordedFourthOfFour = d;
        ++staticRecorderHits;
        return (long) a + b + (long) c + (long) d;
    }

    /**
     * The long+double two-slot boundary in isolation: a long (slots 0-1) followed
     * by a double (slots 2-3), both recorded and summed.  If either two-slot
     * value were mis-aligned (e.g. a receiver consuming slot 0) the recorded
     * fields and the sum would be wrong.
     */
    public static double sLongDouble(final long lv, final double dv)
    {
        recordedTwoSlotLong   = lv;
        recordedTwoSlotDouble = dv;
        ++staticRecorderHits;
        return (double) lv + dv;
    }

    /**
     * Takes an object argument (a MethodStaticCall) and returns its seed, so the
     * native side proves an object reference reached the static method as
     * parameter zero (no receiver in front of it).  Null -> -1 sentinel.
     */
    public static int sReadObjSeed(final MethodStaticCall arg)
    {
        if (arg == null)
        {
            recordedObjSeed = -1;
            ++staticRecorderHits;
            return -1;
        }
        recordedObjSeed = arg.seed;
        ++staticRecorderHits;
        return arg.seed;
    }

    // ======================================================================
    //  OVERLOADED static methods — the recently-FIXED crash path.
    //
    //  sOver(...) shares ONE name across primitive AND reference parameter
    //  kinds.  static_method("sOver") latches whatever overload appears first in
    //  the InstanceKlass._methods array (Symbol-identity order, effectively
    //  arbitrary across builds); resolve_compatible_method() must then re-pick
    //  the overload matching the C++ argument TYPE.  Each overload returns a
    //  DISTINCT sentinel so the native side proves WHICH was chosen.  A
    //  mis-resolution that put a primitive into the (String)/(object) slot used
    //  to AV the JVM (reference-store barrier on a bogus oop) — so a correct
    //  sentinel here is a direct proof the _pool_holder fix works.
    // ======================================================================

    public static final int OVER_INT    = 5001;
    public static final int OVER_LONG   = 5002;
    public static final int OVER_DOUBLE = 5003;
    public static final int OVER_STRING = 5004;
    public static final int OVER_OBJECT = 5005;

    public static int sOver(final int v)             { return OVER_INT; }
    public static int sOver(final long v)            { return OVER_LONG; }
    public static int sOver(final double v)          { return OVER_DOUBLE; }
    public static int sOver(final String v)          { return OVER_STRING; }
    public static int sOver(final MethodStaticCall v){ return OVER_OBJECT; }

    // ======================================================================
    //  PRIMITIVE-ONLY overload set — used for the "arg matches NO overload"
    //  characterization.  sNum has int/long/double overloads ONLY (no float,
    //  no reference).  Calling it with a C++ float (descriptor F) matches none;
    //  the fallback lands in a PRIMITIVE slot, so it can never trigger the
    //  reference-slot AV — the worst case is a wrong int, which the native side
    //  records (not asserts).
    // ======================================================================

    public static final int NUM_INT    = 6001;
    public static final int NUM_LONG   = 6002;
    public static final int NUM_DOUBLE = 6003;

    public static int sNum(final int v)    { return NUM_INT; }
    public static int sNum(final long v)   { return NUM_LONG; }
    public static int sNum(final double v) { return NUM_DOUBLE; }

    // ======================================================================
    //  INSTANCE methods — only to prove is_static()==FALSE for non-static via
    //  the portable path (get_method through the live receiver).
    // ======================================================================

    public int iGetSeed()           { return this.seed; }
    public int iEcho(final int v)   { return this.seed + v; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodStaticCall.go && !MethodStaticCall.done;
            }

            @Override
            public void run()
            {
                // Reset allow-through counters so per-cycle assertions are
                // deterministic, THEN drive trigger() through normal bytecode
                // dispatch.  That fires the native interpreter detour, which
                // performs every static_method(...).call() the module asserts on
                // (the only context where the current JavaThread is set, which
                // method_proxy::call() requires).
                staticRecorderHits    = 0;
                recordedLongArg       = 0L;
                recordedDoubleArg     = 0.0d;
                recordedFirstOfFour   = 0;
                recordedSecondOfFour  = 0L;
                recordedThirdOfFour   = 0.0d;
                recordedFourthOfFour  = 0;
                recordedTwoSlotLong   = 0L;
                recordedTwoSlotDouble = 0.0d;
                recordedObjSeed       = 0;
                recordedBoolArg       = false;
                recordedFloatArg      = 0.0f;

                final MethodStaticCall instance = new MethodStaticCall();
                instance.trigger(7);
                MethodStaticCall.done = true;
            }
        });
    }
}
