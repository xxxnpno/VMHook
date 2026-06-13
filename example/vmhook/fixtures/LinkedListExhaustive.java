package vmhook.fixtures;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.LinkedList;
import java.util.Queue;

import vmhook.Harness;

/**
 * Companion fixture for the collection_linked_list feature (area: collections).
 *
 * The primary fixture, {@link LinkedListProbe}, is the focused authority for the
 * three-element {@code first -> next} Node-chain walk read three ways (value_t
 * path, typed {@code linked_list} wrapper, and the direct
 * {@code linked_list_walk_items} free function).  This SECOND, DISTINCT fixture
 * makes the "every java.util.LinkedList / node-chain shape read through the
 * library" goal EXHAUSTIVE, WITHOUT touching the mature LinkedListProbe.java /
 * collection_linked_list.cpp pair:
 *
 *   - LinkedList used as a LIST and as a Deque/Queue.  The same
 *     {@code java.util.LinkedList} klass underlies all three roles, so the
 *     {@code first}+{@code size} / no-{@code elementData} field shape — and thus
 *     the dedicated chain walk — is selected regardless of which interface the
 *     code drove.  The Deque/Queue list is built with addFirst/offer/push so its
 *     NODE order (what the chain walk yields) is well-defined and published.
 *
 *   - Element TYPES: String, boxed Integer, boxed Long whose value carries a
 *     non-zero HIGH 32 bits (so a 32-bit misread of Long.value corrupts the
 *     checksum), a real enum (Day: name + ordinal, both on java.lang.Enum), a
 *     user class (Elem: id + tag), and NULL elements (LinkedList permits null,
 *     surfaced as a nullptr slot in walk order).
 *
 *   - SIZES 0, 1, 2, 10, 1000 (String elements, element k == "w<k>"): the full
 *     chain walk with no early stop, no cycle, and no overrun past the tail.  The
 *     1000-node chain is the deep-chain proof: the walk must terminate by SIZE,
 *     never by chasing a null off the end, and must never loop (every decoded
 *     element OOP distinct).
 *
 *   - NODE ORDER != INSERTION ORDER: a list built by interleaving addFirst /
 *     addLast / add(index).  The published {@code interleaveSeq} is the resulting
 *     head->tail node order; the chain walk must reproduce THAT, not the order in
 *     which the elements were inserted.
 *
 *   - AFTER A MIDDLE REMOVE (node unlink): a list whose middle node was removed.
 *     The published {@code afterRemoveSeq} is the surviving head->tail order; the
 *     chain walk must reproduce it exactly (the unlink rewired prev/next, and the
 *     forward walk must follow the new {@code next} links).
 *
 * Every cross-check value (sizes, ordered id sequences, value sums) is published
 * as a static field the native side reads as a plain field — the worker body
 * NEVER calls a Java method (forbidden outside a detour).  Java 8 syntax only
 * (no var / records / switch-expr / text-blocks).  Mirrors the go/done/mode
 * handshake and exposes a hookable {@code touch(int)} so the companion module can
 * also prove an interpreter hook fires through the modular path.
 */
public final class LinkedListExhaustive
{
    /** Native sets this true to request the action; the action clears it. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect of the hookable method (pilot-style proof). */
    public static volatile int observed;

    /**
     * Scenario selector (set by the native module BEFORE raising `go`):
     *   0 = build/refresh every list field (default; also runs once at startup)
     *   1 = call touch() so the interpreter hook fires on real dispatch
     */
    public static volatile int mode;

    /**
     * User element type for the user-class scenario.  `id` drives the order /
     * identity checks; `tag` is "e&lt;id&gt;" for the reference-field readback.
     */
    public static final class Elem
    {
        public final int id;
        public final String tag;

        public Elem(final int id)
        {
            this.id = id;
            this.tag = "e" + id;
        }
    }

