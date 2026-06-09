package vmhook.fixtures;

import vmhook.Harness;

import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.TreeSet;

/**
 * Fixture for the collection_set feature (area: collections).
 *
 * Exercises vmhook::collection::to_vector&lt;wrapper&gt;() /
 * field_proxy::value_t::to_vector&lt;element&gt;() over real java.util.Set fields on
 * a live JVM, across every Set shape and boundary the two audit analyses
 * (to_vector_hashset_bucket_walk.md, to_vector_treeset_redblack.md) flagged:
 *
 *   - HashSet&lt;Elem&gt; / HashSet&lt;String&gt;   "map" field → hash_map_walk_keys
 *                                          (table bucket array + Node.next chain).
 *       empty / single / many (forces a backing-HashMap resize) / a deliberately
 *       TREEIFIED bin (&gt;8 colliding keys → at least one bucket head is a
 *       TreeNode; the Node.next chain must still surface every key) / a HashSet
 *       containing the legal single null element (→ a nullptr slot) / a large
 *       set (many buckets + chains).
 *
 *   - LinkedHashSet&lt;Elem&gt;                also "map" field → SAME hash_map_walk_keys
 *       path.  Java contract is INSERTION order, but vmhook walks BUCKET order;
 *       the native module verifies CONTENT (membership, order-independent) and
 *       deliberately does NOT require insertion order — pinning the documented
 *       [low] "LinkedHashSet insertion order is silently lost" behaviour.
 *
 *   - TreeSet&lt;Elem&gt; / TreeSet&lt;String&gt;   "m" field → tree_map_walk_keys
 *                                          (iterative in-order red-black walk).
 *       empty / single / small / many — in-order == sorted element order, which
 *       the native module asserts exactly.
 *
 *   - Collections.newSetFromMap(new HashMap&lt;&gt;())   the JDK Set wrapper whose
 *       backing-map field is ALSO literally named "m" (collides with TreeSet).
 *       vmhook routes it to the TreeSet fast path, find_field(mapKlass,"root")
 *       misses on a HashMap, and an EMPTY vector is returned for a NON-empty Set.
 *       The native module CHARACTERIZES this real [medium] vmhook bug (asserts
 *       the actual empty/partial result + records an [INFO] flaw note); it never
 *       crashes and never edits vmhook.
 *
 *   - a Set field left NULL                  to_vector must return empty, never throw.
 *
 * Element ORDER is NOT guaranteed for HashSet/LinkedHashSet, so the native side
 * verifies the SET of decoded values (count + membership + order-independent
 * id checksums), NOT a sequence.  TreeSet additionally gets a strict sorted-order
 * assertion because its in-order walk order is defined.
 *
 * Each Elem carries an int `id` and a String `tag` ("e&lt;id&gt;") so the native value
 * wrapper can verify BOTH a primitive and a reference field round-trip through
 * each decoded element OOP, and so order-independent aggregates (idSum / idXor /
 * tagCharSum) can cross-check content without depending on iteration order.
 * Elem implements Comparable&lt;Elem&gt; (by id) so TreeSet&lt;Elem&gt; has a stable,
 * run-independent order — identity hashCode would make object ordering random.
 *
 * Canonical go/done handshake with a `mode` selector (mirrors CollMap): the
 * native module sets `mode`, clears `done`, raises `go`; the Harness loop runs
 * the matching scenario on the Java thread; the module polls `done`.  Because the
 * sets are plain static object fields, the native side reads them directly; a
 * hookable touch() is still provided so the module proves an interpreter hook
 * fires through the modular path, exactly like the pilot.
 *
 * Java 8 syntax only (no var/records/switch-expr/text-blocks).
 */
