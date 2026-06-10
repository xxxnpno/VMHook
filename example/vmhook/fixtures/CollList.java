package vmhook.fixtures;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedList;
import java.util.List;

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

        populated = true;
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
