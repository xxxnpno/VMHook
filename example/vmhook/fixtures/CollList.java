package vmhook.fixtures;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Stack;
import java.util.Vector;
import java.util.concurrent.CopyOnWriteArrayList;

import vmhook.Harness;

/**
 * Fixture for the collection_list feature (area: collections).
 *
 * Exercises the ArrayList / LinkedList / Object[] BACKING-STORE layouts on a
 * live JVM.  The native module reaches every list by reading the static
 * {@link #SINGLETON} field, then walking each instance list field's backing
 * store BY HAND (ArrayList: elementData + size; LinkedList: first + node
 * item/next chain; Object[]: the raw reference array) — exactly the layouts the
 * library's own fast paths use, but decoded in the module so the test does not
 * depend on the library's templated {@code to_vector<T>()} (which mis-decodes on
 * the MSVC-14.51 Windows runner; see collection_list.cpp ROOT_CAUSE note).
 *
 * Because every list is a plain field reachable from a static singleton, the
 * native side reads them DIRECTLY off the worker thread after a build probe has
 * populated them on the Java thread (the same no-capture-detour pattern
 * CollSet.java / collection_set.cpp use, which is CI-green on MSVC-14.51).  No
 * interpreter capture-detour is needed: the module drives the canonical go/done
 * handshake to make {@link #populate()} run on the Java thread, then walks the
 * now-populated backing stores.
 *
 * What the lists prove (decoded by the module's hand-walk):
 *   - ArrayList backing store (elementData + size): empty, single, many
 *     (12 &gt; default capacity 10, so a grow happened), a trimToSize() list
 *     (size == capacity), and an ensureCapacity(100) list (size != capacity) —
 *     the walk bound MUST be `size`, never `elementData.length`.
 *   - LinkedList backing store (first + node item/next chain): empty, single,
 *     many (12), and a LARGE 4096-element chain so the native side can wall-clock
 *     the first-&gt;next walk and prove it visits every node exactly once in order
 *     (no cycle, no dup, no early stop).
 *   - null elements become nullptr slots in BOTH containers (ArrayList array
 *     slot == null; LinkedList Node.item == null).
 *   - element order preserved: element k carries id == k, so the native side
 *     asserts vec[k].id == k for every k — the strongest order+identity signal.
 *
 * EXHAUSTIVE shape coverage:
 *   - size 2 in BOTH containers (arrTwo / linkTwo): the smallest "more than one"
 *     case, distinct from single.
 *   - duplicate-VALUE list (arrDup): several elements share the same id/tag but
 *     are DISTINCT heap objects — proves the walk neither collapses equal-valued
 *     elements nor re-emits one (decoded OOPs stay all-distinct) and that the
 *     value readback is per-element, not deduplicated.
 *   - Object[] reference array (elemArray, an Elem[]): a '[L...;' field the
 *     native side walks DIRECTLY as a raw Java array — the sibling entry point to
 *     the List object backing stores the rest of this fixture exercises.
 *   - nested List-of-Lists (nested, an ArrayList&lt;List&lt;Elem&gt;&gt;): the outer takes
 *     the ArrayList backing-store walk and each element is itself a List the
 *     native side re-walks by hand — proves a decoded element OOP is a real,
 *     fully-walkable container, with mixed inner ArrayList/LinkedList types.
 *   - the JDK Collections wrappers whose ELEMENTS are reachable by a direct
 *     backing-field walk (NO Java get(int) call from the worker body, which the
 *     suite forbids):
 *       * Arrays.asList(...)             -&gt; java.util.Arrays$ArrayList (field "a",
 *                                          an Object[] the native side array-walks)
 *       * Collections.emptyList()        -&gt; Collections$EmptyList (no element field, size 0)
 *       * Collections.singletonList(x)   -&gt; Collections$SingletonList (field "element")
 *       * Collections.unmodifiableList() -&gt; Collections$UnmodifiableList (field "list"
 *                                          -&gt; backing ArrayList, re-walked by hand)
 *       * Collections.synchronizedList() -&gt; Collections$SynchronizedList (field "c"
 *                                          -&gt; backing ArrayList, re-walked by hand)
 *
 *   - the other RandomAccess / legacy List families, each reached by its own
 *     stable backing-field shape:
 *       * java.util.Vector / java.util.Stack -&gt; "elementData" Object[] bounded by
 *         "elementCount" (NOT "size"!): empty / many (default cap 10 grows to 20,
 *         so size != elementData.length) / ensureCapacity(100) (size != capacity)
 *         / with-null / size-2.  The Vector bound MUST be elementCount, never
 *         elementData.length — the SAME size-vs-capacity property the ArrayList
 *         oversized case proves, on a container whose grow policy DOUBLES capacity.
 *       * java.util.concurrent.CopyOnWriteArrayList -&gt; "array" Object[] whose
 *         length IS the size (no separate size field): empty / many / with-null
 *         (COW permits null) / size-2.
 *       * boxed element type: ArrayList&lt;Integer&gt; / Vector&lt;Integer&gt; whose elements
 *         are java.lang.Integer; the native side reads Integer.value per slot
 *         (order == value), proving the backing walk is element-type-agnostic.
 *       * nested List-of-Map: an outer ArrayList whose elements are HashMaps; the
 *         outer walk recovers each inner Map OOP and proves it is a real, distinct
 *         heap object (Map CONTENT decode is collection_map's job, not this one).
 *
 *   - List families the backing-field walk deliberately CHARACTERIZES rather than
 *     decodes (it pins the Java-published size() as an oracle and records [INFO],
 *     never FAILs — exactly the CollSet.java handling for non-fast-path Sets):
 *       * List.of(...) (JDK 9+ immutable, built reflectively so the fixture still
 *         compiles at -source 8).  List.of() / List.of(4 elems) are ListN ("elements"
 *         Object[]) and ARE hand-walked; List.of(1) / List.of(1,2) are List12 whose
 *         absent second slot "e1" holds a shared non-null EMPTY SENTINEL object (not
 *         null), so a raw e0/e1 read cannot tell size-1 from size-2 without size()
 *         — those two are characterized, not decoded.  `listOfAvailable` is false on
 *         Java 8.
 *       * subList(from, to) views (ArrayList- and LinkedList-backed): the backing
 *         field shape moved across JDKs (8: "parent"/"parentOffset"; 9+:
 *         "root"/"parent"/"offset") with no element array of its own, so the walk
 *         characterizes them via the published size() instead of a fragile raw walk.
 *
 * Each Elem also carries a String `tag` ("e<id>") so the native side can do the
 * reference-field readback the scope asks for, through a wrapper it built from a
 * decoded element OOP (proves the decoded element OOP is a real, walkable heap
 * object).
 *
 * Java 8 syntax only (no var/records/switch-expr/text-blocks).
 */
