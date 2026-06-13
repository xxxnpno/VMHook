package vmhook.fixtures;

import vmhook.Harness;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.TreeSet;

/**
 * Companion fixture for the collection_set feature (area: collections).
 *
 * The primary fixture, {@link CollSet}, already covers the bulk of the Set
 * decode matrix (HashSet / LinkedHashSet / TreeSet across empty / single / two /
 * many / big / treeified / collision-chain / null-element shapes, the
 * setFromMap and ConcurrentHashMap.newKeySet routes, and boxed-Integer
 * elements).  This SECOND, DISTINCT fixture fills the remaining gaps the
 * "every java.util.Set shape read through the library" goal calls for, WITHOUT
 * touching the mature CollSet.java / collection_set.cpp pair:
 *
 *   - BOXED Long element decode (HashSet&lt;Long&gt; / TreeSet&lt;Long&gt;) — the
 *     java.lang.Long.value field is a 64-bit primitive, a different read width
 *     from Integer.value; both the bucket walk and the in-order red-black walk
 *     must surface every boxed Long, and the TreeSet must come out ASCENDING.
 *
 *   - REAL enum-element decode (HashSet&lt;Day&gt; / TreeSet&lt;Day&gt;).  CollSet only
 *     CHARACTERIZES EnumSet (whose primitive-bitmask storage has no fast-path
 *     field shape); here a HashSet/TreeSet OF enum constants is genuinely
 *     DECODED — each element OOP is a real enum instance whose `name` (String)
 *     and `ordinal` (int) the native side reads back.  TreeSet&lt;Day&gt; orders by
 *     the enum's natural order (ordinal), so the in-order walk is ascending by
 *     ordinal.
 *
 *   - Exact HashSet RESIZE-BOUNDARY sizes: 16 (the default table capacity, where
 *     the 13th add triggers the first resize to 32) and 17 (one past it), so the
 *     bucket walk is proven across the precise grow threshold, plus a 1000-element
 *     HashSet (a mid-scale many-bucket walk between CollSet's 50 and 5000).
 *
 *   - Set&lt;List&lt;Integer&gt;&gt;: a HashSet whose ELEMENTS are themselves
 *     ArrayList&lt;Integer&gt;.  The outer "map"→hash_map_walk_keys walk yields the
 *     inner List OOPs; the native side then decodes EACH inner List via the
 *     ArrayList fast path (a pure memory walk, no Java call) and verifies its
 *     contents — proving a Set can hold another live collection and both layers
 *     decode from the worker body.
 *
 * Verification is order-independent for the (unordered) HashSets — count +
 * membership + order-independent checksums cross-checked against values Java
 * computed the same way — and STRICT ascending order for the TreeSets, whose
 * in-order walk order is defined.  Every populated set also publishes its own
 * size() for the count==size oracle.
 *
 * Mirrors CollSet's go/done/mode handshake and its hookable touch(int) so the
 * companion native module can also prove an interpreter hook fires through the
 * modular path.  Java 8 syntax only (no var/records/switch-expr/text-blocks).
 */
public final class CollSetExhaustive
{
    /** Native sets this true to request the action; the action clears it. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect of the hookable method (pilot-style proof). */
    public static volatile int observed;

    /**
     * Scenario selector (set by the native module BEFORE raising `go`):
     *   0 = build/refresh every set field (default; also runs once at startup)
     *   1 = call touch() so the interpreter hook fires on real dispatch
     */
    public static volatile int mode;

    /**
     * Element type wrapped by the native side for the user-class scenarios.
     * `id` drives the order-independent checksums AND (via Comparable) the
     * TreeSet ordering; `tag` is "e&lt;id&gt;" for the reference-field readback.
     * equals/hashCode are by id, consistent with compareTo.
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

    /**
     * A small enum used for the REAL enum-element decode.  A Java enum constant
     * is an ordinary heap object with a String `name` and an int `ordinal`
     * (both declared on java.lang.Enum), which the native side reads back from
     * each decoded element OOP.  TreeSet&lt;Day&gt; uses the natural enum order
     * (by ordinal), so the in-order walk is ascending by ordinal.
     */
    public enum Day
    {
        MON, TUE, WED, THU, FRI
    }

    // ---- Scenario sizes (mirrored on the native side) ----------------------

