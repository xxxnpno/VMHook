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
 *   - reference overloads: String (Ljava/lang/String;) vs Object (the wildcard
 *     L...; branch) so the native side proves String resolves distinctly from
 *     a generic Object overload,
 *   - ARITY overloads: pick() / pick(int) / pick(int,int) / pick(int,int,int)
 *     all share the name "pick" and are told apart purely by argument count,
 *   - two-arg type-order overloads: pick(int,long) vs pick(long,int) vs
 *     pick(int,String) — proves the matcher checks EACH parameter slot, not just
 *     the first, and respects order,
 *   - ARRAY-vs-scalar overloads: pick(int[]) / pick(long[]) ("[I" / "[J") sit
 *     alongside pick(int) / pick(long) so the native side proves the resolver's
 *     array-token parser walks past a '[' descriptor when matching a scalar arg
 *     and reaches the array body for an explicit "([I)I" / "([J)I" call,
 *   - per-overload boundary values (INT_MIN/MAX, LONG_MIN/MAX, the float-vs-
 *     double-vs-int-vs-long ambiguity of the literal 3) recorded via lastArg*,
 *   - STATIC overloads mirrored as spick(...) so the native side can exercise
 *     the static-call resolution path (a KNOWN-broken path on JDKs where the
 *     call stub is absent — see the native module + agent notes),
 *   - a no-match probe: callNoOverload is a name that exists with exactly one
 *     signature the native side will deliberately call with a non-matching arg
 *     type, to observe the fallback-to-first behaviour.
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
    public static final int RET_INT_INT     = 1021;  // pick(int,int)
    public static final int RET_INT_INT_INT = 1022;  // pick(int,int,int)
    public static final int RET_INT_LONG    = 1023;  // pick(int,long)
    public static final int RET_LONG_INT    = 1024;  // pick(long,int)
    public static final int RET_INT_STRING  = 1025;  // pick(int,String)
    // Array overloads: descriptor "[I" / "[J" — a leading '[' the matcher must
    // parse via next_argument_descriptor's array branch and treat as DISTINCT
    // from the scalar "I" / "J" overloads (array-vs-scalar disambiguation).
    public static final int RET_INT_ARRAY   = 1031;  // pick(int[])
    public static final int RET_LONG_ARRAY  = 1032;  // pick(long[])

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

    public int pick()                                       { return RET_NOARG; }
    public int pick(final int a, final int b)               { lastArg2A = a; lastArg2B = b; return RET_INT_INT; }
    public int pick(final int a, final int b, final int c)  { return RET_INT_INT_INT; }
    public int pick(final int a, final long b)              { lastArg2A = a; lastArg2B = b; return RET_INT_LONG; }
    public int pick(final long a, final int b)              { lastArg2A = b; lastArg2B = a; return RET_LONG_INT; }
    public int pick(final int a, final String b)            { lastArg2A = a; lastStringArg = b; return RET_INT_STRING; }

    // Array overloads — a leading '[' in the descriptor ("[I" / "[J").  These
    // exist so the resolver's array-token parsing is exercised: scanning the
    // "pick" overloads to resolve a SCALAR int/long must walk PAST these array
    // descriptors (not mis-match them), and an explicit "([I)I" / "([J)I" call
    // must reach exactly these bodies.  echoed via lastArrayLen / lastArrayHead.
    public int pick(final int[] a)  { lastArrayLen = (a == null ? -1 : a.length); lastArrayHead = (a == null || a.length == 0 ? 0L : a[0]);          return RET_INT_ARRAY;  }
    public int pick(final long[] a) { lastArrayLen = (a == null ? -1 : a.length); lastArrayHead = (a == null || a.length == 0 ? 0L : a[0]);          return RET_LONG_ARRAY; }

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

    // ── A method with exactly ONE signature, for the no-overload baseline ──
    // Single signature => no ambiguity => resolves on every path.  The native
    // side calls it both with the matching type and with a non-matching type to
    // observe the "fall back to this->method" behaviour.
    public int onlyInt(final int a) { lastIntArg = a; return 7000 + a; }

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
