package vmhook.fixtures;

import vmhook.Harness;

import java.util.Collections;
import java.util.EnumMap;
import java.util.HashMap;
import java.util.Hashtable;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.SortedMap;
import java.util.TreeMap;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Fixture for the collection_map feature (area: collections).
 *
 * Exercises field_proxy::value_t::to_entries&lt;K,V&gt;() / vmhook::map::to_entries
 * and the two underlying walkers, on a live JVM, across every container shape and
 * boundary the audit flagged:
 *
 *   - HashMap&lt;String,Box&gt;          empty / one / two / small / MANY (forces
 *                                    resize + a treeified bin) / one-null-key /
 *                                    one-null-value / empty-string key+value.
 *   - LinkedHashMap&lt;String,Box&gt;    small + MANY (the HashMap "table" fast path
 *                                    is taken, so iteration is BUCKET order, NOT
 *                                    insertion order — pinned as a known quirk).
 *   - TreeMap&lt;String,Box&gt;          empty / one / two / small / MANY (red-black
 *                                    in-order walk; in-order == sorted key order),
 *                                    plus a DESCENDING-inserted tree (proves the
 *                                    in-order walk re-sorts) and a null-VALUE tree
 *                                    (TreeMap permits null values, never null keys).
 *   - Collections.* views            emptyMap() / singletonMap() /
 *                                    unmodifiableMap(HashMap) /
 *                                    unmodifiableSortedMap(TreeMap): NONE expose a
 *                                    "table"/"root" field on their own klass, so
 *                                    to_entries returns EMPTY for every one of them
 *                                    (a faithful, characterized contract — the
 *                                    unmodifiable wrappers do NOT see through to the
 *                                    map they wrap).
 *   - a Map field left NULL          and a field that does not exist at all
 *                                    (to_entries must return empty, never throw).
 *
 * The VALUE type is the nested {@code CollMap.Box} (compiled to
 * {@code vmhook/fixtures/CollMap$Box}); its {@code $} keeps the Harness fixture
 * loader from trying to Class.forName it as a probe.  The KEY type is plain
 * java.lang.String — the native key wrapper decodes the String OOP directly via
 * vmhook::read_java_string.
 *
 * Canonical go/done handshake with a `mode` selector (mirrors HookBasic): the
 * native module sets `mode`, clears `done`, raises `go`; the Harness loop runs
 * the matching scenario on the Java thread; the module polls `done`.  Because
 * the maps are plain object fields, the native side reads them directly without
 * needing a hooked dispatch — but a hookable touch() is provided so the module
 * still proves an interpreter hook fires through the modular path, exactly like
 * the pilot.
 */
