package vmhook.fixtures;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

import vmhook.Harness;

/**
 * Fixture for the collection_iteration_safety feature (area: collections).
 *
 * Where the sibling collection_* modules prove iteration CONTENT (exact values,
 * order, identity), THIS fixture is the ROBUSTNESS / SAFETY oracle: it drives
 * vmhook's collection-walk surface
 *   - collection::to_vector            (vmhook.hpp ~14793; field cascade)
 *   - hash_map_walk_keys               (vmhook.hpp ~15336)
 *   - tree_map_walk_keys               (vmhook.hpp ~15528)
 *   - hash_map_walk_entries            (vmhook.hpp ~15250)
 *   - tree_map_walk_entries            (vmhook.hpp ~15415)
 *   - linked_list_walk_items           (vmhook.hpp ~15183)
 *   - get_array_element bounds         (vmhook.hpp ~11563)
 * across every DEGENERATE / ADVERSARIAL shape and size, and asserts only that
 *   (1) the walk NEVER crashes,
 *   (2) the decoded element COUNT equals the Java-known size (size is the
 *       oracle), with the empty case yielding an empty vector,
 *   (3) a heavy walk completes with NO duplicate element OOP (proxy for "no
 *       cycle / no re-emit"), and
 *   (4) get_array_element CLAMPS every out-of-bounds index instead of reading
 *       out of bounds.
 *
 * Shapes covered (each declared field carries a matching *Size constant the
 * native side reads as the oracle, computed via the live Java size()):
 *   - EMPTY:   ArrayList / LinkedList / HashSet / LinkedHashSet / TreeSet /
 *              HashMap / LinkedHashMap / TreeMap  (every walk on a zero-size
 *              container must return empty, never deref a null table/root/first).
 *   - LARGE:   ArrayList(BIG) + LinkedList(BIG), BIG=5000 — the walk must
 *              terminate at exactly BIG, with no duplicate element OOP.
 *   - NULLS:   ArrayList / LinkedList each with several interleaved null
 *              elements — the decoded count still equals size; nulls become
 *              nullptr slots (not dropped, not crashing).
 *   - OVERSIZED ArrayList: capacity 256 but only FEW elements — to_vector must
 *              return `size`, never elementData.length (no phantom-null tail).
 *   - COLLIDING HashMap / HashSet: many keys sharing ONE hashCode, forcing a
 *              treeified red-black bin — the Node.next chain must still surface
 *              every entry/key (count == size).
 *   - TreeMap inserted OUT OF ORDER — size match after the in-order walk.
 *   - newSetFromMap(HashMap): KNOWN vmhook mis-route (its backing-map field is
 *              named "m", so to_vector takes the TreeSet path and HashMap has no
 *              "root"); the native side PINS the (buggy) short decode, never
 *              asserts it as correct.
 *   - ROBUSTNESS: a null collection field + a missing field name -> empty.
 *
 * get_array_element bounds: a primitive int[] (intArr) and an Object[]
 * (objArr) of known length let the native side hammer every boundary index
 * (negative, INT_MIN, length, length+1, INT_MAX) and confirm a clamp (T{}),
 * plus in-bounds reads that return the real value.
 *
 * Element type Elem carries an int id and a String tag ("e<id>") so a decoded
 * element OOP can be proven walkable; Elem is Comparable so the TreeSet/TreeMap
 * key order is stable across runs.
 *
 * Java 8 source only (no var/records/switch-expr/text-blocks/List.of). ASCII
 * only; any non-ASCII would use a \\uXXXX escape.
 */
public final class CollIterSafety
{
    /** Native sets this true to request the action; cleared after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect of the hookable trigger() (detour-fired proof). */
    public static volatile int observed;

    /** A nonce so the trigger() dispatch is always a fresh interpreter frame. */
    public static volatile int triggerNonce;

    /**
     * Element type the native side wraps.  `id` is the insertion index (or a
     * fixed tag for the colliding-key cases); `tag` is "e<id>".  Comparable so
     * TreeSet<Elem> / TreeMap<Elem,*> have a stable, run-independent order.
     * Nested "CollIterSafety$Elem" in JVM internal form.
     */
    public static final class Elem implements Comparable<Elem>
    {
        public final int id;
        public final String tag;

        public Elem(final int id)
        {
            this.id = id;
            this.tag = "e" + id;
        }

