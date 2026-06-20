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
     *  11 = load ProbeIface                   (fresh INTERFACE bytecode shape)
     *  12 = load ProbeArrays                  (fresh class carrying array descriptors)
     *  13 = load ProbeInner                   (fresh NON-static member inner class)
     *  14 = define ProbeCustom via a CUSTOM   (fresh, defined by a user ClassLoader
     *       ClassLoader (not the app loader)   subclass calling defineClass(byte[]))
     *  15 = attempt a NON-EXISTENT class       (load FAILS cleanly; no defineClass)
     *  16 = load ProbeAfterFail                (fresh; proves the watcher survives 15)
     */
    public static volatile int which;

    /** Binary name of the last class run() successfully loaded this cycle. */
    public static volatile String lastLoadedName = "";

    /** How many Class.forName loads run() completed this cycle. */
    public static volatile int loadCount;

    /** True iff every load run() attempted this cycle succeeded. */
    public static volatile boolean loadOk;

    /**
     * True iff the custom-classloader cycle (which==14) defined ProbeCustom
     * through a user ClassLoader subclass (NOT the application loader) and got
     * back a usable Class.  Read independently of the watcher callback.
     */
    public static volatile boolean customLoadOk;

    /**
     * True iff the fail-to-load cycle (which==15) caught the expected load failure
     * WITHOUT escaping — proves the armed watcher does not destabilise a failing
     * load.  The defineClass for a non-existent class never happens, so the
     * watcher must stay silent for that name; this flag only proves the Java side
     * failed cleanly (no crash, exception contained).
     */
    public static volatile boolean loadFailedCleanly;

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

    // ---- Varied class SHAPES (distinct bytecode forms the watcher must see) ----
    // An INTERFACE: a non-class bytecode shape still flows through defineClass.
    public interface ProbeIface { int BEACON = 0xC1; }

    // A class whose <clinit> builds arrays, so its constant pool carries array
    // type descriptors ([I, [[J, [Ljava/lang/String;) — a fatter defineClass.
    public static final class ProbeArrays
    {
        public static final int[]      ONE = new int[] { 0xC2 };
        public static final long[][]   TWO = new long[][] { { 0xC2L } };
        public static final String[]   STR = new String[] { "C2" };
    }

    // A NON-static member inner class — distinct nested shape (synthetic this$0
    // outer reference) from the static nested ProbeN classes above.
    public final class ProbeInner { public final int beacon = 0xC3; }

    // The class the custom loader (below) defines from real .class bytes.  It is
    // never referenced by name elsewhere, so it stays pristine until which==14
    // forces the custom loader to define it.
    public static final class ProbeCustom { public static final int BEACON = 0xC4; }

    // A pristine class loaded AFTER the deliberate fail cycle (which==16) to prove
    // the watcher and JVM survived a failed load and still observe a fresh one.
    public static final class ProbeAfterFail { public static final int BEACON = 0xC5; }

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
    public static final String IFACE_NAME  = "vmhook.fixtures.OnClassLoaded$ProbeIface";
    public static final String ARRAYS_NAME = "vmhook.fixtures.OnClassLoaded$ProbeArrays";
    public static final String INNER_NAME  = "vmhook.fixtures.OnClassLoaded$ProbeInner";
    public static final String CUSTOM_NAME = "vmhook.fixtures.OnClassLoaded$ProbeCustom";
    public static final String AFTERFAIL_NAME = "vmhook.fixtures.OnClassLoaded$ProbeAfterFail";
    /** A name that resolves to NO class on any loader; used by the fail cycle. */
    public static final String MISSING_NAME = "vmhook.fixtures.OnClassLoaded$NoSuchProbe";

    /**
     * A user ClassLoader subclass that defines ProbeCustom from the real .class
     * bytes (read out of the application loader as a resource).  It calls the
     * inherited, hooked {@code java.lang.ClassLoader.defineClass(String, byte[],
     * int, int, ProtectionDomain)} directly, so the watcher must observe the load
     * even though the loader is NOT the application loader.  Java-8 safe: plain
     * stream copy, no NIO/var/try-with-resources-on-multiple needed.
     */
    static final class CustomLoader extends ClassLoader
    {
        CustomLoader(final ClassLoader parent)
        {
            super(parent);
        }

        Class<?> define(final String binaryName) throws Exception
        {
            final String resource = binaryName.replace('.', '/') + ".class";
            final java.io.InputStream in = getParent().getResourceAsStream(resource);
            if (in == null)
            {
                throw new ClassNotFoundException("no bytes for " + binaryName);
            }
            try
            {
                final java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream();
                final byte[] chunk = new byte[4096];
                int n;
                while ((n = in.read(chunk)) > 0)
                {
                    buf.write(chunk, 0, n);
                }
                final byte[] bytes = buf.toByteArray();
                // Inherited, hooked ClassLoader.defineClass(String,byte[],int,int,PD).
                return defineClass(binaryName, bytes, 0, bytes.length, null);
            }
            finally
            {
                in.close();
            }
        }
    }

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

    /**
     * Define {@code binaryName} through a fresh CustomLoader (a user ClassLoader
     * subclass), NOT the application loader.  Routes through the inherited hooked
     * {@code ClassLoader.defineClass(String,byte[],int,int,ProtectionDomain)}, so
     * the armed watcher must observe it.  Records the binary name + bumps the
     * count exactly like {@link #force}, so the native witnesses are uniform.
     */
    private static boolean forceCustom(final String binaryName)
    {
        try
        {
            final CustomLoader loader = new CustomLoader(OnClassLoaded.class.getClassLoader());
            final Class<?> defined = loader.define(binaryName);
            customLoadOk = defined != null
                    && defined.getName().equals(binaryName)
                    && defined.getClassLoader() == loader;
            lastLoadedName = binaryName;
            loadCount++;
            return customLoadOk;
        }
        catch (final Throwable t)
        {
            System.err.println("[WARN] OnClassLoaded.forceCustom(" + binaryName + "): " + t);
            return false;
        }
    }

    private static void runScenario()
    {
        loadCount = 0;
        loadOk = false;
        customLoadOk = false;
        loadFailedCleanly = false;
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
            case 11:
                // Fresh INTERFACE — a non-class bytecode shape still defineClass'd.
                loadOk = force(IFACE_NAME);
                break;
            case 12:
                // Fresh class carrying array descriptors in its constant pool.
                loadOk = force(ARRAYS_NAME);
                break;
            case 13:
                // Fresh NON-static member inner class (synthetic this$0 shape).
                loadOk = force(INNER_NAME);
                break;
            case 14:
                // Fresh class defined by a CUSTOM ClassLoader (not the app loader).
                loadOk = forceCustom(CUSTOM_NAME);
                break;
            case 15:
                // A class that FAILS to load: Class.forName must throw and be
                // caught cleanly.  No defineClass occurs, so the armed watcher
                // must stay silent; loadOk stays false, loadFailedCleanly true.
                loadOk = force(MISSING_NAME);
                loadFailedCleanly = !loadOk;
                break;
            case 16:
                // A fresh good load AFTER the failed one — proves the watcher
                // (and the JVM) survived the failure and still observes loads.
                loadOk = force(AFTERFAIL_NAME);
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
