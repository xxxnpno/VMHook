package vmhook.fixtures;

import vmhook.Harness;

/**
 * Warm-up fixture for the standalone process-global JVM warm-up module
 * (tests/jvm/modules/aaa_warmup.cpp).
 *
 * The modular harness used to lean on the legacy run_test_suite battery to warm
 * the JIT / class-loader / GC and pace probes before the JIT/deopt/class-load-
 * heavy modules ran; on a COLD JVM those modules abort, and on MinGW/gcc (no SEH
 * net) the abort takes the whole JVM down.  This fixture gives the warm-up module
 * a cheap, contained place to force the first deopt / i2i-patch / compile cycle
 * and to trigger a collection, so the heavy modules start against a warm JVM.
 *
 * It mirrors Pilot.java's shape exactly — a go/done handshake and a static-block
 * self-registration — and adds a `mode` selector (like DeoptProbe / DontInline-
 * Probe) so the one native warm-up module can drive two cheap kinds of work:
 *   1 = drive the hookable touch() method in a small bounded loop (lets the
 *       warm-up's scoped hook fire on a real bytecode dispatch and exercise the
 *       interpreter-entry patch / deopt path once, contained);
 *   2 = allocate a little garbage and call System.gc() a couple of times (lets
 *       the warm-up settle the collector before the GC-pressure modules run).
 *
 * Everything here is deliberately tiny and bounded so a single run() completes
 * comfortably inside the native probe's poll window, and so the warm-up can
 * never itself wedge or crash the suite.
 */
public final class Warmup
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which warm-up action run() executes.  Native sets this BEFORE
     * raising `go` so one probe cycle drives exactly the work it wants.
     *   1 = drive touch() in a small bounded loop (JIT / hook warm-up)
     *   2 = allocate garbage + System.gc() x2 (collector settle)
     */
    public static volatile int mode;

    /** Observable side effect so a warm-up touch() dispatch is visible. */
    public static volatile int observed;

    /** Number of touch() calls the last loop made (lets native sanity-check). */
    public static volatile int touchCalls;

    private int seed = 1000;

    /**
     * Iterations for the warm-up touch loop.  Small and bounded: enough to let
     * the scoped hook fire and the interpreter path get exercised, but nowhere
     * near a JIT-pressure loop — the warm-up must stay cheap and quick.
     */
    public static final int WARM_TOUCH_CALLS = 64;

    /** Allocation iterations for the GC-settle action. */
    public static final int GC_GARBAGE_ALLOCS = 32;

    /** Hookable instance method — the native warm-up module hooks this. */
    public int touch(final int delta)
    {
        return this.seed + delta;
    }

    private static void runTouchLoop()
    {
        final Warmup instance = new Warmup();
        int made = 0;
        int last = 0;
        for (int i = 0; i < WARM_TOUCH_CALLS; ++i)
        {
            // Calling touch() through normal bytecode dispatch is what makes the
            // native interpreter hook fire and the i2i patch / deopt path warm.
            last = instance.touch(i & 0xFF);
            ++made;
        }
        observed = last;
        touchCalls = made;
    }

    private static void runGcSettle()
    {
        // Allocate real garbage so a collection happens even when
        // -XX:+DisableExplicitGC would neuter System.gc() on its own.
        for (int n = 0; n < GC_GARBAGE_ALLOCS; ++n)
        {
            final byte[] junk = new byte[64 * 1024];
            junk[0] = (byte) n;
        }
        System.gc();
        System.gc();
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return Warmup.go && !Warmup.done;
            }

            @Override
            public void run()
            {
                switch (Warmup.mode)
                {
                    case 1:
                        runTouchLoop();
                        break;
                    case 2:
                        runGcSettle();
                        break;
                    default:
                        break;
                }
                Warmup.done = true;
            }
        });
    }
}