public final class CollList
{
    /** Native sets this true to request the build action; clears it after. */
    public static volatile boolean go;

    /** The build action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Element type the native side wraps.  `id` is the insertion index (so
     * order checks are exact); `tag` is "e<id>" for the String field readback.
     * A nested public static final class is "CollList$Elem" in JVM internal
     * form — that is the name the native module registers.
     */
    public static final class Elem
    {
        public int id;
        public String tag;

        public Elem(final int id)
        {
            this.id = id;
            this.tag = "e" + id;
        }
    }

    /**
     * Enum element type.  Each constant is a genuine enum instance whose `name`
     * (String) and `ordinal` (int) live on java.lang.Enum (the shared superclass),
     * so the native side reads them through a wrapper registered as java/lang/Enum
     * — proving a List's decoded element OOP can be a real enum constant whose
     * inherited Enum fields read back correctly, in positional (ordinal) order.
     * The internal name is "CollList$Color".  Three constants keep the list small.
     */
    public enum Color
    {
        RED, GREEN, BLUE
    }

    // ── Sizes the native side mirrors ──────────────────────────────────────
    /** Element count of the "many" ArrayList / LinkedList (> default cap 10). */
    public static final int MANY = 12;

    /**
     * Element count of the large LinkedList used for the chain-walk canary.
     * 4096 keeps the chain a genuinely large, multi-region first-&gt;next walk
     * (every order/distinctness/identity invariant on the native side stays a
     * hard assert) while staying modest.  The native side reads it off the
     * worker thread AFTER the build probe has finished (the Java thread is
     * parked in the Harness loop), so — unlike a read from inside a live
     * interpreter detour — no concurrent allocation/relocation runs under the
     * raw-oop walk.  Keep this in lockstep with BIG in
     * tests/jvm/modules/collection_list.cpp.
     */
    public static final int BIG = 4096;

    /** Index inside the "with null" lists that holds a null element. */
    public static final int NULL_AT = 2;

    /** Total element count of the "with null" lists (one slot is null). */
    public static final int NULL_LIST_LEN = 4;

    /** Element count of the size-2 lists (smallest "more than one"). */
    public static final int TWO = 2;

    /** Element count of the duplicate-value ArrayList. */
    public static final int DUP_LEN = 6;

    /**
     * The single id/tag shared by EVERY element of the duplicate-value list.
     * Each element is still a distinct heap object (a fresh new Elem(DUP_VAL)),
     * so the native side sees DUP_LEN equal values but DUP_LEN distinct OOPs.
     */
    public static final int DUP_VAL = 9;

    /** Element count of the Elem[] object-array field (the '[L' array branch). */
    public static final int OBJ_ARR_LEN = 5;

    /** Outer length of the nested List-of-Lists. */
    public static final int NESTED_OUTER = 3;

    /** Inner length of EACH list inside the nested List-of-Lists. */
    public static final int NESTED_INNER = 4;