public final class CollMap
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect of the hookable method (pilot-style proof). */
    public static volatile int observed;

    /**
     * Scenario selector.  The native module sets this BEFORE raising `go`.
     *   0  = build/refresh every map field (default; runs once at startup too)
     *   1  = call touch() so the interpreter hook fires on real dispatch
     */
    public static volatile int mode;

    /**
     * Immutable value object stored in every map.  Two fields so the native
     * value wrapper can verify BOTH a primitive (id) and a reference (name)
     * round-trip through the decoded value OOP.
     */
    public static final class Box
    {
        public final int id;
        public final String name;

        public Box(final int id, final String name)
        {
            this.id = id;
            this.name = name;
        }
    }

    // ---- The map fields the native module reads via to_entries -------------

    /** Empty HashMap (size 0). */
    public static HashMap<String, Box> hashEmpty = new HashMap<String, Box>();

    /** Single-entry HashMap (size 1, a normal non-empty key — the minimal populated bucket walk). */
    public static HashMap<String, Box> hashOne = new HashMap<String, Box>();

    /** Two-entry HashMap (size 2 — the smallest map that can place entries in two buckets). */
    public static HashMap<String, Box> hashTwo = new HashMap<String, Box>();

    /** Small HashMap: exactly SMALL_N deterministic entries. */
    public static HashMap<String, Box> hashSmall = new HashMap<String, Box>();

    /** Large HashMap: MANY_N entries — forces several table resizes. */
    public static HashMap<String, Box> hashMany = new HashMap<String, Box>();

    /**
     * HashMap with a SINGLE null key (legal: HashMap permits one null key,
     * stored in bucket 0).  Holds NULL_KEY_N entries, one of which has a null key.
     */
    public static HashMap<String, Box> hashNullKey = new HashMap<String, Box>();

    /** HashMap where exactly one entry has a non-null key but a NULL value. */
    public static HashMap<String, Box> hashNullValue = new HashMap<String, Box>();

    /**
     * HashMap whose keys/values are the EMPTY string "" and a Box whose name is
     * "" — exercises the read_java_string length&lt;=0 → "" boundary on both ends.
     */
    public static HashMap<String, Box> hashEmptyStr = new HashMap<String, Box>();

    /**
     * HashMap engineered so at least one bucket TREEIFIES: &gt;=8 keys that all
     * hash to the same bucket at the current capacity.  Holds TREEIFY_N entries.
     */
    public static HashMap<String, Box> hashTreeified = new HashMap<String, Box>();

    /** Small LinkedHashMap (insertion-ordered in Java; bucket-ordered via vmhook). */
    public static LinkedHashMap<String, Box> linkedSmall = new LinkedHashMap<String, Box>();

    /** Large LinkedHashMap. */
    public static LinkedHashMap<String, Box> linkedMany = new LinkedHashMap<String, Box>();

    /** Empty TreeMap. */
    public static TreeMap<String, Box> treeEmpty = new TreeMap<String, Box>();

    /** Single-entry TreeMap (size 1 — the minimal red-black root with no children). */
    public static TreeMap<String, Box> treeOne = new TreeMap<String, Box>();

    /** Two-entry TreeMap (size 2 — a root + one child; in-order yields the sorted pair). */
    public static TreeMap<String, Box> treeTwo = new TreeMap<String, Box>();

    /** Small TreeMap: SMALL_N entries; in-order walk == sorted key order. */
    public static TreeMap<String, Box> treeSmall = new TreeMap<String, Box>();

    /** Large TreeMap: MANY_N entries; deep red-black tree. */
    public static TreeMap<String, Box> treeMany = new TreeMap<String, Box>();

    /**
     * TreeMap whose keys are inserted in STRICT DESCENDING order ("k(N-1)" first,
     * "k0" last).  A correct red-black in-order walk must STILL emit them in
     * ascending order, so this proves the native walk re-sorts and is not merely
     * echoing insertion order.  Holds SMALL_N entries.
     */
    public static TreeMap<String, Box> treeReverseInsert = new TreeMap<String, Box>();

    /**
     * TreeMap with a null VALUE under a non-null key.  TreeMap permits null
     * values (it forbids only null keys), so this exercises the tree walk
     * surfacing a nullptr value while keeping the key — the TreeMap analogue of
     * hashNullValue.  Holds 2 entries, one of which has a null value.
     */
    public static TreeMap<String, Box> treeNullValue = new TreeMap<String, Box>();

    // ---- Collections.* views: NONE expose a "table"/"root" field of their own,
    //      so to_entries returns EMPTY for each (characterized contract). --------

    /** Collections.emptyMap() — an EmptyMap singleton with no backing table/root. */
    public static Map<String, Box> emptyMapColl = Collections.emptyMap();

    /** Collections.singletonMap(k,v) — a SingletonMap (fields k,v, not table/root). */
    public static Map<String, Box> singletonMapColl =
        Collections.singletonMap("only", new Box(42, "only-value"));

    /** Collections.unmodifiableMap(hashSmall) — an UnmodifiableMap wrapping (field m). */
    public static Map<String, Box> unmodifiableHash = Collections.unmodifiableMap(hashSmall);

    /** Collections.unmodifiableSortedMap(treeSmall) — an UnmodifiableSortedMap wrapper. */
    public static SortedMap<String, Box> unmodifiableTree =
        Collections.unmodifiableSortedMap(treeSmall);

    /** A declared Map field that is deliberately left NULL. */
    public static Map<String, Box> nullMap = null;

    /** A plain (non-Map) reference field — to_entries on it must stay empty. */
    public static String notAMap = "not a map";

    // ---- POSITIVELY-DECODING extra container families ----------------------
    //      These expose a Node layout the native walkers DO decode.

    /**
     * java.util.Hashtable: its "table" is an Entry[] whose Entry exposes the
     * SAME key/value/next fields as HashMap.Node, so the HashMap "table" fast
     * path decodes it FULLY (a distinct, positively-decoding container family —
     * not just HashMap/LinkedHashMap).  SMALL_N deterministic entries.
     */
    public static Hashtable<String, Box> hashtableSmall = new Hashtable<String, Box>();

    /**
     * HashMap with COLL6_N colliding-hashCode keys ("Aa"/"BB" family) that all
     * land in ONE bucket but stay BELOW the treeify threshold (8), so the bucket
     * head is a plain Node and the bucket is a linked Node.next chain — NOT a
     * TreeNode.  Proves the next-chain walk on an ordinary (un-treeified) chain,
     * the case between "one entry per bucket" and "treeified bin".
     */
    public static HashMap<String, Box> hashColl6 = new HashMap<String, Box>();

    /**
     * HashMap&lt;Integer,Integer&gt; whose BOXED keys 0,16,32,48 all collide into
     * bucket 0 of the default 16-slot table (Integer.hashCode()==value, spread
     * leaves the low 4 bits 0).  Proves a boxed-primitive key AND value decode
     * (java.lang.Integer.value) plus a collision chain with non-String keys.
     */
    public static HashMap<Integer, Integer> hashIntKey = new HashMap<Integer, Integer>();

    // ---- Key/value TYPE-COVERAGE maps (every one decodes POSITIVELY) -------
    //      The walkers are generic over the key/value wrapper, so any boxed
    //      reference key/value decodes; these pin the remaining type pairs the
    //      audit called out beyond String->Box and Integer->Integer.

    /**
     * HashMap&lt;String,String&gt; — both key AND value are java.lang.String, so
     * the value side is decoded via read_java_string just like the key (not via
     * a Box).  SMALL_N entries "sk"+i -&gt; "sv"+i.
     */
    public static HashMap<String, String> hashStrStr = new HashMap<String, String>();

    /**
     * HashMap&lt;Integer,String&gt; — boxed Integer KEY, String VALUE.  Proves a
     * boxed-primitive key paired with a String value (a different value type
     * from hashIntKey's boxed Integer value).  SMALL_N entries i -&gt; "iv"+i.
     */
    public static HashMap<Integer, String> hashIntStr = new HashMap<Integer, String>();

    /**
     * HashMap&lt;Long,Long&gt; — boxed Long KEY and boxed Long VALUE (64-bit
     * primitive box).  Proves the java.lang.Long.value (a long, not an int)
     * round-trips on BOTH sides.  Keys are large (&gt; Integer range) so a
     * truncating read would be caught.  SMALL_N entries.
     */
    public static HashMap<Long, Long> hashLongLong = new HashMap<Long, Long>();

    /**
     * HashMap whose KEYS are enum constants (CollMap.Day) and values are Box.
     * Unlike EnumMap (a parallel vals[] with no Node), an ORDINARY HashMap keyed
     * by an enum stores real Node objects, so it decodes POSITIVELY: each key OOP
     * is a Day constant whose java.lang.Enum.name field the native side reads.
     * One entry per Day constant (3).
     */
    public static HashMap<Day, Box> hashEnumKey = new HashMap<Day, Box>();

    /**
     * HashMap at the DEFAULT-CAPACITY RESIZE BOUNDARY.  16 entries: with the
     * default capacity 16 and load factor 0.75 the threshold is 12, so inserting
     * the 13th entry already forced ONE resize to capacity 32.  Pins that the
     * bucket walk visits every entry across a table that has been resized exactly
     * at/after the boundary.  Keys "k"+i, values Box(i,"v"+i).
     */
    public static HashMap<String, Box> hashResize16 = new HashMap<String, Box>();

    /**
     * HashMap with 17 entries — one past the size-16 case, still in the capacity
     * 32 table (next resize threshold is 24).  A companion to hashResize16 so the
     * boundary is bracketed on both sides (16 and 17).  Keys "k"+i.
     */
    public static HashMap<String, Box> hashResize17 = new HashMap<String, Box>();

    /**
     * HashMap whose VALUES are themselves Maps (a nested HashMap&lt;String,Box&gt;
     * per entry).  The outer walk decodes each value OOP; the native side then
     * re-wraps that value OOP as a vmhook::map and decodes the INNER entries,
     * proving nested-Map values round-trip.  NESTED_N outer entries.
     */
    public static HashMap<String, Map<String, Box>> hashNestedMap =
        new HashMap<String, Map<String, Box>>();

    /**
     * HashMap whose VALUES are Lists (a nested java.util.List per entry).  The
     * outer walk decodes each value OOP; the native side re-wraps it as a
     * vmhook::collection and decodes the inner elements, proving nested-List
     * values round-trip.  NESTED_N outer entries, each list holding NESTED_N ids.
     */
    public static HashMap<String, List<Box>> hashNestedList =
        new HashMap<String, List<Box>>();

    // ---- ADDED input coverage: more key/value types + boundary values ------

    /**
     * HashMap&lt;String,String&gt; with NON-ASCII keys AND values.  Exercises
     * read_java_string's NON-ASCII path: LATIN1 (coder 0, 0x80-0xFF -&gt; 2-byte
     * UTF-8) and UTF16 (coder 1, BMP code point -&gt; 3-byte UTF-8) on JDK 9+, and
     * the JDK 8 char[] path — all converge on the SAME UTF-8 output, so the
     * native side asserts exact UTF-8 string equality (a code-unit sum would NOT
     * cross-check, since C++ sees UTF-8 bytes while Java sees UTF-16 units).
     */
    public static HashMap<String, String> hashUnicode = new HashMap<String, String>();

    /**
     * HashMap&lt;Character,Character&gt; — boxed 16-bit char key AND value
     * (descriptor "C"); proves a Character.value (unsigned 16-bit) round-trips.
     */
    public static HashMap<Character, Character> hashCharKey = new HashMap<Character, Character>();

    /**
     * HashMap&lt;Short,Short&gt; — boxed signed 16-bit key AND value
     * (descriptor "S"); includes a NEGATIVE short to prove sign extension.
     */
    public static HashMap<Short, Short> hashShortKey = new HashMap<Short, Short>();

    /**
     * HashMap&lt;Byte,Byte&gt; — boxed signed 8-bit key AND value
     * (descriptor "B"); includes a NEGATIVE byte to prove sign extension.
     */
    public static HashMap<Byte, Byte> hashByteKey = new HashMap<Byte, Byte>();

    /**
     * HashMap&lt;Boolean,Box&gt; — boxed boolean key (descriptor "Z"); exactly
     * the two constants Boolean.FALSE/TRUE, proving a boolean wrapper decodes.
     */
    public static HashMap<Boolean, Box> hashBoolKey = new HashMap<Boolean, Box>();

    /**
     * HashMap&lt;String,Box&gt; with EXTREME / NEGATIVE Box ids (Integer.MIN_VALUE,
     * -1, 0, Integer.MAX_VALUE).  Proves the value's signed 32-bit "id" field is
     * read without truncation or sign error across the full int range.
     */
    public static HashMap<String, Box> hashNegIds = new HashMap<String, Box>();

    /**
     * TreeMap built with a REVERSE-ORDER comparator.  A correct in-order red-black
     * walk emits keys in the COMPARATOR's order, so this comes out DESCENDING
     * ("k2","k1","k0") — proving the walk honours the comparator, not natural
     * ordering.  SMALL_N entries; firstKey/lastKey published.
     */
    public static TreeMap<String, Box> treeReverseComparator =
        new TreeMap<String, Box>(Collections.reverseOrder());

    /**
     * TreeMap&lt;Integer,Box&gt; — NUMERIC key order (10,2,1 inserted; in-order
     * yields 1,2,10).  Distinct from lexicographic String order, proving the walk
     * emits the natural NUMERIC comparator order for boxed Integer keys.
     */
    public static TreeMap<Integer, Box> treeIntKey = new TreeMap<Integer, Box>();

    // ---- CHARACTERIZED-EMPTY extra Map shapes ------------------------------
    //      Each is a real, non-empty Java Map, but its heap layout exposes no
    //      Node the walkers can follow, so to_entries reads EMPTY.  The Java
    //      size() witness (published below) proves the map is non-empty Java-side,
    //      so these are faithful CHARACTERIZED pins, not vacuous.

    /**
     * ConcurrentHashMap: HAS a "table" field (so the HashMap fast path is
     * selected), but its Node names the value field "val", NOT "value", so the
     * walker's find_field(node,"value") misses and the bucket bails — to_entries
     * reads EMPTY.  Small + MANY both characterized.
     */
    public static ConcurrentHashMap<String, Box> chmSmall = new ConcurrentHashMap<String, Box>();
    public static ConcurrentHashMap<String, Box> chmMany  = new ConcurrentHashMap<String, Box>();

    /**
     * WeakHashMap: HAS a "table" field, but its Entry holds the KEY as the
     * WeakReference referent (no "key" instance field), so find_field(node,"key")
     * misses and the bucket bails — to_entries reads EMPTY.  Keys are held by a
     * strong reference (keyHolder) so they cannot be GC-cleared mid-test.
     */
    public static WeakHashMap<String, Box> weakSmall = new WeakHashMap<String, Box>();

    /**
     * IdentityHashMap: its "table" is a FLAT Object[] of alternating key,value
     * (no Node objects), so a bucket element is a bare String/Box, not a Node —
     * find_field(element,"key") misses and to_entries reads EMPTY.
     */
    public static IdentityHashMap<String, Box> identitySmall = new IdentityHashMap<String, Box>();

    /**
     * EnumMap: stores its values in a parallel "vals" Object[] keyed by ordinal,
     * exposing NEITHER a "table" NOR a "root" field, so the dispatcher finds no
     * fast path and to_entries reads EMPTY.
     */
    public static EnumMap<Day, Box> enumSmall = new EnumMap<Day, Box>(Day.class);

    /** A small enum used as the EnumMap key type. */
    public enum Day { MON, TUE, WED }

    /**
     * Map.of(...) immutable maps (JDK 9+).  Built REFLECTIVELY so this fixture
     * still COMPILES on JDK 8 (where Map.of is absent — the fields then stay
     * null and decode empty too, which is still the characterized contract).
     *   mapOfN : 3-entry MapN — "table" is an Object[] of interleaved k/v (not a
     *            Node[]), so the hash fast path is selected but every slot is a
     *            bare String/Box → EMPTY.
     *   mapOf1 : 1-entry Map1 — fields k0/v0, no "table"/"root" → EMPTY.
     */
    public static Map<String, Box> mapOfN;
    public static Map<String, Box> mapOf1;

    // ---- Scenario sizes (mirrored on the native side) ----------------------

    public static final int SMALL_N = 3;
    public static final int MANY_N = 1000;
    public static final int NULL_KEY_N = 3;     // includes the one null-key entry
    public static final int TREEIFY_N = 12;     // > 8 => bucket treeifies
    public static final int COLL6_N = 6;        // colliding but < 8 => plain Node chain
    public static final int INTKEY_N = 4;       // boxed Integer keys 0,16,32,48 (one bucket)
    public static final int NESTED_N = 2;       // outer entries / inner list size
    public static final int RESIZE16_N = 16;    // == default load threshold-after-one-resize boundary
    public static final int RESIZE17_N = 17;    // one past the size-16 boundary

    // ---- Published checksums so native can verify content without ordering --
    //
    // For the unordered maps the native walker visits entries in bucket order,
    // which is not the Java insertion order, so the module verifies CONTENT via
    // order-independent aggregates that Java computes the same way.  Each key is
    // "k" + i and each value is new Box(i, "v" + i), so:
    //   keyCharSum   = sum over entries of (sum of UTF-16 code units of the key)
    //   idSum        = sum over entries of value.id
    //   idXor        = xor over entries of value.id
    // These are published per-map so the native side can cross-check exactly.

    public static volatile long hashSmallKeyCharSum;
    public static volatile long hashSmallIdSum;
    public static volatile long hashSmallIdXor;

    public static volatile long treeReverseIdSum;

    public static volatile long hashManyKeyCharSum;
    public static volatile long hashManyIdSum;
    public static volatile long hashManyIdXor;

    public static volatile long treeSmallIdSum;
    public static volatile long treeManyIdSum;

    // Positively-decoding extra families.
    public static volatile long hashtableSmallKeyCharSum;
    public static volatile long hashtableSmallIdSum;
    public static volatile long hashtableSmallIdXor;

    public static volatile long hashColl6KeyCharSum;
    public static volatile long hashColl6IdSum;
    public static volatile long hashColl6IdXor;

    /** hashIntKey: sum/xor of the boxed Integer KEYS and the boxed Integer VALUES. */
    public static volatile long hashIntKeyKeySum;
    public static volatile long hashIntKeyValSum;
    public static volatile long hashIntKeyKeyXor;

    // Type-coverage maps: order-independent content fingerprints.
    /** hashStrStr: sum of UTF-16 code units across keys, and across values. */
    public static volatile long hashStrStrKeyCharSum;
    public static volatile long hashStrStrValCharSum;
    /** hashIntStr: sum of boxed-Integer keys, and code-unit sum of String values. */
    public static volatile long hashIntStrKeySum;
    public static volatile long hashIntStrValCharSum;
    /** hashLongLong: sum/xor of the boxed Long KEYS and the boxed Long VALUES. */
    public static volatile long hashLongLongKeySum;
    public static volatile long hashLongLongValSum;
    public static volatile long hashLongLongKeyXor;
    /** hashEnumKey: sum of value.id, and code-unit sum of the enum-constant names. */
    public static volatile long hashEnumKeyIdSum;
    public static volatile long hashEnumKeyNameCharSum;
    /** hashResize16 / hashResize17: sum of value.id (closed-form cross-check). */
    public static volatile long hashResize16IdSum;
    public static volatile long hashResize17IdSum;

    /** Java's own view of each map's size(), for a native cross-check. */
    public static volatile int hashEmptySize;
    public static volatile int hashOneSize;
    public static volatile int hashTwoSize;
    public static volatile int hashSmallSize;
    public static volatile int hashManySize;
    public static volatile int hashNullKeySize;
    public static volatile int hashNullValueSize;
    public static volatile int hashEmptyStrSize;
    public static volatile int hashTreeifiedSize;
    public static volatile int linkedSmallSize;
    public static volatile int linkedManySize;
    public static volatile int treeEmptySize;
    public static volatile int treeOneSize;
    public static volatile int treeTwoSize;
    public static volatile int treeSmallSize;
    public static volatile int treeManySize;
    public static volatile int treeReverseInsertSize;
    public static volatile int treeNullValueSize;

    // Extra-family size witnesses.
    public static volatile int hashtableSmallSize;
    public static volatile int hashColl6Size;
    public static volatile int hashIntKeySize;
    public static volatile int hashStrStrSize;
    public static volatile int hashIntStrSize;
    public static volatile int hashLongLongSize;
    public static volatile int hashEnumKeySize;
    public static volatile int hashResize16Size;
    public static volatile int hashResize17Size;
    public static volatile int hashNestedMapSize;
    public static volatile int hashNestedListSize;
    public static volatile int chmSmallSize;
    public static volatile int chmManySize;
    public static volatile int weakSmallSize;
    public static volatile int identitySmallSize;
    public static volatile int enumSmallSize;
    public static volatile int mapOfNSize;
    public static volatile int mapOf1Size;

    // ADDED-coverage size witnesses + content fingerprints.
    public static volatile int hashUnicodeSize;
    public static volatile int hashCharKeySize;
    public static volatile int hashShortKeySize;
    public static volatile int hashByteKeySize;
    public static volatile int hashBoolKeySize;
    public static volatile int hashNegIdsSize;
    public static volatile int treeReverseComparatorSize;
    public static volatile int treeIntKeySize;

    /** hashCharKey/Short/Byte: sum/xor of boxed keys and values (signed widened to int). */
    public static volatile long hashCharKeyKeySum;
    public static volatile long hashCharKeyValSum;
    public static volatile long hashShortKeyKeySum;
    public static volatile long hashShortKeyValSum;
    public static volatile long hashByteKeyKeySum;
    public static volatile long hashByteKeyValSum;
    /** hashNegIds: sum of value.id across the extreme/negative ids. */
    public static volatile long hashNegIdsIdSum;
    /** treeReverseComparator first/last keys (DESCENDING comparator order). */
    public static volatile String treeReverseComparatorFirstKey;
    public static volatile String treeReverseComparatorLastKey;

    /**
     * Reflective probes confirming the engineered HashMap bucket shapes, so the
     * native side can pin its [INFO] against Java ground truth:
     *   hashColl6Treeified : did the 6-key colliding bucket treeify? (expect NO)
     *   hashIntKeyOneBucket: did the 4 Integer keys land in ONE bucket? (expect YES)
     */
    public static volatile boolean hashColl6Treeified;
    public static volatile boolean hashIntKeyOneBucket;

    /**
     * size() of each Collections.* view (Java's own count).  These are non-zero
     * for the singleton / unmodifiable views even though to_entries reads EMPTY,
     * which is exactly the gap the module characterizes.
     */
    public static volatile int emptyMapCollSize;
    public static volatile int singletonMapCollSize;
    public static volatile int unmodifiableHashSize;
    public static volatile int unmodifiableTreeSize;

    /** TreeMap's first/last keys (sorted), so native can pin the in-order walk. */
    public static volatile String treeSmallFirstKey;
    public static volatile String treeSmallLastKey;
    public static volatile String treeReverseFirstKey;
    public static volatile String treeReverseLastKey;

    /** Whether the treeified HashMap actually treeified at least one bin. */
    public static volatile boolean treeifiedHasTreeBin;

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
        hashEmpty = new HashMap<String, Box>();

        // Minimal populated HashMaps: the size-1 and size-2 general cases (the
        // empty-string and null-bearing maps are special boundaries; these are
        // ordinary tiny maps so a plain single/double bucket walk is exercised).
        hashOne = new HashMap<String, Box>();
        hashOne.put("k0", new Box(0, "v0"));

        hashTwo = new HashMap<String, Box>();
        hashTwo.put("k0", new Box(0, "v0"));
        hashTwo.put("k1", new Box(1, "v1"));

        hashSmall = new HashMap<String, Box>();
        long hsKey = 0, hsId = 0, hsXor = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            final String k = "k" + i;
            hashSmall.put(k, new Box(i, "v" + i));
            hsKey += codeUnitSum(k);
            hsId += i;
            hsXor ^= i;
        }
        hashSmallKeyCharSum = hsKey;
        hashSmallIdSum = hsId;
        hashSmallIdXor = hsXor;

        hashMany = new HashMap<String, Box>();
        long hmKey = 0, hmId = 0, hmXor = 0;
        for (int i = 0; i < MANY_N; ++i)
        {
            final String k = "k" + i;
            hashMany.put(k, new Box(i, "v" + i));
            hmKey += codeUnitSum(k);
            hmId += i;
            hmXor ^= i;
        }
        hashManyKeyCharSum = hmKey;
        hashManyIdSum = hmId;
        hashManyIdXor = hmXor;

        hashNullKey = new HashMap<String, Box>();
        hashNullKey.put(null, new Box(-1, "nullkey"));   // the legal single null key
        hashNullKey.put("a", new Box(1, "va"));
        hashNullKey.put("b", new Box(2, "vb"));

        hashNullValue = new HashMap<String, Box>();
        hashNullValue.put("present", null);              // null value, non-null key
        hashNullValue.put("alsohere", new Box(9, "v9"));

        hashEmptyStr = new HashMap<String, Box>();
        hashEmptyStr.put("", new Box(0, ""));            // empty key AND empty value name

        // Build a HashMap that treeifies a bin: Strings sharing a hashCode would
        // be ideal, but engineering String hash collisions is brittle.  Instead
        // we exploit that HashMap.hash spreads h ^ (h>>>16); at capacity 16 (the
        // default) keys whose spread hash lands in the same low-4-bits bucket
        // collide.  We just insert TREEIFY_N keys; with MANY in one map the odds
        // of a treeified bin are non-trivial, but to GUARANTEE coverage we force
        // every key into bucket 0 by using keys whose String.hashCode is a
        // multiple of a large power of two is also brittle — so we instead build
        // a dedicated map with a tiny initial capacity and a load factor that
        // keeps capacity small while we cram many colliding-by-construction keys.
        // Simplest robust approach: keys "AaAa..." patterns are classic Java
        // hashCode collisions ("Aa".hashCode()=="BB".hashCode()).  We generate a
        // family of length-matched colliding strings.
        hashTreeified = new HashMap<String, Box>();
        final String[] coll = collidingKeys(TREEIFY_N);
        for (int i = 0; i < coll.length; ++i)
        {
            hashTreeified.put(coll[i], new Box(1000 + i, "t" + i));
        }
        treeifiedHasTreeBin = hasTreeNodeBin(hashTreeified);

        linkedSmall = new LinkedHashMap<String, Box>();
        for (int i = 0; i < SMALL_N; ++i)
        {
            linkedSmall.put("k" + i, new Box(i, "v" + i));
        }

        linkedMany = new LinkedHashMap<String, Box>();
        for (int i = 0; i < MANY_N; ++i)
        {
            linkedMany.put("k" + i, new Box(i, "v" + i));
        }

        treeEmpty = new TreeMap<String, Box>();

        treeOne = new TreeMap<String, Box>();
        treeOne.put("k0", new Box(0, "v0"));

        treeTwo = new TreeMap<String, Box>();
        treeTwo.put("k0", new Box(0, "v0"));
        treeTwo.put("k1", new Box(1, "v1"));

        treeSmall = new TreeMap<String, Box>();
        long tsId = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            treeSmall.put("k" + i, new Box(i, "v" + i));
            tsId += i;
        }
        treeSmallIdSum = tsId;
        treeSmallFirstKey = treeSmall.firstKey();
        treeSmallLastKey = treeSmall.lastKey();

        treeMany = new TreeMap<String, Box>();
        long tmId = 0;
        for (int i = 0; i < MANY_N; ++i)
        {
            treeMany.put("k" + i, new Box(i, "v" + i));
            tmId += i;
        }
        treeManyIdSum = tmId;

        // Insert keys in STRICT DESCENDING order so a correct in-order walk has
        // to re-sort them (insertion order != sorted order).  Same content as
        // treeSmall, so the native side can reuse the ascending-order assertion.
        treeReverseInsert = new TreeMap<String, Box>();
        long trId = 0;
        for (int i = SMALL_N - 1; i >= 0; --i)
        {
            treeReverseInsert.put("k" + i, new Box(i, "v" + i));
            trId += i;
        }
        treeReverseIdSum = trId;
        treeReverseFirstKey = treeReverseInsert.firstKey();
        treeReverseLastKey = treeReverseInsert.lastKey();

        // TreeMap permits null VALUES (never null keys): one entry maps a real
        // key to null, the sibling maps to a real Box.
        treeNullValue = new TreeMap<String, Box>();
        treeNullValue.put("present", null);
        treeNullValue.put("alsohere", new Box(9, "v9"));

        // ---- Positively-decoding extra families ----------------------------

        // Hashtable: same key/value/next Entry shape as HashMap.Node, so it
        // decodes fully through the "table" fast path.  Same content recipe as
        // hashSmall so the native side reuses k/v consistency assertions.
        hashtableSmall = new Hashtable<String, Box>();
        long htKey = 0, htId = 0, htXor = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            final String k = "k" + i;
            hashtableSmall.put(k, new Box(i, "v" + i));
            htKey += codeUnitSum(k);
            htId += i;
            htXor ^= i;
        }
        hashtableSmallKeyCharSum = htKey;
        hashtableSmallIdSum = htId;
        hashtableSmallIdXor = htXor;

        // HashMap with a SUB-treeify colliding chain (COLL6_N < 8 keys in one
        // bucket): a plain Node.next chain, no TreeNode.  Uses the same "Aa"/"BB"
        // equal-hashCode family as the treeified map but with fewer keys.  Values
        // Box(2000+i,"c"+i) so its fingerprints are distinct from the others.
        hashColl6 = new HashMap<String, Box>();
        final String[] coll6 = collidingKeys(COLL6_N);
        long c6Key = 0, c6Id = 0, c6Xor = 0;
        for (int i = 0; i < coll6.length; ++i)
        {
            hashColl6.put(coll6[i], new Box(2000 + i, "c" + i));
            c6Key += codeUnitSum(coll6[i]);
            c6Id += (2000 + i);
            c6Xor ^= (2000 + i);
        }
        hashColl6KeyCharSum = c6Key;
        hashColl6IdSum = c6Id;
        hashColl6IdXor = c6Xor;
        hashColl6Treeified = hasTreeNodeBin(hashColl6);   // expect false

        // HashMap<Integer,Integer>: boxed-primitive keys 0,16,32,48 all collide
        // into bucket 0 of the 16-slot default table.  Proves boxed key+value
        // decode and a non-String collision chain.
        hashIntKey = new HashMap<Integer, Integer>();
        long ikKeySum = 0, ikValSum = 0, ikKeyXor = 0;
        for (int i = 0; i < INTKEY_N; ++i)
        {
            final int key = i * 16;          // 0,16,32,48 -> bucket 0
            final int val = 100 + i;
            hashIntKey.put(Integer.valueOf(key), Integer.valueOf(val));
            ikKeySum += key;
            ikValSum += val;
            ikKeyXor ^= key;
        }
        hashIntKeyKeySum = ikKeySum;
        hashIntKeyValSum = ikValSum;
        hashIntKeyKeyXor = ikKeyXor;
        hashIntKeyOneBucket = allInOneBucket(hashIntKey);  // expect true

        // ---- Key/value TYPE-COVERAGE maps ----------------------------------

        // String -> String: value is a java.lang.String, decoded via the same
        // read_java_string path as the key.
        hashStrStr = new HashMap<String, String>();
        long ssKey = 0, ssVal = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            final String k = "sk" + i;
            final String v = "sv" + i;
            hashStrStr.put(k, v);
            ssKey += codeUnitSum(k);
            ssVal += codeUnitSum(v);
        }
        hashStrStrKeyCharSum = ssKey;
        hashStrStrValCharSum = ssVal;

        // Integer -> String: boxed Integer key, String value.
        hashIntStr = new HashMap<Integer, String>();
        long isKey = 0, isVal = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            final String v = "iv" + i;
            hashIntStr.put(Integer.valueOf(i), v);
            isKey += i;
            isVal += codeUnitSum(v);
        }
        hashIntStrKeySum = isKey;
        hashIntStrValCharSum = isVal;

        // Long -> Long: 64-bit boxed key AND value.  Keys deliberately exceed
        // the 32-bit range so a truncating read on the native side would be
        // caught by the sum cross-check.
        hashLongLong = new HashMap<Long, Long>();
        long llKey = 0, llVal = 0, llKeyXor = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            final long k = 0x1_0000_0000L + i;   // > Integer.MAX_VALUE
            final long v = 0x2_0000_0000L + i;
            hashLongLong.put(Long.valueOf(k), Long.valueOf(v));
            llKey += k;
            llVal += v;
            llKeyXor ^= k;
        }
        hashLongLongKeySum = llKey;
        hashLongLongValSum = llVal;
        hashLongLongKeyXor = llKeyXor;

        // Enum-keyed ORDINARY HashMap (NOT EnumMap): stores real Node objects,
        // so it decodes positively.  Each key OOP is a Day constant; native reads
        // its java.lang.Enum.name.
        hashEnumKey = new HashMap<Day, Box>();
        long ekId = 0, ekName = 0;
        final Day[] days = Day.values();
        for (int i = 0; i < days.length; ++i)
        {
            hashEnumKey.put(days[i], new Box(i, "v" + i));
            ekId += i;
            ekName += codeUnitSum(days[i].name());
        }
        hashEnumKeyIdSum = ekId;
        hashEnumKeyNameCharSum = ekName;

        // Resize-boundary HashMaps: 16 (one resize past the default threshold of
        // 12) and 17 (one more).  Keys "k"+i, values Box(i,"v"+i).
        hashResize16 = new HashMap<String, Box>();
        long r16Id = 0;
        for (int i = 0; i < RESIZE16_N; ++i)
        {
            hashResize16.put("k" + i, new Box(i, "v" + i));
            r16Id += i;
        }
        hashResize16IdSum = r16Id;

        hashResize17 = new HashMap<String, Box>();
        long r17Id = 0;
        for (int i = 0; i < RESIZE17_N; ++i)
        {
            hashResize17.put("k" + i, new Box(i, "v" + i));
            r17Id += i;
        }
        hashResize17IdSum = r17Id;

        // HashMap whose values are nested Maps, and another whose values are
        // nested Lists.  Each outer key "n"+i maps to an inner container that
        // itself holds NESTED_N Box entries/elements with ids derived from i.
        hashNestedMap = new HashMap<String, Map<String, Box>>();
        for (int i = 0; i < NESTED_N; ++i)
        {
            final HashMap<String, Box> inner = new HashMap<String, Box>();
            for (int j = 0; j < NESTED_N; ++j)
            {
                inner.put("ik" + j, new Box(i * 10 + j, "iv" + (i * 10 + j)));
            }
            hashNestedMap.put("n" + i, inner);
        }

        hashNestedList = new HashMap<String, List<Box>>();
        for (int i = 0; i < NESTED_N; ++i)
        {
            final java.util.ArrayList<Box> inner = new java.util.ArrayList<Box>();
            for (int j = 0; j < NESTED_N; ++j)
            {
                inner.add(new Box(i * 10 + j, "lv" + (i * 10 + j)));
            }
            hashNestedList.put("n" + i, inner);
        }

        // ---- ADDED input coverage ------------------------------------------

        // Non-ASCII String->String.  Strings are built from explicit char code
        // points so the SOURCE stays pure ASCII (compiles identically under any
        // javac encoding 8..25); the native side asserts the exact UTF-8 decoding:
        //   U+00E9 (e-acute) -> UTF-8 C3 A9   (LATIN1 on JDK9+, char[] on JDK8)
        //   U+00FC (u-umlaut)-> UTF-8 C3 BC
        //   U+4E2D (CJK)     -> UTF-8 E4 B8 AD (UTF16 coder on JDK9+)
        hashUnicode = new HashMap<String, String>();
        final String kLatin = new String(new char[] { (char) 0x00E9 });             // LATIN1 key
        final String vLatin = new String(new char[] { (char) 0x00FC });             // LATIN1 value
        final String kBmp   = new String(new char[] { (char) 0x4E2D });             // UTF16 key
        final String vBmp   = new String(new char[] { (char) 0x00E9, (char) 0x4E2D }); // UTF16 value
        hashUnicode.put(kLatin, vLatin);
        hashUnicode.put(kBmp, vBmp);

        // Boxed Character -> Character: key 'A'(65) -> value 'Z'(90), key '0'(48)
        // -> value '9'(57).  Proves the unsigned-16-bit "C" descriptor decodes.
        hashCharKey = new HashMap<Character, Character>();
        long ckKey = 0, ckVal = 0;
        hashCharKey.put(Character.valueOf('A'), Character.valueOf('Z'));
        hashCharKey.put(Character.valueOf('0'), Character.valueOf('9'));
        ckKey = 'A' + '0';
        ckVal = 'Z' + '9';
        hashCharKeyKeySum = ckKey;
        hashCharKeyValSum = ckVal;

        // Boxed Short -> Short including a NEGATIVE short (sign extension proof).
        hashShortKey = new HashMap<Short, Short>();
        final short[] shKeys = { (short) -1, (short) 7, (short) 30000 };
        long shKey = 0, shVal = 0;
        for (int i = 0; i < shKeys.length; ++i)
        {
            final short v = (short) (shKeys[i] + 1);
            hashShortKey.put(Short.valueOf(shKeys[i]), Short.valueOf(v));
            shKey += shKeys[i];
            shVal += v;
        }
        hashShortKeyKeySum = shKey;
        hashShortKeyValSum = shVal;

        // Boxed Byte -> Byte including a NEGATIVE byte (sign extension proof).
        hashByteKey = new HashMap<Byte, Byte>();
        final byte[] byKeys = { (byte) -1, (byte) 5, (byte) 127 };
        long byKey = 0, byVal = 0;
        for (int i = 0; i < byKeys.length; ++i)
        {
            final byte v = (byte) (byKeys[i] - 1);
            hashByteKey.put(Byte.valueOf(byKeys[i]), Byte.valueOf(v));
            byKey += byKeys[i];
            byVal += v;
        }
        hashByteKeyKeySum = byKey;
        hashByteKeyValSum = byVal;

        // Boxed Boolean -> Box: exactly FALSE and TRUE.
        hashBoolKey = new HashMap<Boolean, Box>();
        hashBoolKey.put(Boolean.FALSE, new Box(0, "false-v"));
        hashBoolKey.put(Boolean.TRUE, new Box(1, "true-v"));

        // Extreme / negative Box ids: full signed-int range round-trip.
        hashNegIds = new HashMap<String, Box>();
        final int[] extremeIds = { Integer.MIN_VALUE, -1, 0, Integer.MAX_VALUE };
        long negSum = 0;
        for (int i = 0; i < extremeIds.length; ++i)
        {
            hashNegIds.put("e" + i, new Box(extremeIds[i], "v" + i));
            negSum += extremeIds[i];
        }
        hashNegIdsIdSum = negSum;

        // TreeMap with a REVERSE comparator: in-order walk must come out DESCENDING.
        treeReverseComparator = new TreeMap<String, Box>(Collections.reverseOrder());
        for (int i = 0; i < SMALL_N; ++i)
        {
            treeReverseComparator.put("k" + i, new Box(i, "v" + i));
        }
        treeReverseComparatorFirstKey = treeReverseComparator.firstKey();   // "k2"
        treeReverseComparatorLastKey = treeReverseComparator.lastKey();     // "k0"

        // TreeMap<Integer,Box>: NUMERIC key order (insert 10,2,1 -> walk 1,2,10).
        treeIntKey = new TreeMap<Integer, Box>();
        treeIntKey.put(Integer.valueOf(10), new Box(10, "v10"));
        treeIntKey.put(Integer.valueOf(2), new Box(2, "v2"));
        treeIntKey.put(Integer.valueOf(1), new Box(1, "v1"));

        // ---- Characterized-empty extra shapes ------------------------------

        chmSmall = new ConcurrentHashMap<String, Box>();
        for (int i = 0; i < SMALL_N; ++i)
        {
            chmSmall.put("k" + i, new Box(i, "v" + i));
        }
        chmMany = new ConcurrentHashMap<String, Box>();
        for (int i = 0; i < MANY_N; ++i)
        {
            chmMany.put("k" + i, new Box(i, "v" + i));
        }

        // WeakHashMap: hold the keys strongly so they cannot be cleared while the
        // native side reads the table.
        keyHolder = new String[SMALL_N];
        weakSmall = new WeakHashMap<String, Box>();
        for (int i = 0; i < SMALL_N; ++i)
        {
            // new String(...) so each key is a distinct, strongly-held instance.
            final String k = new String("wk" + i);
            keyHolder[i] = k;
            weakSmall.put(k, new Box(i, "v" + i));
        }

        identitySmall = new IdentityHashMap<String, Box>();
        for (int i = 0; i < SMALL_N; ++i)
        {
            identitySmall.put("k" + i, new Box(i, "v" + i));
        }

        enumSmall = new EnumMap<Day, Box>(Day.class);
        enumSmall.put(Day.MON, new Box(0, "v0"));
        enumSmall.put(Day.TUE, new Box(1, "v1"));
        enumSmall.put(Day.WED, new Box(2, "v2"));

        // Map.of(...) reflectively (so JDK 8 still compiles this source).  On a
        // JDK without Map.of these stay null and the native side reads empty too.
        mapOfN = buildMapOfN();
        mapOf1 = buildMapOf1();

        // Collections.* views.  unmodifiable* must wrap the maps AFTER they are
        // populated above so their size() witnesses are non-zero; to_entries on
        // each still reads EMPTY (no "table"/"root" on the wrapper's own klass).
        emptyMapColl = Collections.emptyMap();
        singletonMapColl = Collections.singletonMap("only", new Box(42, "only-value"));
        unmodifiableHash = Collections.unmodifiableMap(hashSmall);
        unmodifiableTree = Collections.unmodifiableSortedMap(treeSmall);

        nullMap = null;

        hashEmptySize = hashEmpty.size();
        hashOneSize = hashOne.size();
        hashTwoSize = hashTwo.size();
        hashSmallSize = hashSmall.size();
        hashManySize = hashMany.size();
        hashNullKeySize = hashNullKey.size();
        hashNullValueSize = hashNullValue.size();
        hashEmptyStrSize = hashEmptyStr.size();
        hashTreeifiedSize = hashTreeified.size();
        linkedSmallSize = linkedSmall.size();
        linkedManySize = linkedMany.size();
        treeEmptySize = treeEmpty.size();
        treeOneSize = treeOne.size();
        treeTwoSize = treeTwo.size();
        treeSmallSize = treeSmall.size();
        treeManySize = treeMany.size();
        treeReverseInsertSize = treeReverseInsert.size();
        treeNullValueSize = treeNullValue.size();

        emptyMapCollSize = emptyMapColl.size();
        singletonMapCollSize = singletonMapColl.size();
        unmodifiableHashSize = unmodifiableHash.size();
        unmodifiableTreeSize = unmodifiableTree.size();

        hashtableSmallSize = hashtableSmall.size();
        hashColl6Size = hashColl6.size();
        hashIntKeySize = hashIntKey.size();
        hashStrStrSize = hashStrStr.size();
        hashIntStrSize = hashIntStr.size();
        hashLongLongSize = hashLongLong.size();
        hashEnumKeySize = hashEnumKey.size();
        hashResize16Size = hashResize16.size();
        hashResize17Size = hashResize17.size();
        hashNestedMapSize = hashNestedMap.size();
        hashNestedListSize = hashNestedList.size();
        chmSmallSize = chmSmall.size();
        chmManySize = chmMany.size();
        weakSmallSize = weakSmall.size();
        identitySmallSize = identitySmall.size();
        enumSmallSize = enumSmall.size();
        mapOfNSize = (mapOfN == null) ? -1 : mapOfN.size();
        mapOf1Size = (mapOf1 == null) ? -1 : mapOf1.size();

        hashUnicodeSize = hashUnicode.size();
        hashCharKeySize = hashCharKey.size();
        hashShortKeySize = hashShortKey.size();
        hashByteKeySize = hashByteKey.size();
        hashBoolKeySize = hashBoolKey.size();
        hashNegIdsSize = hashNegIds.size();
        treeReverseComparatorSize = treeReverseComparator.size();
        treeIntKeySize = treeIntKey.size();
    }

    /** Strong holder keeping WeakHashMap keys reachable for the duration. */
    private static String[] keyHolder;

    /**
     * Reflectively builds Map.of("k0",Box(0,"v0"),...) with SMALL_N entries so
     * this source compiles on JDK 8 (no Map.of).  Returns null if Map.of is
     * unavailable (pre-9) or reflection is blocked.
     */
    private static Map<String, Box> buildMapOfN()
    {
        try
        {
            final Object[] kv = new Object[SMALL_N * 2];
            for (int i = 0; i < SMALL_N; ++i)
            {
                kv[i * 2]     = "k" + i;
                kv[i * 2 + 1] = new Box(i, "v" + i);
            }
            final java.lang.reflect.Method ofEntries =
                Map.class.getMethod("ofEntries", java.util.Map.Entry[].class);
            @SuppressWarnings("unchecked")
            final java.util.Map.Entry<String, Box>[] entries =
                (java.util.Map.Entry<String, Box>[]) new java.util.Map.Entry[SMALL_N];
            for (int i = 0; i < SMALL_N; ++i)
            {
                entries[i] = new java.util.AbstractMap.SimpleImmutableEntry<String, Box>(
                    (String) kv[i * 2], (Box) kv[i * 2 + 1]);
            }
            @SuppressWarnings("unchecked")
            final Map<String, Box> result =
                (Map<String, Box>) ofEntries.invoke(null, (Object) entries);
            return result;
        }
        catch (final Throwable t)
        {
            return null;
        }
    }

    /** Reflectively builds Map.of("only",Box(...)) — the 1-entry Map1 shape. */
    private static Map<String, Box> buildMapOf1()
    {
        try
        {
            final java.lang.reflect.Method of =
                Map.class.getMethod("of", Object.class, Object.class);
            @SuppressWarnings("unchecked")
            final Map<String, Box> result =
                (Map<String, Box>) of.invoke(null, "only", new Box(42, "only-value"));
            return result;
        }
        catch (final Throwable t)
        {
            return null;
        }
    }

    /**
     * Reflectively reports whether ALL entries of a HashMap live in a single
     * bucket (used to confirm the Integer-key collision construction).  Returns
     * false if reflection is blocked.
     */
    private static boolean allInOneBucket(final HashMap<?, ?> map)
    {
        try
        {
            final java.lang.reflect.Field tableField = HashMap.class.getDeclaredField("table");
            tableField.setAccessible(true);
            final Object table = tableField.get(map);
            if (table == null)
            {
                return map.isEmpty();
            }
            final int len = java.lang.reflect.Array.getLength(table);
            int nonEmptyBuckets = 0;
            for (int i = 0; i < len; ++i)
            {
                if (java.lang.reflect.Array.get(table, i) != null)
                {
                    ++nonEmptyBuckets;
                }
            }
            return nonEmptyBuckets == 1;
        }
        catch (final Throwable t)
        {
            return false;
        }
    }

    /**
     * Generates n strings that all share the SAME String.hashCode by chaining
     * the classic "Aa"/"BB" collision (both hash to 2112).  Concatenating any
     * sequence of these 2-char blocks preserves the equal-hashCode property as
     * long as every generated string has the same length, so all n keys collide
     * into one bucket and the bin treeifies once it exceeds 8 entries.
     */
    private static String[] collidingKeys(final int n)
    {
        // 2-char blocks that pairwise collide: "Aa" and "BB" both == 2112.
        final String[] blocks = { "Aa", "BB" };
        // Use enough blocks per key to make 2^blocks >= n distinct combinations.
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
     * Reflectively inspects the HashMap.table to report whether any bucket head
     * is a TreeNode (i.e. the bin treeified).  Returns false if reflection is
     * blocked (e.g. a strict module system) — the native check tolerates that.
     */
    private static boolean hasTreeNodeBin(final HashMap<String, Box> map)
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
                return CollMap.go && !CollMap.done;
            }

            @Override
            public void run()
            {
                if (CollMap.mode == 1)
                {
                    final CollMap instance = new CollMap();
                    CollMap.observed = instance.touch(42);
                }
                else
                {
                    // mode 0: (re)build all maps so the native reads see a fresh,
                    // deterministic population on this exact thread.
                    buildAll();
                }
                CollMap.done = true;
            }
        });
    }
}
