package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_overload feature (area: methods).
 *
 * The ONE thing under test: vmhook::method_proxy overload resolution.
 *
 *     get_method("name")->call(args)   // picks the overload whose JVM parameter
 *                                       // descriptors match the C++ arg TYPES
 *
 * get_method("name") (vmhook.hpp object::get_method, name-only) latches onto the
 * FIRST method with that name in HotSpot's InstanceKlass._methods array.  That
 * array is sorted by the name Symbol's identity, NOT by declaration order, so
 * for an overloaded name "which overload is first" is effectively arbitrary.
 * method_proxy::resolve_compatible_method() must therefore walk the klass and
 * re-pick the overload whose descriptor type-checks against the C++ argument
 * pack (int->I, long->J, double->D, float->F, std::string->Ljava/lang/String;,
 * bool->Z, int8->B, int16->S, uint16->C, wrapper/oop->L...;), disambiguating
 * also by ARITY.  This is the exact vanilla-Minecraft-1.8.9 "EntityPlayerSP.a"
 * regression the feature exists to fix.
 *
 * To make resolution observable, EVERY overload returns a DISTINCT int sentinel
 * (encoded as RET_* below).  The native side calls pick(<typed arg>) and asserts
 * the returned sentinel equals the sentinel of the overload whose parameter type
 * matches that C++ type.  A wrong pick returns a different sentinel, so a
 * mis-resolution is caught as a value mismatch rather than "it didn't crash".
 *
 * Coverage layers:
 *   - single-arg primitive overloads: int / long / double / float / boolean /
 *     byte / short / char  (the full primitive descriptor set I J D F Z B S C),
 *   - reference overloads: String (Ljava/lang/String;) vs Object (Ljava/lang/
 *     Object;) vs Integer (Ljava/lang/Integer;) — THREE distinct reference types
 *     so the native side proves the matcher discriminates on the FULL class name
 *     inside 'L...;', not merely "is a reference"; a wrapper registered as each
 *     class resolves to exactly its overload,
 *   - ARITY overloads: pick() / pick(int) / pick(int,int) / pick(int,int,int)
 *     all share the name "pick" and are told apart purely by argument count,
 *     including the HIGH-arity pair pick(int x8) / pick(int x9) that straddles
 *     the 8-jvalue interpreter/JNI argument-packing boundary,
 *   - two-arg type-order overloads: pick(int,long) vs pick(long,int) vs
 *     pick(int,String) vs pick(String,int) vs pick(long,double) — proves the
 *     matcher checks EACH parameter slot (primitive AND reference), respects
 *     ORDER (int,String vs String,int share a type multiset but differ by slot),
 *     and walks two CONSECUTIVE wide (two-slot) params (long,double -> "(JD)I"),
 *   - same-type two-arg overloads: pick(double,double) / pick(float,float) /
 *     pick(boolean,boolean) and the two-reference shapes pick(Object,Object) /
 *     pick(Integer,Integer) — each pair told apart from the mixed pairs and from
 *     each other purely by per-slot descriptor, with both same-width slots proven
 *     to survive independently,
 *   - ARRAY-vs-scalar overloads: pick(int[]) / pick(long[]) / pick(char[])
 *     ("[I" / "[J" / "[C") sit alongside pick(int) / pick(long) / pick(char) so
 *     the native side proves the resolver's array-token parser walks past a '['
 *     descriptor when matching a scalar arg and reaches the array body for an
 *     explicit "([I)I" / "([J)I" / "([C)I" call,
 *   - per-overload boundary values (INT_MIN/MAX, LONG_MIN/MAX, the float-vs-
 *     double-vs-int-vs-long ambiguity of the literal 3) recorded via lastArg*,
 *   - STATIC overloads mirrored as spick(...) so the native side can exercise
 *     the static-call resolution path (a KNOWN-broken path on JDKs where the
 *     call stub is absent — see the native module + agent notes),
 *   - no-match probes (graceful failure, never a crash): onlyInt(I)I called with
 *     a double (type mismatch) and with zero args (arity mismatch) — both
 *     primitive, so the fallback dispatch is memory-safe; and onlyRef(Integer)
 *     called with a wrapper registered as java/lang/Double (reference-type
 *     mismatch) — its descriptor matches no onlyRef overload, so resolution falls
 *     back to the SOLE onlyRef(Integer) and returns deterministically (8500).
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks).
 */