    /**
     * A small enum for the REAL enum-element decode.  Each constant is an
     * ordinary heap object with a String `name` and an int `ordinal` (both on
     * java.lang.Enum), which the native side reads back from each decoded
     * element OOP, in chain (insertion) order.
     */
    public enum Day
    {
        MON, TUE, WED, THU, FRI
    }

    // ---- Scenario sizes (mirrored on the native side) ----------------------

    public static final int SIZE0 = 0;
    public static final int SIZE1 = 1;
    public static final int SIZE2 = 2;
    public static final int SIZE10 = 10;
    public static final int THOUSAND = 1000;

    /** java.lang.Integer element count for the boxed-int list. */
    public static final int INT_N = 10;

    /** java.lang.Long element count for the boxed-long list. */
    public static final int LONG_N = 10;

    /** Number of Day constants in the enum list. */
    public static final int DAY_N = 5;

    /** Length of the user-class (Elem) list. */
    public static final int ELEM_N = 8;

    /** Length of the null-bearing list; index NULL_AT holds null. */
    public static final int NULL_LEN = 5;
    public static final int NULL_AT = 2;

    // ---- The LinkedList fields the native module reads via to_vector --------

    /** Empty (size 0): the chain walk must return an empty vector. */
    public static LinkedList<String> words0 = new LinkedList<String>();

    /** Single element. */
    public static LinkedList<String> words1 = new LinkedList<String>();

    /** Two elements (smallest "more than one"). */
    public static LinkedList<String> words2 = new LinkedList<String>();

    /** Ten elements. */
    public static LinkedList<String> words10 = new LinkedList<String>();

    /** A deep 1000-node chain: terminate by size, never loop, never overrun. */
    public static LinkedList<String> words1000 = new LinkedList<String>();

    /** Boxed java.lang.Integer elements, value k == index k. */
    public static LinkedList<Integer> ints = new LinkedList<Integer>();

    /** Boxed java.lang.Long elements with a non-zero HIGH 32 bits. */
    public static LinkedList<Long> longs = new LinkedList<Long>();

    /** Real enum constants (insertion order MON..FRI). */
    public static LinkedList<Day> days = new LinkedList<Day>();

    /** User-class Elem elements, id k == index k. */
    public static LinkedList<Elem> elems = new LinkedList<Elem>();

    /** A list that contains a null element (LinkedList permits null). */
    public static LinkedList<String> withNull = new LinkedList<String>();

    /**
     * Driven as a {@link Deque}: pushes/offers leave a well-defined NODE order.
     * Declared as the Deque interface to prove the role does not change the
     * decoded backing shape (still {@code first}+{@code size}).
     */
    public static Deque<String> deque = new LinkedList<String>();

    /**
     * Driven as a {@link Queue}: FIFO offer() leaves head->tail == insertion
     * order.  Declared as the Queue interface for the same reason as `deque`.
     */
    public static Queue<String> queue = new LinkedList<String>();

    /**
     * Built by interleaving addFirst / addLast / add(index): the resulting NODE
     * order differs from the insertion order.  The chain walk must reproduce the
     * NODE order ({@link #interleaveSeq}), not the order things were added.
     */
    public static LinkedList<Integer> interleave = new LinkedList<Integer>();

    /**
     * Built, then a MIDDLE node removed (unlink).  The chain walk must reproduce
     * the surviving head->tail order ({@link #afterRemoveSeq}).
     */
    public static LinkedList<Integer> afterRemove = new LinkedList<Integer>();

    /** A declared field deliberately left NULL (to_vector must stay empty). */
    public static LinkedList<String> nullList = null;

    // ---- Published cross-check values (size oracle + ordered sequences) -----

    public static volatile int words0Size;
    public static volatile int words1Size;
    public static volatile int words2Size;
    public static volatile int words10Size;
    public static volatile int words1000Size;
    public static volatile int intsSize;
    public static volatile int longsSize;
    public static volatile int daysSize;
    public static volatile int elemsSize;
    public static volatile int withNullSize;
    public static volatile int dequeSize;
    public static volatile int queueSize;
    public static volatile int interleaveSize;
    public static volatile int afterRemoveSize;