    /** Element count of the Arrays.asList(...) view (JDK wrapper). */
    public static final int ASLIST_LEN = 3;

    /** The id/tag of the Collections.singletonList(...) element. */
    public static final int SINGLETON_ID = 0;

    /** Element count of the Vector / Stack / COW "many" lists (> default cap 10). */
    public static final int VEC_MANY = 12;

    /** Element count of the boxed-Integer lists. */
    public static final int INT_LEN = 6;

    /** Outer length of the nested List-of-Map. */
    public static final int MAP_OUTER = 3;

    /** Entry count of EACH map inside the nested List-of-Map. */
    public static final int MAP_INNER = 2;

    /** Element count of the List.of(...) ListN cases that ARE hand-walked. */
    public static final int LISTOF_N = 4;

    /** sublist window [SUB_FROM, SUB_TO) taken from the MANY lists. */
    public static final int SUB_FROM = 3;
    public static final int SUB_TO = 9;
    /** Resulting size of the subList window (SUB_TO - SUB_FROM). */
    public static final int SUB_LEN = SUB_TO - SUB_FROM;

    // ── Extra element-TYPE coverage (String / boxed Long / enum) ────────────
    /** Element count of the String-element ArrayList (strList). */
    public static final int STR_LEN = 5;

    /** Element count of the boxed-Long ArrayList (longArrList). */
    public static final int LONG_LEN = 6;

    /**
     * Base offset for the boxed-Long values.  Strictly &gt; 2^32 so element k holds
     * LONG_BASE + k, a value whose high 32 bits are non-zero.  A truncating 32-bit
     * read of the boxed long would drop the high word and collapse every element to
     * its low word (== k), so reading back LONG_BASE + k proves the element decode
     * (and the java.lang.Long.value field read) is full-width, not truncated.
     */
    public static final long LONG_BASE = 0x1_0000_0007L; // 4294967303

    // ── Extra SIZE coverage (10 / 16 / 1000) on both backing families ───────
    /** Default ArrayList capacity boundary (a 10-element list fits exactly). */
    public static final int TEN = 10;

    /** Power-of-two element count (16) — a grow/resize boundary. */
    public static final int SIXTEEN = 16;

    /** Round large element count (1000) for ArrayList and LinkedList. */
    public static final int THOUSAND = 1000;

    // ── ArrayList fields (each takes the "elementData"+"size" backing store) ─
    /** Empty ArrayList — size 0, the walk must be empty (no element read). */
    public final ArrayList<Elem> arrEmpty = new ArrayList<Elem>();

    /** Single-element ArrayList. */
    public final ArrayList<Elem> arrSingle = new ArrayList<Elem>();

    /** Many-element ArrayList (12 elements; grew past the default cap of 10). */
    public final ArrayList<Elem> arrMany = new ArrayList<Elem>();

    /** ArrayList trimmed so elementData.length == size (capacity == size). */
    public final ArrayList<Elem> arrTrimmed = new ArrayList<Elem>();

    /** ArrayList with capacity 100 but only MANY elements (size != capacity). */
    public final ArrayList<Elem> arrOversized = new ArrayList<Elem>(100);

    /** ArrayList whose element at index NULL_AT is null (nullptr-slot proof). */
    public final ArrayList<Elem> arrWithNull = new ArrayList<Elem>();

    /** Size-2 ArrayList (smallest "more than one"). */
    public final ArrayList<Elem> arrTwo = new ArrayList<Elem>();

    /**
     * ArrayList of DUP_LEN elements that all share id/tag == DUP_VAL but are
     * distinct heap objects.  Proves equal-valued elements are neither collapsed
     * nor re-emitted: the native side reads DUP_LEN equal ids yet DUP_LEN
     * distinct element OOPs.
     */
    public final ArrayList<Elem> arrDup = new ArrayList<Elem>();

    // ── LinkedList fields (each takes the "first"+node item/next chain) ──────
    /** Empty LinkedList — size 0, the walk must be empty. */
    public final LinkedList<Elem> linkEmpty = new LinkedList<Elem>();

    /** Single-element LinkedList. */
    public final LinkedList<Elem> linkSingle = new LinkedList<Elem>();

    /** Many-element LinkedList (12 elements). */
    public final LinkedList<Elem> linkMany = new LinkedList<Elem>();

    /** LinkedList whose node at index NULL_AT has item == null. */
    public final LinkedList<Elem> linkWithNull = new LinkedList<Elem>();

    /** Size-2 LinkedList (smallest "more than one"). */
    public final LinkedList<Elem> linkTwo = new LinkedList<Elem>();

    /**
     * Large LinkedList (BIG elements).  The native side wall-clocks the
     * first->next walk over this and proves the walk terminates at exactly BIG
     * nodes in order.  Declared as the supertype List to underline that the
     * native walk selects the backing-store shape from the runtime klass, not
     * from the Java static type.
     */
    public final List<Elem> linkBig = new LinkedList<Elem>();