        @Override
        public int compareTo(final Elem other)
        {
            return Integer.compare(this.id, other.id);
        }
    }

    // ── Sizes the native side mirrors ───────────────────────────────────────
    /** Large container size for the cycle/termination canary. */
    public static final int BIG = 5000;

    /** Element count of the null-bearing lists (some slots are null). */
    public static final int NULL_LIST_LEN = 6;

    /** Number of NON-null elements in the null-bearing lists. */
    public static final int NULL_LIST_NONNULL = 4;

    /** Element count of the oversized (high-capacity, few-element) ArrayList. */
    public static final int OVERSIZED_LEN = 5;

    /** Backing capacity of the oversized ArrayList (>> OVERSIZED_LEN). */
    public static final int OVERSIZED_CAP = 256;

    /** Number of colliding keys (forces a treeified red-black bin: > 8, cap 64). */
    public static final int COLLIDE_N = 64;

    /** Element count of the out-of-order-inserted TreeMap / TreeSet. */
    public static final int TREE_N = 32;

    /** Length of the primitive int[] used for get_array_element bounds. */
    public static final int INT_ARR_LEN = 8;

    /** Length of the Object[] used for get_array_element bounds. */
    public static final int OBJ_ARR_LEN = 4;

    // ── EMPTY containers (every walk on these must return empty) ────────────
    public static List<Elem>        emptyArrayList     = new ArrayList<Elem>();
    public static List<Elem>        emptyLinkedList    = new LinkedList<Elem>();
    public static Set<Elem>         emptyHashSet       = new HashSet<Elem>();
    public static Set<Elem>         emptyLinkedHashSet = new LinkedHashSet<Elem>();
    public static Set<Elem>         emptyTreeSet       = new TreeSet<Elem>();
    public static Map<Elem, Elem>   emptyHashMap       = new HashMap<Elem, Elem>();
    public static Map<Elem, Elem>   emptyLinkedHashMap = new LinkedHashMap<Elem, Elem>();
    public static Map<Elem, Elem>   emptyTreeMap       = new TreeMap<Elem, Elem>();

    // ── LARGE list + linked list (cycle / termination canary) ───────────────
    public static List<Elem> bigArrayList  = new ArrayList<Elem>();
    public static List<Elem> bigLinkedList = new LinkedList<Elem>();

    // ── NULL-bearing list + linked list ─────────────────────────────────────
    public static List<Elem> nullArrayList  = new ArrayList<Elem>();
    public static List<Elem> nullLinkedList = new LinkedList<Elem>();

    // ── OVERSIZED ArrayList (capacity >> size) ──────────────────────────────
    public static List<Elem> oversizedArrayList = new ArrayList<Elem>(OVERSIZED_CAP);

    // ── COLLIDING-key HashMap / HashSet (treeified bin) ─────────────────────
    public static Map<String, Elem> collideHashMap = new HashMap<String, Elem>();
    public static Set<String>       collideHashSet = new HashSet<String>();

    // ── Out-of-order TreeMap / TreeSet ──────────────────────────────────────
    public static Map<Elem, Elem> outOfOrderTreeMap = new TreeMap<Elem, Elem>();
    public static Set<Elem>       outOfOrderTreeSet = new TreeSet<Elem>();

    // ── KNOWN mis-route: Collections.newSetFromMap(HashMap) ─────────────────
    public static Set<Elem> setFromHashMap =
        Collections.newSetFromMap(new HashMap<Elem, Boolean>());
    public static final int SETFROMMAP_N = 5;

    // ── ROBUSTNESS: a declared-but-null collection field ────────────────────
    public static Set<Elem> nullSet = null;

    // ── Raw arrays for get_array_element bounds clamping ─────────────────────
    public static int[]    intArr = new int[INT_ARR_LEN];
    public static Object[] objArr = new Object[OBJ_ARR_LEN];

    // ── Published Java-side sizes (the native oracle) ───────────────────────
    public static volatile int emptyArrayListSize;
    public static volatile int emptyLinkedListSize;
    public static volatile int emptyHashSetSize;
    public static volatile int emptyLinkedHashSetSize;
    public static volatile int emptyTreeSetSize;
    public static volatile int emptyHashMapSize;
    public static volatile int emptyLinkedHashMapSize;
    public static volatile int emptyTreeMapSize;