    /** Sum of all boxed-int values (order-independent value oracle). */
    public static volatile long intsValSum;

    /** Sum / xor of all boxed-long values (high word must survive). */
    public static volatile long longsValSum;
    public static volatile long longsValXor;

    /** Sum of enum ordinals (closed form 0+1+2+3+4 == 10). */
    public static volatile long daysOrdinalSum;

    /**
     * Head->tail NODE order of the interleave list, as a comma-joined string of
     * the Integer values (e.g. "30,10,20,40,...").  The native side joins the
     * decoded values the same way and compares.
     */
    public static volatile String interleaveSeq = "";

    /** Head->tail order of the after-remove list, comma-joined Integer values. */
    public static volatile String afterRemoveSeq = "";

    /** Head->tail order of the Deque list, comma-joined element strings. */
    public static volatile String dequeSeq = "";

    /** Head->tail order of the Queue list, comma-joined element strings. */
    public static volatile String queueSeq = "";

    // ---- Hookable method (pilot-style interpreter-hook proof) --------------

    private int seed = 8000;

    public int touch(final int delta)
    {
        return this.seed + delta;
    }

    // ---- Builders ----------------------------------------------------------

    private static String joinInts(final LinkedList<Integer> list)
    {
        final StringBuilder sb = new StringBuilder();
        boolean first = true;
        for (final Integer v : list)
        {
            if (!first)
            {
                sb.append(',');
            }
            sb.append(v.intValue());
            first = false;
        }
        return sb.toString();
    }

    private static String joinStrings(final Iterable<String> list)
    {
        final StringBuilder sb = new StringBuilder();
        boolean first = true;
        for (final String v : list)
        {
            if (!first)
            {
                sb.append(',');
            }
            sb.append(v);
            first = false;
        }
        return sb.toString();
    }

