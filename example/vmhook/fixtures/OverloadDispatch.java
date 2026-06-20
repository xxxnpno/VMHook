package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_overload_java_dispatch feature (area: methods).
 *
 * This is the JAVA-SIDE READBACK companion to MethodOverload (the selection-logic
 * fixture).  MethodOverload proves WHICH overload the resolver picks (each overload
 * returns a distinct sentinel, captured inside the detour).  THIS fixture proves
 * the picked overload's REAL EFFECT — its computed return value AND a per-overload
 * Java-recorded side effect — flows back correctly through method_proxy::call(),
 * mirroring the legacy example.cpp test_overloaded_methods (overloadProbe*):
 *
 *      f(int 30)      -> 130       (x + 100)
 *      f(String foo)  -> "[foo]"   ("[" + s + "]")
 *      f(2, 3)        -> 5         (a + b)
 *
 * The native module dispatches each overload TWO independent ways and asserts the
 * SAME result + side effect each way:
 *   (1) the C++-typed call():            get_method("f")->call(<typed arg>)
 *       — resolution follows the C++ argument TYPE (int->I, std::string->
 *         Ljava/lang/String;, (int,int)->(II)), proving descriptor-aware
 *         overload resolution reaches the right Java body for primitive,
 *         String, and multi-arg forms;
 *   (2) the explicit-signature call():   get_method("f","(I)I")->call(...)
 *       — resolution is pinned to the exact descriptor.
 * Both must compute the legacy value AND fire the matching side-effect recorder
 * (lastIntResult / lastStrArg / lastDualSum + per-overload hit counters) so the
 * native side can confirm, from Java's own state, that the intended body ran and
 * no sibling overload did.
 *
 * It ALSO carries a primitive-only family `h` (h(int) and h(long), NO reference
 * overload) so the native side can exercise the documented no-match fallback
 * (call with a C++ type matching NO overload -> resolve_compatible_method walks
 * the hierarchy, finds no descriptor match, and returns the FIRST-by-name overload
 * — NOT monostate; vmhook.hpp resolve_compatible_method final `return this->method`)
 * WITHOUT ever blasting a primitive into a reference slot (every `h` overload has a
 * primitive parameter, so whichever one HotSpot orders first is a safe primitive
 * dispatch — no reference-slot access violation).
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks);
 * the Harness.Probe is registered as an ANONYMOUS inner class in the static
 * initializer, exactly like the other fixtures.
 */
public final class OverloadDispatch
{
    /** Native sets this true to request the action; the probe clears it via done. */
    public static volatile boolean go;

