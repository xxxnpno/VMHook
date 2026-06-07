package vmhook;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

// Central probe target for vmhook.  Every Java type and JVM situation
// that vmhook needs to handle has at least one field, method, or hook
// site here.
public class Example
{
    // ── Primitive fields with regular values ──────────────────────────────
    public static boolean staticBool   = true;
    public static byte    staticByte   = 0xf;
    public static short   staticShort  = 0xff;
    public static int     staticInt    = 0xffff;
    public static long    staticLong   = 0xffffffffL;
    public static float   staticFloat  = Float.intBitsToFloat(0x7f7fffff);
    public static double  staticDouble = Double.longBitsToDouble(0x7fefffffffffffffL);
    public static char    staticChar   = 0xffff;
    public static String  staticString = "fortnite";

    public boolean notStaticBool   = true;
    public byte    notStaticByte   = 0xf;
    public short   notStaticShort  = 0xff;
    public int     notStaticInt    = 0xffff;
    public long    notStaticLong   = 0xffffffffL;
    public float   notStaticFloat  = Float.intBitsToFloat(0x7f7fffff);
    public double  notStaticDouble = Double.longBitsToDouble(0x7fefffffffffffffL);
    public char    notStaticChar   = 0xffff;
    public String  notStaticString = "big yahu";

    // ── Edge / boundary primitive values ──────────────────────────────────
    // Test vmhook's handling of min/max, NaN, infinities, and negatives.
    public static int    intMinValue   = Integer.MIN_VALUE;
    public static int    intMaxValue   = Integer.MAX_VALUE;
    public static long   longMinValue  = Long.MIN_VALUE;
    public static long   longMaxValue  = Long.MAX_VALUE;
    public static byte   byteMin       = Byte.MIN_VALUE;
    public static byte   byteMax       = Byte.MAX_VALUE;
    public static short  shortMin      = Short.MIN_VALUE;
    public static short  shortMax      = Short.MAX_VALUE;
    public static float  floatNaN      = Float.NaN;
    public static float  floatPosInf   = Float.POSITIVE_INFINITY;
    public static float  floatNegInf   = Float.NEGATIVE_INFINITY;
    public static double doubleNaN     = Double.NaN;
    public static double doublePosInf  = Double.POSITIVE_INFINITY;
    public static double doubleNegInf  = Double.NEGATIVE_INFINITY;
    public static int    negativeInt   = -12345;
    public static long   negativeLong  = -9876543210L;

    // ── String edge cases ─────────────────────────────────────────────────
    // Empty, unicode, long, and interned strings.
    public static String emptyString    = "";
    public static String unicodeString  = "héllo éè ?";   // includes non-ASCII codepoints
    // Note: explicit concatenation instead of String.repeat(int) so this
    // compiles on Java 8 (repeat() was added in Java 11).
    public static String longString     =
        "abcdefghijklmnopqrstuvwxyz0123456789"
      + "abcdefghijklmnopqrstuvwxyz0123456789"
      + "abcdefghijklmnopqrstuvwxyz0123456789"
      + "abcdefghijklmnopqrstuvwxyz0123456789"
      + "abcdefghijklmnopqrstuvwxyz0123456789"
      + "abcdefghijklmnopqrstuvwxyz0123456789"
      + "abcdefghijklmnopqrstuvwxyz0123456789"
      + "abcdefghijklmnopqrstuvwxyz0123456789";
    public static String internedLiteral = "INTERNED";
    public static String nullString     = null;     // null-reference field

    // ── Final field ───────────────────────────────────────────────────────
    // Java `final` does not prevent vmhook from reading the slot, but the
    // backing memory is the same as a regular field; this probe verifies
    // the read path works on final-marked fields too.
    public static final int finalInt = 0xC0FFEE;

    // ── Volatile field ────────────────────────────────────────────────────
    // Already-volatile fields used elsewhere in the file are listed lower
    // down for the probe machinery; this one is purely a "read a volatile
    // primitive" check.
    public static volatile long volatileLong = 0x123456789ABCDEF0L;

