package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the on_class_loaded feature (area: hooks / class-load watcher).
 *
 * Drives vmhook::on_class_loaded(...) — the callback that fires when the JVM
 * defines a NEW class through java.lang.ClassLoader.defineClass(String, byte[],
 * int, int, ProtectionDomain) — on a LIVE JVM and proves, with real bytecode
 * dispatch, that:
 *   - installing the callback then forcing a fresh class load makes the callback
 *     fire exactly once with the loaded class's JVM-internal ('/'-separated) name,
 *   - multiple distinct loads in one cycle each report their own name,
 *   - an ALREADY-loaded class re-requested via Class.forName does NOT re-report
 *     (Class.forName short-circuits on findLoadedClass, so no defineClass occurs),
 *   - the callback is REMOVABLE (after the watch_handle drops, a fresh load is
 *     not observed), and re-registering arms a working callback again.
 *
 * The fresh-load targets are NESTED classes (OnClassLoaded$ProbeN).  Main's
 * fixture auto-discovery only Class.forName's TOP-LEVEL .class files (it skips
 * names containing '$'), so these nested classes are NOT loaded at startup —
 * each stays a pristine, never-defined class until a probe cycle forces it via
 * Class.forName("vmhook.fixtures.OnClassLoaded$ProbeN").  That guarantees the
 * watcher observes a genuinely brand-new klass, exactly like the legacy
 * vmhook.LateClass probe it replaces.
 *
 * Canonical go/done handshake: the native module sets `which` (the load
 * selector), raises `go`, the Harness loop runs run() on the Java thread
 * (genuine bytecode dispatch, which is what makes the interpreter hook fire),
 * then the module polls the latched `done`.  Because `done` latches, every
 * Java load a single assertion needs happens inside one run() invocation; the
 * module selects the scenario via `which`.  After each cycle the native side
 * reads back `lastLoadedName` / `loadCount` / `loadOk` to confirm the Java
 * action happened, INDEPENDENTLY of whether the (possibly removed) callback
 * fired — so "callback did not fire" can be distinguished from "load never ran".
 */
public final class OnClassLoaded
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which load run() performs.  The native module sets this BEFORE it
     * raises `go` so a single probe cycle forces exactly the class loads it is
     * about to assert on.  Each value (except 3) targets a DISTINCT nested class
     * that has never been defined before, so the watcher sees a brand-new klass.
     *   1 = load Probe1                       (single fresh load)
     *   2 = load Probe2 then Probe3           (two distinct fresh loads, one cycle)
     *   3 = load Probe1 AGAIN                  (already-loaded; NO new defineClass)
     *   4 = load Probe4                       (fresh; used for removable / re-arm)
     *   5 = load Probe5                       (fresh)
     *   6 = load Probe6                       (fresh)
     *   7 = load Probe7                       (fresh)
     *   8 = load Probe8                       (fresh)
     *   9 = load Probe9                       (fresh; fire-capable settle canary)
     *  10 = load Probe10                      (fresh; post-shutdown settle canary)
     */
    public static volatile int which;

    /** Binary name of the last class run() successfully loaded this cycle. */
    public static volatile String lastLoadedName = "";

    /** How many Class.forName loads run() completed this cycle. */
    public static volatile int loadCount;

    /** True iff every load run() attempted this cycle succeeded. */
    public static volatile boolean loadOk;

    // ---- Fresh-load targets ------------------------------------------------
    // Distinct nested classes, never referenced elsewhere, so HotSpot does not
    // load them until the probe forces it.  Each holds a unique beacon so the
    // class is non-trivial and unmistakably itself.
    public static final class Probe1 { public static final int BEACON = 0xB1; }
    public static final class Probe2 { public static final int BEACON = 0xB2; }
    public static final class Probe3 { public static final int BEACON = 0xB3; }
    public static final class Probe4 { public static final int BEACON = 0xB4; }
    public static final class Probe5 { public static final int BEACON = 0xB5; }
    public static final class Probe6 { public static final int BEACON = 0xB6; }
    public static final class Probe7 { public static final int BEACON = 0xB7; }
    public static final class Probe8 { public static final int BEACON = 0xB8; }
    public static final class Probe9 { public static final int BEACON = 0xB9; }
    public static final class Probe10 { public static final int BEACON = 0xBA; }

    /** The simple ('$'-joined) nested name the native side expects, per `which`. */
    public static final String PROBE1_NAME = "vmhook.fixtures.OnClassLoaded$Probe1";
    public static final String PROBE2_NAME = "vmhook.fixtures.OnClassLoaded$Probe2";
    public static final String PROBE3_NAME = "vmhook.fixtures.OnClassLoaded$Probe3";
    public static final String PROBE4_NAME = "vmhook.fixtures.OnClassLoaded$Probe4";
    public static final String PROBE5_NAME = "vmhook.fixtures.OnClassLoaded$Probe5";
    public static final String PROBE6_NAME = "vmhook.fixtures.OnClassLoaded$Probe6";
    public static final String PROBE7_NAME = "vmhook.fixtures.OnClassLoaded$Probe7";
    public static final String PROBE8_NAME = "vmhook.fixtures.OnClassLoaded$Probe8";
    public static final String PROBE9_NAME = "vmhook.fixtures.OnClassLoaded$Probe9";
    public static final String PROBE10_NAME = "vmhook.fixtures.OnClassLoaded$Probe10";

    /**
     * Force one class to load through the application class loader.  Class.forName
     * with initialize=true and this fixture's own loader routes through
     * loadClass -> findClass -> URLClassLoader.defineClass, i.e. the hooked
     * java.lang.ClassLoader.defineClass(String,byte[],int,int,ProtectionDomain).
     * If the class is already loaded, findLoadedClass short-circuits and NO
     * defineClass call (and therefore no watcher event) occurs.
     */
    private static boolean force(final String binaryName)
    {
        try
        {
            Class.forName(binaryName, true, OnClassLoaded.class.getClassLoader());
            lastLoadedName = binaryName;
            loadCount++;
            return true;
        }
        catch (final Throwable t)
        {
            // The native side observes loadOk == false / a stale lastLoadedName
            // and fails the corresponding check; never let the loop wedge.
            System.err.println("[WARN] OnClassLoaded.force(" + binaryName + "): " + t);
            return false;
        }
    }

    private static void runScenario()
    {
        loadCount = 0;
        loadOk = false;
        switch (which)
        {
            case 1:
                loadOk = force(PROBE1_NAME);
                break;
            case 2:
                // Two DISTINCT fresh loads inside a single cycle.
                loadOk = force(PROBE2_NAME) & force(PROBE3_NAME);
                break;
            case 3:
                // Probe1 was already defined by an earlier cycle: Class.forName
                // returns the cached Class WITHOUT a fresh defineClass, so the
                // armed watcher must NOT observe an event for it.
                loadOk = force(PROBE1_NAME);
                break;
            case 4:
                loadOk = force(PROBE4_NAME);
                break;
            case 5:
                loadOk = force(PROBE5_NAME);
                break;
            case 6:
                loadOk = force(PROBE6_NAME);
                break;
            case 7:
                loadOk = force(PROBE7_NAME);
                break;
            case 8:
                loadOk = force(PROBE8_NAME);
                break;
            case 9:
                loadOk = force(PROBE9_NAME);
                break;
            case 10:
                loadOk = force(PROBE10_NAME);
                break;
            default:
                break;
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return OnClassLoaded.go && !OnClassLoaded.done;
            }

            @Override
            public void run()
            {
                runScenario();
                OnClassLoaded.done = true;
            }
        });
    }
}