    /** The probe action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ── Legacy-mirrored arguments + expected results (single source of truth) ──
    // Kept in lockstep with the native module's constants.
    public static final int    F_INT_ARG      = 30;          // f(int)    -> 130
    public static final int    F_INT_EXPECT   = 130;         // 30 + 100
    public static final String F_STR_ARG      = "foo";       // f(String) -> "[foo]"
    public static final String F_STR_EXPECT   = "[foo]";     // "[" + "foo" + "]"
    public static final int    F_DUAL_A       = 2;           // f(int,int) -> 5
    public static final int    F_DUAL_B       = 3;
    public static final int    F_DUAL_EXPECT  = 5;           // 2 + 3

    // The primitive-only no-match family arguments + results.
    public static final int    H_INT_ARG      = 4;           // h(int)  -> 44
    public static final int    H_INT_EXPECT   = 44;          // x + 40
    public static final long   H_LONG_ARG     = 7L;          // h(long) -> 7007
    public static final long   H_LONG_EXPECT  = 7007L;       // x + 7000

    // ── Distinct single-slot primitive descriptors for the `g` family ──────────
    // Each overload has a UNIQUE JVM descriptor (S / B / C / Z / F / I) so a
    // C++-typed call() must pick exactly one via argument_matches_descriptor's
    // EXACT-WIDTH mapping (int16_t->S, int8_t->B, char16_t->C, bool->Z, float->F,
    // int32_t->I) with NO Java-widening between them.  All are category-1 (single
    // interpreter slot) so they dispatch correctly on BOTH the call_stub and
    // call_jni paths on every JDK.  Each body bumps its own counter and echoes its
    // (widened-to-long) argument so the native side can confirm WHICH ran.
    public static final short  G_SHORT_ARG    = 12;          // g(short)   -> 1012
    public static final int    G_SHORT_EXPECT = 1012;        // x + 1000
    public static final byte   G_BYTE_ARG     = 5;           // g(byte)    -> 2005
    public static final int    G_BYTE_EXPECT  = 2005;        // x + 2000
    public static final char   G_CHAR_ARG     = 'A';         // g(char)    -> 'A'(65)+3000 = 3065
    public static final int    G_CHAR_EXPECT  = 3065;        // x + 3000
    public static final boolean G_BOOL_ARG    = true;        // g(boolean) -> 1
    public static final int    G_BOOL_EXPECT  = 1;           // true -> 1
    public static final float  G_FLOAT_ARG    = 2.5f;        // g(float)   -> 2.5 + 4000.25 = 4002.75
    public static final float  G_FLOAT_EXPECT = 4002.75f;    // x + 4000.25
    public static final int    G_INT_ARG      = 9;           // g(int)     -> 5009
    public static final int    G_INT_EXPECT   = 5009;        // x + 5000

    // ── Position-dependent multi-arg family `p` ────────────────────────────────
    // p(int,String) and p(String,int) share a name and arity but differ only by
    // argument ORDER, so the descriptor the resolver must build depends on the
    // C++ argument-pack order: (ILjava/lang/String;) vs (Ljava/lang/String;I).
    public static final int    P_IS_INT       = 7;           // p(int,String)
    public static final String P_IS_STR       = "x";
    public static final String P_IS_EXPECT    = "IS:7:x";
    public static final int    P_SI_INT       = 8;           // p(String,int)
    public static final String P_SI_STR       = "y";
    public static final String P_SI_EXPECT    = "SI:y:8";

    // ── Widen-only family `w` (no narrow overload) ─────────────────────────────
    // w(long) and w(double) ONLY — there is NO w(int).  Calling w() with a C++
    // int (descriptor I) matches NEITHER, so resolve_compatible_method falls back
    // to the first-by-name overload: vmhook does NOT perform Java widening
    // (int->long / int->double).  Both fallbacks are primitive (no reference
    // slot), so the no-match dispatch is safe.

    // ── Per-overload Java-recorded side effects (proof of WHICH body ran) ──────
    // Each overload records its own argument(s)/result + bumps its hit counter, so
    // the native side can confirm — purely from Java's observable state — that the
    // intended overload executed and the siblings did not.
    public static volatile int     lastIntArg;       // last f(int) argument
    public static volatile int     lastIntResult;    // last f(int) result (x + 100)
    public static volatile String  lastStrArg;       // last f(String) argument
    public static volatile String  lastStrResult;    // last f(String) result ("[s]")
    public static volatile int     lastDualA;        // last f(int,int) first arg
    public static volatile int     lastDualB;        // last f(int,int) second arg
    public static volatile int     lastDualSum;      // last f(int,int) result (a + b)

    public static volatile int     fIntHits;         // f(int)        invocation count
    public static volatile int     fStrHits;         // f(String)     invocation count
    public static volatile int     fDualHits;        // f(int,int)    invocation count

    // no-match family echoes
    public static volatile long    lastHArg;         // last h(*) argument (widened)
    public static volatile long    lastHResult;      // last h(*) result (widened)
    public static volatile int     hIntHits;         // h(int)  invocation count
    public static volatile int     hLongHits;        // h(long) invocation count

    // `g` family echoes (each overload widens its arg/result into a long slot) +
    // per-overload hit counters, so the native side proves WHICH descriptor the
    // C++-typed call() resolved without depending on any field-typed readback.
    public static volatile long    lastGArg;         // last g(*) argument (widened)
    public static volatile long    lastGResult;      // last g(*) result (widened, *1000 scaled)
    public static volatile int     gShortHits;       // g(short)   invocation count
    public static volatile int     gByteHits;        // g(byte)    invocation count
    public static volatile int     gCharHits;        // g(char)    invocation count
    public static volatile int     gBoolHits;        // g(boolean) invocation count
    public static volatile int     gFloatHits;       // g(float)   invocation count
    public static volatile int     gIntHits;         // g(int)     invocation count

    // `p` family echoes (position-dependent multi-arg).
    public static volatile String  lastPResult;      // last p(*) formatted result
    public static volatile int     pIsHits;          // p(int,String) invocation count
    public static volatile int     pSiHits;          // p(String,int) invocation count

    // single-String-overload family `sf` echoes (const char* / string_view / std::string
    // all map to Ljava/lang/String; and must reach this ONE overload).
    public static volatile String  lastSfArg;        // last sf(String) argument
    public static volatile int     sfHits;           // sf(String) invocation count

    // `w` widen-only family echoes (no narrow overload).
    public static volatile long    lastWArg;         // last w(*) argument (widened)
    public static volatile int     wLongHits;        // w(long)   invocation count
    public static volatile int     wDoubleHits;      // w(double) invocation count

    /** tick() invocation count — handshake proof the detour fired. */
    public static volatile int     tickCount;

    /**
     * The last nonce tick() actually received from Java.  The native detour also
     * observes this same argument off the interpreter frame; the module
     * cross-checks native-observed == Java-recorded so "the detour saw the right
     * argument" is proven from BOTH sides, not just the native read.
     */
    public static volatile int     lastTickNonce;

    // ── Hook site ─────────────────────────────────────────────────────────────
    /**
     * The native module hooks this; inside the detour current_java_thread is live,
     * which is the only context in which method_proxy::call() may invoke the call
     * gate.  The detour performs every f(...) / h(...) call on `self`.
     */
    public int tick(final int nonce)
    {
        tickCount++;
        lastTickNonce = nonce;
        return nonce + 1;
    }