    // ── Array fields (1-D) ────────────────────────────────────────────────
    public static boolean[] staticBoolArray   = { true, false, true };
    public static byte[]    staticByteArray   = { 0x1, 0x2, 0x3 };
    public static short[]   staticShortArray  = { 0x10, 0x20, 0x30 };
    public static int[]     staticIntArray    = { 0x100, 0x200, 0x300 };
    public static long[]    staticLongArray   = { 0x1000L, 0x2000L, 0x3000L };
    public static float[]   staticFloatArray  =
    {
        Float.intBitsToFloat(0x3f800000),
        Float.intBitsToFloat(0x40000000),
        Float.intBitsToFloat(0x40400000)
    };
    public static double[]  staticDoubleArray =
    {
        Double.longBitsToDouble(0x3ff0000000000000L),
        Double.longBitsToDouble(0x4000000000000000L),
        Double.longBitsToDouble(0x4008000000000000L)
    };
    public static char[]    staticCharArray   = { 'A', 'B', 'C' };
    // Non-interned backings: the legacy driver writes "omega" IN PLACE over
    // element [1] (set_static_string_array), and if these were interned string
    // literals that write would corrupt the shared, JVM-wide interned "world"
    // constant other tests rely on (e.g. field_static's `"world".equals(setStr)`).
    // new String(...) gives each element a private backing array so the in-place
    // write can never alias an interned literal.
    public static String[]  staticStringArray = { new String("hello".toCharArray()), new String("world".toCharArray()), new String("!".toCharArray()) };

    public boolean[] notStaticBoolArray   = { true, false, true };
    public byte[]    notStaticByteArray   = { 0x1, 0x2, 0x3 };
    public short[]   notStaticShortArray  = { 0x10, 0x20, 0x30 };
    public int[]     notStaticIntArray    = { 0x100, 0x200, 0x300 };
    public long[]    notStaticLongArray   = { 0x1000L, 0x2000L, 0x3000L };
    public float[]   notStaticFloatArray  =
    {
        Float.intBitsToFloat(0x3f800000),
        Float.intBitsToFloat(0x40000000),
        Float.intBitsToFloat(0x40400000)
    };
    public double[]  notStaticDoubleArray =
    {
        Double.longBitsToDouble(0x3ff0000000000000L),
        Double.longBitsToDouble(0x4000000000000000L),
        Double.longBitsToDouble(0x4008000000000000L)
    };
    public char[]    notStaticCharArray   = { 'X', 'Y', 'Z' };
    public String[]  notStaticStringArray = { "we", "like", "vmhook" };

    // ── Array edge cases ──────────────────────────────────────────────────
    public static int[]    emptyIntArray  = new int[0];
    public static String[] emptyStrArray  = new String[0];
    public static int[]    largeIntArray  = new int[256];    // populated below
    public static long[]   longEdgeArray  = { Long.MIN_VALUE, 0L, Long.MAX_VALUE };

    static
    {
        // Populate largeIntArray with deterministic content so the C++
        // side can read selected indices and check them.
        for (int i = 0; i < largeIntArray.length; ++i)
        {
            largeIntArray[i] = i * 3 + 1;
        }
    }

    // ── Object reference and inheritance ──────────────────────────────────
    // vmhook.dll needs an instance to access non static fields and methods
    // since it can access static fields without an instance, it gets this
    // one and then it can obtain non-static field/method handles.
    public static Example instance = new Example();

