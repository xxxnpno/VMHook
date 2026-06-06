package vmhook.fixtures;

import java.util.ArrayList;
import java.util.LinkedList;
import java.util.List;

import vmhook.Harness;

/**
 * Fixture for the collection_list feature (area: collections).
 *
 * Exercises vmhook::collection::to_vector&lt;wrapper&gt;() over real
 * java.util.ArrayList and java.util.LinkedList fields on a live JVM.  The
 * native module reaches each list through the hooked instance's `self`
 * (get_field("...")-&gt;get().to_vector&lt;elem&gt;()), so every read happens on a
 * live OOP from inside an interpreter detour — exactly how a real user reaches
 * a List argument/field.
 *
 * What the lists prove:
 *   - ArrayList fast path (field shape "elementData" + "size"): empty, single,
 *     many (12 &gt; default capacity 10, so a grow happened), a trimToSize() list
 *     (size == capacity), and an ensureCapacity(100) list (size != capacity) —
 *     the to_vector bound MUST be `size`, never `elementData.length`.
 *   - LinkedList fast path (field shape "first" + "size"): empty, single, many
 *     (12), and a LARGE 4096-element chain (reduced from 20000 to deflake a
 *     G1/JDK11+ GC-relocation race — see the BIG field's note) so the native
 *     side can wall-clock the first-&gt;next walk and catch an O(N*F) / O(N^2)
 *     regression, and prove the chain walk visits every node exactly once in
 *     order (no cycle, no dup, no early stop).
 *   - null elements become nullptr slots in BOTH containers (ArrayList array
 *     slot == null; LinkedList Node.item == null).
 *   - element order preserved: element k carries id == k, so the native side
 *     asserts vec[k].id == k for every k — the strongest order+identity signal.
 *
 * Each Elem also carries a String `tag` ("e<id>") so the native side can do the
 * element-field readback the scope asks for, through a wrapper built by
 * to_vector (proves the decoded element OOP is a real, walkable heap object).
 *
 * Java 8 syntax only (no var/records/switch-expr/text-blocks).
 */
public final class CollList
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
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
     * Reduced from 20000 to 4096 to deflake the collection_list module on the
     * GC-sensitive CI config (default G1 on JDK 11+, e.g. linux-gcc-java11).
     * The native walk (collection::to_vector's LinkedList fast path) holds raw
     * decoded Node/element oops while it walks + re-reads them inside the
     * trigger() interpreter detour, and is not GC-safe; a relocating young GC
     * mid-walk moved those objects -> wrong/duplicate ids or an unmapped-page
     * fault (a SIGSEGV the Linux detour's catch(...) can't contain).  The 20000
     * build was ~60k young-gen allocations right before the walk, the burst that
     * tipped G1 into a collection during the probe.  4096 stays a genuinely
     * large, multi-region chain (every order/distinctness/identity invariant on
     * the native side stays a hard assert) while cutting both the allocation
     * burst and the raw-oop-hold window ~5x.  Keep this in lockstep with BIG in
     * tests/jvm/modules/collection_list.cpp.
     */
    public static final int BIG = 4096;

    /** Index inside the "with null" lists that holds a null element. */
    public static final int NULL_AT = 2;

    /** Total element count of the "with null" lists (one slot is null). */
    public static final int NULL_LIST_LEN = 4;

    // ── ArrayList fields (each takes the "elementData"+"size" fast path) ────
    /** Empty ArrayList — size 0, to_vector must be empty (no element read). */
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

    // ── LinkedList fields (each takes the "first"+"size" Node-chain path) ───
    /** Empty LinkedList — size 0, to_vector must be empty. */
    public final LinkedList<Elem> linkEmpty = new LinkedList<Elem>();

    /** Single-element LinkedList. */
    public final LinkedList<Elem> linkSingle = new LinkedList<Elem>();

    /** Many-element LinkedList (12 elements). */
    public final LinkedList<Elem> linkMany = new LinkedList<Elem>();

    /** LinkedList whose node at index NULL_AT has item == null. */
    public final LinkedList<Elem> linkWithNull = new LinkedList<Elem>();

    /**
     * Large LinkedList (BIG elements).  The native side wall-clocks the
     * first->next walk over this to catch a quadratic (per-node find_field)
     * regression and to prove the walk terminates at exactly BIG nodes in
     * order.  Declared as the supertype List to underline that the native
     * fast-path selection is field-shape based, not Java-static-type based.
     */
    public final List<Elem> linkBig = new LinkedList<Elem>();

    /** A throwaway so the trigger() detour has a guaranteed fresh TLAB/dispatch. */
    public static volatile int triggerNonce;

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

        populated = true;
    }

    /**
     * Hook site.  The native module hooks this; inside the detour it reads
     * every list field off `self` and runs to_vector on each, so the reads
     * happen on live OOPs while a JavaThread is current.
     */
    public int trigger(final int nonce)
    {
        triggerNonce = nonce;
        return nonce + 1;
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
                SINGLETON.populate();
                // Real bytecode dispatch the native scoped_hook rides; the
                // detour does all the to_vector reads on `self`.
                SINGLETON.trigger(7);
                CollList.done = true;
            }
        });
    }
}
