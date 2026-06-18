package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_chaining feature (area: hooks / shared i2i stub
 * demultiplexing).
 *
 * THIS IS NOT cross-DLL "hook chaining" (that is hook<T>'s chain_resume path
 * for a second injector stomping the shared injection point).  Here "chaining"
 * means the in-process consequence of HotSpot sharing ONE interpreter-to-
 * interpreter (i2i) stub across many Java methods: vmhook patches that stub
 * exactly once (one midi2i_hook / one trampoline) and registers each hooked
 * Method* in g_hooked_methods.  Every intercepted call lands in the same
 * common_detour, which LINEAR-SCANS g_hooked_methods and on the FIRST
 * hook.method == current_method match fires that method's detour and returns.
 * The structural guarantee under test: with hooks installed on SEVERAL distinct
 * methods at once, each detour fires for ITS method ONLY and never cross-fires
 * onto a sibling, all through the single shared stub.
 *
 * The decisive difference from the scoped_hook_raii fixture (which drives one
 * method per probe cycle) is that this fixture's primary scenario calls EVERY
 * hookable method inside a SINGLE run() invocation -- one continuous bytecode
 * dispatch pass.  That forces all the sibling hooks to coexist live on the one
 * shared i2i stub simultaneously and proves common_detour demultiplexes a mixed
 * call stream (int / long / String / double / no-arg / static) to the correct
 * per-method detour with exact per-method counts and zero cross-fire.
 *
 * Methods deliberately span every argument-decode path so the shared detour is
 * exercised across the J/D two-slot boundary, a reference (String) decode, the
 * no-argument frame, and the static (no `this`) frame:
 *   a(int)      -> seed + x          (instance, one-slot primitive)
 *   b(long)     -> seed + y          (instance, two-slot primitive)
 *   c(String)   -> seed + s.length() (instance, reference arg)
 *   d(double)   -> seed + (long)z    (instance, two-slot primitive)
 *   e()         -> seed              (instance, NO args)
 *   s(int)      -> x * 2             (STATIC, no `this`, arg at slot 0)
 *
 * Canonical go/done handshake with a `mode` selector the native module sets
 * BEFORE raising go.  `done` latches (the Harness loop never clears it), so each
 * native scenario resets `done`, programs `mode`, then runs ONE probe cycle and
 * reads back the recorded observables.  Per-method call counters accumulate
 * across the whole process, so the native side asserts post-probe DELTAS and
 * needs no Java-side reset.
 *
 * JAVA 8 SOURCE: no var/records/switch-expr/text-blocks/lambdas-as-fields/
 * List.of; an anonymous Probe and ordinary statements only, so this compiles
 * identically under javac 8 and javac 25+.
 */
