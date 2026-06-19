package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code find_class_with_context_loader} / classloader-reanchor
 * feature family (area: class lookup).
 *
 * <p>This is the companion of {@code FindClassProbe} / find_class_fallback but
 * targets a DELIBERATELY DISJOINT slice of the public API.  Where
 * find_class_fallback focuses on the HotSpot ClassLoaderDataGraph walk in
 * {@code vmhook::find_class}, the native module
 * {@code tests/jvm/modules/find_class_context_loader.cpp} drives the
 * <em>context-classloader</em> resolution surface that exists for app-loaded
 * classes the bare graph walk can miss:</p>
 *
 * <ul>
 *   <li>{@code vmhook::jni::find_class_with_context_loader(name)} — the public
 *       JNI multi-loader resolver (thread context loader -&gt; system loader
 *       -&gt; Minecraft Launch loader), run from a real Java thread so the
 *       calling thread's context loader IS the application loader that owns this
 *       fixture;</li>
 *   <li>{@code vmhook::find_class_via_oop(anchor, name)} — resolve {@code name}
 *       through the classloader of a LIVE anchor object (this fixture instance),
 *       the "reanchor against an object I already hold" primitive;</li>
 *   <li>{@code vmhook::reanchor_classes_via_oop(anchor, {...})} +
 *       {@code override_class_lookup} / {@code evict_class_lookup} — pin a set of
 *       names to the anchor loader's copy so every later {@code find_class}
 *       follows that copy, then forget it again;</li>
 *   <li>{@code vmhook::detail::klass_to_class_loader_oop(klass)} — the
 *       BOOTSTRAP-vs-context-loader comparison axis: a bootstrap class
 *       (java/lang/String) reports a null loader oop, this app-loaded fixture
 *       reports a NON-null one;</li>
 *   <li>{@code vmhook::detail::capture_host_classloader_klass} /
 *       {@code host_classloader_klass} — once find_class sees this app class, the
 *       host (non-bootstrap) klass is captured.</li>
 * </ul>
 *
 * <p>Every resolution is additionally proven USABLE natively
 * ({@code klass-&gt;find_field(...)} resolves the sentinel field's entry and
 * {@code klass-&gt;get_java_mirror()} is a valid pointer), and the JVM itself
 * cross-checks the sentinel through real bytecode in {@link #captureWitness()}.</p>
 *
 * <p>The context-loader resolvers all need a JavaThread whose context loader is
 * the application loader, so the native side hooks {@link #anchorTick()} and does
 * its reanchor work from inside that detour (where {@code self} — a live instance
 * of THIS class — is also the anchor object whose loader to borrow).  A second
 * trivial method {@link #secondaryTick()} gives the module a distinct dispatch
 * site if it wants one; both just flip witnesses.</p>
 *
 * <p>Pure-ASCII, Java-8 syntax only (anonymous {@code Harness.Probe}, no var /
 * records / switch-expressions / text blocks / lambdas-in-fields), so it compiles
 * identically under javac 8 and javac 25 with any {@code -encoding}.</p>
 */
public final class FindClassCtxLoader
{
    // ── go / done handshake driven by the native module via run_probe ──────────
    /** Native raises this to request the probe action; the action clears done first. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped by anchorTick() so the hook has a real bytecode dispatch to ride. */
    public static volatile int anchorTicks;

    /** Bumped by secondaryTick(); a distinct dispatch site if the module wants one. */
    public static volatile int secondaryTicks;

    /**
     * Scenario selector the native side sets on the rising edge of {@code go}
     * (mode 0 = ordinary reanchor pass; mode 1 = GC-churn pass that drives a few
     * {@link System#gc()} rounds BEFORE dispatching {@link #anchorTick()} again, so
     * the native detour re-exercises {@code find_class_via_oop(self, ...)} on a live
     * anchor after a (possibly relocating) collection).  The GC pass is only ever
     * driven on non-Windows toolchains by the native module (the forced-GC platform
     * gate), so mode stays 0 on Windows.
     */
    public static volatile int mode;

    /** How many {@link System#gc()} rounds the GC-churn pass has driven. */
    public static volatile int gcRounds;

    /**
     * A distinctive known-value static field on this APP-loaded class.  The native
     * side resolves this field's entry through the context-loader-resolved klass
     * (proving the klass is genuinely usable for member access, not merely
     * non-null) and the JVM echoes it back through captureWitness().
     */
    public static int sentinel = 0x0CAFEC0D;

    /** A second known static the native side reads through the getter path. */
    public static int getSentinel()
    {
        return sentinel;
    }

    /**
     * An instance field the native side resolves on a context-loader-resolved
     * klass via the super-chain-walking top-level find_field, distinct value from
     * the static sentinel so a static/instance mix-up is caught.
     */
    public int instanceMark = 0x0BADBEEF;

    // ── Java-visible witnesses the native side reads back after the probe ───────
    /** What the JVM sees in {@link #sentinel} (must equal the native-read value). */
    public static int observedSentinel;

    /** Set true by captureWitness() so the native side knows real bytecode ran. */
    public static boolean witnessCaptured;

    /**
     * Force-loaded anchor so the {@code [I} primitive-array klass is reachable via
     * this fixture's loader chain too (used by the reanchor angle which resolves a
     * mix of names through the anchor's loader).
     */
    public static int[] intArrayAnchor;

    static
    {
        // Anchor the [I array klass so reanchor lookups have it in the graph.
        intArrayAnchor = new int[] { 7, 8, 9 };

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FindClassCtxLoader.go && !FindClassCtxLoader.done;
            }

            @Override
            public void run()
            {
                // Mode 1: drive a handful of collections (with young-gen churn) so a
                // relocating collector has every chance to MOVE live objects before
                // the native detour re-resolves classes through the live anchor.  The
                // anchorTick() dispatch below then runs post-GC on a live JavaThread,
                // so find_class_via_oop(self, ...) is exercised across a collection.
                if (FindClassCtxLoader.mode == 1)
                {
                    for (int round = 0; round < 4; round++)
                    {
                        final byte[] churn = new byte[1 << 16];
                        if (churn.length != (1 << 16))
                        {
                            throw new IllegalStateException("unreachable");
                        }
                        System.gc();
                        FindClassCtxLoader.gcRounds++;
                    }
                }

                // Real bytecode dispatch the native scoped_hook rides.  Inside the
                // anchorTick() detour the native side has a live JavaThread (whose
                // context loader is this app loader) AND a live `self` instance to
                // use as the reanchor anchor object.
                final FindClassCtxLoader self = new FindClassCtxLoader();
                self.anchorTick();
                self.secondaryTick();

                // Pure-Java witness: echo the sentinel the native side also read,
                // so the module can cross-check the JVM agrees.
                captureWitness();

                FindClassCtxLoader.done = true;
            }
        });
    }

    /**
     * Primary hookable instance method.  The native module installs a scoped_hook
     * here; its detour resolves classes via the context loader and via
     * {@code find_class_via_oop(self, ...)} while a JavaThread is guaranteed live.
     */
    public void anchorTick()
    {
        anchorTicks++;
    }

    /** Secondary trivial dispatch site (a distinct method the module may hook). */
    public void secondaryTick()
    {
        secondaryTicks++;
    }

    /**
     * Snapshots — with genuine getstatic bytecode — what the JVM observes in the
     * sentinel field, so the native side can assert the JVM agrees with its own
     * context-loader-resolved read.
     */
    private static void captureWitness()
    {
        observedSentinel = sentinel;
        witnessCaptured = true;
    }
}
