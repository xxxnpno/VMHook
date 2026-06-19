package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_call_jni_fallback" feature: exercises
 * vmhook::method_proxy::call() over EVERY return type and argument shape, with
 * the deliberate intent of driving the JNI invocation FALLBACK path
 * (method_proxy::call_jni, vmhook.hpp ~12488-13064).
 *
 * WHY THIS HITS call_jni: method_proxy::call() first probes
 * detail::find_call_stub_entry() (StubRoutines::_call_stub_entry).  When that
 * VMStruct is present (typically JDK 8..20) call() dispatches through the
 * interpreter call-stub fast path; when it is ABSENT (JDK 21+, and in fact on
 * every JDK the CI exercises, where the entry is not exported via VMStructs)
 * call() short-circuits into call_jni(), which marshals args into a jvalue[]
 * and dispatches via Call(Static)?<Type>MethodA.  The native module records
 * which path is live (find_call_stub_entry) so the same assertions are valid on
 * either dispatcher — the converted value_t must be identical.
 *
 * Coverage shape (every return type x arg shape the audit's scenario list
 * enumerates):
 *   - return types : void / boolean(Z) / byte(B) / char(C) / short(S) / int(I)
 *                    / long(J) / float(F) / double(D) / String / Object,
 *   - arg shapes   : no-arg, single primitive, String arg, Object arg,
 *                    MULTI-ARG including long + double (each occupies TWO local
 *                    slots on the interpreter path; ONE jvalue cell on the JNI
 *                    path — the marshaller must agree either way),
 *   - dispatch kind: INSTANCE (Call<Type>MethodA) AND STATIC
 *                    (CallStatic<Type>MethodA — static path resolves the jclass
 *                    via the declaring class name through FindClass),
 *   - JNI-fallback stress:
 *       * a TIGHT LOOP of String-returning calls (the audit flags a local-ref
 *         leak: every String return / String arg creates a JNI local ref that
 *         must be released or HotSpot's default 16-entry local-ref table
 *         overflows; once starved, later calls return "" — the native side
 *         asserts the result is stable across the loop),
 *       * a TIGHT LOOP of String-ARG calls (NewStringUTF local ref per call),
 *       * a TIGHT LOOP of long+double MULTI-ARG primitive calls (the
 *         union-aliasing footgun: a primitive jvalue cell must NEVER be handed
 *         to DeleteLocalRef — the loop must not corrupt state),
 *       * REPEATED calls on the SAME proxy (cache warm-up: cached_method_id /
 *         cached_class_handle reused — no state corruption across iterations),
 *       * INSTANCE vs STATIC interleaving.
 *
 * Every value-returning method here uses a recognizable boundary value so the
 * native side can pin the exact decode (sign-extension for B/S/I/J,
 * zero-extension for C, IEEE-754 fidelity for F/D, modified-UTF-8 for String on
 * the JNI path).  Every void / arg-consuming method records its invocation and
 * arguments into observable static fields so the native side can prove the body
 * actually ran with the right arguments even when nothing is returned.
 *
 * How the native module drives it: it hooks {@link #trigger(int)} (the only
 * context where vmhook::hotspot::current_java_thread is set, which call()
 * requires).  The probe's run() calls trigger() through a real bytecode
 * dispatch; the detour performs every call() below and records observations.
 *
 * Java 8 syntax only (no var / records / switch-expressions / lambdas).
 */