    // ── Object-array field ('[L...;') — the native side array-walks it ───────
    /**
     * A raw Elem[] (NOT a List).  Its field signature is "[Lvmhook/fixtures/
     * CollList$Elem;", so the native side walks it DIRECTLY as a Java array (the
     * array branch), never routing it through any List backing store — the
     * sibling of the List object path the rest of this fixture exercises.
     * Populated in populate() with ids 0..OBJ_ARR_LEN-1.
     */
    public final Elem[] elemArray = new Elem[OBJ_ARR_LEN];

    // ── Nested List-of-Lists — outer walk, inner re-walk ────────────────────
    /**
     * An ArrayList whose elements are themselves Lists.  The outer takes the
     * ArrayList backing-store walk; the native side then re-walks each element
     * (an inner List OOP) by hand to prove a decoded element is a real,
     * fully-walkable container.  Inner list j holds ids 0..NESTED_INNER-1.
     */
    public final ArrayList<List<Elem>> nested = new ArrayList<List<Elem>>();

    // ── JDK Collections wrappers (element-reachable by a backing-field walk) ─
    /** Arrays.asList(...) view — java.util.Arrays$ArrayList (backing Object[] "a"). */
    public List<Elem> asListView;

    /** Collections.emptyList() — Collections$EmptyList, no element field, size 0. */
    public List<Elem> emptyImmutable;

    /** Collections.singletonList(x) — Collections$SingletonList (field "element"). */
    public List<Elem> singletonView;

    /** Collections.unmodifiableList(arrMany) — wraps the 12-element ArrayList. */
    public List<Elem> unmodifiableView;

    /** Collections.synchronizedList(arrMany) — backing field "c" -&gt; arrMany. */
    public List<Elem> synchronizedView;

    // ── Vector / Stack ("elementData" + "elementCount") ─────────────────────
    /** Empty Vector — size 0, the elementData walk must be empty. */
    public final Vector<Elem> vecEmpty = new Vector<Elem>();

    /**
     * Many-element Vector (VEC_MANY=12).  A default Vector starts at capacity 10
     * and DOUBLES on grow, so after 12 adds elementData.length == 20 != size 12.
     * The walk MUST bound by elementCount (12), never elementData.length (20) —
     * the headline size-vs-capacity property, here on Vector's doubling policy.
     */
    public final Vector<Elem> vecMany = new Vector<Elem>();

    /** Vector pre-sized to capacity 100 holding only VEC_MANY elems (size != cap). */
    public final Vector<Elem> vecOversized = new Vector<Elem>(100);

    /** Vector whose element at index NULL_AT is null (nullptr-slot proof). */
    public final Vector<Elem> vecWithNull = new Vector<Elem>();

    /** Size-2 Vector (smallest "more than one"). */
    public final Vector<Elem> vecTwo = new Vector<Elem>();

    /** Stack (extends Vector) with VEC_MANY pushed elements. */
    public final Stack<Elem> stackMany = new Stack<Elem>();

    // ── CopyOnWriteArrayList ("array" Object[], length == size) ──────────────
    /** Empty COW list — backing "array" has length 0, the walk must be empty. */
    public final CopyOnWriteArrayList<Elem> cowEmpty = new CopyOnWriteArrayList<Elem>();

    /** Many-element COW list (VEC_MANY); "array".length == size (no slack). */
    public final CopyOnWriteArrayList<Elem> cowMany = new CopyOnWriteArrayList<Elem>();

    /** COW list whose element at index NULL_AT is null (COW permits null). */
    public final CopyOnWriteArrayList<Elem> cowWithNull = new CopyOnWriteArrayList<Elem>();

    /** Size-2 COW list. */
    public final CopyOnWriteArrayList<Elem> cowTwo = new CopyOnWriteArrayList<Elem>();

    // ── Boxed-Integer element lists (element decode is type-agnostic) ────────
    /** ArrayList&lt;Integer&gt; with values 0..INT_LEN-1; native reads Integer.value. */
    public final ArrayList<Integer> intArrList = new ArrayList<Integer>();

    /** Vector&lt;Integer&gt; with values 0..INT_LEN-1 (elementData walk, boxed elems). */
    public final Vector<Integer> intVecList = new Vector<Integer>();

    // ── Extra element-TYPE lists (String / boxed Long / enum) ────────────────
    /**
     * ArrayList&lt;String&gt; with values "s0".."s{STR_LEN-1}".  The native side reads
     * each decoded element OOP as a java.lang.String via read_java_string — the
     * element decode is type-agnostic, and String is the canonical reference
     * element type the scope calls for.
     */
    public final ArrayList<String> strList = new ArrayList<String>();

