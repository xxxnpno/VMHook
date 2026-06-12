package vmhook.fixtures;

import vmhook.Harness;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Fixture for the {@code method_return_types} feature: exercises
 * {@code vmhook::method_proxy::call()} / {@code static_method(...)->call()}
 * DECODING every Java return type back into a C++ {@code value_t} -- one method
 * per {@code BasicType} ({@code Z B S C I J F D}), {@code java.lang.String}, and
 * an {@code Object}-typed method that returns {@code null}.
 *
 * <p>This is the modular-harness sibling of the legacy
 * {@code example.cpp::test_return_types}: the legacy probe made Java itself
 * accumulate the returns into one int; here the NATIVE side calls each method
 * through {@code method_proxy::call()} and asserts the decoded C++ value equals a
 * fixed sentinel, so the DECODE path (variant alternative + conversion operator)
 * is what is under test, not Java arithmetic.</p>
 *
 * <p>How the native module drives it: it hooks {@link #trigger(int)} (the only
 * context in which {@code vmhook::hotspot::current_java_thread} is established,
 * which {@code call()} requires).  The probe's {@code run()} calls
 * {@code trigger(7)} on {@link #SINGLETON} through a real bytecode dispatch; that
 * fires the interpreter detour, and the detour performs every {@code call()} the
 * native module asserts on against this very instance (and the static methods).</p>
 *
 * <p>Sentinel values (the native module hard-codes the matching expectations).
 * The headline value of each primitive mirrors the legacy {@code Example.returnsX}
 * so the two suites agree; additional boundary returners cover sign-extension,
 * zero-extension, IEEE special bit patterns and signed min/max:</p>
 * <ul>
 *   <li>{@code boolean returnsBool}    -> {@code true}        (and {@code returnsBoolFalse} -> {@code false})</li>
 *   <li>{@code byte    returnsByte}    -> {@code (byte)126}   (0x7E; + Max 127, Min -128, NegOne -1)</li>
 *   <li>{@code short   returnsShort}   -> {@code (short)12345}(+ Max 32767, Min -32768, NegOne -1)</li>
 *   <li>{@code char    returnsChar}    -> {@code '?'} (63)    (+ Max 0xFFFF=65535 unsigned)</li>
 *   <li>{@code int     returnsInt}     -> {@code 0x12345678}  (305419896; + Max, Min)</li>
 *   <li>{@code long    returnsLong}    -> {@code 0x123456789ABCDEF0L} (+ Min, a negative -9876543210)</li>
 *   <li>{@code float   returnsFloat}   -> {@code 3.1415926f}  (bits 0x40490FDA; + NaN, a fixed bit pattern)</li>
 *   <li>{@code double  returnsDouble}  -> {@code 2.718281828459045} (bits 0x4005BF0A8B145769; + NaN, fixed bits)</li>
 *   <li>{@code String  returnsString}  -> {@code "hello-from-jvm"} (+ empty, + a multibyte unicode string)</li>
 *   <li>{@code Object  returnsNull}    -> {@code null}        (the null-reference boundary)</li>
 * </ul>
 *
 * <p>The non-ASCII String constant is written with {@code \\uXXXX} escapes so the
 * source is pure ASCII and javac decodes it identically on every CI host
 * regardless of file encoding.  Java 8 syntax only (no var / records /
 * switch-expressions / lambdas): the probe is an anonymous {@link Harness.Probe}.</p>
 */
public final class ReturnTypes
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time {@link #trigger(int)} actually runs (hook liveness). */
    public static volatile int triggerCount;

    // -- Canonical multibyte unicode return (ASCII-safe \\uXXXX source) ----------
    // "caf" + e-acute (U+00E9) + space + nihongo (U+65E5 U+672C U+8A9E): a mix of
    // a Latin-1 high char (2-byte modified-UTF-8) and CJK chars (3-byte each), so
    // the String decode is proven on more than pure ASCII.  Written purely with
    // \\uXXXX escapes (like MethodString) so javac decodes it identically on every
    // CI host no matter the file encoding; the native module hard-codes the
    // matching modified-UTF-8 bytes.  Logical text: "cafe<acute> <nihongo>".
    public static final String UNICODE = "caf\u00e9 \u65e5\u672c\u8a9e";  // cafe-acute + nihongo

    // -- Long ASCII String return (well under read_java_string's 4096 cap, so the
    //    decode is exact -- proves a multi-segment String round-trips byte-for-byte,
    //    not just the short headline).  300 chars: '0'..'9' repeated. --------------
    public static final String LONG_STRING = buildLongString();

    private static String buildLongString()
    {
        final StringBuilder sb = new StringBuilder(300);
        for (int i = 0; i < 300; i++)
        {
            sb.append((char) ('0' + (i % 10)));
        }
        return sb.toString();
    }

    // -- A String with an INTERIOR NUL (U+0000) between two ASCII halves.  The two
    //    decode paths legitimately DIFFER on this input: read_java_string (call_stub
    //    / compressed-OOP path) emits standard UTF-8 (a single 0x00 byte), whereas
    //    the call_jni String path goes through GetStringUTFChars, which yields Java's
    //    MODIFIED UTF-8 (U+0000 -> 0xC0 0x80).  The native module CHARACTERIZES the
    //    decoded bytes for whichever path this JDK takes rather than over-asserting. -
    public static final String INTERIOR_NUL = "ab\u0000cd";

    // -- A STABLE non-null Object the native side decodes + cross-checks by OOP
    //    identity.  returnsObject() hands back THIS instance (not a fresh Object each
    //    call), so the decoded wrapper's heap pointer must equal this object's OOP. --
    public static final Object OBJECT_SINGLETON = new Object();

    /** identityHashCode of OBJECT_SINGLETON, published for the native cross-check. */
    public static volatile int objectIdentity;

    /** identityHashCode of SINGLETON (the receiver), for the self-as-Object check. */
    public static volatile int selfIdentity;

    /** Bumped by {@link #returnsVoidWithSideEffect()} so the native side can prove a
     *  VOID-returning call() actually EXECUTED the method (the side effect is
     *  observable), not merely decoded its absent result to a monostate. */
    public static volatile int voidSideEffect;

    // -- the method the native module hooks to obtain a live thread --------------

    /** Hookable instance method.  The native detour on this method performs every
     *  {@code method_proxy::call()} the test asserts on. */
    public int trigger(final int delta)
    {
        triggerCount++;
        return delta + 1;
    }

    // ========================================================================
    //  void (V) -- the no-result return
    // ========================================================================
    /** A void-returning method: side effect only.  call() must decode to a
     *  monostate value_t (is_void() true) on BOTH dispatch paths. */
    public void returnsVoid()
    {
        triggerCount += 0;   // a real (no-op) side effect so the body is non-trivial
    }

    /** A void-returning method with an OBSERVABLE side effect: it increments
     *  {@link #voidSideEffect}.  The native side snapshots that field, invokes this
     *  through call() (which decodes to monostate), then re-reads the field -- proving
     *  the void dispatch RAN the method body, not just produced an empty value_t. */
    public void returnsVoidWithSideEffect()
    {
        voidSideEffect++;
    }

    // ========================================================================
    //  boolean (Z)
    // ========================================================================
    public boolean returnsBool()      { return true; }
    public boolean returnsBoolFalse() { return false; }

    // ========================================================================
    //  byte (B) -- signed 8-bit
    // ========================================================================
    public byte returnsByte()       { return (byte) 0x7e; }   // 126 (mirrors Example)
    public byte returnsByteMax()    { return Byte.MAX_VALUE; }  // 127
    public byte returnsByteMin()    { return Byte.MIN_VALUE; }  // -128
    public byte returnsByteNegOne() { return (byte) -1; }       // sign-extension probe

    // ========================================================================
    //  short (S) -- signed 16-bit
    // ========================================================================
    public short returnsShort()       { return (short) 12345; } // mirrors Example
    public short returnsShortMax()    { return Short.MAX_VALUE; }  // 32767
    public short returnsShortMin()    { return Short.MIN_VALUE; }  // -32768
    public short returnsShortNegOne() { return (short) -1; }

    // ========================================================================
    //  char (C) -- UNSIGNED 16-bit
    // ========================================================================
    public char returnsChar()    { return '?'; }          // '?' == 63 (mirrors Example)
    public char returnsCharMax() { return (char) 0xFFFF; }      // 65535, zero-extension probe

    // ========================================================================
    //  int (I) -- signed 32-bit
    // ========================================================================
    public int returnsInt()    { return 0x12345678; }          // 305419896 (mirrors Example)
    public int returnsIntMax() { return Integer.MAX_VALUE; }   // 2147483647
    public int returnsIntMin() { return Integer.MIN_VALUE; }   // -2147483648

    // ========================================================================
    //  long (J) -- signed 64-bit (two interpreter slots)
    // ========================================================================
    public long returnsLong()    { return 0x123456789ABCDEF0L; } // mirrors Example
    public long returnsLongMin() { return Long.MIN_VALUE; }
    public long returnsLongNeg() { return -9876543210L; }        // a negative > 32-bit magnitude

    // ========================================================================
    //  float (F)
    // ========================================================================
    public float returnsFloat()    { return 3.1415926f; }                   // bits 0x40490FDA (mirrors Example)
    public float returnsFloatNaN() { return Float.NaN; }
    public float returnsFloatBits(){ return Float.intBitsToFloat(0x7f7fffff); } // FLT_MAX exact bit pattern

    // ========================================================================
    //  double (D)
    // ========================================================================
    public double returnsDouble()    { return 2.718281828459045; }                 // bits 0x4005BF0A8B145769 (mirrors Example)
    public double returnsDoubleNaN() { return Double.NaN; }
    public double returnsDoubleBits(){ return Double.longBitsToDouble(0x7fefffffffffffffL); } // DBL_MAX exact bits

    // ========================================================================
    //  java.lang.String
    // ========================================================================
    public String returnsString()        { return "hello-from-jvm"; }   // mirrors Example
    public String returnsStringEmpty()   { return ""; }                 // empty != null boundary
    public String returnsStringUnicode() { return UNICODE; }            // multibyte modified-UTF-8
    public String returnsStringLong()    { return LONG_STRING; }        // 300 ASCII chars, exact decode
    public String returnsStringInteriorNul() { return INTERIOR_NUL; }   // embedded U+0000

    // ========================================================================
    //  primitive arrays ([Z [B [C [S [I [J [F [D) and Object[]
    //  The reference return decodes to a compressed-OOP value_t alternative; the
    //  native side recovers the array OOP (decode_oop_pointer) and reads length +
    //  elements via vmhook::array_length / get_array_element<T>.
    // ========================================================================
    public boolean[] returnsBoolArray()  { return new boolean[] { true, false, true }; }
    public byte[]    returnsByteArray()   { return new byte[] { (byte) -128, 0, (byte) 127 }; }
    public char[]    returnsCharArray()   { return new char[] { 'A', '?', (char) 0xFFFF }; }
    public short[]   returnsShortArray()  { return new short[] { (short) -32768, 0, (short) 32767 }; }
    public int[]     returnsIntArray()    { return new int[] { Integer.MIN_VALUE, 0, 0x12345678, Integer.MAX_VALUE }; }
    public long[]    returnsLongArray()   { return new long[] { Long.MIN_VALUE, 0x123456789ABCDEF0L, Long.MAX_VALUE }; }
    public float[]   returnsFloatArray()  { return new float[] { 1.0f, 3.1415926f }; }
    public double[]  returnsDoubleArray() { return new double[] { 1.0, 2.718281828459045 }; }
    /** Object[] -- the reference-element array branch. */
    public Object[]  returnsObjectArray() { return new Object[] { OBJECT_SINGLETON, null }; }
    /** Empty int[] -- the zero-length array boundary (length 0, valid OOP). */
    public int[]     returnsEmptyIntArray() { return new int[0]; }

    // ========================================================================
    //  boxed wrapper types (java.lang.Integer / Long / Double)
    //  Each decodes to a reference value_t; the native side wraps it and reads the
    //  boxed value back through a method call (intValue()/longValue()/doubleValue()).
    // ========================================================================
    public Integer returnsBoxedInteger() { return Integer.valueOf(0x12345678); }
    public Long    returnsBoxedLong()    { return Long.valueOf(0x123456789ABCDEF0L); }
    public Double  returnsBoxedDouble()  { return Double.valueOf(2.718281828459045); }

    // ========================================================================
    //  Object / null-returning method
    // ========================================================================
    /** Returns Java {@code null} through an Object-typed signature -- the
     *  null/empty-wrapper boundary the native side characterizes. */
    public Object returnsNull() { return null; }

    /** Returns the STABLE non-null Object singleton (NOT a fresh Object) so the
     *  native side can cross-check the decoded wrapper's OOP against this object's
     *  published identity. */
    public Object returnsObject() { return OBJECT_SINGLETON; }

    /** Returns {@code this} through an Object-typed signature: the decoded wrapper's
     *  instance must equal the receiver OOP (identity via the reference path). */
    public Object returnsSelfAsObject() { return this; }

    // ========================================================================
    //  DESCRIPTOR EDGE CASES -- the RUNTIME object is identical across several of
    //  these, but the DECLARED return type in the method descriptor is what drives
    //  call()'s decode.  These prove the dispatch keys off the descriptor, not the
    //  runtime class:
    //    * an INTERFACE-typed return whose runtime value is a String decodes through
    //      the generic-reference branch (descriptor "Ljava/lang/CharSequence;" != the
    //      special-cased "Ljava/lang/String;"), NOT the eager-std::string branch.
    //    * the method's OWN class type as the return ("Lvmhook/fixtures/ReturnTypes;").
    //    * a deeply-nested generic ERASED to a bare interface descriptor
    //      ("Ljava/util/List;") -- the generic parameters vanish at the bytecode
    //      level, so the descriptor the native side sees carries no <...>.
    // ========================================================================

    /** Interface-typed return ({@code CharSequence}) whose runtime value is a
     *  {@code String}.  The descriptor is {@code Ljava/lang/CharSequence;}, so call()
     *  does NOT take the {@code Ljava/lang/String;} eager-decode branch -- it stores a
     *  generic compressed-OOP reference the native side decodes via read_java_string
     *  on the recovered String OOP.  Proves descriptor-driven (not runtime-class)
     *  String special-casing. */
    public CharSequence returnsCharSequence() { return "iface-charseq"; }

    /** The fixture's OWN class type as the declared return
     *  ({@code Lvmhook/fixtures/ReturnTypes;}); returns {@code this} so the decoded
     *  wrapper's instance OOP equals the receiver. */
    public ReturnTypes returnsOwnType() { return this; }

    /** A deeply-nested generic return: {@code List<Map<String,int[]>>}.  At the
     *  bytecode level the descriptor is the bare erased {@code Ljava/util/List;}, so
     *  call() decodes a generic reference (a non-null usable OOP) -- the native side
     *  cannot and does not try to recover the erased type arguments. */
    public List<Map<String, int[]>> returnsNestedGeneric()
    {
        final List<Map<String, int[]>> outer = new ArrayList<Map<String, int[]>>();
        final Map<String, int[]> inner = new HashMap<String, int[]>();
        inner.put("k", new int[] { 1, 2, 3 });
        outer.add(inner);
        return outer;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnTypes.go && !ReturnTypes.done;
            }

            @Override
            public void run()
            {
                // Publish OOP identities the native side cross-checks against the
                // decoded reference returns (returnsObject / returnsSelfAsObject).
                ReturnTypes.objectIdentity = System.identityHashCode(OBJECT_SINGLETON);
                ReturnTypes.selfIdentity   = System.identityHashCode(SINGLETON);

                // Real bytecode dispatch -> the native hook on trigger() fires,
                // and the detour exercises every return-typed method on this very
                // SINGLETON instance.
                SINGLETON.trigger(7);
                ReturnTypes.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly so
     * the native side reaches the instance methods on a stable OOP and so the
     * receiver the detour sees as {@code self} is this very object.
     */
    public static final ReturnTypes SINGLETON = new ReturnTypes();
}
