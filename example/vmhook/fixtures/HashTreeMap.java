package vmhook.fixtures;

import vmhook.Harness;

import java.util.HashMap;
import java.util.TreeMap;

/**
 * Fixture for the collection_hash_tree_map feature (area: collections).
 *
 * This is the live-JVM successor to the legacy
 * {@code Example.test_hash_map_probe} / {@code Example.test_tree_map_probe}
 * pair.  It exercises vmhook's two Map walkers through the EXPLICIT typed
 * wrappers the native module declares its intent with:
 *
 *   - {@code java.util.HashMap<String,String>} read via
 *     {@code vmhook::hash_map} (the "table" Node[] bucket walk).  Keys are
 *     "h0","h1","h2" with known values; HashMap iteration order is BUCKET
 *     order, so the native side asserts the three (key,value) pairs are all
 *     PRESENT and CORRECT, order-independent.
 *
 *   - {@code java.util.TreeMap<String,String>} read via {@code vmhook::map}
 *     (the "root" red-black in-order walk).  Keys are INSERTED OUT OF ORDER
 *     ("t2" then "t0" then "t1") so that a correct in-order traversal proving
 *     itself is non-trivial: the native side asserts the entries come back in
 *     NATURAL SORTED order t0 &lt; t1 &lt; t2 with the correct values.
 *
 * The VALUE type is plain {@code java.lang.String} (not a nested Box): both the
 * key and value OOPs decode directly through {@code vmhook::read_java_string},
 * so the native module can assert exact value CONTENT, not just identity.
 *
 * Canonical go/done handshake with a `mode` selector (mirrors CollMap/CollList):
 * the native module sets `mode`, clears `done`, raises `go`; the Harness loop
 * runs the matching scenario on the Java thread; the module polls `done`.  The
 * maps are plain object fields, so the native side reads them directly without a
 * hooked dispatch — there are no hooks in this feature, only reads.
 *
 * Java 8 syntax ONLY (anonymous Harness.Probe; no var/records/switch-expr/
 * text-blocks/lambdas), so the fixture compiles under {@code --release 8}.
 */
public final class HashTreeMap
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  The native module sets this BEFORE raising `go`.
     *   0 = (re)build both map fields on the Java thread (default; also runs
     *       once at class-init).
     */
    public static volatile int mode;

    // ---- Known key/value contents (mirrored on the native side) ------------
    //
    // HashMap keys h0/h1/h2 -> known values; the native side asserts each
    // (key,value) pair is present (order-independent).
    public static final String H0_KEY = "h0";
    public static final String H1_KEY = "h1";
    public static final String H2_KEY = "h2";
    public static final String H0_VAL = "hash-zero";
    public static final String H1_VAL = "hash-one";
    public static final String H2_VAL = "hash-two";

    // TreeMap keys t0/t1/t2 -> known values, but INSERTED OUT OF ORDER so the
    // native in-order walk must re-sort them to t0 < t1 < t2.
    public static final String T0_KEY = "t0";
    public static final String T1_KEY = "t1";
    public static final String T2_KEY = "t2";
    public static final String T0_VAL = "tree-zero";
    public static final String T1_VAL = "tree-one";
    public static final String T2_VAL = "tree-two";

    // ---- The map fields the native module reads via the typed wrappers -----

    /** HashMap with distinct keys h0/h1/h2 -> known values (bucket-ordered). */
    public static HashMap<String, String> hashMap = new HashMap<String, String>();

    /** TreeMap with keys inserted out of order; in-order walk == sorted. */
    public static TreeMap<String, String> treeMap = new TreeMap<String, String>();

    // ---- Published witnesses so native can cross-check Java's own view -----

    /** Java's own view of each map's size(), for a native cross-check. */
    public static volatile int hashMapSize;
    public static volatile int treeMapSize;

    /** TreeMap's first/last keys (sorted), so native can pin the in-order walk. */
    public static volatile String treeFirstKey;
    public static volatile String treeLastKey;

    // ---- Builder -----------------------------------------------------------

    private static void buildAll()
    {
        hashMap = new HashMap<String, String>();
        hashMap.put(H0_KEY, H0_VAL);
        hashMap.put(H1_KEY, H1_VAL);
        hashMap.put(H2_KEY, H2_VAL);

        // Insert the TreeMap keys OUT OF NATURAL ORDER (t2, t0, t1).  A correct
        // red-black in-order walk re-sorts them to t0 < t1 < t2 regardless.
        treeMap = new TreeMap<String, String>();
        treeMap.put(T2_KEY, T2_VAL);
        treeMap.put(T0_KEY, T0_VAL);
        treeMap.put(T1_KEY, T1_VAL);

        hashMapSize = hashMap.size();
        treeMapSize = treeMap.size();
        treeFirstKey = treeMap.firstKey();
        treeLastKey = treeMap.lastKey();
    }

    static
    {
        // Build once at class-init so the maps are populated even before the
        // first probe (the native module also re-requests a build via mode 0).
        buildAll();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HashTreeMap.go && !HashTreeMap.done;
            }

            @Override
            public void run()
            {
                // mode 0 (the only mode): (re)build both maps so the native
                // reads see a fresh, deterministic population on this thread.
                buildAll();
                HashTreeMap.done = true;
            }
        });
    }
}
