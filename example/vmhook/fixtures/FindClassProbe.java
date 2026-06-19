package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the find_class / JNI-fallback-chain feature (area: class lookup).
 *
 * The native module find_class_fallback.cpp exhaustively exercises
 * vmhook::find_class(internal_name) and its resolution fallback chain on a LIVE
 * JVM:
 *
 *     ClassLoaderDataGraph / SystemDictionary walk  (HotSpot-internal, zero JNI)
 *         -> vmhook::detail::jni_find_class_with_context_loader  (JNI fallback:
 *            thread context loader -> system loader -> Forge LaunchWrapper)
 *
 * What this fixture proves, by being the native side's resolution targets:
 *   - find_class resolves BOOTSTRAP classes (java/lang/Object, String, Integer,
 *     the [I primitive-array klass, java/util/ArrayList) — the C++ module asks
 *     for those directly, they need no fixture state.
 *   - find_class resolves THIS class — vmhook/fixtures/FindClassProbe — which is
 *     loaded by the APPLICATION class loader (Main.loadFixtures Class.forName's
 *     it), proving app-classloader resolution (not just bootstrap).  The native
 *     side confirms the returned klass is USABLE by reading the SENTINEL static
 *     field below through the registered wrapper and by reading the klass's own
 *     internal-name symbol.
 *   - find_class resolves the NESTED/INNER class
 *     vmhook/fixtures/FindClassProbe$Inner.  HotSpot lazy-loads nested classes
 *     and Main.loadFixtures SKIPS files containing '$', so this fixture FORCE
 *     LOADS Inner in its static initializer (a real `new Inner()` + a static
 *     reference) to guarantee the inner klass is reachable in the graph when the
 *     native module looks it up.
 *   - find_class on the OBJECT-ARRAY name [Ljava/lang/String; and the
 *     PRIMITIVE-ARRAY name [I (both forced loaded below so the graph holds them).
 *
 * Why a hook + handshake at all?  find_class itself is a pure HotSpot-internal
 * read and the native module calls it straight from its worker thread.  But the
 * JNI FALLBACK path (jni_find_class_with_context_loader) resolves through the
 * CALLING THREAD's context class loader, and that only meaningfully exercises
 * the app loader when run on a real Java thread.  So this fixture exposes a
 * hookable trigger() and a go/done handshake; the native module installs a
 * scoped_hook on trigger() and, from inside that detour (on the Java thread,
 * whose context loader is the application loader that owns this class), calls
 * vmhook::jni::find_class_with_context_loader(...) directly so the fallback
 * chain is driven and its null/non-null contract checked WITHOUT crashing.
 *
 * Java 8 syntax only (no var/records/switch-expr/text-blocks/lambda-in-field).
 */
public final class FindClassProbe
{
    // ── go / done handshake driven by the native module via run_probe ──────────
    /** Native raises this to request the probe action; clears it afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /** Set true by the trigger() body so the hook has a real dispatch to ride. */
    public static volatile boolean triggerRan;

    /**
     * A known-value static field.  The native module reads this back through the
     * resolved klass (via the registered wrapper's static_field accessor) to
     * prove the klass find_class returned is genuinely USABLE — not merely a
     * non-null pointer.  The value is a distinctive sentinel.
     */
    public static int sentinel = 0x5A11C0DE;

    /** A second sentinel returned by getSentinel() for a getter-based readback. */
    public static int getSentinel()
    {
        return sentinel;
    }

    // ── Forced-load anchors for the nested + array classes ─────────────────────
    //
    // Main.loadFixtures() skips '$' files, and HotSpot never loads a class until
    // it is referenced.  These static references (initialized in the static
    // block) force Inner, [I and [Ljava/lang/String; into the ClassLoaderData
    // graph so the native module's find_class for those names can hit the graph
    // walk rather than depending solely on the JNI fallback.

    /** Keeps the inner klass loaded + reachable. */
    public static Inner innerAnchor;

    /** Keeps the [I primitive-array klass loaded + reachable. */
    public static int[] primIntArrayAnchor;

    /** Keeps the [Ljava/lang/String; object-array klass loaded + reachable. */
    public static String[] strArrayAnchor;

    /** Keeps the [[I two-dimensional primitive-array klass loaded + reachable. */
    public static int[][] prim2dArrayAnchor;

    /** Keeps the [Ljava/lang/Object; object-array klass loaded + reachable. */
    public static Object[] objArrayAnchor;

    /**
     * A nested (inner, non-static) class.  Its internal name is
     * vmhook/fixtures/FindClassProbe$Inner.  Carries its own sentinel so the
     * native side can confirm the resolved inner klass is usable too.
     */
    public final class Inner
    {
        public int innerTag = 0xBEEF;

        public int tag()
        {
            return this.innerTag;
        }
    }

    static
    {
        // Force-load Inner: instantiate it (needs an enclosing instance) and keep
        // a static reference so the klass stays reachable for the graph walk.
        final FindClassProbe outer = new FindClassProbe();
        innerAnchor = outer.new Inner();

        // Force-load the array klasses and anchor them.
        primIntArrayAnchor = new int[] { 1, 2, 3 };
        strArrayAnchor = new String[] { "alpha", "omega" };
        prim2dArrayAnchor = new int[][] { { 1 }, { 2, 3 } };
        objArrayAnchor = new Object[] { "obj", Integer.valueOf(7) };

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FindClassProbe.go && !FindClassProbe.done;
            }

            @Override
            public void run()
            {
                // Real bytecode dispatch the native scoped_hook rides; the native
                // detour runs jni_find_class_with_context_loader on THIS (Java)
                // thread, whose context loader is the application loader.
                new FindClassProbe().trigger();
                FindClassProbe.done = true;
            }
        });
    }

    /**
     * Hookable instance method.  The native module hooks this; its detour drives
     * the JNI context-loader fallback while a JavaThread is guaranteed live and
     * the thread's context loader is the application loader that owns this class.
     */
    public void trigger()
    {
        triggerRan = true;
    }
}