public final class MethodCallJni
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time the hooked trigger() runs (handshake sanity). */
    public static volatile int triggerCount;

    // ── void side-effect counters (prove a void body executed) ──────────────

    /** Bumped by the instance void method. */
    public static volatile int voidInstanceHits;

    /** Bumped by the static void method. */
    public static volatile int voidStaticHits;

    // ── recorded multi-arg primitives (prove the arg block was marshalled) ──

    /** Set true once sumILD(int,long,double) has run at least once. */
    public static volatile boolean multiPrimCalled;
    public static volatile int     multiArgInt;
    public static volatile long    multiArgLong;
    public static volatile double  multiArgDouble;

    /** Set true once the long+double two-slot loop body has run. */
    public static volatile boolean twoSlotLoopCalled;
    public static volatile long    twoSlotLastLong;
    public static volatile double  twoSlotLastDouble;

    // ── recorded String arg (prove a String reached the body) ───────────────

    /** Set true once consumeString(String) has run. */
    public static volatile boolean stringArgCalled;
    public static volatile String  stringArgValue;
    public static volatile int     stringArgLen;

    // ── recorded Object arg (prove a reference reached the body) ────────────

    /** Set true once consumeObject(Object) has run. */
    public static volatile boolean objectArgCalled;
    public static volatile boolean objectArgNonNull;
    public static volatile int     objectArgIdentity;

    /** identityHashCode of SINGLETON, so the native side can pass SINGLETON as
     *  the Object arg and prove byte-for-byte the body got the exact object. */
    public static volatile int     selfIdentity;

    // ── non-corruption breadcrumb ───────────────────────────────────────────

    /** The arg the LAST echoInt(int) received — proves a value-returning call
     *  performed after the stress loops still delivers its argument intact. */
    public static volatile int     lastEchoArg;

    // ── recorded String-arg shape (NUL / astral arg proof) ──────────────────
    //
    // The String-RETURN decode (call_jni -> GetStringUTFChars, modified UTF-8 +
    // NUL-terminated) CANNOT round-trip an interior NUL (truncates) or an astral
    // scalar (6-byte surrogate form), so a pure "echoString then compare" cannot
    // prove the ARG path (NewString / length-counted UTF-16, the #27 fix) carried
    // them.  These INTEGER observations are immune to any string decoding: the
    // body measures the Java String the JVM actually received and publishes plain
    // ints, so the native side proves the exact code units reached the JVM.
    public static volatile boolean recordStringCalled;
    public static volatile int     recordStringCharLen;   // String.length()  (UTF-16 units)
    public static volatile int     recordStringCpCount;   // codePointCount() (scalars)
    public static volatile int     recordStringFirstCp;   // codePointAt(0)
    public static volatile int     recordStringLastCp;    // last code point
    public static volatile int     recordStringHash;      // String.hashCode() — content fingerprint

    // ── constructor-invocation counter (CallNonvirtual / invokespecial proof) ─
    //
    // The ONLY path in method_proxy::call() that dispatches through JNI
    // CallNonvirtualVoidMethodA (slot 93) is a void <init> / <clinit> resolved on
    // an INSTANCE proxy (see call_jni's 'V' arm).  Re-invoking this no-arg
    // constructor on the already-built SINGLETON via get_method("<init>","()V")
    // ->call() exercises that nonvirtual slot; the constructor only bumps this
    // static counter, so re-running it on a live receiver is a safe no-op whose
    // single observable effect is the increment.
    public static volatile int     ctorCalls;

    // ── recorded narrow / wide primitive ARG shapes (marshaller arm proof) ───
    //
    // The jvalue marshaller (convert_jni_arg) has DISTINCT arms for bool(Z),
    // narrow integrals via out.i (byte/char/short/int), 64-bit via out.j (long),
    // float(F), double(D).  The existing module only echoes int + long; these
    // recorders publish pure-int / raw-bit measurements of every other arm so the
    // native side proves each marshalled arg arrived in the JVM verbatim.
    public static volatile boolean argBoolCalled;
    public static volatile boolean argBoolValue;        // echoBool(true)
    public static volatile boolean argByteCalled;
    public static volatile int     argByteValue;        // (int) byte arg, sign-extended
    public static volatile boolean argCharCalled;
    public static volatile int     argCharValue;        // (int) char arg, zero-extended
    public static volatile boolean argShortCalled;
    public static volatile int     argShortValue;       // (int) short arg, sign-extended
    public static volatile boolean argFloatCalled;
    public static volatile int     argFloatBits;        // Float.floatToRawIntBits(arg)
    public static volatile boolean argDoubleCalled;
    public static volatile long    argDoubleBits;       // Double.doubleToRawLongBits(arg)

    // ── recorded many-arg / all-two-slot shapes (slot-array layout proof) ────
    public static volatile boolean sixArgCalled;
    public static volatile long    sixArgPacked;        // a derived sum proving all six landed
    public static volatile boolean fourWideCalled;
    public static volatile long    fourWideResult;      // (J,D,J,D) interleave proof

    // ── recorded null-reference arg contract ────────────────────────────────
    public static volatile boolean nullStrArgCalled;
    public static volatile boolean nullStrArgWasNull;   // const char* nullptr -> Java null
    public static volatile boolean nullObjArgCalled;
    public static volatile boolean nullObjArgWasNull;   // unique_ptr null -> Java null

    /** No-arg constructor: side-effect is ONLY the static counter bump, so the
     *  nonvirtual re-invocation test can re-run it on a live object harmlessly. */
    public MethodCallJni()
    {
        ctorCalls++;
    }

    // ── the method the native module hooks to obtain a live thread ──────────

    /** Hookable instance method.  The native detour on this method performs
     *  every method_proxy::call() the test asserts on. */
    public int trigger(final int delta)
    {
        triggerCount++;
        return delta + 1;
    }

    // ════════════════════════════════════════════════════════════════════════
    //  INSTANCE value returners — one per primitive return type, no args.
    //  Boundary values chosen so the native decode is unambiguous.
    // ════════════════════════════════════════════════════════════════════════

    public void    retVoid()        { voidInstanceHits++; }
    public boolean retBoolTrue()    { return true; }
    public boolean retBoolFalse()   { return false; }
    public byte    retByte()        { return (byte) -7; }          // sign-extends to -7
    public char    retChar()        { return 'Z'; }                // 90, zero-extends
    public char    retCharMax()     { return (char) 0xFFFF; }      // 65535, NOT -1
    public short   retShort()       { return (short) -12345; }     // sign-extends
    public int     retInt()         { return 0x0BADF00D; }         // 195948557
    public long    retLong()        { return 0x0123456789ABCDEFL; }
    public float   retFloat()       { return 3.5f; }               // exact in binary
    public double  retDouble()      { return 2.718281828459045; }
    public String  retString()      { return "jni-instance-hello"; }

    // Single-primitive ARG echoes (prove a primitive arg is marshalled).
    public int  echoInt(final int v)   { lastEchoArg = v; return v; }
    public long echoLong(final long v) { return v; }

    // ── float / double SPECIAL-VALUE returners (IEEE-754 fidelity decode) ────
    // The existing module only checks 3.5f / e; these pin the special-value
    // decode of the 'F' and 'D' return arms (NaN canonical bits, +/-inf, -0.0,
    // a denormal) so the raw-bit transfer through Call(Static)?<F|D>MethodA is
    // proven for the full IEEE-754 range, not just two ordinary finite values.
    public float  retFloatNaN()      { return Float.NaN; }
    public float  retFloatPosInf()   { return Float.POSITIVE_INFINITY; }
    public float  retFloatNegInf()   { return Float.NEGATIVE_INFINITY; }
    public float  retFloatNegZero()  { return -0.0f; }
    public float  retFloatDenormal() { return Float.MIN_VALUE; }            // smallest subnormal
    public double retDoubleNaN()     { return Double.NaN; }
    public double retDoublePosInf()  { return Double.POSITIVE_INFINITY; }
    public double retDoubleNegZero() { return -0.0d; }
    public double retDoubleDenormal(){ return Double.MIN_VALUE; }           // smallest subnormal

    // ── primitive-ARG echoes at boundary values (round-trip into the JVM) ────
    // Return the arg derived so the native side can pin the exact marshalled
    // value.  echoIntId does NOT touch lastEchoArg (so it never clobbers the
    // sibling mcj_echo_int_side_effect breadcrumb) — it just returns its arg.
    public int    echoIntId(final int v)        { return v; }
    public long   echoLongId(final long v)      { return v; }

    // ── narrow / wide primitive ARG recorders (one per marshaller arm) ───────
    // Each records a pure-int / raw-bit measurement of the JVM-side value so the
    // native side proves the specific convert_jni_arg arm marshalled correctly.
    public void recordBool(final boolean b)
    {
        argBoolValue  = b;
        argBoolCalled = true;
    }
    public void recordByte(final byte b)
    {
        argByteValue  = b;            // widens with sign extension in Java
        argByteCalled = true;
    }
    public void recordChar(final char c)
    {
        argCharValue  = c;            // widens with ZERO extension in Java
        argCharCalled = true;
    }
    public void recordShort(final short s)
    {
        argShortValue  = s;           // widens with sign extension
        argShortCalled = true;
    }
    public void recordFloat(final float f)
    {
        argFloatBits   = Float.floatToRawIntBits(f);
        argFloatCalled = true;
    }
    public void recordDouble(final double d)
    {
        argDoubleBits   = Double.doubleToRawLongBits(d);
        argDoubleCalled = true;
    }

    // float ARG -> float return and double ARG -> double return (round-trip the
    // F and D marshaller arms AND the F/D return decode in one call each).
    public float  echoFloat(final float f)   { return f; }
    public double echoDouble(final double d) { return d; }

    // ── many-arg / all-two-slot shapes (slot-array layout beyond I,J,D) ──────
    // Six args mixing single- and two-slot kinds: int, long, int, double, long,
    // int.  Returns a value derived from ALL SIX so a misplaced slot is caught.
    //   result = a + c + f + b + e + (long) d
    public long sixArg(final int a, final long b, final int c,
                       final double d, final long e, final int f)
    {
        sixArgPacked = ((long) a) + b + ((long) c) + (long) d + e + ((long) f);
        sixArgCalled = true;
        return sixArgPacked;
    }

    // Four consecutive TWO-SLOT args (long, double, long, double): the hardest
    // case for the jvalue slot array — every cell is 8 bytes.  Returns a derived
    // long so one value pins the order.
    //   result = a + c + (long) b - (long) d
    public long fourWide(final long a, final double b, final long c, final double d)
    {
        fourWideResult = a + c + (long) b - (long) d;
        fourWideCalled = true;
        return fourWideResult;
    }

    // ── null-reference ARG consumers (null-pointer convention proof) ─────────
    // The library maps a null const char* / null unique_ptr to Java null; these
    // bodies publish whether the received reference was null so the native side
    // proves the null-arg path reaches the JVM as a genuine null.
    public void consumeNullableString(final String s)
    {
        nullStrArgWasNull = (s == null);
        nullStrArgCalled  = true;
    }
    public void consumeNullableObject(final Object o)
    {
        nullObjArgWasNull = (o == null);
        nullObjArgCalled  = true;
    }

    // Multi-arg primitive with a long (2 slots) and a double (2 slots) between
    // single-slot ints.  Returns a derived long so the native side can verify
    // every argument arrived in the correct slot.
    //   result = i + j + (long) d
    public long sumILD(final int i, final long j, final double d)
    {
        multiArgInt    = i;
        multiArgLong   = j;
        multiArgDouble = d;
        multiPrimCalled = true;
        return ((long) i) + j + (long) d;
    }

    // Two-slot-heavy multi-arg used by the tight loop: long, double, long.
    // Returns long+long+(long)double so a single returned value pins all three.
    public long twoSlot(final long a, final double b, final long c)
    {
        twoSlotLastLong   = a;
        twoSlotLastDouble = b;
        twoSlotLoopCalled = true;
        return a + c + (long) b;
    }

    // String ARG -> String return (round-trip through the JNI String paths).
    public String echoString(final String s)
    {
        return s;
    }

    // String ARG -> void (prove a String reaches a no-return body).
    public void consumeString(final String s)
    {
        stringArgValue  = s;
        stringArgLen    = (s == null) ? -1 : s.length();
        stringArgCalled = true;
    }

    // Object ARG -> void (prove a reference reaches a no-return body).
    public void consumeObject(final Object o)
    {
        objectArgNonNull  = (o != null);
        objectArgIdentity = (o == null) ? 0 : System.identityHashCode(o);
        objectArgCalled   = true;
    }

    // Object return (non-String reference): returns SINGLETON itself, so the
    // native side can prove the returned wrapper's OOP == the receiver OOP.
    public MethodCallJni retSelf()
    {
        return this;
    }

    // Object return that is null (null reference contract -> null unique_ptr).
    public MethodCallJni retNullObject()
    {
        return null;
    }

    // Array reference return ('[' descriptor) — a non-null int[].  The exact
    // contents { 11, 22, 33 } are asserted value-correct on the native side via
    // vmhook::array_length + vmhook::get_array_element (the crash-safe library
    // array readers), so the '[' arm's OOP decode is proven to land on the real
    // array, not just "non-null".
    public int[] retIntArray()
    {
        return new int[] { 11, 22, 33 };
    }

    // Long[] return — a DIFFERENT element stride (8 bytes) from int[], so the
    // value-correct read also exercises get_array_element<int64_t> stride math.
    public long[] retLongArray()
    {
        return new long[] { 0x1111111111111111L, 0x2222222222222222L,
                            0x3333333333333333L, 0x4444444444444444L };
    }

    // Object array return ('[Ljava/lang/String;') — exercises the '[' arm with a
    // REFERENCE element descriptor.  The native side asserts a non-null decode and
    // the exact length (object-element contents are not walked — that's the user's
    // job — but the length read proves the array header decoded correctly).
    public String[] retStringArray()
    {
        return new String[] { "alpha", "bravo", "charlie" };
    }

    // Additional primitive-array returns covering EVERY element stride the
    // crash-safe get_array_element reader must handle on the '[' arm:
    //   byte[]   -> 1-byte stride (sign-extending read)
    //   boolean[]-> 1-byte stride (0/1)
    //   short[]  -> 2-byte stride (sign)
    //   char[]   -> 2-byte stride (zero-extend)
    //   double[] -> 8-byte stride (IEEE bits)
    //   float[]  -> 4-byte stride (IEEE bits)
    // Small (<=4 elements) to keep the heap budget modest.
    public byte[] retByteArray()
    {
        return new byte[] { (byte) -1, (byte) 0, (byte) 127, (byte) -128 };
    }
    public boolean[] retBoolArray()
    {
        return new boolean[] { true, false, true };
    }
    public short[] retShortArray()
    {
        return new short[] { (short) -32768, (short) 0, (short) 32767 };
    }
    public char[] retCharArray()
    {
        return new char[] { 'A', (char) 0xFFFF, '0' };          // 65, 65535, 48
    }
    public double[] retDoubleArray()
    {
        return new double[] { 1.5d, -2.5d, 1024.0d };
    }
    public float[] retFloatArray()
    {
        return new float[] { 0.5f, -0.5f, 3.5f, -3.5f };
    }

    // String return used by the tight leak loop: a fresh constant each call.
    public String loopString()
    {
        return "loop-stable-value";
    }

    // ── String-arg shape recorder (NUL / astral ARG proof) ──────────────────
    // Publishes pure-int measurements of the received String so the native side
    // proves the length-counted UTF-16 arg path delivered every code unit
    // (interior NUL, astral surrogate pair) to the JVM, independent of any
    // string-DECODE limitation on the return side.
    public void recordString(final String s)
    {
        if (s == null)
        {
            recordStringCharLen = -1;
            recordStringCpCount = -1;
            recordStringFirstCp = 0;
            recordStringLastCp  = 0;
            recordStringHash    = 0;
        }
        else
        {
            recordStringCharLen = s.length();
            recordStringCpCount = s.codePointCount(0, s.length());
            recordStringFirstCp = s.isEmpty() ? 0 : s.codePointAt(0);
            recordStringLastCp  = s.isEmpty() ? 0 : s.codePointBefore(s.length());
            recordStringHash    = s.hashCode();
        }
        recordStringCalled = true;
    }

    // ── methods that THROW (exception-discipline proof) ─────────────────────
    // The native side calls these, then asserts the library OBSERVED and CLEARED
    // the pending JNI exception (call_jni's check_callee_exception -> Describe,
    // which clears), so a subsequent call succeeds and nothing escapes to crash a
    // sibling module.  Distinct messages let the trace identify each.
    public void throwVoid()
    {
        throw new IllegalStateException("mcj-throw-void");
    }

    // Throwing method with a NON-void return: proves the value path also leaves a
    // pending exception that the library clears (JNI returns 0 on a pending
    // exception, so the returned value is meaningless — only the discipline
    // matters here).
    public int throwReturningInt()
    {
        if (triggerCount >= 0)            // always true; defeats unreachable-code
        {
            throw new ArithmeticException("mcj-throw-int");
        }
        return 0;
    }

    public static void sThrowVoid()
    {
        throw new IllegalStateException("mcj-throw-static");
    }

    // ════════════════════════════════════════════════════════════════════════
    //  STATIC value returners — mirror the instance set (CallStatic*MethodA).
    //  The static path resolves the jclass from the declaring class name; this
    //  fixture lives on the application/system classloader, reachable via
    //  FindClass from the detour thread, so the static branch succeeds.
    // ════════════════════════════════════════════════════════════════════════

    public static void    sRetVoid()      { voidStaticHits++; }
    public static boolean sRetBoolTrue()  { return true; }
    public static byte    sRetByte()      { return (byte) 99; }
    public static char    sRetChar()      { return 'k'; }            // 107
    public static short   sRetShort()     { return (short) 20000; }
    public static int     sRetInt()       { return -2147483648; }    // Integer.MIN_VALUE
    public static long    sRetLong()      { return Long.MAX_VALUE; }
    public static float   sRetFloat()     { return -0.5f; }
    public static double  sRetDouble()    { return -1.5; }
    public static String  sRetString()    { return "jni-static-hello"; }

    public static int     sEchoInt(final int v) { return v; }

    public static long    sSumILD(final int i, final long j, final double d)
    {
        return ((long) i) + j + (long) d;
    }

    // Static float / double special-value returns (CallStatic<F|D>MethodA arm).
    public static float  sRetFloatNaN()     { return Float.NaN; }
    public static double sRetDoublePosInf() { return Double.POSITIVE_INFINITY; }
    public static double sRetDoubleNegZero(){ return -0.0d; }

    // Static six-arg (mirrors the instance sixArg slot-array layout, but on the
    // CallStatic*MethodA path where there is NO receiver slot).
    //   result = a + c + f + b + e + (long) d
    public static long sSixArg(final int a, final long b, final int c,
                               final double d, final long e, final int f)
    {
        return ((long) a) + b + ((long) c) + (long) d + e + ((long) f);
    }

    // Static float/double ARG echoes (the F/D arg arm on the static path).
    public static float  sEchoFloat(final float f)   { return f; }
    public static double sEchoDouble(final double d) { return d; }

    // Static Object return (non-String reference): the published SINGLETON.
    public static MethodCallJni sRetSingleton()
    {
        return SINGLETON;
    }

    public static MethodCallJni sRetNullObject()
    {
        return null;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodCallJni.go && !MethodCallJni.done;
            }

            @Override
            public void run()
            {
                // Publish the receiver identity so the Object-arg check is exact.
                MethodCallJni.selfIdentity = System.identityHashCode(SINGLETON);

                // Drive trigger() through normal bytecode dispatch -> fires the
                // native interpreter hook; that detour performs every
                // method_proxy::call() the test asserts on.
                SINGLETON.trigger(11);

                MethodCallJni.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side reaches the instance methods on a stable OOP and so the
     * published identity matches the receiver the detour sees as `self`.
     */
    public static final MethodCallJni SINGLETON = new MethodCallJni();
}
