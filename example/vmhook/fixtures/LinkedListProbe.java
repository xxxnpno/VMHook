package vmhook.fixtures;

import java.util.LinkedList;

import vmhook.Harness;

/**
 * Fixture for the collection_linked_list feature (area: collections).
 *
 * The single, focused authority for vmhook's LinkedList Node-chain walk: it
 * publishes ONE {@code java.util.LinkedList<String>} populated with exactly
 * three known String elements in a known insertion order, so the native module
 * can prove that
 *
 *     get_field("words")-&gt;get().to_vector&lt;elem&gt;()
 *     // and, equivalently,
 *     std::unique_ptr&lt;vmhook::linked_list&gt; ll = get_field("words")-&gt;get();
 *     ll-&gt;to_vector&lt;elem&gt;();
 *
 * takes the LinkedList-specific {@code first -> next} Node-chain fast path
 * (selected by the live OOP's field shape "first"+"size") and NOT the generic
 * {@code List.get(int)} O(N^2) fallback, returning the three elements as
 * valid, non-null wrappers in insertion order with the expected content.
 *
 * Why a LinkedList of String (not a custom Elem like CollList): the scope is
 * the chain-walk routing + element content, and java.lang.String is loaded
 * from the very first moment of JVM life, so the element wrappers' content can
 * be read with vmhook::read_java_string(elem-&gt;get_instance()) WITHOUT the
 * native side having to register any element klass (it only registers the host
 * LinkedListProbe so get_field can resolve the field).  This keeps the fixture
 * minimal while still proving "the decoded element OOP is a real, walkable heap
 * object" — a String whose value the native side reads back exactly.
 *
 * Shape mirrors CollList / FieldObjectRef: an eagerly-created SINGLETON the
 * native module reaches through a static field; a trigger() hook site whose
 * real bytecode dispatch makes the interpreter hook fire (so all reads happen
 * on a live OOP while a JavaThread is current); a go/done handshake; and the
 * Java-observed size republished so the C++ side can cross-check it against the
 * size the native walk produced.
 *
 * Java 8 syntax only (no var / records / switch-expr / text-blocks): the Probe
 * is an anonymous inner class, exactly as required.
 */
