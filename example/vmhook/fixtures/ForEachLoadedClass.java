package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the for_each_loaded_class feature (area: class enumeration /
 * SystemDictionary + ClassLoaderDataGraph walk).
 *
 * vmhook::for_each_loaded_class(visitor) takes a SNAPSHOT of every Java class
 * currently reachable through the global ClassLoaderDataGraph and invokes
 * `visitor(internalName, klass*)` once per Klass.  Enumeration is a pure
 * HotSpot-internal read — it needs NO bytecode dispatch — so unlike the
 * method-hook fixtures this one does not have to drive a hooked Java method on
 * the Harness tick thread for its STATIC content.  Its real contribution is:
 *
 *   - It is a top-level, non-'$' class under vmhook/fixtures, so Main.loadFixtures()
 *     Class.forName's it at startup.  That places vmhook/fixtures/ForEachLoadedClass
 *     into the ClassLoaderData graph under the APPLICATION class loader, giving the
 *     native module a known APP-LOADED class to find in the enumeration (proving the
 *     walk reaches more than just bootstrap classes).
 *
 * The native side asserts only PORTABLE invariants (a sane class count, the
 * universal bootstrap classes java/lang/Object + java/lang/String, this OWN
 * fixture class, all klass pointers valid + non-null, and that the walk
 * terminates).  Launcher-entry classes (vmhook/Main) and array / nested klass
 * anchors are treated as BEST-EFFORT: HotSpot's JDK 8 SystemDictionary walk
 * omits some of them, so the module records their presence as [INFO] and never
 * hard-fails on them (HARD only on JDK 9+, where the per-CLD _klasses walk lists
 * every Klass).
 *
 * To give the native module additional, richer (still portable) targets without
 * relying on the launcher class, this fixture force-loads a couple of anchors in
 * its static initializer:
 *   - a NESTED class vmhook/fixtures/ForEachLoadedClass$Inner (instantiated +
 *     statically referenced, since Main.loadFixtures SKIPS '$' files and HotSpot
 *     would otherwise never load it), so the module can confirm a nested app
 *     class also appears in the snapshot;
 *   - the primitive-array klass [I and the object-array klass
 *     [Ljava/lang/String; (anchored so the graph holds them) — array klasses are
 *     a separate Klass family the walk should still surface.
 *
 * ── SNAPSHOT-FRESHNESS support (the go/done/which probe is REAL here) ──────────
 *
 * for_each_loaded_class is documented to be a SNAPSHOT: a class loaded AFTER the
 * call is not visited, but a class loaded BEFORE a later call IS.  To prove that
 * freshness property the native module needs a class that is provably NOT loaded
 * at the time of a first snapshot, then loaded on demand, then seen in a second
 * snapshot.  The nested class {@link LateAnchor} is exactly that target:
 *
 *   - It is a '$'-nested class, so Main.loadFixtures() (which skips '$' files)
 *     never loads it at startup, AND — crucially — this fixture's static
 *     initializer DELIBERATELY DOES NOT reference it, so loading
 *     ForEachLoadedClass itself does not drag LateAnchor in.  It stays UNLOADED
 *     until something explicitly asks for it.
 *   - When the native module raises {@code go}, the registered probe runs on the
 *     Harness tick thread and Class.forName's LateAnchor (recording the outcome
 *     in {@code lateLoaded} / {@code lateLoadName}), then sets {@code done}.
 *
 * That handshake is the ONLY Java-thread work this fixture does; the enumeration
 * itself remains a pure native read.  A native module that never raises {@code go}
 * simply leaves LateAnchor unloaded (the probe's run() is a no-op acknowledge).
 *
 * Java 8 syntax only (anonymous Probe; no var / lambda-in-field / records).
 */
public final class ForEachLoadedClass
{
    /** Native raises this to request the probe action; clears it afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /**
     * A distinctive static sentinel.  The native module is free to read this back
     * through the resolved klass (via the registered wrapper) to confirm the klass
     * the enumeration surfaced for THIS class is genuinely usable — not merely a
     * non-null pointer.
     */
    public static int sentinel = 0x10ADC1A5;

    // ── Snapshot-freshness read-back (written by the probe on the tick thread) ──

    /** True once the probe successfully Class.forName'd LateAnchor. */
    public static volatile boolean lateLoaded;

    /** The internal-ish name the probe loaded (dotted form, for cross-check). */
    public static volatile String lateLoadName = "";

    // ── Forced-load anchors for the nested + array classes ─────────────────────
    //
    // Main.loadFixtures() skips '$' files and HotSpot never loads a class until it
    // is referenced, so these static references force Inner, [I and
    // [Ljava/lang/String; into the ClassLoaderData graph so the native module can
    // find them in the enumeration snapshot.

    /** Keeps the nested klass loaded + reachable in the graph. */
    public static Inner innerAnchor;

    /** Keeps the [I primitive-array klass loaded + reachable. */
    public static int[] primIntArrayAnchor;

    /** Keeps the [Ljava/lang/String; object-array klass loaded + reachable. */
    public static String[] strArrayAnchor;

    /**
     * A nested (inner, non-static) class.  Its internal name is
     * vmhook/fixtures/ForEachLoadedClass$Inner.  Force-loaded below so the native
     * module can confirm a nested application class is enumerated too.
     */
    public final class Inner
    {
        public int innerTag = 0xC0DE;

        public int tag()
        {
            return this.innerTag;
        }
    }

    /**
     * A SECOND nested class that the fixture DELIBERATELY does NOT force-load.
     * Its internal name is vmhook/fixtures/ForEachLoadedClass$LateAnchor.  It
     * stays unloaded until the native module raises {@code go}, at which point the
     * probe Class.forName's it — letting the module prove a class loaded AFTER a
     * first snapshot appears in a SECOND snapshot (snapshot freshness).  Keep it
     * trivially independent (a static class with no outer reference) so loading it
     * pulls in nothing surprising.
     */
    public static final class LateAnchor
    {
        public static int lateTag = 0x1A7EC1A5;

        public int marker()
        {
            return lateTag;
        }
    }

    static
    {
        // Force-load Inner (needs an enclosing instance) and anchor it.
        final ForEachLoadedClass outer = new ForEachLoadedClass();
        innerAnchor = outer.new Inner();

        // Force-load + anchor the array klasses.
        primIntArrayAnchor = new int[] { 1, 2, 3 };
        strArrayAnchor = new String[] { "alpha", "omega" };

        // NOTE: LateAnchor is intentionally NOT referenced here — it must stay
        // unloaded until the freshness probe asks for it.

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ForEachLoadedClass.go && !ForEachLoadedClass.done;
            }

            @Override
            public void run()
            {
                // Freshness handshake: on the rising edge of `go`, load the
                // late nested class so the native module's SECOND snapshot can
                // see a class that its FIRST snapshot could not.  Enumeration
                // itself is a pure native read; this is the only Java-thread work.
                try
                {
                    Class<?> c = Class.forName("vmhook.fixtures.ForEachLoadedClass$LateAnchor");
                    ForEachLoadedClass.lateLoadName = c.getName();
                    ForEachLoadedClass.lateLoaded = true;
                }
                catch (final Throwable t)
                {
                    ForEachLoadedClass.lateLoaded = false;
                    ForEachLoadedClass.lateLoadName = "ERROR:" + t;
                }
                finally
                {
                    ForEachLoadedClass.done = true;
                }
            }
        });
    }
}
