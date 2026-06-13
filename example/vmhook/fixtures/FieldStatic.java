package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_static feature (area: fields).
 *
 * Focus: STATIC field GET and (above all) SET through vmhook's zero-JNI
 * static_field("name") accessor, for EVERY JVM primitive (Z B C S I J F D),
 * java.lang.String, and an object reference -- with the written value proven
 * VISIBLE TO JAVA ITSELF (the contract: set-then-read-back through Java).
 *
 * Division of labour vs the sibling field modules:
 *   - field_primitives_get already covers static GET decoding/variant-index.
 *   - field_string already covers String GET/SET decode corner cases.
 *   - THIS fixture is the SET + Java-readback + GCC-portability authority:
 *       (a) the native side writes each static field via static_field(name)->set(v)
 *           BEFORE raising `go` (field_proxy::set mutates heap memory directly,
 *           no bytecode needed),
 *       (b) the probe (mode 1) then snapshots every static field into a parallel
 *           "seen*" witness field using GENUINE putstatic / getstatic bytecode,
 *           so the native side reads back what the *JVM* observed, not what C++
 *           thinks it wrote, and
 *       (c) Java getter methods (getX()) return each field so the native side can
 *           additionally pull the value back through static_method("getX")->call()
 *           -- which also exercises that static_field/static_method work when
 *           CALLED FROM A STATIC C++ WRAPPER METHOD on every compiler incl. GCC.
 *
 * mode selector (native sets `mode` + clears `done` on the rising edge of go):
 *   1 = snapshot every settable static field into its seen* witness (putstatic),
 *       AND publish each value via the getX() getters' results into seen* too.
 *   2 = writeRuntime(): putstatic fresh boundary values so native GET sees live,
 *       post-dispatch state (proves get() is not reading stale init constants).
 *   3 = resetTargets(): restore the set* targets to known initial values so the
 *       module could (in principle) re-run; also re-publishes objA into objRef.
 *   4 = objA.touch(0x5A): a real bytecode dispatch so the native interpreter
 *       hook on touch() fires; INSIDE that detour the module pulls every static
 *       field back through the getX() getters (method_proxy::call needs a live
 *       current_java_thread, which only exists on the Java thread in a detour).
 *   5 = snapshotInherited(): getstatic each INHERITED static (declared on
 *       FieldStaticBase) into its seenInh* witness, after the native side wrote
 *       it via static_field(name)->set on the subclass wrapper.
 *   6 = snapshotArrays(): getstatic the static ARRAY reference state (after a
 *       native whole-array replace / null) and publish the element sum.
 *   7 = publishEnum(): publish Tier constant identities/ordinals via bytecode.
 *   8 = reset inherited + array targets to their initial values.
 *
 * PORTABILITY/ENCODING NOTES:
 *   - All char values are numeric / \\uXXXX escapes (lexer-level, encoding
 *     independent) so javac on Cp1252 (Windows) and UTF-8 (Linux/macOS) agree.
 *   - String SET targets are ASCII and are only ever overwritten with an
 *     equal-or-shorter ASCII value, so the in-place backing-array write is
 *     deterministic across JDK 8 (char[]) and JDK 9+ (compact LATIN1/UTF-16),
 *     where write_java_string keeps the existing length and never resizes.
 *   - String SET targets are built with new String(char[]) (freshAscii), NOT
 *     bare literals, so each owns a PRIVATE backing array.  write_java_string
 *     mutates the backing array in place; a literal-initialised target would
 *     share its backing with the interned constant-pool String, and the write
 *     would corrupt that literal everywhere it is used in the JVM (including
 *     the "world" literal in seenStrEqWorld).  Mirrors FieldString.java.
 *
 * Java 8 syntax only (anonymous class, no var/lambda-in-field/switch-expr).
 *
 * EXHAUSTIVE EXTENSIONS (field_static "every possible input"):
 *   - extends {@link FieldStaticBase}: inherited static GET *and* SET through
 *     the subclass wrapper (declaring-klass mirror), plus an inherited
 *     reference replace and an inherited static-final constant.
 *   - nested {@link Tier} enum: each constant is a public-static-final field of
 *     FieldStatic$Tier — read as a static reference, plus a static enum-ref
 *     field on FieldStatic itself.
 *   - static ARRAY reference fields (int[] / String[] / a null array): read the
 *     compressed-OOP identity, REPLACE the whole array reference, null it.
 *   - many-statics offset sweep: every set* slot resolves to a DISTINCT mirror
 *     address (offset correctness across a class with dozens of statics).
 *   - nested {@code Unloaded} class that nothing references: a genuine
 *     not-yet-loaded class so the module can characterize find_class on it.
 */