    // ── The overloaded family `f` (legacy semantics) ──────────────────────────
    // Declaration order is intentionally scrambled (String first) so the resolver
    // must not depend on source order.

    /** f(Ljava/lang/String;)Ljava/lang/String; -> "[" + s + "]" */
    public String f(final String s)
    {
        final String r = "[" + s + "]";
        lastStrArg = s;
        lastStrResult = r;
        fStrHits++;
        return r;
    }

    /** f(I)I -> x + 100 */
    public int f(final int x)
    {
        final int r = x + 100;
        lastIntArg = x;
        lastIntResult = r;
        fIntHits++;
        return r;
    }

    /** f(II)I -> a + b */
    public int f(final int a, final int b)
    {
        final int r = a + b;
        lastDualA = a;
        lastDualB = b;
        lastDualSum = r;
        fDualHits++;
        return r;
    }

    // ── The primitive-only no-match family `h` ────────────────────────────────
    // BOTH overloads take a primitive — no reference parameter exists — so the
    // documented no-match fallback (which dispatches the first-by-name overload)
    // can be exercised with a non-matching C++ primitive type (e.g. double) with
    // zero risk of a primitive-into-reference-slot access violation.

    /** h(I)I -> x + 40 */
    public int h(final int x)
    {
        lastHArg = x;
        lastHResult = x + 40;
        hIntHits++;
        return x + 40;
    }

    /** h(J)J -> x + 7000 */
    public long h(final long x)
    {
        lastHArg = x;
        lastHResult = x + 7000L;
        hLongHits++;
        return x + 7000L;
    }

    // ── The distinct single-slot `g` family (one overload per descriptor) ──────
    // Declaration order is again scrambled relative to descriptor order; the
    // resolver must pick by the C++ argument TYPE, not source position.

    /** g(S)I -> x + 1000 */
    public int g(final short x)
    {
        final int r = x + 1000;
        lastGArg = x;
        lastGResult = r;
        gShortHits++;
        return r;
    }

    /** g(Z)I -> (b ? 1 : 0) */
    public int g(final boolean b)
    {
        final int r = b ? 1 : 0;
        lastGArg = b ? 1L : 0L;
        lastGResult = r;
        gBoolHits++;
        return r;
    }

    /** g(B)I -> x + 2000 */
    public int g(final byte x)
    {
        final int r = x + 2000;
        lastGArg = x;
        lastGResult = r;
        gByteHits++;
        return r;
    }

    /** g(F)F -> x + 4000.25 */
    public float g(final float x)
    {
        final float r = x + 4000.25f;
        lastGArg = (long) x;
        lastGResult = (long) r;
        gFloatHits++;
        return r;
    }

    /** g(C)I -> x + 3000 */
    public int g(final char x)
    {
        final int r = x + 3000;
        lastGArg = x;
        lastGResult = r;
        gCharHits++;
        return r;
    }

    /** g(I)I -> x + 5000 */
    public int g(final int x)
    {
        final int r = x + 5000;
        lastGArg = x;
        lastGResult = r;
        gIntHits++;
        return r;
    }

    // ── Position-dependent multi-arg family `p` ────────────────────────────────

    /** p(ILjava/lang/String;)Ljava/lang/String; -> "IS:" + n + ":" + s */
    public String p(final int n, final String s)
    {
        final String r = "IS:" + n + ":" + s;
        lastPResult = r;
        pIsHits++;
        return r;
    }

    /** p(Ljava/lang/String;I)Ljava/lang/String; -> "SI:" + s + ":" + n */
    public String p(final String s, final int n)
    {
        final String r = "SI:" + s + ":" + n;
        lastPResult = r;
        pSiHits++;
        return r;
    }

    // ── Widen-only family `w` (no narrow overload; proves NO Java widening) ─────

    /** w(J)J -> x + 60000 */
    public long w(final long x)
    {
        lastWArg = x;
        wLongHits++;
        return x + 60000L;
    }

    /** w(D)D -> x + 70000.0 */
    public double w(final double x)
    {
        lastWArg = (long) x;
        wDoubleHits++;
        return x + 70000.0;
    }

    // ── Single-String-overload family `sf` (one overload, three C++ spellings) ─

    /** sf(Ljava/lang/String;)Ljava/lang/String; -> "{" + s + "}" */
    public String sf(final String s)
    {
        final String r = "{" + s + "}";
        lastSfArg = s;
        sfHits++;
        return r;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return OverloadDispatch.go && !OverloadDispatch.done;
            }

            @Override
            public void run()
            {
                // Drive tick() on the shared SINGLETON so the native detour's
                // `self` is exactly this instance.  Every f(...)/h(...) call the
                // module performs happens inside that detour where a JavaThread
                // is live.
                SINGLETON.tick(11);
                OverloadDispatch.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps.  Created eagerly so the
     * detour's `self` OOP is deterministic and matches what the probe drove.
     */
    public static final OverloadDispatch SINGLETON = new OverloadDispatch();
}