    public Example()
    {
        listOfAs = new ArrayList<>();
        listOfAs.add(new A());
        listOfAs.add(new A());
        listOfAs.add(new A());

        // Populate the new container fixtures with three deterministic
        // entries each so the C++ side can check both size and content.
        linkedListOfAs = new LinkedList<>();
        linkedListOfAs.add(new A());
        linkedListOfAs.add(new A());
        linkedListOfAs.add(new A());

        setOfAs = new HashSet<>();
        setOfAs.add(new A());
        setOfAs.add(new A());
        setOfAs.add(new A());

        // Insertion-ordered Map so the C++ side can assert on keys "k0/k1/k2"
        // appearing in a known order in addition to the size check.
        mapOfAs = new LinkedHashMap<>();
        mapOfAs.put("k0", new A());
        mapOfAs.put("k1", new A());
        mapOfAs.put("k2", new A());

        // Plain HashMap variant so the unordered fast path is exercised too.
        hashMapOfAs = new HashMap<>();
        hashMapOfAs.put("h0", new A());
        hashMapOfAs.put("h1", new A());
        hashMapOfAs.put("h2", new A());

        // TreeMap to exercise the red-black BST walk.
        treeMapOfAs = new TreeMap<>();
        treeMapOfAs.put("t0", new A());
        treeMapOfAs.put("t1", new A());
        treeMapOfAs.put("t2", new A());
    }

    // ── Method invocation counters (for probe machinery) ──────────────────
    public static int staticCalled = 0;
    public int  nonStaticCalled = 0;
    public int  cancelCalled    = 0;

    // ── Probe-coordination fields for every hook test ─────────────────────
    public static volatile boolean hookProbeRequested = false;
    public static volatile boolean hookProbeDone      = false;
    public static volatile boolean forceReturnProbeRequested = false;
    public static volatile boolean forceReturnProbeDone      = false;
    public static volatile int     forceReturnProbeValue     = 0;
    public static volatile boolean cancelProbeRequested = false;
    public static volatile boolean cancelProbeDone      = false;
    public static volatile boolean staticForceReturnProbeRequested = false;
    public static volatile boolean staticForceReturnProbeDone      = false;
    public static volatile int     staticForceReturnProbeValue     = 0;
    public static volatile boolean makeUniqueProbeRequested = false;
    public static volatile boolean makeUniqueProbeDone      = false;

    // ── List<A> probe ─────────────────────────────────────────────────────
    public List<A> listOfAs;
    public static volatile boolean listProbeRequested = false;
    public static volatile boolean listProbeDone      = false;
    public static volatile int     listProbeSize      = 0;
    public static volatile boolean listProbeElementsCorrect = false;

    // ── LinkedList<A> probe ───────────────────────────────────────────────
    public LinkedList<A> linkedListOfAs;
    public static volatile boolean linkedListProbeRequested = false;
    public static volatile boolean linkedListProbeDone      = false;
    public static volatile int     linkedListProbeSize      = 0;

    // ── HashSet<A> probe ──────────────────────────────────────────────────
    public Set<A> setOfAs;
    public static volatile boolean setProbeRequested = false;
    public static volatile boolean setProbeDone      = false;
    public static volatile int     setProbeSize      = 0;

    // ── LinkedHashMap<String, A> probe (covers HashMap fast path too) ─────
    public Map<String, A> mapOfAs;
    public static volatile boolean mapProbeRequested = false;
    public static volatile boolean mapProbeDone      = false;
    public static volatile int     mapProbeSize      = 0;

    // ── HashMap<String, A> probe (unordered, distinct keys "h0/h1/h2") ────
    public HashMap<String, A> hashMapOfAs;
    public static volatile boolean hashMapProbeRequested = false;
    public static volatile boolean hashMapProbeDone      = false;
    public static volatile int     hashMapProbeSize      = 0;

    // ── TreeMap<String, A> probe (red-black BST walk) ─────────────────────
    public TreeMap<String, A> treeMapOfAs;
    public static volatile boolean treeMapProbeRequested = false;
    public static volatile boolean treeMapProbeDone      = false;
    public static volatile int     treeMapProbeSize      = 0;

    // ── Inheritance / polymorphism probe ──────────────────────────────────
    public B bInstance = new B();
    public static volatile boolean polyProbeRequested = false;
    public static volatile boolean polyProbeDone      = false;
    public static volatile boolean polyProbeInheritedField  = false;
    public static volatile boolean polyProbeInheritedMethod = false;
    public static volatile boolean polyProbeOwnField        = false;

