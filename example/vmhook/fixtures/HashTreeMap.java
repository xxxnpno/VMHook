package vmhook.fixtures;

import vmhook.Harness;

import java.util.HashMap;
import java.util.TreeMap;

/**
 * EXHAUSTIVE fixture for the collection_hash_tree_map feature (area: collections).
 *
 * This module is the dedicated authority for vmhook's two HotSpot Map
 * INTERNAL-STRUCTURE walkers, decoded straight from a raw OOP with NO Java
 * call-gate dispatch per entry:
 *
 *   - {@code java.util.HashMap}  -> {@code hash_map_walk_entries}: the "table"
 *     Node[] BUCKET walk, following each bucket's {@code key/value/next} chain.
 *     A bucket head is either a {@code HashMap$Node} (a linked bin) or, once a
 *     bucket exceeds the treeify threshold (8 colliding keys), a
 *     {@code HashMap$TreeNode} (a red-black TREE bin).  Because TreeNode keeps
 *     the {@code Node.next} threading, the SAME next-chain walk visits both bin
 *     shapes — this fixture builds BOTH so the walker is proven on each.
 *
 *   - {@code java.util.TreeMap}  -> {@code tree_map_walk_entries}: the "root"
 *     red-black IN-ORDER walk reading {@code key/value/left/right} per node.
 *
 * COVERAGE this fixture builds (the companion native module asserts each):
 *
 *   HashMap sizes 0 / 1 / 8 / 9 / 16 / 17 / 64 / 1000 — straddling the default
 *     capacity (16, threshold 12) so the bucket walk is proven across the
 *     12 -> 32 resize boundary and at mid scale; every entry present, no miss /
 *     no duplicate (HARD).
 *   HashMap collision chain (7 colliding keys, below treeify=8) — a single
 *     bucket holding a plain {@code Node.next} chain; the walk follows next.
 *   HashMap TREE bin (>= 12 colliding keys) — a single bucket that TREEIFIES to
 *     a red-black {@code TreeNode} bin; the next-chain walk must still surface
 *     every entry (the linked-bin AND tree-bin cases both decode).
 *   HashMap resize boundary (exactly 13 entries) — one past the threshold 12,
 *     so the table has resized 16 -> 32; every key survives the rehash.
 *   Key/value type matrix: String->String, Integer->Integer, String->Long
 *     (values > 2^32 so a truncating read is caught), enum->String, a null KEY,
 *     and a null VALUE (both legal in HashMap; surface as nullptr entries).
 *
 *   TreeMap sizes 0 / 1 / 8 / 64 / 1000 — the iterative red-black in-order walk
 *     across a trivial root, small, and deep tree; strict ascending key order.
 *   TreeMap inserted OUT OF ORDER and DESCENDING — the in-order walk re-sorts
 *     (it does not echo insertion order).
 *   TreeMap with a null VALUE (TreeMap allows null values, never null keys) and
 *     an Integer-keyed tree (natural numeric order).
 *
 * Java's own witnesses (size(), firstKey()/lastKey(), order-independent
 * checksums computed the identical way, and best-effort reflective tree-bin
 * confirmation) are published so the native side can cross-check every shape.
 *
 * Canonical go/done handshake with a `mode` selector (mode 0 = (re)build all),
 * mirroring CollSetExhaustive / CollMap.  Reads-only feature: no hooks.
 *
 * Java 8 syntax ONLY (anonymous Harness.Probe; no var/records/switch-expr/
 * text-blocks/lambdas), so the fixture compiles under {@code --release 8} and
 * loads on every JDK in the matrix (8/11/17/21/24/25/26).
 */