    private static void buildAll()
    {
        // ---- String lists of sizes 0, 1, 2, 10, 1000: element k == "w<k>". ----
        words0 = new LinkedList<String>();

        words1 = new LinkedList<String>();
        words1.add("w0");

        words2 = new LinkedList<String>();
        words2.add("w0");
        words2.add("w1");

        words10 = new LinkedList<String>();
        for (int i = 0; i < SIZE10; ++i)
        {
            words10.add("w" + i);
        }

        words1000 = new LinkedList<String>();
        for (int i = 0; i < THOUSAND; ++i)
        {
            words1000.add("w" + i);
        }

        // ---- Boxed Integer: value k == index k. ----
        ints = new LinkedList<Integer>();
        long intSum = 0;
        for (int i = 0; i < INT_N; ++i)
        {
            ints.add(Integer.valueOf(i));
            intSum += i;
        }
        intsValSum = intSum;

        // ---- Boxed Long with a non-zero high 32 bits. ----
        longs = new LinkedList<Long>();
        long lSum = 0;
        long lXor = 0;
        for (int i = 0; i < LONG_N; ++i)
        {
            final long v = 0x1_0000_0000L + i;
            longs.add(Long.valueOf(v));
            lSum += v;
            lXor ^= v;
        }
        longsValSum = lSum;
        longsValXor = lXor;

        // ---- Real enum constants in natural insertion order. ----
        days = new LinkedList<Day>();
        long ordSum = 0;
        for (final Day d : Day.values())
        {
            days.add(d);
            ordSum += d.ordinal();
        }
        daysOrdinalSum = ordSum;

        // ---- User class Elem: id k == index k. ----
        elems = new LinkedList<Elem>();
        for (int i = 0; i < ELEM_N; ++i)
        {
            elems.add(new Elem(i));
        }

        // ---- Null-bearing list: index NULL_AT holds null, others "w<k>". ----
        withNull = new LinkedList<String>();
        for (int i = 0; i < NULL_LEN; ++i)
        {
            withNull.add(i == NULL_AT ? null : ("w" + i));
        }

        // ---- Deque role: build a deterministic node order via Deque ops. ----
        // addFirst("b"), addFirst("a") -> [a, b]; addLast("c") -> [a, b, c];
        // push("z") puts z at the HEAD -> [z, a, b, c]; offerLast("d") -> tail.
        // Final head->tail node order: z, a, b, c, d.
        final LinkedList<String> dq = new LinkedList<String>();
        final Deque<String> dqv = dq;
        dqv.addFirst("b");
        dqv.addFirst("a");
        dqv.addLast("c");
        dqv.push("z");
        dqv.offerLast("d");
        deque = dq;
        dequeSeq = joinStrings(dq);

        // ---- Queue role: FIFO offer() -> head..tail == insertion order. ----
        final LinkedList<String> q = new LinkedList<String>();
        final Queue<String> qv = q;
        qv.offer("q0");
        qv.offer("q1");
        qv.offer("q2");
        queue = q;
        queueSeq = joinStrings(q);

        // ---- Interleave: NODE order != insertion order. ----
        // add(10) -> [10]; addFirst(30) -> [30,10]; addLast(40) -> [30,10,40];
        // add(1, 20) -> [30,20,10,40]; addFirst(50) -> [50,30,20,10,40].
        interleave = new LinkedList<Integer>();
        interleave.add(Integer.valueOf(10));
        interleave.addFirst(Integer.valueOf(30));
        interleave.addLast(Integer.valueOf(40));
        interleave.add(1, Integer.valueOf(20));
        interleave.addFirst(Integer.valueOf(50));
        interleaveSeq = joinInts(interleave);   // "50,30,20,10,40"

        // ---- After a middle remove (node unlink). ----
        // Build [0,1,2,3,4,5,6], then remove the element at index 3 (value 3):
        // surviving head->tail order is [0,1,2,4,5,6].
        afterRemove = new LinkedList<Integer>();
        for (int i = 0; i < 7; ++i)
        {
            afterRemove.add(Integer.valueOf(i));
        }
        afterRemove.remove(3);   // unlink the middle node
        afterRemoveSeq = joinInts(afterRemove);   // "0,1,2,4,5,6"

        nullList = null;

        // ---- Publish sizes (the count==size oracle). ----
        words0Size = words0.size();
        words1Size = words1.size();
        words2Size = words2.size();
        words10Size = words10.size();
        words1000Size = words1000.size();
        intsSize = ints.size();
        longsSize = longs.size();
        daysSize = days.size();
        elemsSize = elems.size();
        withNullSize = withNull.size();
        dequeSize = deque.size();
        queueSize = queue.size();
        interleaveSize = interleave.size();
        afterRemoveSize = afterRemove.size();
    }

    /**
     * Force vmhook.fixtures.LinkedListExhaustive$Elem and $Day to LOAD at
     * class-init time so the native register_class for the element types finds
     * the klass already present.  Never put into any walked list.
     */
    private static final Elem ELEM_CLASS_PIN = new Elem(-999);
    private static final Day DAY_CLASS_PIN = Day.MON;

    static
    {
        if (ELEM_CLASS_PIN == null || DAY_CLASS_PIN == null)
        {
            throw new IllegalStateException("unreachable");
        }

        // Build once at class-init so the lists are populated even before the
        // first probe (the native module also re-requests a build via mode 0).
        buildAll();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return LinkedListExhaustive.go && !LinkedListExhaustive.done;
            }

            @Override
            public void run()
            {
                if (LinkedListExhaustive.mode == 1)
                {
                    final LinkedListExhaustive instance = new LinkedListExhaustive();
                    LinkedListExhaustive.observed = instance.touch(42);
                }
                else
                {
                    buildAll();
                }
                LinkedListExhaustive.done = true;
            }
        });
    }
}