    /** Boxed-Long / enum / nested-List element counts. */
    public static final int LONG_N = 40;
    public static final int NESTED_N = 6;     // number of inner Lists in setOfLists
    public static final int INNER_LEN = 4;    // length of each inner List

    /**
     * The default HashMap table capacity.  A HashSet built with the no-arg ctor
     * starts at capacity 16; the resize fires when size exceeds 16 * 0.75 = 12
     * (i.e. the 13th element), so a 16-element set has ALREADY resized to 32 and
     * a 17-element set sits just past the boundary — both exercise the bucket
     * walk across the precise grow threshold.
     */
    public static final int CAP16 = 16;
    public static final int CAP17 = 17;

    /** A mid-scale many-bucket HashSet (between CollSet's 50 and 5000). */
    public static final int THOUSAND = 1000;

    /** Number of Day constants (== Day.values().length). */
    public static final int DAY_N = 5;

    // ---- The Set fields the native module reads via to_vector --------------

    /** HashSet&lt;Long&gt; — boxed 64-bit element decode (bucket walk). */
    public static HashSet<Long> hashLongs = new HashSet<Long>();

    /** TreeSet&lt;Long&gt; — boxed 64-bit element, ascending in-order walk. */
    public static TreeSet<Long> treeLongs = new TreeSet<Long>();

    /** HashSet&lt;Day&gt; — REAL enum-element decode (bucket walk). */
    public static HashSet<Day> hashEnums = new HashSet<Day>();

    /** TreeSet&lt;Day&gt; — enum natural order (by ordinal), ascending in-order. */
    public static TreeSet<Day> treeEnums = new TreeSet<Day>();

    /** HashSet of exactly CAP16 Elem (table resized to 32; at the boundary). */
    public static HashSet<Elem> hashCap16 = new HashSet<Elem>();

    /** HashSet of exactly CAP17 Elem (one past the resize boundary). */
    public static HashSet<Elem> hashCap17 = new HashSet<Elem>();

    /** HashSet of THOUSAND Elem (mid-scale many-bucket walk). */
    public static HashSet<Elem> hashThousand = new HashSet<Elem>();

    /**
     * HashSet whose ELEMENTS are ArrayList&lt;Integer&gt; — a Set containing other
     * collections.  Inner list k holds [k*10 + 0 .. k*10 + INNER_LEN-1].  The
     * outer bucket walk yields the inner List OOPs; the native side decodes each
     * inner List via the ArrayList fast path and verifies its contents (the total
     * value sum across all of them), cross-checked against an order-independent
     * aggregate.
     */
    public static HashSet<List<Integer>> setOfLists = new HashSet<List<Integer>>();

    /** A declared Set field deliberately left NULL (to_vector must stay empty). */
    public static HashSet<Elem> nullSet = null;

    // ---- Published cross-check values (order-independent + size oracle) -----

    public static volatile long hashLongsValSum;
    public static volatile long hashLongsValXor;
    public static volatile long treeLongsValSum;

    public static volatile long hashEnumsOrdinalSum;   // sum of ordinals
    public static volatile long hashEnumsNameCharSum;  // sum of name code units

    public static volatile long hashCap16IdSum;
    public static volatile long hashCap17IdSum;
    public static volatile long hashThousandIdSum;
    public static volatile long hashThousandIdXor;

    public static volatile long setOfListsValSum;      // sum over ALL inner ints

    /** Java's own size() for the count==size oracle. */
    public static volatile int hashLongsSize;
    public static volatile int treeLongsSize;
    public static volatile int hashEnumsSize;
    public static volatile int treeEnumsSize;
    public static volatile int hashCap16Size;
    public static volatile int hashCap17Size;
    public static volatile int hashThousandSize;
    public static volatile int setOfListsSize;

    // ---- Hookable method (pilot-style interpreter-hook proof) --------------

    private int seed = 7000;

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
        // HashSet<Long> / TreeSet<Long>: boxed 64-bit element decode.  Use values
        // offset well beyond int range on the Long set so a 32-bit misread of the
        // 64-bit Long.value would corrupt the checksum (the high word matters).
        hashLongs = new HashSet<Long>();
        long hlSum = 0, hlXor = 0;
        for (int i = 0; i < LONG_N; ++i)
        {
            final long v = 0x1_0000_0000L + i;   // forces a non-zero high 32 bits
            hashLongs.add(Long.valueOf(v));
            hlSum += v;
            hlXor ^= v;
        }
        hashLongsValSum = hlSum;
        hashLongsValXor = hlXor;