public final class CollSet
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect of the hookable method (pilot-style proof). */
    public static volatile int observed;

    /**
     * Scenario selector.  The native module sets this BEFORE raising `go`.
     *   0  = build/refresh every set field (default; also runs once at startup)
     *   1  = call touch() so the interpreter hook fires on real dispatch
     */
    public static volatile int mode;

    /**
     * Element type the native side wraps.  `id` drives order-independent
     * checksums AND the TreeSet comparator (so TreeSet order is stable across
     * runs); `tag` is "e&lt;id&gt;" for the reference-field readback.  A nested public
     * static final class is "CollSet$Elem" in JVM internal form — that is the
     * name the native module registers.
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

        // equals/hashCode are BY id, consistent with compareTo, so a HashSet
        // (and HashMap-backed SetFromMap) deduplicates two Elems that share an
        // id — exercised by the duplicate-add scenario.  Every other scenario
        // uses DISTINCT ids, so adding value equality leaves their element
        // counts and OOP-distinctness assertions unchanged (distinct ids are
        // still distinct objects with distinct hashCodes).
        @Override
        public boolean equals(final Object other)
        {
            if (this == other)
            {
                return true;
            }
            if (!(other instanceof Elem))
            {
                return false;
            }
            return this.id == ((Elem) other).id;
        }

        @Override
        public int hashCode()
        {
            return this.id;
        }
    }

    // ---- Scenario sizes (mirrored on the native side) ----------------------

    /** Element count of the "many" HashSet/LinkedHashSet (> default cap 16). */
    public static final int MANY_N = 50;

    /** Element count of the large HashSet (many buckets + chains). */
    public static final int BIG_N = 5000;

    /**
     * Element count of the treeified HashSet.  All keys collide into one bucket;
     * HashMap only treeifies a bin once the table capacity reaches
     * MIN_TREEIFY_CAPACITY (64) AND the bin exceeds TREEIFY_THRESHOLD (8) — below
     * cap 64 it resizes instead.  64 colliding keys forces the table to grow to
     * capacity 64 (resize at 12/24/48 → 32/64/128) with all 64 in one bucket, so
     * that bucket is GUARANTEED to convert to a red-black TreeNode bin.
     */
    public static final int TREEIFY_N = 64;

    /** Element count of the TreeSet "many" case. */
    public static final int TREE_MANY_N = 200;

    /** The smallest "more than one" size — exercised in every Set flavour. */
    public static final int TWO = 2;

    /**
     * Number of elements added to hashDup BEFORE the duplicate re-adds.  We then
     * re-add each one a second time (by a value-equal but distinct Elem object),
     * so the Set size stays DUP_DISTINCT — proving the HashSet key walk surfaces
     * each distinct element exactly once and the duplicate adds were absorbed by
     * equals/hashCode (no phantom extra elements).
     */
    public static final int DUP_DISTINCT = 4;

    /**
     * Size of the small hash-COLLISION chain that stays BELOW the treeify
     * threshold (8): a handful of keys that all share one String.hashCode land in
     * one bucket as a plain singly-linked Node.next chain (no TreeNode).  The key
     * walk must follow Node.next and surface every one.
     */
    public static final int COLLISION_CHAIN_N = 5;

    // ---- The Set fields the native module reads via to_vector --------------

    /** Empty HashSet — size 0, to_vector must be empty (no element read). */
    public static HashSet<Elem> hashEmpty = new HashSet<Elem>();

    /** Single-element HashSet. */
    public static HashSet<Elem> hashSingle = new HashSet<Elem>();

    /** Many-element HashSet (MANY_N elements; backing HashMap resized). */
    public static HashSet<Elem> hashMany = new HashSet<Elem>();

    /** Large HashSet (BIG_N elements; many buckets and chains to walk). */
    public static HashSet<Elem> hashBig = new HashSet<Elem>();

    /** HashSet of Strings ("s0".."s(MANY_N-1)") — String element decode path. */
    public static HashSet<String> hashStrings = new HashSet<String>();

    /**
     * HashSet whose backing HashMap TREEIFIES a bin: TREEIFY_N String elements
     * that all share one hashCode (classic "Aa"/"BB" collision family) so &gt;8
     * land in the same bucket and the bin converts to a red-black TreeNode bin.
     * The Node.next chain (kept populated even after treeification) must still
     * surface every element.
     */
    public static HashSet<String> hashTreeified = new HashSet<String>();

    /**
     * HashSet containing the legal single null element plus NULL_SET_NONNULL
     * real elements.  vmhook surfaces the null element as a nullptr slot.
     */
    public static HashSet<Elem> hashWithNull = new HashSet<Elem>();

    /** Number of NON-null elements in hashWithNull (plus one null → +1 size). */
    public static final int NULL_SET_NONNULL = 3;

    /** Two-element HashSet — the smallest multi-element bucket-walk case. */
    public static HashSet<Elem> hashTwo = new HashSet<Elem>();

    /**
     * HashSet built by adding DUP_DISTINCT elements and then re-adding each one a
     * SECOND time via a value-equal (same id) but DISTINCT Elem object.  Because
     * Elem.equals/hashCode are by id, the duplicate adds are absorbed and the set
     * size stays DUP_DISTINCT — the decode must surface each id exactly once.
     */
    public static HashSet<Elem> hashDup = new HashSet<Elem>();

    /**
     * HashSet of COLLISION_CHAIN_N Strings that all share one String.hashCode but
     * stay below the treeify threshold (8) and below MIN_TREEIFY_CAPACITY, so they
     * form a plain Node.next singly-linked chain in ONE bucket (NOT a TreeNode
     * bin).  Proves the key walk follows a real multi-node bucket chain.
     */
    public static HashSet<String> hashCollisionChain = new HashSet<String>();

    /** Two-element LinkedHashSet (also the "map" → hash_map_walk_keys path). */
    public static LinkedHashSet<Elem> linkedTwo = new LinkedHashSet<Elem>();

    /** Small LinkedHashSet (insertion-ordered in Java; bucket-ordered via vmhook). */
    public static LinkedHashSet<Elem> linkedSmall = new LinkedHashSet<Elem>();

    /** Large LinkedHashSet. */
    public static LinkedHashSet<Elem> linkedMany = new LinkedHashSet<Elem>();

    /** Empty TreeSet. */
    public static TreeSet<Elem> treeEmpty = new TreeSet<Elem>();

    /** Single-element TreeSet. */
    public static TreeSet<Elem> treeSingle = new TreeSet<Elem>();

    /** Two-element TreeSet (smallest multi-node red-black in-order walk). */
    public static TreeSet<Elem> treeTwo = new TreeSet<Elem>();

    /** Small TreeSet: SMALL_N Elem (in-order walk == sorted by id). */
    public static TreeSet<Elem> treeSmall = new TreeSet<Elem>();

    /**
     * TreeSet ordered by an EXPLICIT reverse comparator (Collections
     * .reverseOrder()).  TreeMap's in-order red-black walk visits nodes in the
     * COMPARATOR's order, so the decode must come out DESCENDING by id —
     * [SMALL_N-1 .. 0] — proving the in-order walk honours a custom comparator,
     * not the element's natural ordering.
     */
    public static TreeSet<Elem> treeReverse =
        new TreeSet<Elem>(java.util.Collections.<Elem>reverseOrder());

    /** Large TreeSet: TREE_MANY_N Elem; deep red-black tree. */
    public static TreeSet<Elem> treeMany = new TreeSet<Elem>();

    /** TreeSet of Strings — String element decode through the in-order walk. */
    public static TreeSet<String> treeStrings = new TreeSet<String>();

    public static final int SMALL_N = 3;

    /**
     * Set backed by a HashMap via Collections.newSetFromMap(...).  Its private
     * backing-map field is named "m" (same as TreeSet's), but the map is a
     * HashMap (no "root" field), so vmhook's TreeSet fast path returns empty for
     * this NON-empty Set.  Holds SETFROMMAP_N elements.  The native module
     * characterizes the resulting empty/partial decode as a known vmhook bug.
     */
    public static Set<Elem> setFromHashMap =
        Collections.newSetFromMap(new HashMap<Elem, Boolean>());

    public static final int SETFROMMAP_N = 4;

    /**
     * Collections.emptySet(): the JDK EmptySet has NO "map"/"m"/"elementData"/
     * "first" field, so vmhook's cascade reaches the generic size()+get(int)
     * fallback, sees size()==0, and returns empty.  This is the CORRECT result
     * (an empty Set decodes to an empty vector) and is asserted HARD.
     */
    public static Set<Elem> emptySet = java.util.Collections.emptySet();

    /**
     * Collections.singleton(Elem): SingletonSet stores its element in a field
     * "element" — none of the four fast-path field names — so the cascade reaches
     * the generic fallback, which needs a get(int) method.  A Set has no get(int),
     * so the decode returns EMPTY for this NON-empty (size 1) Set.  The native
     * module CHARACTERIZES this as a known limitation (Set wrappers without a
     * fast-path field shape are not decodable through the List-only fallback) —
     * the SAME root cause as the setFromHashMap [medium] bug, surfaced on a
     * different JDK wrapper.  size() is 1.
     */
    public static Set<Elem> singletonSet =
        java.util.Collections.singleton(new Elem(300));

    /**
     * Collections.unmodifiableSet(hashTwo): UnmodifiableSet extends
     * UnmodifiableCollection, whose backing field is "c" — again none of the
     * fast-path names — so the cascade falls to the List-only get(int) fallback
     * and returns EMPTY for this NON-empty Set.  Characterized like singletonSet.
     * size() mirrors the backing set (TWO).
     */
    public static Set<Elem> unmodifiableSet =
        java.util.Collections.unmodifiableSet(hashTwo);

    /**
     * Collections.synchronizedSet(hashTwo): SynchronizedSet extends
     * SynchronizedCollection, backing field "c" + a "mutex" — no fast-path field
     * shape — so it also decodes EMPTY through the List-only fallback.  size()
     * mirrors the backing set (TWO).  Same characterization.
     */
    public static Set<Elem> synchronizedSet =
        java.util.Collections.synchronizedSet(hashTwo);

    /** A declared Set field deliberately left NULL (to_vector must stay empty). */
    public static Set<Elem> nullSet = null;

    // ---- Published cross-check values (order-independent) -------------------
    //
    // The native walker visits HashSet/LinkedHashSet elements in BUCKET order,
    // not insertion order, so the module verifies CONTENT via order-independent
    // aggregates that Java computes the same way.  Each Elem has id i, so:
    //   idSum  = sum over elements of id
    //   idXor  = xor over elements of id
    // and for String sets:
    //   strCharSum = sum over elements of (sum of UTF-16 code units of the text)

    public static volatile long hashManyIdSum;
    public static volatile long hashManyIdXor;

    public static volatile long hashBigIdSum;
    public static volatile long hashBigIdXor;

    public static volatile long linkedManyIdSum;
    public static volatile long linkedManyIdXor;

    public static volatile long hashStringsCharSum;
    public static volatile long hashTreeifiedCharSum;
    public static volatile long hashCollisionChainCharSum;

    public static volatile long treeManyIdSum;

    /** Java's own view of each set's size(), for a native cross-check. */
    public static volatile int hashEmptySize;
    public static volatile int hashSingleSize;
    public static volatile int hashTwoSize;
    public static volatile int hashManySize;
    public static volatile int hashBigSize;
    public static volatile int hashDupSize;
    public static volatile int hashStringsSize;
    public static volatile int hashTreeifiedSize;
    public static volatile int hashCollisionChainSize;
    public static volatile int hashWithNullSize;
    public static volatile int linkedTwoSize;
    public static volatile int linkedSmallSize;
    public static volatile int linkedManySize;
    public static volatile int treeEmptySize;
    public static volatile int treeSingleSize;
    public static volatile int treeTwoSize;
    public static volatile int treeSmallSize;
    public static volatile int treeManySize;
    public static volatile int treeReverseSize;
    public static volatile int treeStringsSize;
    public static volatile int setFromHashMapSize;
    public static volatile int emptySetSize;
    public static volatile int singletonSetSize;
    public static volatile int unmodifiableSetSize;
    public static volatile int synchronizedSetSize;

    /** Whether the treeified HashSet actually treeified at least one bin. */
    public static volatile boolean treeifiedHasTreeBin;

    /**
     * Whether the small collision-chain HashSet stayed a PLAIN Node chain (no
     * TreeNode) AND actually placed more than one element in one bucket — i.e. a
     * genuine multi-node singly-linked bucket the key walk has to traverse.
     */
    public static volatile boolean collisionChainIsPlainChain;

    /** Java's idSum / idXor for the two-element + dup HashSets (order-free). */
    public static volatile long hashTwoIdSum;
    public static volatile long hashTwoIdXor;
    public static volatile long hashDupIdSum;
    public static volatile long hashDupIdXor;
    public static volatile long linkedTwoIdSum;

    // ---- Hookable method (pilot-style interpreter-hook proof) --------------

    private int seed = 6000;

    public int touch(final int delta)
    {
        return this.seed + delta;
    }

    // ---- Builders ----------------------------------------------------------

    private static long codeUnitSum(final String s)
    {
        long sum = 0;
        for (int i = 0; i < s.length(); ++i)
        {
            sum += s.charAt(i);
        }
        return sum;
    }

    private static void buildAll()
    {
        hashEmpty = new HashSet<Elem>();

        hashSingle = new HashSet<Elem>();
        hashSingle.add(new Elem(0));

        hashTwo = new HashSet<Elem>();
        hashTwo.add(new Elem(0));
        hashTwo.add(new Elem(1));
        hashTwoIdSum = 0 + 1;
        hashTwoIdXor = 0 ^ 1;

        // Duplicate-add: insert DUP_DISTINCT ids, then re-add each via a SECOND,
        // value-equal but distinct Elem object.  equals/hashCode are by id, so
        // every re-add is absorbed and the size stays DUP_DISTINCT.
        hashDup = new HashSet<Elem>();
        long hdId = 0, hdXor = 0;
        for (int i = 0; i < DUP_DISTINCT; ++i)
        {
            hashDup.add(new Elem(i));
            hdId += i;
            hdXor ^= i;
        }
        for (int i = 0; i < DUP_DISTINCT; ++i)
        {
            // A brand-new object that is equals() to the one already present.
            hashDup.add(new Elem(i));
        }
        hashDupIdSum = hdId;
        hashDupIdXor = hdXor;

        hashMany = new HashSet<Elem>();
        long hmId = 0, hmXor = 0;
        for (int i = 0; i < MANY_N; ++i)
        {
            hashMany.add(new Elem(i));
            hmId += i;
            hmXor ^= i;
        }
        hashManyIdSum = hmId;
        hashManyIdXor = hmXor;

        hashBig = new HashSet<Elem>();
        long hbId = 0, hbXor = 0;
        for (int i = 0; i < BIG_N; ++i)
        {
            hashBig.add(new Elem(i));
            hbId += i;
            hbXor ^= i;
        }
        hashBigIdSum = hbId;
        hashBigIdXor = hbXor;

        hashStrings = new HashSet<String>();
        long hsChar = 0;
        for (int i = 0; i < MANY_N; ++i)
        {
            final String s = "s" + i;
            hashStrings.add(s);
            hsChar += codeUnitSum(s);
        }
        hashStringsCharSum = hsChar;

        // Treeified HashSet: TREEIFY_N keys that all share one String.hashCode so
        // >8 collide into one bucket and the bin treeifies.
        hashTreeified = new HashSet<String>();
        long htChar = 0;
        final String[] coll = collidingKeys(TREEIFY_N);
        for (int i = 0; i < coll.length; ++i)
        {
            hashTreeified.add(coll[i]);
            htChar += codeUnitSum(coll[i]);
        }
        hashTreeifiedCharSum = htChar;
        treeifiedHasTreeBin = hasTreeNodeBin(hashTreeified);

        // Small hash-collision chain that stays BELOW the treeify threshold:
        // COLLISION_CHAIN_N (5) colliding-hashCode keys → one bucket, a plain
        // Node.next chain (5 < TREEIFY_THRESHOLD 8, and the default cap-16 table
        // is below MIN_TREEIFY_CAPACITY 64 anyway, so it never treeifies).
        hashCollisionChain = new HashSet<String>();
        long hcChar = 0;
        final String[] chainKeys = collidingKeys(COLLISION_CHAIN_N);
        for (int i = 0; i < chainKeys.length; ++i)
        {
            hashCollisionChain.add(chainKeys[i]);
            hcChar += codeUnitSum(chainKeys[i]);
        }
        hashCollisionChainCharSum = hcChar;
        collisionChainIsPlainChain = isPlainMultiNodeChain(hashCollisionChain);

        // HashSet with the legal single null element + NULL_SET_NONNULL reals.
        hashWithNull = new HashSet<Elem>();
        hashWithNull.add(null);
        for (int i = 0; i < NULL_SET_NONNULL; ++i)
        {
            hashWithNull.add(new Elem(100 + i));
        }

        linkedTwo = new LinkedHashSet<Elem>();
        linkedTwo.add(new Elem(0));
        linkedTwo.add(new Elem(1));
        linkedTwoIdSum = 0 + 1;

        linkedSmall = new LinkedHashSet<Elem>();
        for (int i = 0; i < SMALL_N; ++i)
        {
            linkedSmall.add(new Elem(i));
        }

        linkedMany = new LinkedHashSet<Elem>();
        long lmId = 0, lmXor = 0;
        for (int i = 0; i < MANY_N; ++i)
        {
            linkedMany.add(new Elem(i));
            lmId += i;
            lmXor ^= i;
        }
        linkedManyIdSum = lmId;
        linkedManyIdXor = lmXor;

        treeEmpty = new TreeSet<Elem>();

        treeSingle = new TreeSet<Elem>();
        treeSingle.add(new Elem(0));

        treeTwo = new TreeSet<Elem>();
        treeTwo.add(new Elem(1));   // insert out of order: in-order walk re-sorts
        treeTwo.add(new Elem(0));

        treeSmall = new TreeSet<Elem>();
        for (int i = 0; i < SMALL_N; ++i)
        {
            treeSmall.add(new Elem(i));
        }

        // Reverse-comparator TreeSet: insert ascending, but the comparator orders
        // descending, so the in-order red-black walk must come out [N-1 .. 0].
        treeReverse = new TreeSet<Elem>(java.util.Collections.<Elem>reverseOrder());
        for (int i = 0; i < SMALL_N; ++i)
        {
            treeReverse.add(new Elem(i));
        }

        treeMany = new TreeSet<Elem>();
        long tmId = 0;
        for (int i = 0; i < TREE_MANY_N; ++i)
        {
            treeMany.add(new Elem(i));
            tmId += i;
        }
        treeManyIdSum = tmId;

        // TreeSet<String>: natural lexicographic order; firstKey "a", etc.
        treeStrings = new TreeSet<String>();
        treeStrings.add("banana");
        treeStrings.add("apple");
        treeStrings.add("cherry");

        setFromHashMap = Collections.newSetFromMap(new HashMap<Elem, Boolean>());
        for (int i = 0; i < SETFROMMAP_N; ++i)
        {
            setFromHashMap.add(new Elem(200 + i));
        }

        // JDK Collections Set wrappers (re-created so they wrap the freshly built
        // hashTwo).  emptySet decodes correctly (size 0); singleton/unmodifiable/
        // synchronized have no fast-path field shape and decode EMPTY through the
        // List-only get(int) fallback — characterized natively, never fixed here.
        emptySet = java.util.Collections.emptySet();
        singletonSet = java.util.Collections.singleton(new Elem(300));
        unmodifiableSet = java.util.Collections.unmodifiableSet(hashTwo);
        synchronizedSet = java.util.Collections.synchronizedSet(hashTwo);

        nullSet = null;

        hashEmptySize = hashEmpty.size();
        hashSingleSize = hashSingle.size();
        hashTwoSize = hashTwo.size();
        hashManySize = hashMany.size();
        hashBigSize = hashBig.size();
        hashDupSize = hashDup.size();
        hashStringsSize = hashStrings.size();
        hashTreeifiedSize = hashTreeified.size();
        hashCollisionChainSize = hashCollisionChain.size();
        hashWithNullSize = hashWithNull.size();
        linkedTwoSize = linkedTwo.size();
        linkedSmallSize = linkedSmall.size();
        linkedManySize = linkedMany.size();
        treeEmptySize = treeEmpty.size();
        treeSingleSize = treeSingle.size();
        treeTwoSize = treeTwo.size();
        treeSmallSize = treeSmall.size();
        treeManySize = treeMany.size();
        treeReverseSize = treeReverse.size();
        treeStringsSize = treeStrings.size();
        setFromHashMapSize = setFromHashMap.size();
        emptySetSize = emptySet.size();
        singletonSetSize = singletonSet.size();
        unmodifiableSetSize = unmodifiableSet.size();
        synchronizedSetSize = synchronizedSet.size();
    }

    /**
     * Generates n strings that all share the SAME String.hashCode by chaining
     * the classic "Aa"/"BB" collision (both hash to 2112).  Concatenating any
     * sequence of these equal-length 2-char blocks preserves the equal-hashCode
     * property, so all n keys collide into one bucket and the bin treeifies once
     * it exceeds 8 entries.
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
     * Reflectively inspects the HashSet's backing HashMap.table to report
     * whether any bucket head is a TreeNode (i.e. the bin treeified).  Returns
     * false if reflection is blocked — the native check tolerates that.
     */
    private static boolean hasTreeNodeBin(final HashSet<String> set)
    {
        try
        {
            final java.lang.reflect.Field mapField = HashSet.class.getDeclaredField("map");
            mapField.setAccessible(true);
            final Object map = mapField.get(set);
            if (map == null)
            {
                return false;
            }
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

    /**
     * Reflectively confirms the HashSet's backing HashMap has at least one bucket
     * that is a PLAIN multi-node Node.next chain (length &gt; 1) and that NO bucket
     * head is a TreeNode — i.e. a genuine singly-linked collision chain that
     * stayed below the treeify threshold.  Returns false if reflection is blocked
     * (the native check tolerates that and records it [INFO]).
     */
    private static boolean isPlainMultiNodeChain(final HashSet<String> set)
    {
        try
        {
            final java.lang.reflect.Field mapField = HashSet.class.getDeclaredField("map");
            mapField.setAccessible(true);
            final Object map = mapField.get(set);
            if (map == null)
            {
                return false;
            }
            final java.lang.reflect.Field tableField = HashMap.class.getDeclaredField("table");
            tableField.setAccessible(true);
            final Object table = tableField.get(map);
            if (table == null)
            {
                return false;
            }
            // Node.next is declared on java.util.HashMap$Node.
            final Class<?> nodeClass = Class.forName("java.util.HashMap$Node");
            final java.lang.reflect.Field nextField = nodeClass.getDeclaredField("next");
            nextField.setAccessible(true);

            final int len = java.lang.reflect.Array.getLength(table);
            boolean sawMultiNode = false;
            for (int i = 0; i < len; ++i)
            {
                Object head = java.lang.reflect.Array.get(table, i);
                if (head == null)
                {
                    continue;
                }
                if (head.getClass().getSimpleName().equals("TreeNode"))
                {
                    // Any TreeNode means the bin treeified — not a plain chain.
                    return false;
                }
                int chainLen = 0;
                Object node = head;
                while (node != null && chainLen < (1 << 20))
                {
                    ++chainLen;
                    node = nextField.get(node);
                }
                if (chainLen > 1)
                {
                    sawMultiNode = true;
                }
            }
            return sawMultiNode;
        }
        catch (final Throwable t)
        {
            return false;
        }
    }

    /**
     * Forces vmhook.fixtures.CollSet$Elem to be LOADED at CollSet class-init time
     * (when Main.loadFixtures() Class.forName's CollSet at JVM startup).  Without
     * this, Elem would not load until the first new Elem(...) inside buildAll() —
     * which runs in the same static block, but the native register_class for the
     * element must find the klass already present in the loaded graph; pinning
     * one instance here guarantees it.  This instance is never put into any set,
     * so it perturbs no size/membership assertion.
     */
    private static final Elem ELEM_CLASS_PIN = new Elem(-999);

    static
    {
        if (ELEM_CLASS_PIN == null)
        {
            throw new IllegalStateException("unreachable");
        }

        // Build once at class-init so the sets are populated even before the
        // first probe (the native module also re-requests a build via mode 0).
        buildAll();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return CollSet.go && !CollSet.done;
            }

            @Override
            public void run()
            {
                if (CollSet.mode == 1)
                {
                    final CollSet instance = new CollSet();
                    CollSet.observed = instance.touch(42);
                }
                else
                {
                    // mode 0: (re)build all sets so the native reads see a fresh,
                    // deterministic population on this exact thread.
                    buildAll();
                }
                CollSet.done = true;
            }
        });
    }
}