    /**
     * ArrayList&lt;Long&gt; whose element k is Long.valueOf(LONG_BASE + k).  Every value
     * exceeds 2^32, so a truncating 32-bit read of the boxed long would collapse
     * the high word; the native side reads back the FULL 64-bit Long.value and
     * asserts it equals LONG_BASE + k — a positive catch for any truncating read.
     */
    public final ArrayList<Long> longArrList = new ArrayList<Long>();

    /**
     * ArrayList&lt;Color&gt; holding the enum constants in ordinal order (RED, GREEN,
     * BLUE).  The native side reads each element's inherited Enum name/ordinal and
     * asserts ordinal == index, proving positional order for a non-Elem reference
     * element type.
     */
    public final ArrayList<Color> enumList = new ArrayList<Color>();

    // ── Extra SIZE lists (10 / 16 / 1000) — positional order at each size ─────
    /** 10-element ArrayList (exactly the default capacity). */
    public final ArrayList<Elem> arrTen = new ArrayList<Elem>();

    /** 16-element ArrayList (power-of-two grow boundary). */
    public final ArrayList<Elem> arrSixteen = new ArrayList<Elem>();

    /** 1000-element ArrayList (round large size). */
    public final ArrayList<Elem> arrThousand = new ArrayList<Elem>();

    /** 1000-element LinkedList (round large size; chain walk at scale). */
    public final LinkedList<Elem> linkThousand = new LinkedList<Elem>();

    // ── All-null / null-at-boundary lists (null-PATTERN coverage) ────────────
    /** Total length of the all-null and null-boundary lists. */
    public static final int NULL_PATTERN_LEN = 4;

    /**
     * ArrayList whose EVERY slot is null.  Proves the walk emits exactly
     * NULL_PATTERN_LEN slots, all null (null_count == size, no element decoded) —
     * the degenerate all-null case the single-null lists do not cover.
     */
    public final ArrayList<Elem> arrAllNull = new ArrayList<Elem>();

    /** LinkedList whose every Node.item is null (all-null chain walk). */
    public final LinkedList<Elem> linkAllNull = new LinkedList<Elem>();

    /**
     * ArrayList whose FIRST slot (index 0) is null, the rest non-null with
     * id == index.  Null at the head boundary — distinct from NULL_AT (middle).
     */
    public final ArrayList<Elem> arrNullFirst = new ArrayList<Elem>();

    /**
     * ArrayList whose LAST slot (index NULL_PATTERN_LEN-1) is null, the rest
     * non-null.  Null at the tail boundary — proves the bound is `size` and the
     * trailing null is a REAL element slot, not a phantom capacity-tail null.
     */
    public final ArrayList<Elem> arrNullLast = new ArrayList<Elem>();

    /** LinkedList whose first Node.item is null (null at head of the chain). */
    public final LinkedList<Elem> linkNullFirst = new LinkedList<Elem>();

    // ── Extra boxed element types (Boolean / Byte / Short / Character / Double)
    /** Element count of the small boxed-type lists. */
    public static final int BOX_LEN = 3;

    /**
     * ArrayList&lt;Boolean&gt; alternating false,true,false.  java.lang.Boolean.value
     * is a boolean; the native side reads it as (value() != 0) and asserts the
     * pattern, proving a Z-typed boxed primitive element decodes through the
     * element-type-agnostic backing walk.
     */
    public final ArrayList<Boolean> boolArrList = new ArrayList<Boolean>();

    /**
     * ArrayList&lt;Byte&gt; with values 0,1,2.  java.lang.Byte.value is a byte; the
     * native side reads it as an int and asserts value == index.
     */
    public final ArrayList<Byte> byteArrList = new ArrayList<Byte>();

    /**
     * ArrayList&lt;Short&gt; with values 0,1,2.  java.lang.Short.value is a short; the
     * native side reads it as an int and asserts value == index.
     */
    public final ArrayList<Short> shortArrList = new ArrayList<Short>();

    /**
     * ArrayList&lt;Character&gt; with values 'a','b','c'.  java.lang.Character.value is
     * a char; the native side reads it as an int and asserts value == 'a'+index.
     */
    public final ArrayList<Character> charArrList = new ArrayList<Character>();

    /**
     * ArrayList&lt;Double&gt; with values 0.0,1.0,2.0.  java.lang.Double.value is a
     * double; the native side reads it FULL-WIDTH and asserts value == index — a
     * wide (8-byte) boxed primitive, the floating-point analogue of the Long list.
     */
    public final ArrayList<Double> doubleArrList = new ArrayList<Double>();

    /** First Character value the native side mirrors ('a'). */
    public static final char CHAR_BASE = 'a';

    // ── Nested list whose inners are Vector / COW (broaden inner re-walk) ─────
    /** Outer length of the mixed-shape nested list (Vector inner + COW inner). */
    public static final int NESTED_MIX_OUTER = 2;