public final class HashTreeMap
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  The native module sets this BEFORE raising `go`.
     *   0 = (re)build every map field on the Java thread (default; also runs
     *       once at class-init).
     */
    public static volatile int mode;

    // ---- Scenario sizes (mirrored on the native side) ----------------------

    public static final int N0 = 0;
    public static final int N1 = 1;
    public static final int N8 = 8;
    public static final int N9 = 9;
    public static final int N16 = 16;
    public static final int N17 = 17;
    public static final int N64 = 64;
    public static final int N1000 = 1000;

    /** Colliding-chain key count (below treeify threshold 8 => plain Node chain). */
    public static final int COLL7 = 7;
    /** Tree-bin key count (>= 8 colliding => the bucket treeifies to TreeNode). */
    public static final int TREE_BIN = 12;
    /**
     * Resize-boundary entry count.  Default capacity 16, load factor 0.75 =>
     * threshold 12; the 13th add resizes the table to capacity 32.  So 13
     * entries sit exactly one past the boundary, the table HAS rehashed, and the
     * bucket walk must surface every key across the rehash.
     */
    public static final int RESIZE13 = 13;

    // =======================================================================
    //  LEGACY fields — kept so the routing-characterization checks still pin
    //  the exact String key/value CONTENT through both wrappers.  hashMap holds
    //  {h0->hash-zero, h1->hash-one, h2->hash-two}; treeMap is inserted OUT OF
    //  natural order (t2,t0,t1) so the in-order walk re-sorts to t0<t1<t2.
    // =======================================================================
    public static final String H0_KEY = "h0";
    public static final String H1_KEY = "h1";
    public static final String H2_KEY = "h2";
    public static final String H0_VAL = "hash-zero";
    public static final String H1_VAL = "hash-one";
    public static final String H2_VAL = "hash-two";

    public static final String T0_KEY = "t0";
    public static final String T1_KEY = "t1";
    public static final String T2_KEY = "t2";
    public static final String T0_VAL = "tree-zero";
    public static final String T1_VAL = "tree-one";
    public static final String T2_VAL = "tree-two";

    public static HashMap<String, String> hashMap = new HashMap<String, String>();
    public static TreeMap<String, String> treeMap = new TreeMap<String, String>();

    public static volatile int hashMapSize;
    public static volatile int treeMapSize;
    public static volatile String treeFirstKey;
    public static volatile String treeLastKey;

    // =======================================================================
    //  HashMap shapes — every value is a plain java.lang.String so both key and
    //  value OOPs decode through read_java_string.  Keys "k"+i, values "v"+i.
    // =======================================================================

    public static HashMap<String, String> hashEmpty   = new HashMap<String, String>();
    public static HashMap<String, String> hashOne      = new HashMap<String, String>();
    public static HashMap<String, String> hashEight    = new HashMap<String, String>();
    public static HashMap<String, String> hashNine     = new HashMap<String, String>();
    public static HashMap<String, String> hashSixteen  = new HashMap<String, String>();
    public static HashMap<String, String> hashSeventeen= new HashMap<String, String>();
    public static HashMap<String, String> hashSixtyFour= new HashMap<String, String>();
    public static HashMap<String, String> hashThousand = new HashMap<String, String>();

    /** RESIZE13 entries: one past the 12->32 resize boundary. */
    public static HashMap<String, String> hashResize13 = new HashMap<String, String>();

    /** COLL7 colliding-hashCode keys in ONE bucket; plain Node.next chain. */
    public static HashMap<String, String> hashColl7    = new HashMap<String, String>();

    /** TREE_BIN colliding-hashCode keys in ONE bucket; treeifies to a TreeNode bin. */
    public static HashMap<String, String> hashTreeBin  = new HashMap<String, String>();

    /** Integer->Integer (boxed-primitive key AND value). */
    public static HashMap<Integer, Integer> hashIntKey = new HashMap<Integer, Integer>();

    /** String->Long, values > 2^32 (a truncating read would corrupt the sum). */
    public static HashMap<String, Long> hashLongVal    = new HashMap<String, Long>();

    /** Enum key -> String value (an ORDINARY HashMap, not EnumMap; decodes positively). */
    public static HashMap<Day, String> hashEnumKey     = new HashMap<Day, String>();

    /** One null KEY (legal in HashMap) plus two real keys. */
    public static HashMap<String, String> hashNullKey  = new HashMap<String, String>();

    /** One null VALUE (legal in HashMap) plus one real value. */
    public static HashMap<String, String> hashNullVal  = new HashMap<String, String>();

    /** A declared HashMap field deliberately left NULL (walk must stay empty). */
    public static HashMap<String, String> hashNull     = null;

    // =======================================================================
    //  TreeMap shapes — String key/value unless noted.
    // =======================================================================

    public static TreeMap<String, String> treeEmpty     = new TreeMap<String, String>();
    public static TreeMap<String, String> treeOne        = new TreeMap<String, String>();
    public static TreeMap<String, String> treeEight       = new TreeMap<String, String>();
    public static TreeMap<String, String> treeSixtyFour   = new TreeMap<String, String>();
    public static TreeMap<String, String> treeThousand    = new TreeMap<String, String>();

    /** Inserted DESCENDING; the in-order walk must come out ascending. */
    public static TreeMap<String, String> treeDescending  = new TreeMap<String, String>();

    /** TreeMap with one null VALUE (TreeMap permits null values, never null keys). */
    public static TreeMap<String, String> treeNullVal     = new TreeMap<String, String>();

    /** Integer keys -> Integer values; natural numeric ascending order. */
    public static TreeMap<Integer, Integer> treeIntKey    = new TreeMap<Integer, Integer>();

    /** A declared TreeMap field deliberately left NULL (walk must stay empty). */
    public static TreeMap<String, String> treeNull        = null;

    // =======================================================================
    //  Published cross-check witnesses (size oracle + order-independent sums).
    // =======================================================================

    public static volatile int hashEmptySize;
    public static volatile int hashOneSize;
    public static volatile int hashEightSize;
    public static volatile int hashNineSize;
    public static volatile int hashSixteenSize;
    public static volatile int hashSeventeenSize;
    public static volatile int hashSixtyFourSize;
    public static volatile int hashThousandSize;
    public static volatile int hashResize13Size;
    public static volatile int hashColl7Size;
    public static volatile int hashTreeBinSize;
    public static volatile int hashIntKeySize;
    public static volatile int hashLongValSize;
    public static volatile int hashEnumKeySize;
    public static volatile int hashNullKeySize;
    public static volatile int hashNullValSize;

    public static volatile int treeEmptySize;
    public static volatile int treeOneSize;
    public static volatile int treeEightSize;
    public static volatile int treeSixtyFourSize;
    public static volatile int treeThousandSize;
    public static volatile int treeDescendingSize;
    public static volatile int treeNullValSize;
    public static volatile int treeIntKeySize;

    /** Order-independent key code-unit sums for the larger HashMaps. */
    public static volatile long hashThousandKeyCharSum;
    public static volatile long hashSixtyFourKeyCharSum;

    /** Integer-key map: key sum / value sum (boxed-primitive decode cross-check). */
    public static volatile long hashIntKeyKeySum;
    public static volatile long hashIntKeyValSum;

    /** String->Long map: value sum (64-bit; truncating read would break it). */
    public static volatile long hashLongValValSum;

    /** Enum-key map: ordinal sum + value char sum. */
    public static volatile long hashEnumKeyOrdinalSum;
    public static volatile long hashEnumKeyValCharSum;

    /** TreeMap bounds (sorted) so native can pin the in-order endpoints. */
    public static volatile String treeOneFirst;          // == treeOneLast (single node)
    public static volatile String treeFirstKeyEight;
    public static volatile String treeLastKeyEight;
    public static volatile String treeSixtyFourFirst;
    public static volatile String treeSixtyFourLast;
    public static volatile String treeThousandFirstKey;
    public static volatile String treeThousandLastKey;

    /** Integer TreeMap bounds (natural numeric order). */
    public static volatile int treeIntKeyFirst;
    public static volatile int treeIntKeyLast;

    /** Best-effort reflective confirmation a bucket treeified (true on success). */
    public static volatile boolean hashTreeBinIsTree;
    /** Best-effort reflective confirmation the 7-key chain stayed a plain Node bin. */
    public static volatile boolean hashColl7IsTree;

    /** A small enum used for the enum-key HashMap. */
    public enum Day
    {
        MON, TUE, WED, THU, FRI
    }

    // ---- Helpers -----------------------------------------------------------

    private static long codeUnitSum(final String s)
    {
        long sum = 0;
        for (int i = 0; i < s.length(); ++i)
        {
            sum += s.charAt(i);
        }
        return sum;
    }

    /**
     * Generates n strings that all share the SAME String.hashCode by chaining
     * the classic "Aa"/"BB" collision (both hash to 2112).  Every generated key
     * has the same length, so the equal-hashCode property is preserved and all n
     * keys land in ONE bucket — the bin treeifies once it exceeds 8 entries.
     */
    private static String[] collidingKeys(final int n)
    {
        final String[] blocks = { "Aa", "BB" };
        int blockCount = 1;
        while ((1 << blockCount) < n)
        {
            ++blockCount;
        }
        final String[] out = new String[n];
        for (int i = 0; i < n; ++i)
        {
            final StringBuilder sb = new StringBuilder(blockCount * 2);
            for (int b = 0; b < blockCount; ++b)
            {
                sb.append(blocks[(i >> b) & 1]);
            }
            out[i] = sb.toString();
        }
        return out;
    }

    /**
     * Reflectively reports whether any bucket head of a HashMap is a TreeNode
     * (i.e. the bin treeified).  Returns false if reflection is blocked (strict
     * encapsulation on newer JDKs) — the native side treats this as best-effort
     * [INFO], never a hard assertion.
     */
    private static boolean hasTreeNodeBin(final HashMap<?, ?> map)
    {
        try
        {
            final java.lang.reflect.Field tableField = HashMap.class.getDeclaredField("table");
            tableField.setAccessible(true);
            final Object table = tableField.get(map);
            if (table == null)
            {
                return false;
            }
            final int len = java.lang.reflect.Array.getLength(table);
            for (int i = 0; i < len; ++i)
            {
                final Object head = java.lang.reflect.Array.get(table, i);
                if (head != null && head.getClass().getSimpleName().equals("TreeNode"))
                {
                    return true;
                }
            }
            return false;
        }
        catch (final Throwable t)
        {
            return false;
        }
    }

    private static HashMap<String, String> buildStrHash(final int n)
    {
        final HashMap<String, String> m = new HashMap<String, String>();
        for (int i = 0; i < n; ++i)
        {
            m.put("k" + i, "v" + i);
        }
        return m;
    }

    private static TreeMap<String, String> buildStrTree(final int n)
    {
        final TreeMap<String, String> m = new TreeMap<String, String>();
        for (int i = 0; i < n; ++i)
        {
            m.put("k" + i, "v" + i);
        }
        return m;
    }

    // ---- Builder -----------------------------------------------------------

    private static void buildAll()
    {
        // Legacy String content maps (routing characterization).
        hashMap = new HashMap<String, String>();
        hashMap.put(H0_KEY, H0_VAL);
        hashMap.put(H1_KEY, H1_VAL);
        hashMap.put(H2_KEY, H2_VAL);

        treeMap = new TreeMap<String, String>();
        treeMap.put(T2_KEY, T2_VAL);
        treeMap.put(T0_KEY, T0_VAL);
        treeMap.put(T1_KEY, T1_VAL);

        hashMapSize = hashMap.size();
        treeMapSize = treeMap.size();
        treeFirstKey = treeMap.firstKey();
        treeLastKey = treeMap.lastKey();

        // HashMap sizes 0/1/8/9/16/17/64/1000.
        hashEmpty = new HashMap<String, String>();
        hashOne = buildStrHash(N1);
        hashEight = buildStrHash(N8);
        hashNine = buildStrHash(N9);
        hashSixteen = buildStrHash(N16);
        hashSeventeen = buildStrHash(N17);
        hashSixtyFour = buildStrHash(N64);
        hashThousand = buildStrHash(N1000);
        hashResize13 = buildStrHash(RESIZE13);

        hashEmptySize = hashEmpty.size();
        hashOneSize = hashOne.size();
        hashEightSize = hashEight.size();
        hashNineSize = hashNine.size();
        hashSixteenSize = hashSixteen.size();
        hashSeventeenSize = hashSeventeen.size();
        hashSixtyFourSize = hashSixtyFour.size();
        hashThousandSize = hashThousand.size();
        hashResize13Size = hashResize13.size();

        long h64KeyChars = 0;
        for (int i = 0; i < N64; ++i) { h64KeyChars += codeUnitSum("k" + i); }
        hashSixtyFourKeyCharSum = h64KeyChars;

        long h1000KeyChars = 0;
        for (int i = 0; i < N1000; ++i) { h1000KeyChars += codeUnitSum("k" + i); }
        hashThousandKeyCharSum = h1000KeyChars;

        // HashMap collision chain (7 < 8 => plain Node.next chain, no TreeNode).
        hashColl7 = new HashMap<String, String>();
        final String[] coll7 = collidingKeys(COLL7);
        for (int i = 0; i < coll7.length; ++i)
        {
            hashColl7.put(coll7[i], "c" + i);
        }
        hashColl7Size = hashColl7.size();
        hashColl7IsTree = hasTreeNodeBin(hashColl7);   // expect false

        // HashMap TREE bin (>= 8 colliding keys => bucket treeifies to TreeNode).
        hashTreeBin = new HashMap<String, String>();
        final String[] collT = collidingKeys(TREE_BIN);
        for (int i = 0; i < collT.length; ++i)
        {
            hashTreeBin.put(collT[i], "t" + i);
        }
        hashTreeBinSize = hashTreeBin.size();
        hashTreeBinIsTree = hasTreeNodeBin(hashTreeBin);   // expect true (best-effort)

        // HashMap<Integer,Integer>: key i -> value 100+i.
        hashIntKey = new HashMap<Integer, Integer>();
        long ikKeySum = 0, ikValSum = 0;
        for (int i = 0; i < N8; ++i)
        {
            hashIntKey.put(Integer.valueOf(i), Integer.valueOf(100 + i));
            ikKeySum += i;
            ikValSum += (100 + i);
        }
        hashIntKeySize = hashIntKey.size();
        hashIntKeyKeySum = ikKeySum;
        hashIntKeyValSum = ikValSum;

        // HashMap<String,Long>: value 0x1_0000_0000 + i (forces a non-zero high word).
        hashLongVal = new HashMap<String, Long>();
        long lvSum = 0;
        for (int i = 0; i < N8; ++i)
        {
            final long v = 0x1_0000_0000L + i;
            hashLongVal.put("k" + i, Long.valueOf(v));
            lvSum += v;
        }
        hashLongValSize = hashLongVal.size();
        hashLongValValSum = lvSum;

        // HashMap<Day,String>: enum key -> "d"+ordinal.
        hashEnumKey = new HashMap<Day, String>();
        long ekOrd = 0, ekValChars = 0;
        for (final Day d : Day.values())
        {
            final String val = "d" + d.ordinal();
            hashEnumKey.put(d, val);
            ekOrd += d.ordinal();
            ekValChars += codeUnitSum(val);
        }
        hashEnumKeySize = hashEnumKey.size();
        hashEnumKeyOrdinalSum = ekOrd;
        hashEnumKeyValCharSum = ekValChars;

        // HashMap with a null KEY (legal) + two real keys.
        hashNullKey = new HashMap<String, String>();
        hashNullKey.put(null, "null-val");
        hashNullKey.put("a", "va");
        hashNullKey.put("b", "vb");
        hashNullKeySize = hashNullKey.size();

        // HashMap with a null VALUE (legal) + one real value.
        hashNullVal = new HashMap<String, String>();
        hashNullVal.put("present", null);
        hashNullVal.put("alsohere", "v9");
        hashNullValSize = hashNullVal.size();

        hashNull = null;

        // TreeMap sizes 0/1/8/64/1000 (k0..k{n-1}, in-order == sorted).
        treeEmpty = new TreeMap<String, String>();
        treeOne = buildStrTree(N1);
        treeEight = buildStrTree(N8);
        treeSixtyFour = buildStrTree(N64);
        treeThousand = buildStrTree(N1000);

        treeEmptySize = treeEmpty.size();
        treeOneSize = treeOne.size();
        treeEightSize = treeEight.size();
        treeSixtyFourSize = treeSixtyFour.size();
        treeThousandSize = treeThousand.size();

        treeOneFirst = treeOne.firstKey();
        treeFirstKeyEight = treeEight.firstKey();
        treeLastKeyEight = treeEight.lastKey();
        treeSixtyFourFirst = treeSixtyFour.firstKey();
        treeSixtyFourLast = treeSixtyFour.lastKey();
        treeThousandFirstKey = treeThousand.firstKey();
        treeThousandLastKey = treeThousand.lastKey();

        // TreeMap inserted DESCENDING; in-order walk must re-sort ascending.
        treeDescending = new TreeMap<String, String>();
        for (int i = N8 - 1; i >= 0; --i)
        {
            treeDescending.put("k" + i, "v" + i);
        }
        treeDescendingSize = treeDescending.size();

        // TreeMap with one null VALUE (allowed); keys remain sorted.
        treeNullVal = new TreeMap<String, String>();
        treeNullVal.put("a", "va");
        treeNullVal.put("b", null);
        treeNullVal.put("c", "vc");
        treeNullValSize = treeNullVal.size();

        // TreeMap<Integer,Integer>: inserted scrambled, walk natural numeric order.
        treeIntKey = new TreeMap<Integer, Integer>();
        final int[] order = { 5, 2, 9, 0, 7, 3, 8, 1, 6, 4 };
        for (int i = 0; i < order.length; ++i)
        {
            treeIntKey.put(Integer.valueOf(order[i]), Integer.valueOf(order[i] * 10));
        }
        treeIntKeySize = treeIntKey.size();
        treeIntKeyFirst = treeIntKey.firstKey().intValue();
        treeIntKeyLast = treeIntKey.lastKey().intValue();

        treeNull = null;
    }

    /**
     * Force vmhook.fixtures.HashTreeMap$Day to be LOADED at class-init so the
     * native register_class for the enum finds the klass already present.  Never
     * put into any user-visible assertion path.
     */
    private static final Day DAY_CLASS_PIN = Day.MON;

    static
    {
        if (DAY_CLASS_PIN == null)
        {
            throw new IllegalStateException("unreachable");
        }

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
                // mode 0 (the only mode): (re)build every map so the native reads
                // see a fresh, deterministic population on this exact thread.
                buildAll();
                HashTreeMap.done = true;
            }
        });
    }
}
