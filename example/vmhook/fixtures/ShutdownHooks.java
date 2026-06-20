package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the shutdown_hooks_teardown feature (area: hooks / lifecycle).
 *
 * Drives vmhook::shutdown_hooks() — the bulk "remove ALL installed hooks and
 * restore the JVM to a clean state" teardown — through a LIVE JVM and proves it
 * is REVERSIBLE (not one-shot / latched).  The native module installs hooks via
 * the low-level vmhook::hook<T>() path (NOT scoped_hook, which auto-removes), so
 * the ONLY thing that takes a hook back down is shutdown_hooks() itself.  This is
 * the regression fixture for the recently-fixed "permanently latched
 * g_shutdown_requested" bug, where after one shutdown_hooks() a fresh hook<T>()
 * returned true but its detour was silently dead (common_detour early-returns on
 * the flag, and the auto-repair watchdog refused to respawn).
 *
 * Every method body is a PURE, deterministic function of its argument and the
 * instance `seed`, so the native side can assert the result is byte-exact the
 * original after teardown — there is no hidden state that a hook could perturb.
 *
 * Canonical go/done handshake: the native module sets `mode`, raises `go`, the
 * Harness loop runs run() on the Java thread (genuine bytecode dispatch, which is
 * what makes an interpreter hook fire), then the module polls the latched `done`.
 * Because `done` latches, every Java call a single assertion needs must happen in
 * one run() invocation; the module selects the scenario via `mode`.
 */
public final class ShutdownHooks
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Set by the native module BEFORE it
     * raises `go` so a single probe cycle drives exactly the calls it asserts on.
     *   1 = instance alpha(int) called ALPHA_CALLS times
     *       (used both while-hooked and after-teardown; same code path, the
     *        native side compares fire counts and the byte-exact original result)
     *   2 = static  beta(int) called BETA_CALLS times
     *   3 = both alpha(int) once AND beta(int) once in the same run()
     *       (multi-method teardown: one shutdown_hooks() must silence BOTH)
     *   4 = instance gamma(int,long,int) once
     *       (a third, multi-slot method so "removes hooks on multiple methods"
     *        spans 3 distinct Method* across instance/static/multi-arg shapes)
     *   5 = alpha(int) once AND beta(int) once AND gamma(int,long,int) once, all
     *       in the SAME run() (single-probe whole-set teardown: one
     *       shutdown_hooks() must silence all THREE shapes observed in one
     *       bytecode dispatch, and every original body must read back byte-exact)
     */
    public static volatile int mode;

    // ---- Instance scenario data -------------------------------------------

    /** Seed for the instance; alpha(delta) returns seed + delta. */
    private int seed = SEED;

    /** Last value the original alpha() body computed (byte-exact-original proof). */
    public static volatile int lastAlphaResult;

    /** Last value the original beta() body computed. */
    public static volatile int lastBetaResult;

    /** Last value the original gamma() body computed. */
    public static volatile long lastGammaResult;

    /** Number of times run() actually invoked the instance alpha(). */
    public static volatile int alphaCallsMade;

    /** Number of times run() actually invoked the static beta(). */
    public static volatile int betaCallsMade;

    /** Number of times run() actually invoked the instance gamma(). */
    public static volatile int gammaCallsMade;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; alpha/gamma fold this into their result. */
    public static final int SEED = 4242;

    /** How many times each repeat-mode drives its method. */
    public static final int ALPHA_CALLS = 3;
    public static final int BETA_CALLS = 2;

    /** The exact delta alpha() is fed (same every call so the result is stable). */
    public static final int ALPHA_DELTA = 17;

    /** The exact delta beta() is fed. */
    public static final int BETA_DELTA = 23;

    /** gamma()'s multi-slot args (long sits between two ints). */
    public static final int GAMMA_A = 9;
    public static final long GAMMA_B = 0x0102030405060708L;
    public static final int GAMMA_C = -31;

    // ---- Hookable methods (pure functions; no side effects in the body) ----

    /** Hookable instance method.  Returns seed + delta. */
    public int alpha(final int delta)
    {
        return this.seed + delta;
    }

    /** Hookable static method.  Returns delta * 3. */
    public static int beta(final int delta)
    {
        return delta * 3;
    }

    /**
     * Hookable multi-slot instance method: a long between two ints, so a hook's
     * decoder must widen the long across two interpreter slots.  Returns
     * seed + a + b + c.
     */
    public long gamma(final int a, final long b, final int c)
    {
        return this.seed + a + b + c;
    }

    private static void runAlpha()
    {
        final ShutdownHooks obj = new ShutdownHooks();
        obj.seed = SEED;
        int made = 0;
        int last = 0;
        for (int n = 0; n < ALPHA_CALLS; ++n)
        {
            last = obj.alpha(ALPHA_DELTA);
            ++made;
        }
        lastAlphaResult = last;
        alphaCallsMade = made;
    }

    private static void runBeta()
    {
        int made = 0;
        int last = 0;
        for (int n = 0; n < BETA_CALLS; ++n)
        {
            last = beta(BETA_DELTA);
            ++made;
        }
        lastBetaResult = last;
        betaCallsMade = made;
    }

    private static void runBoth()
    {
        final ShutdownHooks obj = new ShutdownHooks();
        obj.seed = SEED;
        lastAlphaResult = obj.alpha(ALPHA_DELTA);
        alphaCallsMade = 1;
        lastBetaResult = beta(BETA_DELTA);
        betaCallsMade = 1;
    }

    private static void runGamma()
    {
        final ShutdownHooks obj = new ShutdownHooks();
        obj.seed = SEED;
        lastGammaResult = obj.gamma(GAMMA_A, GAMMA_B, GAMMA_C);
        gammaCallsMade = 1;
    }

    private static void runAll()
    {
        final ShutdownHooks obj = new ShutdownHooks();
        obj.seed = SEED;
        lastAlphaResult = obj.alpha(ALPHA_DELTA);
        alphaCallsMade = 1;
        lastBetaResult = beta(BETA_DELTA);
        betaCallsMade = 1;
        lastGammaResult = obj.gamma(GAMMA_A, GAMMA_B, GAMMA_C);
        gammaCallsMade = 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ShutdownHooks.go && !ShutdownHooks.done;
            }

            @Override
            public void run()
            {
                switch (ShutdownHooks.mode)
                {
                    case 1:
                        runAlpha();
                        break;
                    case 2:
                        runBeta();
                        break;
                    case 3:
                        runBoth();
                        break;
                    case 4:
                        runGamma();
                        break;
                    case 5:
                        runAll();
                        break;
                    default:
                        break;
                }
                ShutdownHooks.done = true;
            }
        });
    }
}