    // ── Method-call-return-value probe ────────────────────────────────────
    public static volatile boolean methodCallReturnProbeRequested = false;
    public static volatile boolean methodCallReturnProbeDone      = false;

    // ── Arg-mutation probes ───────────────────────────────────────────────
    public static volatile boolean argMutationProbeRequested = false;
    public static volatile boolean argMutationProbeDone      = false;
    public static volatile int     argMutationProbeValue     = 0;
    public static volatile boolean stringArgMutationProbeRequested = false;
    public static volatile boolean stringArgMutationProbeDone      = false;
    public static volatile String  stringArgMutationProbeValue     = "";

    // ── Enum / interface / nested-class probes ────────────────────────────
    public        Color  favoriteColor = Color.GREEN;
    public static Color  staticColor   = Color.BLUE;
    public        Dog    pet           = new Dog("Rex", 5);
    public        Animal animal        = new Dog("Buddy", 3);   // declared interface, runtime Dog
    public        NestedHost host          = new NestedHost();
    public        NestedHost.StaticNested staticNested = new NestedHost.StaticNested(42);
    public        NestedHost.Inner        innerInst    = host.newInner();

    public static volatile boolean enumProbeRequested      = false;
    public static volatile boolean enumProbeDone           = false;
    public static volatile int     enumProbeBrightness     = 0;

    public static volatile boolean interfaceProbeRequested = false;
    public static volatile boolean interfaceProbeDone      = false;
    public static volatile int     interfaceProbeKingdoms  = 0;

    public static volatile boolean nestedProbeRequested    = false;
    public static volatile boolean nestedProbeDone         = false;
    public static volatile int     nestedProbeValue        = 0;

    // ── Throwing-method probe ─────────────────────────────────────────────
    public static volatile boolean throwProbeRequested      = false;
    public static volatile boolean throwProbeDone           = false;
    public static volatile boolean throwProbeExceptionSeen  = false;

    // ── Overloaded-method probe ───────────────────────────────────────────
    public static volatile boolean overloadProbeRequested   = false;
    public static volatile boolean overloadProbeDone        = false;
    public static volatile int     overloadProbeIntResult   = 0;
    public static volatile String  overloadProbeStrResult   = "";
    public static volatile int     overloadProbeDualResult  = 0;

    // ── Void-return / each-primitive-return probes ────────────────────────
    public static volatile boolean returnTypesProbeRequested = false;
    public static volatile boolean returnTypesProbeDone      = false;
    public static volatile int     returnTypesProbeAccum     = 0;

    // ── Edge-value probe (Long.MIN/MAX, NaN, +Inf, -Inf, empty/null strings) ─
    public static volatile boolean edgeProbeRequested = false;
    public static volatile boolean edgeProbeDone      = false;
    public static volatile boolean edgeProbeAllSeen   = false;

    // ── Class-load watch probe ────────────────────────────────────────────
    // The C++ side registers a class-load watcher; this probe triggers
    // loading of vmhook.LateClass so the watcher has something to observe.
    public static volatile boolean classLoadProbeRequested = false;
    public static volatile boolean classLoadProbeDone      = false;

    // ── Probe targets ─────────────────────────────────────────────────────

    public static void staticCallMe(final int value)       { staticCalled++; }
    public void   nonStaticCallMe (final int value)        { this.nonStaticCalled++; }
    public int    nonStaticReturnMe(final int value)       { return value + 1; }
    public void   nonStaticCancelMe(final int value)       { this.cancelCalled += value; }
    public static int staticReturnMe (final int value)     { return value + 2; }

    public void nonStaticArgMutationMe       (final int    value) { argMutationProbeValue       = value; }
    public void nonStaticStringArgMutationMe (final String value) { stringArgMutationProbeValue = value; }