public final class HookChaining
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which method(s) run() drives.  The native module sets this BEFORE
     * raising go so one probe cycle drives exactly the calls about to be asserted.
     *   1 = ALL six methods, each once, in ONE dispatch pass (a,b,c,d,e,s)
     *   2 = a(int) once only            (count-isolation / single-method baseline)
     *   3 = b(long) once only
     *   4 = c(String) once only
     *   5 = a + b + c, each once        (subset; used by the drop-one scenario)
     *   6 = a called A_REPEAT times, b once, c once (per-method count fidelity)
     *   7 = a once + s(int) once        (instance + static share the stub)
     *   8 = e() once + d(double) once   (no-arg frame + two-slot double)
     *   9 = a once + s(int) S_REPEAT times (static-method count fidelity; the
     *       instance sibling fires once while the static sibling fires many)
     */
    public static volatile int mode;

    // ---- Instance seed (self-identity proof) -------------------------------

    /** Seed for instances; the native detour cross-checks `self` against this. */
    private int seed = 1000;

    /** The seed value the native side cross-checks `self` against. */
    public static final int SEED = 1000;

    // ---- Per-method call counters (accumulate across the whole process) -----

    public static volatile int aCalls;
    public static volatile int bCalls;
    public static volatile int cCalls;
    public static volatile int dCalls;
    public static volatile int eCalls;
    public static volatile int sCalls;

    // ---- Last original return of each method (allow-through proof) ----------

    public static volatile int  aResult;
    public static volatile long bResult;
    public static volatile int  cResult;
    public static volatile long dResult;
    public static volatile int  eResult;
    public static volatile int  sResult;

    // ---- Exact argument values each mode feeds (mirrored on native side) ----

    public static final int    A_ARG     = 7;
    public static final long   B_ARG     = 0x1122334455667788L;
    public static final String C_ARG     = "vmhook";   // length 6
    public static final int    C_ARG_LEN = 6;
    public static final double D_ARG     = 2.5;
    public static final int    S_ARG     = 21;
    /** How many times mode 6 calls a(int) (per-method count fidelity check). */
    public static final int    A_REPEAT  = 4;
    /** How many times mode 9 calls the STATIC s(int) (static count fidelity). */
    public static final int    S_REPEAT  = 3;

    // ---- Hookable methods (distinct argument-decode shapes) ----------------

    /** Instance, one-slot int arg.  Returns seed + x. */
    public int a(final int x)
    {
        return this.seed + x;
    }

    /** Instance, two-slot long arg.  Returns seed + y. */
    public long b(final long y)
    {
        return this.seed + y;
    }

    /** Instance, reference (String) arg.  Returns seed + s.length(). */
    public int c(final String s)
    {
        final int slen = (s == null) ? -1 : s.length();
        return this.seed + slen;
    }

    /** Instance, two-slot double arg.  Returns seed + (long) z. */
    public long d(final double z)
    {
        return this.seed + (long) z;
    }

    /** Instance, NO arguments.  Returns seed. */
    public int e()
    {
        return this.seed;
    }

    /** STATIC method (no `this`): the int arg is at slot 0.  Returns x * 2. */
    public static int s(final int x)
    {
        return x * 2;
    }

    // ---- run() drivers (one per scenario) ----------------------------------

    /** All six methods, each once, inside a SINGLE bytecode dispatch pass. */
    private static void runAll()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        aResult = obj.a(A_ARG);          aCalls += 1;
        bResult = obj.b(B_ARG);          bCalls += 1;
        cResult = obj.c(C_ARG);          cCalls += 1;
        dResult = obj.d(D_ARG);          dCalls += 1;
        eResult = obj.e();               eCalls += 1;
        sResult = s(S_ARG);              sCalls += 1;
    }

    private static void runA()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        aResult = obj.a(A_ARG);
        aCalls += 1;
    }

    private static void runB()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        bResult = obj.b(B_ARG);
        bCalls += 1;
    }

    private static void runC()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        cResult = obj.c(C_ARG);
        cCalls += 1;
    }

    /** a + b + c, each once (subset used by the drop-one scenario). */
    private static void runAbc()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        aResult = obj.a(A_ARG);          aCalls += 1;
        bResult = obj.b(B_ARG);          bCalls += 1;
        cResult = obj.c(C_ARG);          cCalls += 1;
    }

    /** a called A_REPEAT times, b once, c once (per-method count fidelity). */
    private static void runCounts()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        for (int n = 0; n < A_REPEAT; ++n)
        {
            aResult = obj.a(A_ARG);
            aCalls += 1;
        }
        bResult = obj.b(B_ARG);          bCalls += 1;
        cResult = obj.c(C_ARG);          cCalls += 1;
    }

    /** a (instance) once + s (static) once: instance and static share the stub. */
    private static void runAAndStatic()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        aResult = obj.a(A_ARG);          aCalls += 1;
        sResult = s(S_ARG);              sCalls += 1;
    }

    /** e() (no-arg frame) once + d(double) (two-slot) once. */
    private static void runEAndD()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        eResult = obj.e();               eCalls += 1;
        dResult = obj.d(D_ARG);          dCalls += 1;
    }

    /** a (instance) once + s (static) S_REPEAT times: static count fidelity. */
    private static void runAAndStaticN()
    {
        final HookChaining obj = new HookChaining();
        obj.seed = SEED;
        aResult = obj.a(A_ARG);          aCalls += 1;
        for (int n = 0; n < S_REPEAT; ++n)
        {
            sResult = s(S_ARG);
            sCalls += 1;
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookChaining.go && !HookChaining.done;
            }

            @Override
            public void run()
            {
                switch (HookChaining.mode)
                {
                    case 1:
                        runAll();
                        break;
                    case 2:
                        runA();
                        break;
                    case 3:
                        runB();
                        break;
                    case 4:
                        runC();
                        break;
                    case 5:
                        runAbc();
                        break;
                    case 6:
                        runCounts();
                        break;
                    case 7:
                        runAAndStatic();
                        break;
                    case 8:
                        runEAndD();
                        break;
                    case 9:
                        runAAndStaticN();
                        break;
                    default:
                        break;
                }
                HookChaining.done = true;
            }
        });
    }
}