    /**
     * Outer ArrayList whose elements are a Vector and a CopyOnWriteArrayList (each
     * NESTED_INNER elements).  Proves the inner shape-detecting re-walk dispatches
     * correctly to the Vector ("elementCount") and COW ("array") backing shapes —
     * the nested case only covered ArrayList/LinkedList inners before.
     */
    public final ArrayList<List<Elem>> nestedMixed = new ArrayList<List<Elem>>();

    // ── Nested List-of-Map (outer ArrayList walk -> inner Map OOPs) ──────────
    /**
     * Outer ArrayList whose elements are HashMaps.  The outer takes the ArrayList
     * backing walk; each decoded element OOP is a Map the native side proves is a
     * real, distinct heap object (Map content decode belongs to collection_map).
     */
    public final ArrayList<Map<String, Elem>> nestedMaps = new ArrayList<Map<String, Elem>>();

    // ── subList views (CHARACTERIZED via size(), not decoded) ────────────────
    /** ArrayList.subList(SUB_FROM, SUB_TO) over arrMany — ArrayList$SubList. */
    public List<Elem> arrSubList;

    /** LinkedList.subList(SUB_FROM, SUB_TO) over linkMany — AbstractList$SubList. */
    public List<Elem> linkSubList;

    // ── List.of(...) immutable (JDK 9+, built reflectively; CHARACTERIZED) ───
    /** List.of() — ImmutableCollections$ListN, "elements" Object[] of length 0. */
    public List<Elem> listOf0;

    /** List.of(x) — ImmutableCollections$List12 (e0 real, e1 = EMPTY sentinel). */
    public List<Elem> listOf1;

    /** List.of(x,y) — ImmutableCollections$List12 (e0, e1 both real). */
    public List<Elem> listOf2;

    /** List.of(...4...) — ImmutableCollections$ListN, "elements" Object[] len 4. */
    public List<Elem> listOfN;

    /** True when List.of(...) resolved (JDK 9+); false on Java 8. */
    public volatile boolean listOfAvailable;

    // ── Published Java size() witnesses (plain int fields the native side reads
    //    WITHOUT a Java call) for the characterized / cross-checked families ──
    public volatile int arrSubListSize;
    public volatile int linkSubListSize;
    public volatile int listOf0Size;
    public volatile int listOf1Size;
    public volatile int listOf2Size;
    public volatile int listOfNSize;
    public volatile int synchronizedViewSize;
    public volatile int vecManySize;
    public volatile int cowManySize;

    private static volatile boolean populated;