    public static volatile int bigArrayListSize;
    public static volatile int bigLinkedListSize;
    public static volatile int nullArrayListSize;
    public static volatile int nullLinkedListSize;
    public static volatile int oversizedArrayListSize;

    public static volatile int collideHashMapSize;
    public static volatile int collideHashSetSize;
    public static volatile int outOfOrderTreeMapSize;
    public static volatile int outOfOrderTreeSetSize;
    public static volatile int setFromHashMapSize;

    /** True iff at least one bin treeified in collideHashMap (red-black bin). */
    public static volatile boolean collideMapHasTreeBin;
    /** True iff at least one bin treeified in collideHashSet. */
    public static volatile boolean collideSetHasTreeBin;

    // ── Hookable site: the native module installs a scoped_hook here so all
    //    its collection reads run inside an interpreter detour where a
    //    JavaThread is current (the same shape collection_list uses). ─────────
    private int seed = 9000;

    public int trigger(final int nonce)
    {
        triggerNonce = nonce;
        observed = this.seed + nonce;
        return observed;
    }

    // ── Builders ────────────────────────────────────────────────────────────

    private static void buildAll()
    {
        emptyArrayList     = new ArrayList<Elem>();
        emptyLinkedList    = new LinkedList<Elem>();
        emptyHashSet       = new HashSet<Elem>();
        emptyLinkedHashSet = new LinkedHashSet<Elem>();
        emptyTreeSet       = new TreeSet<Elem>();
        emptyHashMap       = new HashMap<Elem, Elem>();
        emptyLinkedHashMap = new LinkedHashMap<Elem, Elem>();
        emptyTreeMap       = new TreeMap<Elem, Elem>();

        bigArrayList  = new ArrayList<Elem>();
        bigLinkedList = new LinkedList<Elem>();
        for (int i = 0; i < BIG; ++i)
        {
            final Elem a = new Elem(i);
            final Elem b = new Elem(i);
            bigArrayList.add(a);
            bigLinkedList.add(b);
        }

        // Null-bearing lists: ids at 0,1,_,3,_,5 with null at indices 2 and 4.
        nullArrayList  = new ArrayList<Elem>();
        nullLinkedList = new LinkedList<Elem>();
        for (int i = 0; i < NULL_LIST_LEN; ++i)
        {
            if (i == 2 || i == 4)
            {
                nullArrayList.add(null);
                nullLinkedList.add(null);
            }
            else
            {
                nullArrayList.add(new Elem(i));
                nullLinkedList.add(new Elem(i));
            }
        }

        // Oversized: high capacity, few elements.  ensureCapacity makes the
        // backing array length much larger than size, so the native walk must
        // stop at size, not at elementData.length.
        oversizedArrayList = new ArrayList<Elem>(OVERSIZED_CAP);
        ((ArrayList<Elem>) oversizedArrayList).ensureCapacity(OVERSIZED_CAP);
        for (int i = 0; i < OVERSIZED_LEN; ++i)
        {
            oversizedArrayList.add(new Elem(i));
        }

        // Colliding-key HashMap / HashSet: COLLIDE_N keys all sharing ONE
        // String.hashCode so they pile into a single bucket and treeify.
        collideHashMap = new HashMap<String, Elem>();
        collideHashSet = new HashSet<String>();
        final String[] coll = collidingKeys(COLLIDE_N);
        for (int i = 0; i < coll.length; ++i)
        {
            collideHashMap.put(coll[i], new Elem(i));
            collideHashSet.add(coll[i]);
        }
        collideMapHasTreeBin = hasTreeNodeBin(collideHashMap);
        collideSetHasTreeBin = hasTreeNodeBinSet(collideHashSet);

        // Out-of-order TreeMap / TreeSet: insert ids in a shuffled-ish order so
        // the red-black tree actually rebalances; the in-order walk re-sorts.
        outOfOrderTreeMap = new TreeMap<Elem, Elem>();
        outOfOrderTreeSet = new TreeSet<Elem>();
        for (int i = 0; i < TREE_N; ++i)
        {
            // A simple order-scrambling permutation (coprime stride mod TREE_N).
            final int id = (i * 17 + 5) % TREE_N;
            outOfOrderTreeMap.put(new Elem(id), new Elem(id));
            outOfOrderTreeSet.add(new Elem(id));
        }

        setFromHashMap = Collections.newSetFromMap(new HashMap<Elem, Boolean>());
        for (int i = 0; i < SETFROMMAP_N; ++i)
        {
            setFromHashMap.add(new Elem(300 + i));
        }

        nullSet = null;

        // Raw arrays with known sentinel values for the bounds test.
        intArr = new int[INT_ARR_LEN];
        for (int i = 0; i < INT_ARR_LEN; ++i)
        {
            intArr[i] = 1000 + i;          // intArr[k] == 1000+k
        }
        objArr = new Object[OBJ_ARR_LEN];
        for (int i = 0; i < OBJ_ARR_LEN; ++i)
        {
            objArr[i] = new Elem(700 + i);
        }

        // Publish sizes (the native oracle), computed via the live size().
        emptyArrayListSize     = emptyArrayList.size();
        emptyLinkedListSize    = emptyLinkedList.size();
        emptyHashSetSize       = emptyHashSet.size();
        emptyLinkedHashSetSize = emptyLinkedHashSet.size();
        emptyTreeSetSize       = emptyTreeSet.size();
        emptyHashMapSize       = emptyHashMap.size();
        emptyLinkedHashMapSize = emptyLinkedHashMap.size();
        emptyTreeMapSize       = emptyTreeMap.size();

        bigArrayListSize       = bigArrayList.size();
        bigLinkedListSize      = bigLinkedList.size();
        nullArrayListSize      = nullArrayList.size();
        nullLinkedListSize     = nullLinkedList.size();
        oversizedArrayListSize = oversizedArrayList.size();

        collideHashMapSize     = collideHashMap.size();
        collideHashSetSize     = collideHashSet.size();
        outOfOrderTreeMapSize  = outOfOrderTreeMap.size();
        outOfOrderTreeSetSize  = outOfOrderTreeSet.size();
        setFromHashMapSize     = setFromHashMap.size();
    }

