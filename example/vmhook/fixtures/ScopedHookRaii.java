package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the scoped_hook_raii feature (area: hooks / RAII lifetime).
 *
 * Proves on a LIVE JVM (real bytecode dispatch via the Harness go/done probe)
 * the RAII contract of vmhook::scoped_hook / vmhook::hook_handle:
 *   - a hook installed on hook_handle construction FIRES while the handle is
 *     alive (in scope),
 *   - the same hook does NOT fire once the handle is destroyed / scope-exits
 *     (the core RAII proof),
 *   - handle.installed() reports true while armed and false when empty,
 *   - move-construction transfers ownership (the moved-from handle does not
 *     double-remove the hook on destruction),
 *   - move-ASSIGNMENT tears the LHS hook down before stealing the RHS,
 *   - nested scopes arm/disarm independently (inner removal does not kill the
 *     outer hook),
 *   - re-installing on the same method AFTER an explicit removal works,
 *   - several scoped_hooks on DIFFERENT methods are independent,
 *   - explicit early stop() disarms before the C++ scope ends,
 *   - the short scoped_hook(name, cb) overload resolves an overloaded method's
 *     first descriptor while the long scoped_hook(name, sig, cb) overload picks
 *     a specific descriptor, and a non-matching signature yields installed()==false.
 *
 * Shape mirrors the canonical HookBasic fixture: a go/done handshake, a `mode`
 * selector the native module programs BEFORE raising go, a set of independently
 * hookable methods, per-method call counters + last-arg/last-result observables
 * (so the native side can prove WHICH detour fired and that allow-through left
 * the original body intact), and static-block self-registration.
 *
 * `done` latches (the Harness loop never clears it), so each native scenario
 * resets `done`, programs `mode`, then runs ONE probe cycle and reads back the
 * recorded observations.
 */
public final class ScopedHookRaii
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which method(s) run() drives.  The native module sets this BEFORE
     * raising go so one probe cycle drives exactly the calls about to be asserted.
     *   1  = alpha(int)  once          (primary RAII method)
     *   2  = beta(int)   once          (second distinct method)
     *   3  = gamma(int)  once          (third distinct method)
     *   4  = staticAlpha(int) once     (static method variant)
     *   5  = alpha+beta+gamma each once (three different methods one cycle)
     *   6  = over(int) once            (overloaded; short overload target)
     *   7  = over(int,int) once        (overloaded; long-signature target)
     *   8  = alpha(int) + over(int) one cycle (mixed)
     *   9  = alpha(int) + beta(int) one cycle (two-hook exception-unwind path)
     */
    public static volatile int mode;

    // ---- Per-method observables (reset implicitly by re-reading deltas) -----

    /** Seed for instances; alpha/beta/gamma(delta) return seed + delta. */
    private int seed = 1000;

    /** The seed value the native side cross-checks `self` against. */
    public static final int SEED = 1000;

    /** Number of times run() invoked each hookable method this process. */
    public static volatile int alphaCalls;
    public static volatile int betaCalls;
    public static volatile int gammaCalls;
    public static volatile int staticAlphaCalls;
    public static volatile int overICalls;
    public static volatile int overIICalls;

    /** Last original return of each method (allow-through proof). */
    public static volatile int alphaResult;
    public static volatile int betaResult;
    public static volatile int gammaResult;
    public static volatile int staticAlphaResult;
    public static volatile int overIResult;
    public static volatile int overIIResult;

    /** The exact deltas each mode feeds (mirrored on the native side). */
    public static final int ALPHA_DELTA = 7;
    public static final int BETA_DELTA = 11;
    public static final int GAMMA_DELTA = 23;
    public static final int STATIC_ALPHA_DELTA = 99;
    public static final int OVER_I_ARG = 5;
    public static final int OVER_II_ARG_A = 40;
    public static final int OVER_II_ARG_B = 2;

    // ---- Hookable methods --------------------------------------------------

    /** Primary RAII instance method.  Returns seed + delta. */
    public int alpha(final int delta)
    {
        return this.seed + delta;
    }

    /** Second distinct instance method.  Returns seed + delta. */
    public int beta(final int delta)
    {
        return this.seed + delta;
    }

    /** Third distinct instance method.  Returns seed + delta. */
    public int gamma(final int delta)
    {
        return this.seed + delta;
    }

    /** Static method variant.  Returns delta * 2. */
    public static int staticAlpha(final int delta)
    {
        return delta * 2;
    }

    /** Overload family, 1-arg form.  Returns x + 1. */
    public int over(final int x)
    {
        return x + 1;
    }

    /** Overload family, 2-arg form.  Returns a + b. */
    public int over(final int a, final int b)
    {
        return a + b;
    }

    // ---- run() drivers (one per scenario) ----------------------------------

    private static void runAlpha()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        alphaResult = obj.alpha(ALPHA_DELTA);
        alphaCalls += 1;
    }

    private static void runBeta()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        betaResult = obj.beta(BETA_DELTA);
        betaCalls += 1;
    }

    private static void runGamma()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        gammaResult = obj.gamma(GAMMA_DELTA);
        gammaCalls += 1;
    }

    private static void runStaticAlpha()
    {
        staticAlphaResult = staticAlpha(STATIC_ALPHA_DELTA);
        staticAlphaCalls += 1;
    }

    private static void runAllThree()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        alphaResult = obj.alpha(ALPHA_DELTA);
        alphaCalls += 1;
        betaResult = obj.beta(BETA_DELTA);
        betaCalls += 1;
        gammaResult = obj.gamma(GAMMA_DELTA);
        gammaCalls += 1;
    }

    private static void runOverI()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        overIResult = obj.over(OVER_I_ARG);
        overICalls += 1;
    }

    private static void runOverII()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        overIIResult = obj.over(OVER_II_ARG_A, OVER_II_ARG_B);
        overIICalls += 1;
    }

    private static void runAlphaAndOverI()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        alphaResult = obj.alpha(ALPHA_DELTA);
        alphaCalls += 1;
        overIResult = obj.over(OVER_I_ARG);
        overICalls += 1;
    }

    private static void runAlphaAndBeta()
    {
        final ScopedHookRaii obj = new ScopedHookRaii();
        obj.seed = SEED;
        alphaResult = obj.alpha(ALPHA_DELTA);
        alphaCalls += 1;
        betaResult = obj.beta(BETA_DELTA);
        betaCalls += 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ScopedHookRaii.go && !ScopedHookRaii.done;
            }

            @Override
            public void run()
            {
                switch (ScopedHookRaii.mode)
                {
                    case 1:
                        runAlpha();
                        break;
                    case 2:
                        runBeta();
                        break;
                    case 3:
                        runGamma();
                        break;
                    case 4:
                        runStaticAlpha();
                        break;
                    case 5:
                        runAllThree();
                        break;
                    case 6:
                        runOverI();
                        break;
                    case 7:
                        runOverII();
                        break;
                    case 8:
                        runAlphaAndOverI();
                        break;
                    case 9:
                        runAlphaAndBeta();
                        break;
                    default:
                        break;
                }
                ScopedHookRaii.done = true;
            }
        });
    }
}