public final class FieldStatic extends FieldStaticBase
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    /** Scenario selector; native sets it before raising go. */
    public static volatile int mode;

    // =====================================================================
    //  NESTED ENUM.  Each constant is a synthetic `public static final Tier`
    //  field of FieldStatic$Tier (the constant's singleton OOP).  The module
    //  registers a wrapper for FieldStatic$Tier and reads LOW/MID/HIGH through
    //  static_field(); it also reads the static enum-ref `staticTier` here.
    //  Java 8: a plain enum body (no var / switch-expr).
    // =====================================================================
    public enum Tier
    {
        LOW(10),
        MID(20),
        HIGH(30);

        /** Instance field on the enum body (native reads it off a constant). */
        public final int weight;

        Tier(final int weight)
        {
            this.weight = weight;
        }
    }

    /** STATIC enum-reference field on FieldStatic — resolves to the MID singleton. */
    public static Tier staticTier = Tier.MID;

    // Identity / ordinal witnesses for the enum constants, published Java-side
    // (mode 7) so the native distinctness checks are EXACT, not "non-null".
    public static volatile int tierLowIdentity;
    public static volatile int tierMidIdentity;
    public static volatile int tierHighIdentity;
    public static volatile int staticTierIdentity;   // == tierMidIdentity
    public static volatile int tierValuesLen;        // Tier.values().length == 3
    public static volatile int tierMidOrdinal;       // == 1
    public static volatile int tierMidWeight;        // Tier.MID.weight == 20

    // =====================================================================
    //  STATIC ARRAY reference fields.  A static array slot holds a compressed
    //  OOP exactly like any reference; native reads its identity, REPLACES the
    //  whole reference via set(unique_ptr<wrapper>), and nulls it.  The element
    //  contents are not the subject here (field_arrays_* own element get/set) —
    //  this proves whole-array static reference handling.
    // =====================================================================
    public static int[]    sIntArr   = { 10, 20, 30 };       // native may replace with sIntArrAlt
    public static String[] sStrArr   = { "x", "y" };          // GET identity / signature
    public static int[]    sNullArr  = null;                  // GET of a null array reference
    public static final int[]    sIntArrAlt = { 40, 50 };     // the replacement target
    public static int[]    sArrSum   = { 0 };                 // mode 6 writes the sum of sIntArr's elements

    // Array witnesses (mode 6): identity of sIntArr at snapshot, its length, and
    // whether it now aliases sIntArrAlt / is null.
    public static volatile int     seenIntArrLen;
    public static volatile int     seenIntArrFirst;          // sIntArr == null ? -1 : sIntArr[0]
    public static volatile boolean seenIntArrIsAlt;          // sIntArr == sIntArrAlt
    public static volatile boolean seenIntArrIsNull;

    /**
     * A nested class that NOTHING in the harness references, so HotSpot never
     * loads it during startup (loadFixtures only Class.forName's top-level
     * classes; FieldStatic's own code never names it).  The module uses it to
     * characterize find_class / static_field on a not-yet-loaded class.  It is
     * package-visible-from-here but unreferenced; -Werror-y javac is fine with
     * a nested type that is simply never used.
     */
    static final class Unloaded
    {
        public static int neverTouched = 0x5EED0001; // class stays unloaded: nothing references it
    }

    // =====================================================================
    //  SET TARGETS -- the native side writes these via static_field(name)->set().
    //  Initial values are deliberately DISTINCT from the values the native
    //  side writes, so a no-op write (e.g. a silently-dropped set) is caught:
    //  the seen* witness would still equal the initial value.
    // =====================================================================
    public static boolean setZ = false;           // native writes true
    public static byte    setB = 0;               // native writes Byte.MIN_VALUE (-128)
    public static char    setC = 0x0000;          // native writes 0xFFFF
    public static short   setS = 0;               // native writes Short.MIN_VALUE
    public static int     setI = 0;               // native writes Integer.MIN_VALUE
    public static long    setJ = 0L;              // native writes Long.MAX_VALUE
    public static float   setF = 0.0f;            // native writes a known bit pattern
    public static double  setD = 0.0;             // native writes a known bit pattern

    // A second battery of SET targets so native can write boundary/edge values
    // independent of the "primary" battery above (more angles, no aliasing).
    public static boolean setZ2 = true;           // native writes false
    public static byte    setB2 = 0;              // native writes (byte)0xFF == -1
    public static char    setC2 = 0x0041;         // native writes 0x20AC (euro)
    public static short   setS2 = 0;              // native writes (short)0xBEEF
    public static int     setI2 = 0;              // native writes 0xDEADBEEF
    public static long    setJ2 = 0L;             // native writes Long.MIN_VALUE
    public static float   setF2 = 0.0f;           // native writes Float.NEGATIVE_INFINITY
    public static double  setD2 = 0.0;            // native writes Double.NaN (canonical)

    // Mid-range "ordinary" values, to prove the common case (not just extremes).
    public static int     setIOrd = -1;           // native writes 123456789
    public static long    setJOrd = -1L;          // native writes 0x0123456789ABCDEF
    public static double  setDOrd = -1.0;         // native writes Math.PI
    public static float   setFOrd = -1.0f;        // native writes 1.5f (exact in binary)

    // String SET target: ASCII, overwritten with "world".  field_proxy::set now
    // REBINDS the field to a freshly-built java.lang.String of the exact value
    // (library bug #30 fixed); it no longer mutates the backing array in place.
    //
    // Built with new String(char[]) (via freshAscii) so it starts with a PRIVATE,
    // non-interned backing.  This keeps the rebind-safety guarantee testable: the
    // fixture holds a separate alias to the original object (setStrShortOriginal
    // below) and proves the rebind leaves it untouched.  (Historically a bare
    // literal here also risked the OLD in-place path corrupting the shared
    // interned String process-wide; the private backing preserves clean resets.)
    public static String  setStr = freshAscii("AAAAA");       // native writes "world" -> "world"

    // String SET target overwritten with a SHORTER value ("hi").  The rebind
    // makes the field read exactly "hi" (length 2), NOT the old partial-overwrite
    // "hirld" (length 5).  Private backing (freshAscii) so the original object can
    // be aliased and checked for non-corruption after the rebind.
    public static String  setStrShort = freshAscii("world");  // native writes "hi" -> "hi"

    // A SEPARATE alias to setStrShort's ORIGINAL String object, captured at class
    // init BEFORE any native write.  After set() rebinds setStrShort to a new "hi"
    // String, this still references the original, which must STILL read "world"
    // (object-reference store, not in-place mutate -> no shared-object corruption).
    public static final String setStrShortOriginal = setStrShort;
    public static boolean       seenStrShortOriginalIntact;  // setStrShortOriginal.equals("world")

    // String SET edge targets (private backings, never interned literals):
    //   - setStrEmpty: native writes "" -> the rebind builds a real empty String,
    //     so the field reads "" (NOT the old writable_length<=0 no-op-keep).
    //   - setStrTrunc: native writes "toolongvalue" (len 12) -> the rebind allocates
    //     the full length, so the field reads "toolongvalue" (NOT the old truncate).
    public static String  setStrEmpty = freshAscii("keep");   // native writes "" -> ""
    public static String  setStrTrunc = freshAscii("ABCDE");  // native writes long -> "toolongvalue"

    // =====================================================================
    //  EXHAUSTIVE SET-EDGE targets, each with a matching getX() getter so the
    //  native side can (a) re-read the slot in C++ and (b) pull the value back
    //  through Java bytecode via static_method("getX")->call().  These fill the
    //  0/1/-1 + boundary matrix the primary/secondary batteries don't cover.
    // =====================================================================
    public static int     setIZero   = 7;     // native writes 0
    public static int     setIOne    = 7;     // native writes 1
    public static int     setINegOne = 7;     // native writes -1
    public static int     setIMax    = 0;     // native writes Integer.MAX_VALUE
    public static long    setJZero   = 7L;    // native writes 0
    public static long    setJOne    = 7L;    // native writes 1
    public static long    setJNegOne = 7L;    // native writes -1
    public static byte    setBZero   = 9;     // native writes 0
    public static byte    setBMax    = 0;     // native writes Byte.MAX_VALUE (127)
    public static short   setSZero   = 9;     // native writes 0
    public static short   setSMax    = 0;     // native writes Short.MAX_VALUE
    public static char    setCNul    = 0x0041;// native writes 0x0000 ('\0')
    public static char    setCA      = 0x0000;// native writes 0x0041 ('A')
    public static float   setFPosInf = 0.0f;  // native writes +Infinity
    public static float   setFMin    = 0.0f;  // native writes Float.MIN_VALUE
    public static float   setFMax    = 0.0f;  // native writes Float.MAX_VALUE
    public static float   setFNegZero= 1.0f;  // native writes -0.0
    public static double  setDPosInf = 0.0;   // native writes +Infinity
    public static double  setDMin    = 0.0;   // native writes Double.MIN_VALUE
    public static double  setDMax    = 0.0;   // native writes Double.MAX_VALUE
    public static double  setDNegZero= 1.0;   // native writes -0.0

    // =====================================================================
    //  static final CONSTANTS (compile-time inlined via the ConstantValue
    //  attribute).  Characterizes what vmhook's static_field() actually reads:
    //  vmhook reads the LIVE java.lang.Class mirror slot (which the class
    //  initializer sets to the constant), NOT the inlined literal -- so a GET
    //  returns the real stored value, and a SET overwrites the mirror slot
    //  even though Java *references* to the constant keep using the inlined
    //  literal (a getter that `return CONST_I;` returns the OLD value, while
    //  static_field("CONST_I")->get() returns the NEW one).  getConstIField()
    //  re-reads via getstatic so we can show the mirror slot really changed.
    // =====================================================================
    public static final int     CONST_I = 0x0A0B0C0D;
    public static final long    CONST_J = 0x0102030405060708L;
    public static final boolean CONST_Z = true;
    public static final char    CONST_C = 'Q';                 // 0x0051
    public static final String  CONST_STR = "konst";

    // =====================================================================
    //  PRIMITIVE-vs-PRIMITIVE TYPE/SIZE GUARD targets (audit:
    //  field_proxy_set_size_guard.md).  The native side attempts MISTYPED
    //  writes and we assert (Java-side) the value is UNCHANGED:
    //   - guardInt: native tries set(int64) and set(std::string) -> refused.
    //   - guardLong: native tries set(int32) -> refused (too narrow).
    //   - guardChar: native writes a 1-byte C++ char -> widening shortcut
    //                must land the full 2-byte char (0x00NN), not clobber.
    // =====================================================================
    public static int     guardInt  = 0x11223344; // must remain unchanged
    public static long    guardLong  = 0x1122334455667788L; // must remain unchanged
    public static char    guardChar = 0x0000;      // native writes 'Z'(0x5A) via 1-byte char

    // =====================================================================
    //  OBJECT-REFERENCE SET targets.  objA / objB are two distinct published
    //  instances.  The native side rewrites objRef (initially objA) to point
    //  at objB via set(unique_ptr<wrapper>), then to null via an empty
    //  unique_ptr.  The probe records identity comparisons Java-side.
    // =====================================================================
    public static FieldStatic objA = new FieldStatic();
    public static FieldStatic objB = new FieldStatic();
    public static FieldStatic objRef = objA;       // native rewrites to objB, then null

    // A live instance the native side can wrap for instance-side cross-checks
    // (and to disambiguate static vs instance handling).  Carries a tag so the
    // two published instances are distinguishable when read back.
    public int tag = 0;

    // =====================================================================
    //  GET-only static fields with boundary values, so the native GET path is
    //  re-proven here under the field_static module too (independent of
    //  field_primitives_get).  These are never written by native.
    // =====================================================================
    public static boolean gZTrue  = true;
    public static boolean gZFalse = false;
    public static byte    gBMin   = Byte.MIN_VALUE;
    public static byte    gBMax   = Byte.MAX_VALUE;
    public static short   gSMin   = Short.MIN_VALUE;
    public static short   gSMax   = Short.MAX_VALUE;
    public static char    gCMax   = 0xFFFF;
    public static int     gIMin   = Integer.MIN_VALUE;
    public static int     gIMax   = Integer.MAX_VALUE;
    public static long    gJMin   = Long.MIN_VALUE;
    public static long    gJMax   = Long.MAX_VALUE;
    public static float   gFOne   = Float.intBitsToFloat(0x3F800000); // +1.0
    public static double  gDOne   = Double.longBitsToDouble(0x3FF0000000000000L); // +1.0
    public static String  gStr    = "field_static";

    // ---- EXHAUSTIVE integral boundary GET battery: 0 / 1 / -1 for every
    //      signed integral width, so the static GET decode is proven at the
    //      "ordinary small" boundaries (the prior battery only had MIN/MAX). ----
    public static int     gIZero   = 0;
    public static int     gIOne    = 1;
    public static int     gINegOne = -1;
    public static long    gJZero   = 0L;
    public static long    gJOne    = 1L;
    public static long    gJNegOne = -1L;
    public static byte    gBZero   = 0;
    public static byte    gBOne    = 1;
    public static byte    gBNegOne = -1;
    public static short   gSZero   = 0;
    public static short   gSOne    = 1;
    public static short   gSNegOne = -1;

    // ---- char GET boundaries: '\0' / 'A' / 0xFFFF (the unsigned 16-bit edges). ----
    public static char    gCNul    = 0x0000;
    public static char    gCA      = 0x0041; // 'A'

    // ---- EXHAUSTIVE float boundary GET battery (compared by exact bit pattern
    //      so the check is -Werror clean -- never an == on a float value). ----
    public static float   gFZero    = 0.0f;                              // 0x00000000
    public static float   gFNegZero = Float.intBitsToFloat(0x80000000); // -0.0
    public static float   gFMin     = Float.MIN_VALUE;                  // 0x00000001 (smallest subnormal)
    public static float   gFMax     = Float.MAX_VALUE;                  // 0x7F7FFFFF
    public static float   gFPosInf  = Float.POSITIVE_INFINITY;          // 0x7F800000
    public static float   gFNegInf  = Float.NEGATIVE_INFINITY;          // 0xFF800000
    public static float   gFNan     = Float.NaN;                        // 0x7FC00000 (canonical)

    // ---- EXHAUSTIVE double boundary GET battery (exact bit pattern compares). ----
    public static double  gDZero    = 0.0;                                          // 0x0000000000000000
    public static double  gDNegZero = Double.longBitsToDouble(0x8000000000000000L); // -0.0
    public static double  gDMin     = Double.MIN_VALUE;                             // 0x...0001
    public static double  gDMax     = Double.MAX_VALUE;                             // 0x7FEFFFFFFFFFFFFF
    public static double  gDPosInf  = Double.POSITIVE_INFINITY;                     // 0x7FF0000000000000
    public static double  gDNegInf  = Double.NEGATIVE_INFINITY;                     // 0xFFF0000000000000
    public static double  gDNan     = Double.NaN;                                   // 0x7FF8000000000000

    // ---- a null static String field: GET must resolve, report the String
    //      signature, decode the compressed-0 OOP, and read back "". ----
    public static String  gNullStr = null;

    /** An instance (non-static) int field, to drive the "needs an object" path. */
    public int instanceOnlyInt = 4242;

    /**
     * Hookable instance method.  The native module hooks this and, from INSIDE
     * the detour (where HotSpot's current_java_thread is set, the precondition
     * for method_proxy::call()), pulls each native-written static field back
     * through the Java getX() getters via static_method("getX")->call().  The
     * probe (mode 4) calls this once on objA so the detour fires on a real
     * bytecode dispatch.  Returns delta unchanged so the caller can sanity it.
     */
    public int touch(final int delta)
    {
        return delta;
    }

    // =====================================================================
    //  WITNESS fields ("seen*") -- the probe writes these from the set*
    //  targets using genuine getstatic/putstatic bytecode in run() (mode 1).
    //  The native side reads these back to confirm Java observed the writes.
    // =====================================================================
    public static boolean seenZ;
    public static byte    seenB;
    public static char    seenC;
    public static short   seenS;
    public static int     seenI;
    public static long    seenJ;
    public static int     seenFBits;   // Float.floatToRawIntBits(setF)
    public static long    seenDBits;   // Double.doubleToRawLongBits(setD)

    public static boolean seenZ2;
    public static byte    seenB2;
    public static char    seenC2;
    public static short   seenS2;
    public static int     seenI2;
    public static long    seenJ2;
    public static int     seenF2Bits;
    public static long    seenD2Bits;

    public static int     seenIOrd;
    public static long    seenJOrd;
    public static long    seenDOrdBits;
    public static int     seenFOrdBits;

    public static String  seenStr;        // copy of setStr seen by Java
    public static int      seenStrLen;     // setStr.length() seen by Java
    public static boolean  seenStrEqWorld; // setStr.equals("world")
    public static String   seenStrShort;   // copy of setStrShort
    public static int       seenStrShortLen;

    public static int      seenGuardInt;
    public static long      seenGuardLong;
    public static char      seenGuardChar;

    public static boolean  seenObjRefIsA;  // objRef == objA at snapshot time
    public static boolean  seenObjRefIsB;  // objRef == objB at snapshot time
    public static boolean  seenObjRefIsNull;
    public static int       seenObjRefTag;  // objRef == null ? -1 : objRef.tag

    // =====================================================================
    //  RUNTIME GET targets (mode 2) -- written by writeRuntime() via putstatic
    //  so the native GET path reads live, post-dispatch JVM state.
    // =====================================================================
    public static boolean rZ;
    public static int     rI;
    public static long    rJ;
    public static double  rD;
    public static char    rC;

    // ---- Java getter methods (called natively via static_method to prove the
    //      static-wrapper-method path + that Java bytecode reads the native
    //      write).  Each returns the current field value. ----
    public static boolean getZ() { return setZ; }
    public static byte    getB() { return setB; }
    public static char    getC() { return setC; }
    public static short   getS() { return setS; }
    public static int     getI() { return setI; }
    public static long    getJ() { return setJ; }
    public static float   getF() { return setF; }
    public static double  getD() { return setD; }
    public static int     getIOrd() { return setIOrd; }
    public static String  getStr() { return setStr; }
    public static int     getStrLen() { return setStr == null ? -1 : setStr.length(); }
    public static int     getGuardInt() { return guardInt; }
    public static long    getGuardLong() { return guardLong; }
    public static char    getGuardChar() { return guardChar; }
    public static int     getObjRefTag() { return objRef == null ? -1 : objRef.tag; }
    public static boolean objRefIsB() { return objRef == objB; }
    public static boolean objRefIsNull() { return objRef == null; }

    // ---- getters for the EXHAUSTIVE SET-EDGE targets (genuine getstatic). ----
    public static int     getIZero() { return setIZero; }
    public static int     getIOne() { return setIOne; }
    public static int     getINegOne() { return setINegOne; }
    public static int     getIMax() { return setIMax; }
    public static long    getJZero() { return setJZero; }
    public static long    getJOne() { return setJOne; }
    public static long    getJNegOne() { return setJNegOne; }
    public static byte    getBZero() { return setBZero; }
    public static byte    getBMax() { return setBMax; }
    public static short   getSZero() { return setSZero; }
    public static short   getSMax() { return setSMax; }
    public static char    getCNul() { return setCNul; }
    public static char    getCA() { return setCA; }
    public static int     getFPosInfBits() { return Float.floatToRawIntBits(setFPosInf); }
    public static int     getFMinBits() { return Float.floatToRawIntBits(setFMin); }
    public static int     getFMaxBits() { return Float.floatToRawIntBits(setFMax); }
    public static int     getFNegZeroBits() { return Float.floatToRawIntBits(setFNegZero); }
    public static long    getDPosInfBits() { return Double.doubleToRawLongBits(setDPosInf); }
    public static long    getDMinBits() { return Double.doubleToRawLongBits(setDMin); }
    public static long    getDMaxBits() { return Double.doubleToRawLongBits(setDMax); }
    public static long    getDNegZeroBits() { return Double.doubleToRawLongBits(setDNegZero); }

    // ---- static-final CONSTANT readers.  Two flavours, deliberately:
    //   * getConstIInlined() does `return CONST_I;` -- javac CONSTANT-FOLDS this
    //     to an ldc of the original literal, so it NEVER reflects a mirror write.
    //   * getConstIReflect() reads the LIVE mirror slot via reflection (no
    //     constant folding), so it DOES reflect a vmhook write to the slot.
    //  The pair lets the native module prove that static_field()->set() lands on
    //  the mirror slot (reflective read changes) while Java's inlined references
    //  do not see it (inlined read unchanged) -- the documented constant caveat. ----
    public static int     getConstIInlined() { return CONST_I; }
    public static long    getConstJInlined() { return CONST_J; }
    public static int     getConstCInlined() { return CONST_C; }
    public static int     getConstIReflect()
    {
        try { return FieldStatic.class.getField("CONST_I").getInt(null); }
        catch (Exception e) { return -1; }
    }
    public static long    getConstJReflect()
    {
        try { return FieldStatic.class.getField("CONST_J").getLong(null); }
        catch (Exception e) { return -1L; }
    }
    public static int     getConstCReflect()
    {
        try { return FieldStatic.class.getField("CONST_C").getChar(null); }
        catch (Exception e) { return -1; }
    }
    public static String  getConstStrReflect()
    {
        try
        {
            Object v = FieldStatic.class.getField("CONST_STR").get(null);
            return (v == null) ? "<null>" : (String) v;
        }
        catch (Exception e) { return "<err>"; }
    }
    public static String  getStrEmpty() { return setStrEmpty; }
    public static String  getStrTrunc() { return setStrTrunc; }

    // ---- getters for the static ARRAY fields (genuine getstatic) -----------
    public static int     getIntArrLen()   { return sIntArr == null ? -1 : sIntArr.length; }
    public static int     getIntArrFirst() { return sIntArr == null ? -1 : sIntArr[0]; }
    public static boolean intArrIsAlt()    { return sIntArr == sIntArrAlt; }
    public static boolean intArrIsNull()   { return sIntArr == null; }
    public static int     getArrSum()      { return sArrSum == null ? -1 : sArrSum[0]; }
    // ---- getter for the static enum-ref + the enum constant's field --------
    public static int     getStaticTierWeight() { return staticTier == null ? -1 : staticTier.weight; }
    public static boolean staticTierIsMid()     { return staticTier == Tier.MID; }

    /**
     * Builds a String from the chars of {@code text} via new String(char[]),
     * guaranteeing a PRIVATE backing array never shared with an interned
     * constant-pool literal.  The String SET targets use it so each starts with a
     * private object that can be aliased and checked for non-corruption after the
     * rebind (set() now rebinds the field reference rather than mutating backing
     * in place; see the setStr/setStrShort notes).
     */
    private static String freshAscii(final String text)
    {
        return new String(text.toCharArray());
    }

    private static void snapshot()
    {
        // getstatic each set* target, putstatic into the witness -- genuine
        // bytecode so the JVM's own view of the field is captured.
        seenZ = setZ;
        seenB = setB;
        seenC = setC;
        seenS = setS;
        seenI = setI;
        seenJ = setJ;
        seenFBits = Float.floatToRawIntBits(setF);
        seenDBits = Double.doubleToRawLongBits(setD);

        seenZ2 = setZ2;
        seenB2 = setB2;
        seenC2 = setC2;
        seenS2 = setS2;
        seenI2 = setI2;
        seenJ2 = setJ2;
        seenF2Bits = Float.floatToRawIntBits(setF2);
        seenD2Bits = Double.doubleToRawLongBits(setD2);

        seenIOrd = setIOrd;
        seenJOrd = setJOrd;
        seenDOrdBits = Double.doubleToRawLongBits(setDOrd);
        seenFOrdBits = Float.floatToRawIntBits(setFOrd);

        seenStr = setStr;
        seenStrLen = (setStr == null) ? -1 : setStr.length();
        seenStrEqWorld = "world".equals(setStr);
        seenStrShort = setStrShort;
        seenStrShortLen = (setStrShort == null) ? -1 : setStrShort.length();
        // The rebind bound setStrShort to a new "hi" String; the original object
        // (aliased here) must be untouched -> still "world".
        seenStrShortOriginalIntact = "world".equals(setStrShortOriginal);

        seenGuardInt = guardInt;
        seenGuardLong = guardLong;
        seenGuardChar = guardChar;

        seenObjRefIsA = (objRef == objA);
        seenObjRefIsB = (objRef == objB);
        seenObjRefIsNull = (objRef == null);
        seenObjRefTag = (objRef == null) ? -1 : objRef.tag;
    }

    private static void writeRuntime()
    {
        rZ = true;
        rI = Integer.MIN_VALUE;
        rJ = Long.MAX_VALUE;
        rD = Double.longBitsToDouble(0x7FF8000000000000L); // canonical NaN
        rC = 0xFFFF;
    }

    private static void resetTargets()
    {
        // freshAscii (not bare literals): assigning a literal here would re-alias
        // the interned "AAAAA"/"world" objects that the phase-2 native writes
        // already mutated in place, so the "restored" field would still read the
        // post-mutation content.  A private backing array gives a clean reset.
        setStr = freshAscii("AAAAA");
        setStrShort = freshAscii("world");
        guardInt = 0x11223344;
        guardLong = 0x1122334455667788L;
        guardChar = 0x0000;
        objRef = objA;
    }

    /**
     * mode 6: snapshot the static array reference state (after the native side
     * has optionally replaced/nulled sIntArr) using genuine getstatic, and
     * publish the sum of sIntArr's elements into sArrSum so the native side can
     * prove the array it now sees through the static slot is the live one.
     */
    private static void snapshotArrays()
    {
        seenIntArrIsAlt  = (sIntArr == sIntArrAlt);
        seenIntArrIsNull = (sIntArr == null);
        seenIntArrLen    = (sIntArr == null) ? -1 : sIntArr.length;
        seenIntArrFirst  = (sIntArr == null) ? -1 : sIntArr[0];
        int sum = 0;
        if (sIntArr != null)
        {
            for (int i = 0; i < sIntArr.length; i++)
            {
                sum += sIntArr[i];
            }
        }
        sArrSum = new int[] { sum };
    }

    /** Restore the array references to their initial values (clean re-run). */
    private static void resetArrays()
    {
        sIntArr  = new int[] { 10, 20, 30 };
        sStrArr  = new String[] { "x", "y" };
        sNullArr = null;
    }

    /**
     * mode 7: publish enum identities / ordinals / a constant's field via real
     * bytecode, so the native distinctness + value checks are EXACT.
     */
    private static void publishEnum()
    {
        tierLowIdentity    = System.identityHashCode(Tier.LOW);
        tierMidIdentity    = System.identityHashCode(Tier.MID);
        tierHighIdentity   = System.identityHashCode(Tier.HIGH);
        staticTierIdentity = System.identityHashCode(staticTier);
        tierValuesLen      = Tier.values().length;
        tierMidOrdinal     = Tier.MID.ordinal();
        tierMidWeight      = Tier.MID.weight;
    }

    static
    {
        objA.tag = 0xA;
        objB.tag = 0xB;

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldStatic.go && !FieldStatic.done;
            }

            @Override
            public void run()
            {
                switch (FieldStatic.mode)
                {
                    case 1:
                        snapshot();
                        break;
                    case 2:
                        writeRuntime();
                        break;
                    case 3:
                        resetTargets();
                        break;
                    case 4:
                        // Real bytecode dispatch so the native interpreter hook
                        // on touch() fires; the detour does the getX() call-backs.
                        objA.touch(0x5A);
                        break;
                    case 5:
                        // Snapshot every INHERITED static (declared on the
                        // superclass) into its seenInh* witness via getstatic,
                        // so the module reads back what the JVM observed for the
                        // native writes to the declaring-class mirror.
                        FieldStaticBase.snapshotInherited();
                        break;
                    case 6:
                        // Snapshot the static ARRAY reference state via getstatic.
                        snapshotArrays();
                        break;
                    case 7:
                        // Publish enum identities/ordinals via real bytecode.
                        publishEnum();
                        break;
                    case 8:
                        // Restore inherited + array targets to initial values.
                        FieldStaticBase.resetInherited();
                        resetArrays();
                        break;
                    default:
                        break;
                }
                FieldStatic.done = true;
            }
        });
    }
}