    /**
     * Generates n strings sharing the SAME String.hashCode by chaining the
     * classic "Aa"/"BB" collision (both hash to 2112).  Concatenating equal
     * 2-char blocks preserves the equal-hashCode property, so all n keys collide
     * into one bucket and the bin treeifies once it exceeds 8 entries (and the
     * table has reached MIN_TREEIFY_CAPACITY 64; COLLIDE_N=64 guarantees that).
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

    /** Reports whether any bucket head in a HashMap's table is a TreeNode. */
    private static boolean hasTreeNodeBin(final Map<?, ?> map)
    {
        try
        {
            final java.lang.reflect.Field tableField =
                HashMap.class.getDeclaredField("table");
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
                if (head != null
                    && head.getClass().getSimpleName().equals("TreeNode"))
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

    /** Same probe for a HashSet (via its backing "map" HashMap). */
    private static boolean hasTreeNodeBinSet(final Set<?> set)
    {
        try
        {
            final java.lang.reflect.Field mapField =
                HashSet.class.getDeclaredField("map");
            mapField.setAccessible(true);
            final Object map = mapField.get(set);
            if (!(map instanceof Map))
            {
                return false;
            }
            return hasTreeNodeBin((Map<?, ?>) map);
        }
        catch (final Throwable t)
        {
            return false;
        }
    }

    /** The single instance the native module wraps (reached via SINGLETON). */
    public static final CollIterSafety SINGLETON = new CollIterSafety();

    /**
     * Forces vmhook.fixtures.CollIterSafety$Elem to be LOADED at class-init time
     * so the native register_class<elem>("...$Elem") finds the klass already in
     * the loaded graph.  Never inserted into any container, so it perturbs no
     * size/membership assertion.
     */
    private static final Elem ELEM_CLASS_PIN = new Elem(-1);

    static
    {
        if (ELEM_CLASS_PIN == null)
        {
            throw new IllegalStateException("unreachable");
        }

        // Build once at class-init so every container is populated even before
        // the first probe; the native module also re-requests a build by driving
        // the trigger() probe (which calls buildAll() on the Java thread).
        buildAll();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return CollIterSafety.go && !CollIterSafety.done;
            }

            @Override
            public void run()
            {
                // Rebuild on the Java thread so the native reads (done inside the
                // trigger() detour, below) see a fresh, deterministic snapshot,
                // then fire the hooked trigger() so the detour runs its sweep.
                buildAll();
                SINGLETON.trigger(7);
                CollIterSafety.done = true;
            }
        });
    }
}