public final class MethodOverload
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Only mode 0 is used today (drive
     * tick() so the native detour performs every resolution call on `self`); the
     * selector is present per the harness contract for future scenarios.
     */
    public static volatile int mode;

    // ── Distinct return sentinels: one per overload ────────────────────────
    // The native side mirrors these EXACTLY.  Each value is unique so the
    // returned int uniquely identifies which overload the resolver picked.
    public static final int RET_NOARG       = 1000;  // pick()
    public static final int RET_INT         = 1001;  // pick(int)
    public static final int RET_LONG        = 1002;  // pick(long)
    public static final int RET_DOUBLE      = 1003;  // pick(double)
    public static final int RET_FLOAT       = 1004;  // pick(float)
    public static final int RET_BOOLEAN     = 1005;  // pick(boolean)
    public static final int RET_BYTE        = 1006;  // pick(byte)
    public static final int RET_SHORT       = 1007;  // pick(short)
    public static final int RET_CHAR        = 1008;  // pick(char)
    public static final int RET_STRING      = 1009;  // pick(String)
    public static final int RET_OBJECT      = 1010;  // pick(Object)
    public static final int RET_INTEGER     = 1011;  // pick(Integer)  — a SECOND
    //   reference overload distinct from Object: descriptor Ljava/lang/Integer;.
    //   A wrapper REGISTERED as java/lang/Integer must resolve here, NOT to
    //   pick(Object) (Ljava/lang/Object;) nor pick(String) (Ljava/lang/String;),
    //   proving the matcher compares the FULL class name inside 'L...;', not just
    //   "is a reference".  Body must NOT dereference `a` (the native side may pass
    //   an oop of a different runtime type to drive resolution only).
    public static final int RET_INT_INT     = 1021;  // pick(int,int)
    public static final int RET_INT_INT_INT = 1022;  // pick(int,int,int)
    public static final int RET_INT_LONG    = 1023;  // pick(int,long)
    public static final int RET_LONG_INT    = 1024;  // pick(long,int)
    public static final int RET_INT_STRING  = 1025;  // pick(int,String)
    public static final int RET_STRING_INT  = 1026;  // pick(String,int)  — the
    //   reference-FIRST mirror of pick(int,String): proves the matcher checks the
    //   FIRST parameter slot's descriptor too (a reference) and respects ORDER, so
    //   pick(String,int) and pick(int,String) are told apart by slot, not by the
    //   multiset of their parameter types.
    public static final int RET_LONG_DOUBLE = 1027;  // pick(long,double)  — TWO
    //   consecutive wide (two-interpreter-slot) parameters, "(JD)I".  Here the
    //   concern is RESOLUTION ONLY: a (int64_t,double) call must select THIS
    //   overload, distinct from pick(int,long) "(IJ)I" / pick(long,int) "(JI)I".
    //   (Exhaustive wide-slot PACKING fidelity — truncation / slot-shift across
    //   long+double adjacency — is owned by the method_call_wide_args module's
    //   jd/dj/addD/mixD methods; this overload exists so the RESOLVER's two-wide
    //   parameter walk is exercised in the overload-disambiguation module.)
    // Array overloads: descriptor "[I" / "[J" / "[C" — a leading '[' the matcher
    // must parse via next_argument_descriptor's array branch and treat as DISTINCT
    // from the scalar "I" / "J" / "C" overloads (array-vs-scalar disambiguation).
    public static final int RET_INT_ARRAY   = 1031;  // pick(int[])
    public static final int RET_LONG_ARRAY  = 1032;  // pick(long[])
    public static final int RET_CHAR_ARRAY  = 1033;  // pick(char[])  — a THIRD array
    //   element type "[C", so the array-token parser is exercised on more than the
    //   {int,long} pair and a char[] resolves DISTINCTLY from int[]/long[].

    // Same-type two-arg shapes the resolver must tell apart from the mixed-type
    // pairs above (and from each other) purely by their per-slot descriptors.
    public static final int RET_DOUBLE_DOUBLE = 1041;  // pick(double,double)  "(DD)I"
    public static final int RET_FLOAT_FLOAT   = 1042;  // pick(float,float)    "(FF)I"
    public static final int RET_BOOL_BOOL     = 1043;  // pick(boolean,boolean)"(ZZ)I"
    // Two-REFERENCE overloads: each slot's reference CLASS is matched, so a
    // (Object,Object) call must NOT collapse onto (Integer,Integer) and vice
    // versa — per-slot 'L...;' discrimination in a two-reference shape.
    public static final int RET_OBJ_OBJ       = 1044;  // pick(Object,Object)
    public static final int RET_INT_INT_REFS  = 1045;  // pick(Integer,Integer)

    // High-arity overloads that straddle the 8-argument interpreter/JNI packing
    // boundary: pick(int x8) fills exactly eight jvalue slots, pick(int x9) needs
    // a ninth.  Resolution must disambiguate them from each other and from the
    // lower arities purely by argument COUNT; the native side also proves every
    // slot's value survived (echoed via lastArgCount + lastArgFirst/lastArgLast).
    public static final int RET_INT8  = 1051;  // pick(int x8)  "(IIIIIIII)I"
    public static final int RET_INT9  = 1052;  // pick(int x9)  "(IIIIIIIII)I"

    // Static twins reuse the SAME sentinels + a +100 bias so the native side
    // can tell an instance hit from a static hit even if a JDK quirk routed one
    // through the other.
    public static final int SBIAS = 100;

    // ── Argument echoes (so the native side can prove the RIGHT value reached
    //    the RIGHT slot, not merely that the right overload fired) ──────────
    public static volatile int     lastIntArg;
    public static volatile long    lastLongArg;
    public static volatile double  lastDoubleArg;
    public static volatile float   lastFloatArg;
    public static volatile boolean lastBoolArg;
    public static volatile byte    lastByteArg;
    public static volatile short   lastShortArg;
    public static volatile char    lastCharArg;
    public static volatile String  lastStringArg;
    public static volatile int     lastArg2A;
    public static volatile long    lastArg2B;
    // Array echoes: the length and first element of the last array arg, so the
    // native side can prove the right array landed in the right (array) slot.
    public static volatile int     lastArrayLen;
    public static volatile long    lastArrayHead;
    // High-arity echoes: the argument COUNT plus the FIRST and LAST argument of
    // the last variadic-shaped pick(int x8)/(int x9) call, so the native side can
    // prove that the 8th/9th slot did not get dropped or aliased at the packing
    // boundary.  (The fixture itself sums nothing — it records the edge slots.)
    public static volatile int     lastArgCount;
    public static volatile int     lastArgFirst;
    public static volatile int     lastArgLast;
    // Same-type two-arg echoes: the two double/float/boolean operands of the last
    // (DD)/(FF)/(ZZ) call, so the native side proves both same-width slots survived
    // distinctly (a second double slot must not overwrite the first).
    public static volatile double  lastDoubleArgB;
    public static volatile float   lastFloatArgB;
    public static volatile boolean lastBoolArgB;

    // ── The hook site ──────────────────────────────────────────────────────
    /**
     * The native module hooks this; inside the detour current_java_thread is
     * live, which is the only context in which method_proxy::call() may invoke
     * the call gate.  The detour performs every pick(...) / spick(...) call.
     */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    // ── Instance overloads: ALL named "pick" ───────────────────────────────
    // Declaration order here is intentionally scrambled relative to descriptor
    // sort order; the resolver must not depend on source order.

    public int pick(final int a)        { lastIntArg = a;    return RET_INT; }
    public int pick(final String a)     { lastStringArg = a; return RET_STRING; }
    public int pick(final double a)     { lastDoubleArg = a; return RET_DOUBLE; }
    public int pick(final long a)       { lastLongArg = a;   return RET_LONG; }
    public int pick(final float a)      { lastFloatArg = a;  return RET_FLOAT; }
    public int pick(final boolean a)    { lastBoolArg = a;   return RET_BOOLEAN; }
    public int pick(final byte a)       { lastByteArg = a;   return RET_BYTE; }
    public int pick(final short a)      { lastShortArg = a;  return RET_SHORT; }
    public int pick(final char a)       { lastCharArg = a;   return RET_CHAR; }
    public int pick(final Object a)     { return RET_OBJECT; }
    // SECOND reference overload (distinct class inside 'L...;').  Body ignores `a`
    // on purpose: the native side resolves THIS overload by registering a wrapper
    // type as java/lang/Integer and may hand it an oop of another runtime type —
    // dereferencing it would be unsound, and resolution does not require it.
    public int pick(final Integer a)    { return RET_INTEGER; }

    public int pick()                                       { return RET_NOARG; }
    public int pick(final int a, final int b)               { lastArg2A = a; lastArg2B = b; return RET_INT_INT; }
    public int pick(final int a, final int b, final int c)  { return RET_INT_INT_INT; }
    public int pick(final int a, final long b)              { lastArg2A = a; lastArg2B = b; return RET_INT_LONG; }
    public int pick(final long a, final int b)              { lastArg2A = b; lastArg2B = a; return RET_LONG_INT; }
    public int pick(final int a, final String b)            { lastArg2A = a; lastStringArg = b; return RET_INT_STRING; }
    // Reference-FIRST two-arg overload: slot0 = String (a reference), slot1 = int.
    // Echo b into lastArg2A and the String into lastStringArg so the native side
    // can prove both the right reference and the right int landed in the right
    // slots (and that the order, not just the type multiset, was honoured).
    public int pick(final String a, final int b)            { lastArg2A = b; lastStringArg = a; return RET_STRING_INT; }
    // Two consecutive WIDE params (long then double) — "(JD)I".  Echo a into
    // lastArg2B (long) and b into lastDoubleArg so the native side proves the
    // resolver selected this overload AND both wide values survived into their
    // (distinct, non-overlapping) slots.
    public int pick(final long a, final double b)           { lastArg2B = a; lastDoubleArg = b; return RET_LONG_DOUBLE; }

    // Array overloads — a leading '[' in the descriptor ("[I" / "[J").  These
    // exist so the resolver's array-token parsing is exercised: scanning the
    // "pick" overloads to resolve a SCALAR int/long must walk PAST these array
    // descriptors (not mis-match them), and an explicit "([I)I" / "([J)I" call
    // must reach exactly these bodies.  echoed via lastArrayLen / lastArrayHead.
    public int pick(final int[] a)  { lastArrayLen = (a == null ? -1 : a.length); lastArrayHead = (a == null || a.length == 0 ? 0L : a[0]);          return RET_INT_ARRAY;  }
    public int pick(final long[] a) { lastArrayLen = (a == null ? -1 : a.length); lastArrayHead = (a == null || a.length == 0 ? 0L : a[0]);          return RET_LONG_ARRAY; }
    // THIRD array element type "[C": a char[] must resolve distinctly from the
    // int[]/long[] arrays AND from the scalar char overload (the array-token
    // parser walks the leading '[' then matches 'C').  Head echoed as an int.
    public int pick(final char[] a) { lastArrayLen = (a == null ? -1 : a.length); lastArrayHead = (a == null || a.length == 0 ? 0L : (long) a[0]); return RET_CHAR_ARRAY; }

    // ── Same-type two-arg overloads (per-slot descriptor disambiguation) ────
    // Two wide FP slots "(DD)I": both doubles echoed to prove they did not
    // overlap (the second must not clobber the first).
    public int pick(final double a, final double b) { lastDoubleArg = a; lastDoubleArgB = b; return RET_DOUBLE_DOUBLE; }
    // Two narrow FP slots "(FF)I".
    public int pick(final float a, final float b)   { lastFloatArg = a;  lastFloatArgB = b;  return RET_FLOAT_FLOAT; }
    // Two boolean slots "(ZZ)I" — the narrowest primitive pair.
    public int pick(final boolean a, final boolean b) { lastBoolArg = a; lastBoolArgB = b;   return RET_BOOL_BOOL; }
    // Two-REFERENCE shapes: each slot's reference CLASS is matched, so a wrapper
    // registered as java/lang/Object resolves to pick(Object,Object) and one
    // registered as java/lang/Integer to pick(Integer,Integer) — never the other.
    // Bodies ignore the oops (resolution-only; the native side may pass oops of a
    // different runtime type, as with the single-reference Object/Integer picks).
    public int pick(final Object a, final Object b)   { return RET_OBJ_OBJ; }
    public int pick(final Integer a, final Integer b) { return RET_INT_INT_REFS; }

    // ── High-arity overloads straddling the 8-argument packing boundary ─────
    // pick(int x8) fills exactly eight jvalue slots; pick(int x9) needs a ninth.
    // The resolver tells them apart purely by arg COUNT.  Echo the count and the
    // first/last operand so the native side proves no slot was dropped or aliased.
    public int pick(final int a, final int b, final int c, final int d,
                    final int e, final int f, final int g, final int h)
    {
        lastArgCount = 8; lastArgFirst = a; lastArgLast = h;
        return RET_INT8;
    }
    public int pick(final int a, final int b, final int c, final int d,
                    final int e, final int f, final int g, final int h, final int i)
    {
        lastArgCount = 9; lastArgFirst = a; lastArgLast = i;
        return RET_INT9;
    }

    // ── Static overloads: ALL named "spick" ────────────────────────────────
    // Mirrors the instance "pick" set across the FULL primitive descriptor set
    // (I J D F Z B S C) so the native side can assert that static name-only
    // overload resolution (resolve_compatible_method deriving the declaring
    // klass from the Method's ConstantPool _pool_holder when object==nullptr)
    // re-picks the arg-MATCHING overload — the exact regression that fix #7
    // restored.  Declaration order is scrambled relative to descriptor order.
    public static int spick(final int a)     { lastIntArg = a;    return RET_INT + SBIAS; }
    public static int spick(final String a)  { lastStringArg = a; return RET_STRING + SBIAS; }
    public static int spick(final double a)  { lastDoubleArg = a; return RET_DOUBLE + SBIAS; }
    public static int spick(final long a)    { lastLongArg = a;   return RET_LONG + SBIAS; }
    public static int spick(final float a)   { lastFloatArg = a;  return RET_FLOAT + SBIAS; }
    public static int spick(final boolean a) { lastBoolArg = a;   return RET_BOOLEAN + SBIAS; }
    public static int spick(final byte a)    { lastByteArg = a;   return RET_BYTE + SBIAS; }
    public static int spick(final short a)   { lastShortArg = a;  return RET_SHORT + SBIAS; }
    public static int spick(final char a)    { lastCharArg = a;   return RET_CHAR + SBIAS; }
    public static int spick(final int a, final int b) { lastArg2A = a; lastArg2B = b; return RET_INT_INT + SBIAS; }
    // STATIC reference overload twin (the historically-buggy path): a wrapper
    // registered as java/lang/Integer must re-pick THIS over spick(String).  Body
    // ignores `a` (same reason as pick(Integer)).
    public static int spick(final Integer a) { return RET_INTEGER + SBIAS; }
    // STATIC two-wide-parameter twin "(JD)I": (int64_t,double) must select this,
    // distinct from any single-wide or narrow static overload.  RESOLUTION focus;
    // wide-slot packing fidelity is owned by method_call_wide_args.
    public static int spick(final long a, final double b) { lastArg2B = a; lastDoubleArg = b; return RET_LONG_DOUBLE + SBIAS; }
    // STATIC high-arity twin straddling the 8-arg boundary: spick(int x8) must be
    // selected by an 8-int static call (and told from every lower static arity),
    // exercising the static call packer at exactly eight jvalue slots.  Echoes the
    // count + edge operands (shared lastArg* fields) like its instance twin.
    public static int spick(final int a, final int b, final int c, final int d,
                            final int e, final int f, final int g, final int h)
    {
        lastArgCount = 8; lastArgFirst = a; lastArgLast = h;
        return RET_INT8 + SBIAS;
    }

    // ── A method with exactly ONE signature, for the no-overload baseline ──
    // Single signature => no ambiguity => resolves on every path.  The native
    // side calls it both with the matching type and with a non-matching type to
    // observe the "fall back to this->method" behaviour.
    public int onlyInt(final int a) { lastIntArg = a; return 7000 + a; }

    // A method with exactly ONE *reference* signature, for the reference-typed
    // no-match baseline.  The native side calls it with a wrapper REGISTERED as a
    // DIFFERENT class (java/lang/Double), whose descriptor Ljava/lang/Double;
    // matches no overload of onlyRef (its only signature is Ljava/lang/Integer;).
    // resolve_compatible_method finds no arg-matching overload and falls back to
    // this->method — the SOLE onlyRef(Integer) — which then dispatches.  The
    // return is therefore deterministic (8500), proving the reference no-match
    // fallback is graceful (no crash) AND well-defined for a single-overload name.
    // Body IGNORES `a`: the native side passes an oop of another runtime type, so
    // dereferencing it would be unsound and is unnecessary for this check.
    public int onlyRef(final Integer a) { return 8500; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodOverload.go && !MethodOverload.done;
            }

            @Override
            public void run()
            {
                // Drive tick() on the shared SINGLETON so the native detour's
                // `self` is exactly this instance.  All resolution calls happen
                // inside that detour where a JavaThread is live.
                SINGLETON.tick(7);
                MethodOverload.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps.  Created eagerly so the
     * detour's `self` OOP is deterministic and matches what the probe drove.
     */
    public static final MethodOverload SINGLETON = new MethodOverload();
}
