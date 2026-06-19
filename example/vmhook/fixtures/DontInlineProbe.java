package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the dont_inline_dont_compile_flags feature
 * (area: hooks / JIT inhibitors).
 *
 * Drives vmhook's interpreter-entry JIT inhibitors on a LIVE JVM.  When
 * vmhook::hook<T>() installs a detour it patches the method's i2i (interpreter)
 * stub and — so the JIT can never inline or compile past that patch — sets two
 * HotSpot-internal flags on the Method:
 *   - _dont_inline       in Method::_flags          (bit 2)
 *   - NO_COMPILE         in Method::_access_flags    (NOT_C1/C2/C2_OSR + QUEUED)
 * The native module reads BOTH flags back through the live Method* and asserts
 * they are set after install / cleared after teardown, then drives the method
 * through a hot loop (well past the C1/C2 compile threshold) and confirms the
 * interpreter hook STILL fires — i.e. the JIT did not inline/compile past the
 * patched stub.  Finally it characterises whether the flags survive a GC.
 *
 * The hooked method `hot(int)` is a pure function of `seed` + `delta`, so the
 * native side can also assert byte-exact allow-through (the original body still
 * runs after a non-cancelling detour).  It is deliberately tiny and
 * side-effect-light so HotSpot is eager to inline + compile it once it goes
 * hot — making the _dont_inline / NO_COMPILE inhibitors the ONLY thing that can
 * keep every dispatch routed through the interpreter hook.
 *
 * Canonical go/done handshake.  `done` LATCHES, so every Java call a single
 * assertion needs must happen inside ONE run() invocation; the native module
 * selects the scenario via `mode` BEFORE raising `go`, then reads back the
 * recorded counters.  The hot-loop / GC counts are large but bounded so a
 * single run() completes well within the native probe's poll window.
 */
public final class DontInlineProbe
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Native sets this BEFORE raising
     * `go` so one probe cycle drives exactly the calls about to be asserted on.
     *   1 = call hot() ONCE       (smoke: detour fires once, allow-through)
     *   2 = call hot() HOT_CALLS times in a tight loop (drive JIT compilation;
     *       the _dont_inline / NO_COMPILE inhibitors must keep the hook firing)
     *   3 = GC churn (allocate garbage + System.gc()) then call hot() ONCE
     *       (characterise whether the flags survive a collection / safepoint)
     *   4 = call hot() ONCE       (post-teardown / post-repair re-check)
     *   5 = call hot(callDelta) ONCE using the native-supplied `callDelta`
     *       (edge-value arg decode: 0 / -1 / MIN / MAX through the i2i patch)
     */
    public static volatile int mode;

    /**
     * The delta the native side wants mode 5 to feed hot().  Set BEFORE `go` so
     * one probe cycle drives exactly the boundary value about to be asserted on.
     * Modes 1/3/4 ignore this and use the fixed HOT_DELTA.
     */
    public static volatile int callDelta;

    /** Seed for the hot() instance method; hot(delta) returns seed + delta. */
    private int seed = SEED;

    /** Last value the original hot() body computed (allow-through proof). */
    public static volatile int lastHotResult;

    /** XOR accumulator of every hot() return inside one probe (defeats DCE). */
    public static volatile long hotResultXor;

    /** Number of times run() actually invoked hot() in the last probe cycle. */
    public static volatile int hotCallsMade;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; hot(delta) returns seed + delta. */
    public static final int SEED = 1000;

    /** The delta fed to hot() on the single-call smoke / re-check scenarios. */
    public static final int HOT_DELTA = 7;

    /**
     * Iterations for the JIT-warming hot loop (mode 2).  Comfortably above the
     * default C1 CompileThreshold (~1500-10000 depending on JDK / tiered) so
     * HotSpot *wants* to compile + inline hot(); vmhook's _dont_inline /
     * NO_COMPILE is what keeps every dispatch routed through the interpreter
     * hook while the hook is healthy.
     */
    public static final int HOT_CALLS = 200000;

    /** Allocation iterations for the GC-churn scenario (mode 3). */
    public static final int GC_GARBAGE_ALLOCS = 64;

    // ---- Hookable method --------------------------------------------------

    /**
     * Hookable instance method.  Deliberately tiny and side-effect-light so
     * HotSpot is eager to inline + compile it once it goes hot.  Returns
     * seed + delta.
     */
    public int hot(final int delta)
    {
        return this.seed + delta;
    }

    private static void runHotOnce(final int delta)
    {
        final DontInlineProbe obj = new DontInlineProbe();
        obj.seed = SEED;
        final int r = obj.hot(delta);
        lastHotResult = r;
        hotResultXor = r;
        hotCallsMade = 1;
    }

    private static void runHotLoop(final int iterations)
    {
        final DontInlineProbe obj = new DontInlineProbe();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            // Vary the delta a little so the JIT can't fold the whole loop to a
            // constant, but keep it cheap.  The low 8 bits keep the result
            // bounded and the XOR meaningful.
            final int d = i & 0xFF;
            last = obj.hot(d);
            acc ^= last;
            ++made;
        }
        lastHotResult = last;
        hotResultXor = acc;
        hotCallsMade = made;
    }

    private static void runHotEdge(final int delta)
    {
        // Mode 5: identical shape to runHotOnce but with a caller-chosen delta,
        // so the native side can drive boundary int values (0, -1, MIN, MAX)
        // through the i2i interpreter patch and assert the detour decodes each
        // exactly and the allow-through body computes seed + delta (with the
        // usual two's-complement wraparound for MIN/MAX).
        final DontInlineProbe obj = new DontInlineProbe();
        obj.seed = SEED;
        final int r = obj.hot(delta);
        lastHotResult = r;
        hotResultXor = r;
        hotCallsMade = 1;
    }

    private static void runGcThenHot(final int delta)
    {
        // Force GC churn so the native side can check whether the Method flags
        // survive a collection / safepoint cleanup.  Allocate real garbage so a
        // collection happens even when -XX:+DisableExplicitGC neuters
        // System.gc().
        for (int n = 0; n < GC_GARBAGE_ALLOCS; ++n)
        {
            final byte[] junk = new byte[64 * 1024];
            junk[0] = (byte) n;
        }
        System.gc();
        System.gc();

        final DontInlineProbe obj = new DontInlineProbe();
        obj.seed = SEED;
        final int r = obj.hot(delta);
        lastHotResult = r;
        hotResultXor = r;
        hotCallsMade = 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return DontInlineProbe.go && !DontInlineProbe.done;
            }

            @Override
            public void run()
            {
                switch (DontInlineProbe.mode)
                {
                    case 1:
                        runHotOnce(HOT_DELTA);
                        break;
                    case 2:
                        runHotLoop(HOT_CALLS);
                        break;
                    case 3:
                        runGcThenHot(HOT_DELTA);
                        break;
                    case 4:
                        runHotOnce(HOT_DELTA);
                        break;
                    case 5:
                        runHotEdge(DontInlineProbe.callDelta);
                        break;
                    default:
                        break;
                }
                DontInlineProbe.done = true;
            }
        });
    }
}