        treeLongs = new TreeSet<Long>();
        long tlSum = 0;
        for (int i = LONG_N - 1; i >= 0; --i)   // insert descending; walk re-sorts
        {
            final long v = 0x1_0000_0000L + i;
            treeLongs.add(Long.valueOf(v));
            tlSum += v;
        }
        treeLongsValSum = tlSum;

        // HashSet<Day> / TreeSet<Day>: REAL enum-element decode.
        hashEnums = new HashSet<Day>();
        long heOrd = 0, heChar = 0;
        for (final Day d : Day.values())
        {
            hashEnums.add(d);
            heOrd += d.ordinal();
            heChar += codeUnitSum(d.name());
        }
        hashEnumsOrdinalSum = heOrd;
        hashEnumsNameCharSum = heChar;

        treeEnums = new TreeSet<Day>();
        // Insert in a scrambled order; the in-order walk must re-sort by ordinal.
        treeEnums.add(Day.WED);
        treeEnums.add(Day.MON);
        treeEnums.add(Day.FRI);
        treeEnums.add(Day.TUE);
        treeEnums.add(Day.THU);

        // Exact resize-boundary HashSets.
        hashCap16 = new HashSet<Elem>();
        long c16 = 0;
        for (int i = 0; i < CAP16; ++i)
        {
            hashCap16.add(new Elem(i));
            c16 += i;
        }
        hashCap16IdSum = c16;

        hashCap17 = new HashSet<Elem>();
        long c17 = 0;
        for (int i = 0; i < CAP17; ++i)
        {
            hashCap17.add(new Elem(i));
            c17 += i;
        }
        hashCap17IdSum = c17;

        hashThousand = new HashSet<Elem>();
        long ktId = 0, ktXor = 0;
        for (int i = 0; i < THOUSAND; ++i)
        {
            hashThousand.add(new Elem(i));
            ktId += i;
            ktXor ^= i;
        }
        hashThousandIdSum = ktId;
        hashThousandIdXor = ktXor;

        // Set<List<Integer>>: a Set holding other live collections.  Inner list k
        // holds [k*10 .. k*10 + INNER_LEN-1]; the native side decodes each inner
        // ArrayList and verifies the total value sum across all of them.
        setOfLists = new HashSet<List<Integer>>();
        long solSum = 0;
        for (int k = 0; k < NESTED_N; ++k)
        {
            final ArrayList<Integer> inner = new ArrayList<Integer>();
            for (int j = 0; j < INNER_LEN; ++j)
            {
                final int v = k * 10 + j;
                inner.add(Integer.valueOf(v));
                solSum += v;
            }
            setOfLists.add(inner);
        }
        setOfListsValSum = solSum;

        nullSet = null;

        hashLongsSize = hashLongs.size();
        treeLongsSize = treeLongs.size();
        hashEnumsSize = hashEnums.size();
        treeEnumsSize = treeEnums.size();
        hashCap16Size = hashCap16.size();
        hashCap17Size = hashCap17.size();
        hashThousandSize = hashThousand.size();
        setOfListsSize = setOfLists.size();
    }

    /**
     * Forces vmhook.fixtures.CollSetExhaustive$Elem to be LOADED at class-init
     * time (when Main.loadFixtures() Class.forName's CollSetExhaustive at JVM
     * startup), so the native register_class for the element finds the klass
     * already present.  Never put into any set, so it perturbs no assertion.
     */
    private static final Elem ELEM_CLASS_PIN = new Elem(-999);

    /** Likewise pin the enum class so register_class for Day resolves. */
    private static final Day DAY_CLASS_PIN = Day.MON;

    static
    {
        if (ELEM_CLASS_PIN == null || DAY_CLASS_PIN == null)
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
                return CollSetExhaustive.go && !CollSetExhaustive.done;
            }

            @Override
            public void run()
            {
                if (CollSetExhaustive.mode == 1)
                {
                    final CollSetExhaustive instance = new CollSetExhaustive();
                    CollSetExhaustive.observed = instance.touch(42);
                }
                else
                {
                    // mode 0: (re)build all sets so native reads see a fresh,
                    // deterministic population on this exact thread.
                    buildAll();
                }
                CollSetExhaustive.done = true;
            }
        });
    }
}