public final class LinkedListProbe
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ── The three known elements, in insertion order ───────────────────────
    // Distinct, non-empty ASCII so read_java_string is length-preserving on
    // every JDK (8/11/17/21/24/25) and the order check is unambiguous.
    /** Element expected at index 0. */
    public static final String WORD0 = "alpha";

    /** Element expected at index 1. */
    public static final String WORD1 = "bravo";

    /** Element expected at index 2. */
    public static final String WORD2 = "charlie";

    /** The known element count the native side mirrors. */
    public static final int EXPECTED_SIZE = 3;

    /**
     * The LinkedList the native side walks.  Exactly three String elements,
     * added in order WORD0, WORD1, WORD2, so the Node chain is
     * first -> "alpha" -> "bravo" -> "charlie" -> null and the native
     * first->next walk must yield them in that order.
     */
    public final LinkedList<String> words = new LinkedList<String>();

    // ── Additional LinkedList SHAPES the native side walks ──────────────────
    // Every list is a java.util.LinkedList&lt;String&gt; so the SAME Node-chain
    // (first -&gt; item/next) walk applies to all of them; only the contents and
    // length differ.  This lets the native module prove the walk over empty /
    // single / many / null-bearing / duplicate / empty-string shapes.

    /** Empty LinkedList: size 0, first == null.  Walk must yield 0 elements. */
    public final LinkedList<String> emptyList = new LinkedList<String>();

    /** Single-element LinkedList: first -&gt; "solo" -&gt; null. */
    public final LinkedList<String> singleList = new LinkedList<String>();

    /**
     * Null-bearing LinkedList: first -&gt; null -&gt; "mid" -&gt; null -&gt; null.
     * The chain has real Nodes at every position; the Node.item reference is
     * what is null, so the native walk must preserve each null slot as a
     * nullptr wrapper WITHOUT desyncing the first-&gt;next traversal.
     */
    public final LinkedList<String> nullList = new LinkedList<String>();

    /**
     * Duplicate-element LinkedList: "dup","dup","dup".  A List keeps every
     * occurrence (unlike a Set), so the walk must return all three.
     */
    public final LinkedList<String> dupList = new LinkedList<String>();

    /**
     * Empty-string + normal element: first -&gt; "" -&gt; "tail" -&gt; null.
     * read_java_string returns "" for a legitimately empty Java String; this
     * shape asserts an empty element is still a non-null, pointer-valid wrapper
     * (distinct from a null slot) whose decoded content is exactly "".
     */
    public final LinkedList<String> emptyStrList = new LinkedList<String>();

    /** The element count of {@link #manyList}, mirrored to the native side. */
    public static final int MANY_SIZE = 16;

    /**
     * A modest "many" LinkedList: element k is the decimal string of k, for
     * k in [0, MANY_SIZE).  Long enough that a node-chain walk visiting each
     * link in order is meaningfully exercised, small enough to stay well under
     * the test JVM heap lever.
     */
    public final LinkedList<String> manyList = new LinkedList<String>();

    /** Java-side observed size of {@link #words}, republished each probe run. */
    public static volatile int observedSize;

    /** Java-side observed sizes of the additional shapes (cross-checks). */
    public static volatile int observedEmptySize;
    public static volatile int observedSingleSize;
    public static volatile int observedNullSize;
    public static volatile int observedDupSize;
    public static volatile int observedEmptyStrSize;
    public static volatile int observedManySize;

    /** A nonce the trigger detour writes, guaranteeing fresh bytecode dispatch. */
    public static volatile int triggerNonce;

    private static volatile boolean populated;

    private void populate()
    {
        if (populated)
        {
            return;
        }
        words.add(WORD0);
        words.add(WORD1);
        words.add(WORD2);

        // emptyList stays empty on purpose.

        singleList.add("solo");

        nullList.add(null);
        nullList.add("mid");
        nullList.add(null);
        nullList.add(null);

        dupList.add("dup");
        dupList.add("dup");
        dupList.add("dup");

        emptyStrList.add("");
        emptyStrList.add("tail");

        for (int k = 0; k < MANY_SIZE; k++)
        {
            manyList.add(Integer.toString(k));
        }

        populated = true;
    }

    /**
     * Hook site.  The native module hooks this; calling it through real
     * bytecode dispatch is what makes the interpreter hook fire.  Every
     * LinkedList read in the module happens from native code against the
     * published SINGLETON, so the detour body itself only needs to exist.
     */
    public int trigger(final int nonce)
    {
        triggerNonce = nonce;
        return nonce + 1;
    }

    /** The single instance the native module wraps (reached via SINGLETON). */
    public static final LinkedListProbe SINGLETON = new LinkedListProbe();

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return LinkedListProbe.go && !LinkedListProbe.done;
            }

            @Override
            public void run()
            {
                SINGLETON.populate();
                LinkedListProbe.observedSize = SINGLETON.words.size();
                LinkedListProbe.observedEmptySize = SINGLETON.emptyList.size();
                LinkedListProbe.observedSingleSize = SINGLETON.singleList.size();
                LinkedListProbe.observedNullSize = SINGLETON.nullList.size();
                LinkedListProbe.observedDupSize = SINGLETON.dupList.size();
                LinkedListProbe.observedEmptyStrSize = SINGLETON.emptyStrList.size();
                LinkedListProbe.observedManySize = SINGLETON.manyList.size();
                // Real bytecode dispatch the native scoped_hook rides; the
                // detour is where the native side does its LinkedList reads.
                SINGLETON.trigger(7);
                LinkedListProbe.done = true;
            }
        });
    }
}