    private void populate()
    {
        if (populated)
        {
            return;
        }

        arrSingle.add(new Elem(0));

        for (int i = 0; i < MANY; ++i)
        {
            arrMany.add(new Elem(i));
            linkMany.add(new Elem(i));
        }

        for (int i = 0; i < MANY; ++i)
        {
            arrTrimmed.add(new Elem(i));
        }
        arrTrimmed.trimToSize();          // elementData.length now == size

        arrOversized.ensureCapacity(100); // capacity 100, still only MANY elems
        for (int i = 0; i < MANY; ++i)
        {
            arrOversized.add(new Elem(i));
        }

        // null-bearing lists: ids 0,1,<null>,3 at indices 0,1,2,3.
        for (int i = 0; i < NULL_LIST_LEN; ++i)
        {
            if (i == NULL_AT)
            {
                arrWithNull.add(null);
                linkWithNull.add(null);
            }
            else
            {
                arrWithNull.add(new Elem(i));
                linkWithNull.add(new Elem(i));
            }
        }

        linkSingle.add(new Elem(0));

        for (int i = 0; i < BIG; ++i)
        {
            linkBig.add(new Elem(i));
        }

        // size-2 lists.
        for (int i = 0; i < TWO; ++i)
        {
            arrTwo.add(new Elem(i));
            linkTwo.add(new Elem(i));
        }

        // duplicate-VALUE ArrayList: DUP_LEN distinct Elem objects, every one
        // carrying the SAME id/tag (DUP_VAL).  Equal values, distinct identities.
        for (int i = 0; i < DUP_LEN; ++i)
        {
            arrDup.add(new Elem(DUP_VAL));
        }

        // Elem[] object array (the '[L' array branch): ids 0..OBJ_ARR_LEN-1.
        for (int i = 0; i < OBJ_ARR_LEN; ++i)
        {
            elemArray[i] = new Elem(i);
        }

        // nested List-of-Lists: NESTED_OUTER inner lists, each with ids
        // 0..NESTED_INNER-1.  Alternate inner concrete types so the inner re-walk
        // is proven against both backing stores (ArrayList and LinkedList).
        for (int j = 0; j < NESTED_OUTER; ++j)
        {
            final List<Elem> inner = (j % 2 == 0)
                ? new ArrayList<Elem>()
                : new LinkedList<Elem>();
            for (int i = 0; i < NESTED_INNER; ++i)
            {
                inner.add(new Elem(i));
            }
            nested.add(inner);
        }

        // JDK Collections wrappers.  Their elements are reachable by a direct
        // backing-field walk on the native side (Arrays$ArrayList.a Object[],
        // SingletonList.element, UnmodifiableList.list), so the module decodes
        // them WITHOUT a Java get(int) call.
        final Elem[] asListElems = new Elem[ASLIST_LEN];
        for (int i = 0; i < ASLIST_LEN; ++i)
        {
            asListElems[i] = new Elem(i);
        }
        asListView = Arrays.asList(asListElems);                 // Arrays$ArrayList
        emptyImmutable = Collections.<Elem>emptyList();          // EmptyList
        singletonView = Collections.singletonList(new Elem(SINGLETON_ID));
        unmodifiableView = Collections.unmodifiableList(arrMany);// wraps arrMany
        synchronizedView = Collections.synchronizedList(arrMany);// backing "c"

        // Vector / Stack: "elementData" + "elementCount".  vecMany grows 10->20 so
        // size(12) != elementData.length(20); vecOversized is pre-sized to cap 100.
        for (int i = 0; i < VEC_MANY; ++i)
        {
            vecMany.add(new Elem(i));
            vecOversized.add(new Elem(i));
            stackMany.push(new Elem(i));
        }
        for (int i = 0; i < NULL_LIST_LEN; ++i)
        {
            vecWithNull.add(i == NULL_AT ? null : new Elem(i));
        }
        for (int i = 0; i < TWO; ++i)
        {
            vecTwo.add(new Elem(i));
        }

        // CopyOnWriteArrayList: backing "array" whose length IS the size.  COW
        // permits a null element (unlike List.of), so cowWithNull has a null slot.
        for (int i = 0; i < VEC_MANY; ++i)
        {
            cowMany.add(new Elem(i));
        }
        for (int i = 0; i < NULL_LIST_LEN; ++i)
        {
            cowWithNull.add(i == NULL_AT ? null : new Elem(i));
        }
        for (int i = 0; i < TWO; ++i)
        {
            cowTwo.add(new Elem(i));
        }

        // Boxed-Integer element lists: ids == values 0..INT_LEN-1.
        for (int i = 0; i < INT_LEN; ++i)
        {
            intArrList.add(Integer.valueOf(i));
            intVecList.add(Integer.valueOf(i));
        }

        // String-element list: "s0".."s{STR_LEN-1}".
        for (int i = 0; i < STR_LEN; ++i)
        {
            strList.add("s" + i);
        }

        // Boxed-Long list: element k == LONG_BASE + k (every value > 2^32).
        for (int i = 0; i < LONG_LEN; ++i)
        {
            longArrList.add(Long.valueOf(LONG_BASE + i));
        }

        // Enum-element list: constants in ordinal order (RED, GREEN, BLUE).
        for (final Color c : Color.values())
        {
            enumList.add(c);
        }

        // Extra-size Elem lists (ids 0..size-1) on both backing families.
        for (int i = 0; i < TEN; ++i)
        {
            arrTen.add(new Elem(i));
        }
        for (int i = 0; i < SIXTEEN; ++i)
        {
            arrSixteen.add(new Elem(i));
        }
        for (int i = 0; i < THOUSAND; ++i)
        {
            arrThousand.add(new Elem(i));
            linkThousand.add(new Elem(i));
        }

        // All-null lists: every slot null in both backing families.
        for (int i = 0; i < NULL_PATTERN_LEN; ++i)
        {
            arrAllNull.add(null);
            linkAllNull.add(null);
        }

        // Null at the head boundary (index 0 null, rest id == index).
        for (int i = 0; i < NULL_PATTERN_LEN; ++i)
        {
            arrNullFirst.add(i == 0 ? null : new Elem(i));
            linkNullFirst.add(i == 0 ? null : new Elem(i));
        }

        // Null at the tail boundary (last slot null, rest id == index).
        for (int i = 0; i < NULL_PATTERN_LEN; ++i)
        {
            arrNullLast.add(i == NULL_PATTERN_LEN - 1 ? null : new Elem(i));
        }

        // Boxed element types beyond Integer/Long: Boolean / Byte / Short /
        // Character / Double.  Each is BOX_LEN small elements.
        for (int i = 0; i < BOX_LEN; ++i)
        {
            boolArrList.add(Boolean.valueOf(i == 1));      // false,true,false
            byteArrList.add(Byte.valueOf((byte) i));       // 0,1,2
            shortArrList.add(Short.valueOf((short) i));    // 0,1,2
            charArrList.add(Character.valueOf((char) (CHAR_BASE + i))); // a,b,c
            doubleArrList.add(Double.valueOf((double) i)); // 0.0,1.0,2.0
        }

        // Mixed-shape nested list: a Vector inner and a COW inner, each dense.
        {
            final Vector<Elem> innerVec = new Vector<Elem>();
            final CopyOnWriteArrayList<Elem> innerCow = new CopyOnWriteArrayList<Elem>();
            for (int i = 0; i < NESTED_INNER; ++i)
            {
                innerVec.add(new Elem(i));
                innerCow.add(new Elem(i));
            }
            nestedMixed.add(innerVec);
            nestedMixed.add(innerCow);
        }

        // Nested List-of-Map: MAP_OUTER HashMaps, each with MAP_INNER entries.
        for (int j = 0; j < MAP_OUTER; ++j)
        {
            final Map<String, Elem> inner = new HashMap<String, Elem>();
            for (int i = 0; i < MAP_INNER; ++i)
            {
                inner.put("k" + i, new Elem(i));
            }
            nestedMaps.add(inner);
        }

        // subList views over the MANY lists; characterized via published size().
        arrSubList = arrMany.subList(SUB_FROM, SUB_TO);
        linkSubList = linkMany.subList(SUB_FROM, SUB_TO);

        // List.of(...) (JDK 9+) reflectively so this compiles at -source 8.
        buildListOf();

        // Published Java size() witnesses (read by the native side as plain int
        // fields — no Java call from the worker body).
        arrSubListSize = arrSubList.size();
        linkSubListSize = linkSubList.size();
        synchronizedViewSize = synchronizedView.size();
        vecManySize = vecMany.size();
        cowManySize = cowMany.size();

        populated = true;
    }