    // Three overloads with the same name, different signatures.  vmhook
    // resolves overloads on (name + descriptor); the unit-test calls each
    // explicitly via Reflection-style probing so the C++ side can verify
    // each descriptor is reachable.
    public int    overload(final int x)                    { return x * 10; }
    public String overload(final String s)                 { return "[" + s + "]"; }
    public int    overload(final int x, final int y)       { return x + y; }

    // One method returning each primitive type.  Used by the returnTypes
    // probe; the C++ side hooks each one and checks the value flows through
    // method_proxy::call() correctly.
    public boolean returnsBool()   { return true; }
    public byte    returnsByte()   { return (byte) 0x7e; }
    public short   returnsShort() { return (short) 12345; }
    public int     returnsInt()    { return 0x12345678; }
    public long    returnsLong()   { return 0x123456789ABCDEF0L; }
    public float   returnsFloat()  { return 3.1415926f; }
    public double  returnsDouble() { return 2.718281828459045; }
    public char    returnsChar()   { return '?'; }
    public String  returnsString() { return "hello-from-jvm"; }
    public String  returnsNull()   { return null; }       // null-returning probe

    // Throws a checked exception.  The C++ side hooks this and either
    // observes the exception or cancels via retval.set / retval.cancel.
    public int throwsCheckedException(final int x) throws IllegalStateException
    {
        if (x < 0)
        {
            throw new IllegalStateException("negative input: " + x);
        }
        return x;
    }

    // Synchronized method.  vmhook treats this identically to a regular
    // method (no special handling for the monitor enter/exit at the
    // interpreter level); the probe just confirms that hooking it doesn't
    // deadlock with the lock state.
    public synchronized int synchronizedAdd(final int x)
    {
        return this.notStaticInt + x;
    }

    // Object-typed (existing tests stay here for compatibility) ──────────
    private A a = new A();
    public void useA(final A a) { a.field++; }

    // Inheritance check methods on B (called by the C++ side via the
    // B-instance wrapper, see test_poly_probe).
    // These live in B/A — not duplicated here.