    /**
     * Builds listOf0..listOfN via reflection on List.of(...) so this fixture still
     * compiles at -source 8 (where List.of does not exist).  On Java 8 the methods
     * are absent -> listOfAvailable stays false and the fields stay null (the
     * native module skips its List.of coverage).  On Java 9+ the immutable lists
     * are built and their size() published.  ListN ("elements" Object[]) IS
     * hand-walked natively; List12 (e0/e1 with an EMPTY sentinel) is characterized.
     * List.of rejects null, so every element here is non-null with id == index.
     */
    @SuppressWarnings("unchecked")
    private void buildListOf()
    {
        try
        {
            final java.lang.reflect.Method of0 = List.class.getMethod("of");
            final java.lang.reflect.Method of1 = List.class.getMethod("of", Object.class);
            final java.lang.reflect.Method of2 =
                List.class.getMethod("of", Object.class, Object.class);
            final java.lang.reflect.Method ofN = List.class.getMethod("of", Object[].class);

            listOf0 = (List<Elem>) of0.invoke(null);
            listOf1 = (List<Elem>) of1.invoke(null, new Elem(0));
            listOf2 = (List<Elem>) of2.invoke(null, new Elem(0), new Elem(1));
            final Elem[] nElems = new Elem[LISTOF_N];
            for (int i = 0; i < LISTOF_N; ++i)
            {
                nElems[i] = new Elem(i);
            }
            listOfN = (List<Elem>) ofN.invoke(null, (Object) nElems);

            listOf0Size = listOf0.size();
            listOf1Size = listOf1.size();
            listOf2Size = listOf2.size();
            listOfNSize = listOfN.size();
            listOfAvailable = true;
        }
        catch (final NoSuchMethodException e)
        {
            // Java 8: List.of does not exist.  Leave fields null, flag unavailable.
            listOf0 = null;
            listOf1 = null;
            listOf2 = null;
            listOfN = null;
            listOfAvailable = false;
        }
        catch (final Throwable t)
        {
            listOfAvailable = false;
        }
    }

    /** The single instance the native module wraps (reached via SINGLETON). */
    public static final CollList SINGLETON = new CollList();

    /**
     * Forces vmhook.fixtures.CollList$Elem to be LOADED at CollList class-init
     * time (which happens when Main.loadFixtures() Class.forName's CollList at
     * JVM startup).  Without this, Elem would not load until the first
     * new Elem(...) inside populate() — and that runs only after the native
     * module already called register_class&lt;elem&gt;("...CollList$Elem"), so
     * find_class would miss and the element wrappers could not resolve their
     * klass.  Instantiating one Elem here guarantees the class is in the loaded
     * graph before the module registers it.  This pinned instance is never put
     * into any list, so it does not perturb any size/order assertion.
     */
    private static final Elem ELEM_CLASS_PIN = new Elem(-1);

    static
    {
        if (ELEM_CLASS_PIN == null)
        {
            throw new IllegalStateException("unreachable");
        }
        // Build the lists once at class-init so they are populated even before
        // the first probe (the native module also drives a build probe so the
        // population definitely ran on the Java thread before it reads).
        SINGLETON.populate();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return CollList.go && !CollList.done;
            }

            @Override
            public void run()
            {
                // Populate the singleton's lists on the Java thread, then signal.
                // The native module reads the now-populated backing stores off
                // the worker thread (no capture-detour needed).
                SINGLETON.populate();
                CollList.done = true;
            }
        });
    }
}