    // ── Java-side verification of C++ setter effects ──────────────────────
    public static boolean verifySetterEffects()
    {
        boolean ok = true;

        // Static scalars
        ok = jcheck("staticBool",   staticBool   == false)           & ok;
        ok = jcheck("staticByte",   staticByte   == 7)               & ok;
        ok = jcheck("staticShort",  staticShort  == 127)             & ok;
        ok = jcheck("staticInt",    staticInt    == 0x7fff)          & ok;
        ok = jcheck("staticLong",   staticLong   == 0x7fffffffL)     & ok;
        ok = jcheck("staticFloat",  staticFloat  == 1.0f)            & ok;
        ok = jcheck("staticDouble", staticDouble == 2.0)             & ok;
        ok = jcheck("staticChar",   staticChar   == 'A')             & ok;
        ok = jcheck("staticString", "java_ftw".equals(staticString)) & ok;

        // Non-static scalars (on Example.instance)
        ok = jcheck("notStaticBool",   instance.notStaticBool   == false)       & ok;
        ok = jcheck("notStaticByte",   instance.notStaticByte   == 7)           & ok;
        ok = jcheck("notStaticShort",  instance.notStaticShort  == 127)         & ok;
        ok = jcheck("notStaticInt",    instance.notStaticInt    == 0x7fff)      & ok;
        ok = jcheck("notStaticLong",   instance.notStaticLong   == 0x7fffffffL) & ok;
        ok = jcheck("notStaticFloat",  instance.notStaticFloat  == 1.0f)        & ok;
        ok = jcheck("notStaticDouble", instance.notStaticDouble == 2.0)         & ok;
        ok = jcheck("notStaticChar",   instance.notStaticChar   == 'B')         & ok;
        ok = jcheck("notStaticString", "cppwins!".equals(instance.notStaticString)) & ok;

        // Static arrays
        ok = jcheck("staticBoolArray",
                !staticBoolArray[0] && staticBoolArray[1] && !staticBoolArray[2]) & ok;
        ok = jcheck("staticByteArray",
                staticByteArray[0] == 10 && staticByteArray[1] == 20 && staticByteArray[2] == 30) & ok;
        ok = jcheck("staticShortArray",
                staticShortArray[0] == 256 && staticShortArray[1] == 512 && staticShortArray[2] == 768) & ok;
        ok = jcheck("staticIntArray",
                staticIntArray[0] == 4096 && staticIntArray[1] == 8192 && staticIntArray[2] == 12288) & ok;
        ok = jcheck("staticLongArray",
                staticLongArray[0] == 65536L && staticLongArray[1] == 131072L && staticLongArray[2] == 196608L) & ok;
        ok = jcheck("staticFloatArray",
                staticFloatArray[0] == 4.0f && staticFloatArray[1] == 5.0f && staticFloatArray[2] == 6.0f) & ok;
        ok = jcheck("staticDoubleArray",
                staticDoubleArray[0] == 4.0 && staticDoubleArray[1] == 5.0 && staticDoubleArray[2] == 6.0) & ok;
        ok = jcheck("staticCharArray",
                staticCharArray[0] == 'X' && staticCharArray[1] == 'Y' && staticCharArray[2] == 'Z') & ok;
        // Note: "alpha"/"omega"/"?" are used (not "world"/"hello") to avoid
        // interning collisions — modifying "hello"->"world" and "world"->"hello"
        // would corrupt the JVM string pool for those literals.
        ok = jcheck("staticStringArray",
                "alpha".equals(staticStringArray[0]) &&
                "omega".equals(staticStringArray[1]) &&
                "?".equals(staticStringArray[2])) & ok;

        // Non-static arrays
        ok = jcheck("notStaticBoolArray",
                !instance.notStaticBoolArray[0] && instance.notStaticBoolArray[1] && !instance.notStaticBoolArray[2]) & ok;
        ok = jcheck("notStaticByteArray",
                instance.notStaticByteArray[0] == 10 && instance.notStaticByteArray[1] == 20 && instance.notStaticByteArray[2] == 30) & ok;
        ok = jcheck("notStaticShortArray",
                instance.notStaticShortArray[0] == 256 && instance.notStaticShortArray[1] == 512 && instance.notStaticShortArray[2] == 768) & ok;
        ok = jcheck("notStaticIntArray",
                instance.notStaticIntArray[0] == 4096 && instance.notStaticIntArray[1] == 8192 && instance.notStaticIntArray[2] == 12288) & ok;
        ok = jcheck("notStaticLongArray",
                instance.notStaticLongArray[0] == 65536L && instance.notStaticLongArray[1] == 131072L && instance.notStaticLongArray[2] == 196608L) & ok;
        ok = jcheck("notStaticFloatArray",
                instance.notStaticFloatArray[0] == 4.0f && instance.notStaticFloatArray[1] == 5.0f && instance.notStaticFloatArray[2] == 6.0f) & ok;
        ok = jcheck("notStaticDoubleArray",
                instance.notStaticDoubleArray[0] == 4.0 && instance.notStaticDoubleArray[1] == 5.0 && instance.notStaticDoubleArray[2] == 6.0) & ok;
        ok = jcheck("notStaticCharArray",
                instance.notStaticCharArray[0] == 'D' && instance.notStaticCharArray[1] == 'E' && instance.notStaticCharArray[2] == 'F') & ok;
        ok = jcheck("notStaticStringArray",
                "ab".equals(instance.notStaticStringArray[0]) &&
                "love".equals(instance.notStaticStringArray[1]) &&
                "coding".equals(instance.notStaticStringArray[2])) & ok;

        return ok;
    }

    private static boolean jcheck(final String name, final boolean condition)
    {
        System.out.println(condition ? "[JAVA PASS] " + name : "[JAVA FAIL] " + name);
        return condition;
    }
}
