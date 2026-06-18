package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_arrays_object feature (area: fields).
 *
 * Exercises READING Java reference arrays out of object / static fields, across
 * every element kind, every dimensionality, and every null/empty/large shape:
 *
 *   - String[]                       (signature "[Ljava/lang/String;")
 *   - Item[]   (registered wrapper)  (signature "[Lvmhook/fixtures/FieldArraysObject$Item;")
 *   - Object[] (erased element)      (signature "[Ljava/lang/Object;")
 *   - Integer[] (boxed primitive)    (signature "[Ljava/lang/Integer;")
 *   - Tagged[] (interface element)   (signature "[Lvmhook/fixtures/FieldArraysObject$Tagged;")
 *   - Item[][] / String[][]          (2-D, signatures "[[L...")
 *   - Object[][][]                   (3-D, signature "[[[Ljava/lang/Object;")
 *   - a field typed plain Object that HOLDS an Item[] (array covariance: the
 *     declared field type is "Ljava/lang/Object;" but the runtime oop is an array)
 *
 * through the C++ read paths the native module drives:
 *
 *   (a) the field_proxy implicit-conversion operator into std::vector<std::string>
 *       for String[] (lands in read_array_value -> append_array_value(vector<string>)),
 *   (b) field_proxy::value_t::to_vector<Item>() for the Object[] / Item[] wrapper
 *       path.  This WORKS today: to_vector<T>() branches on the field signature and,
 *       for a "[L.../"[[..." array field, walks the raw array directly (each
 *       non-null slot -> make_unique<Item>, each null slot -> nullptr).  Only plain
 *       "L...;" collection fields fall through to collection::to_vector.  (An
 *       earlier revision mis-routed every Object[] through collection::to_vector and
 *       silently returned an EMPTY vector; the signature branch fixed that.)
 *   (c) a manual decode_array_oop + per-element get_array_element walk that proves
 *       the array DATA is reachable, that inner nulls are distinguishable as real
 *       nullptr slots, and (for the multi-dim cases) that an inner ROW oop is itself
 *       a walkable array — exercising jagged inner lengths and 2-D/3-D descent.
 *
 * Coverage shapes, for BOTH String[] and Item[] (and, where it adds signal, the
 * other element kinds):
 *   - canonical multi-element (all non-null),
 *   - EMPTY array (length 0),
 *   - SINGLE element,
 *   - ALL-null elements (every slot null),
 *   - MIXED null / non-null (interleaved),
 *   - leading-null and trailing-null edge layouts,
 *   - a genuinely null field (the array reference itself is null),
 *   - a LARGE array (length 1000) to stress the per-element loop and the reserve,
 *   - jagged 2-D rows of differing inner width,
 *   - instance (non-static) variants of the canonical + mixed cases.
 *
 * Every element's value (String contents, Item.tag, or Integer.value) and
 * null-ness is published so the native side can verify element COUNT and each
 * element's value / null-ness exactly.  Java 8 syntax only.
 */
public final class FieldArraysObject
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect proving a real bytecode dispatch fired. */
    public static volatile int observed;

    /**
     * Interface element type, so a field can be declared {@code Tagged[]}
     * (signature "[Lvmhook/fixtures/FieldArraysObject$Tagged;") while its runtime
     * elements are concrete {@link Item}s — the array-of-interface / covariance
     * case.  {@link Item} implements it, so the SAME native item_object wrapper
     * decodes a {@code Tagged[]} slot and reads its tag.
     */
    public interface Tagged
    {
        int getTag();
    }

    /**
     * Registered-wrapper element type for the Object[] / Item[] / Tagged[] arrays.
     * Mirrors the MethodObject$Child shape: a readable int field plus a callable
     * method, so the native side can prove a decoded element is a real, usable
     * object (read tag through the wrapper AND call getTag() through it).
     */
    public static final class Item implements Tagged
    {
        public int tag;

        public Item(final int tag)
        {
            this.tag = tag;
        }

        @Override
        public int getTag()
        {
            return this.tag;
        }
    }

    /**
     * NON-final polymorphic base for the inherited-element / polymorphic-array
     * coverage.  {@code Node} carries an INHERITED {@code tag} field and a
     * {@code kind()} method; {@link Leaf} extends it and adds its OWN {@code leaf}
     * field plus overrides {@code kind()}.  A {@code Node[]} can therefore hold a
     * runtime MIX of {@code Node} and {@code Leaf} instances (covariance +
     * polymorphism), and the native side reads the inherited {@code tag} through a
     * single wrapper regardless of the concrete element class — the
     * inherited-field-through-a-polymorphic-array-slot case the final {@link Item}
     * cannot express.  Both implement {@link Tagged} so the same item_object
     * wrapper also decodes them.
     */
    public static class Node implements Tagged
    {
        public int tag;   // INHERITED field — read identically on Node and Leaf.

        public Node(final int tag)
        {
            this.tag = tag;
        }

        @Override
        public int getTag()
        {
            return this.tag;
        }

        public int kind()
        {
            return 1;   // base discriminator.
        }
    }

    /** Subclass of {@link Node}: inherits {@code tag}, adds its own {@code leaf}
        field, overrides {@code kind()}.  Proves a decoded array slot reads the
        BASE field even when the runtime object is the derived type. */
    public static final class Leaf extends Node
    {
        public int leaf;

        public Leaf(final int tag, final int leaf)
        {
            super(tag);
            this.leaf = leaf;
        }

        @Override
        public int kind()
        {
            return 2;   // derived discriminator (override dispatch).
        }
    }

    // ---- String[] fields --------------------------------------------------

    /** Canonical 3-element String[], all non-null. */
    public static volatile String[] staticStrings = { "alpha", "beta", "gamma" };

    /** Empty String[] (length 0). */
    public static volatile String[] emptyStrings = new String[0];

    /** Single-element String[]. */
    public static volatile String[] singleString = { "solo" };

    /** All-null String[] (3 slots, every one null). */
    public static volatile String[] allNullStrings = new String[3];

    /** Mixed null / non-null String[]: { "x", null, "z" }. */
    public static volatile String[] mixedStrings = { "x", null, "z" };

    /** Leading-null layout: { null, "b", "c" }. */
    public static volatile String[] leadingNullStrings = { null, "b", "c" };

    /** Trailing-null layout: { "a", "b", null }. */
    public static volatile String[] trailingNullStrings = { "a", "b", null };

    /** Larger mixed array to exercise the per-element loop at length 6. */
    public static volatile String[] bigMixedStrings =
        { "one", null, "three", null, "five", null };

    /** A null String[] reference (the array itself is null, not its elements). */
    public static volatile String[] nullStringArray = null;

    /** Instance String[] (non-static read path), all non-null. */
    public volatile String[] instStrings = { "inst0", "inst1" };

    /** Instance mixed String[]: { null, "mid", null }. */
    public volatile String[] instMixedStrings = { null, "mid", null };

    // ---- Item[] (Object[] registered-wrapper) fields ----------------------

    /** Canonical 3-element Item[], all non-null. */
    public static volatile Item[] staticItems =
        { new Item(10), new Item(20), new Item(30) };

    /** Empty Item[] (length 0). */
    public static volatile Item[] emptyItems = new Item[0];

    /** Single-element Item[]. */
    public static volatile Item[] singleItem = { new Item(99) };

    /** All-null Item[] (3 slots, every one null). */
    public static volatile Item[] allNullItems = new Item[3];

    /** Mixed null / non-null Item[]: { Item(1), null, Item(3) }. */
    public static volatile Item[] mixedItems = { new Item(1), null, new Item(3) };

    /** Leading-null Item[]: { null, Item(5), Item(6) }. */
    public static volatile Item[] leadingNullItems = { null, new Item(5), new Item(6) };

    /** Trailing-null Item[]: { Item(7), Item(8), null }. */
    public static volatile Item[] trailingNullItems = { new Item(7), new Item(8), null };

    /** A null Item[] reference (the array itself is null). */
    public static volatile Item[] nullItemArray = null;

    /** Instance Item[] (non-static), all non-null. */
    public volatile Item[] instItems = { new Item(41), new Item(42) };

    /** Instance mixed Item[]: { Item(51), null }. */
    public volatile Item[] instMixedItems = { new Item(51), null };

    // ---- RAW Object[] fields (declared type Object[], so the JVM signature is
    //      "[Ljava/lang/Object;" rather than "[L...Item;") ------------------
    // These prove the wrapper layer reads a generic Object[] field — whose
    // ERASED element type is java.lang.Object — by re-wrapping each non-null
    // slot as the concrete element type the caller asks for.  The runtime
    // elements are still Items, so the decoded wrappers read their tags.

    /** Canonical Object[] holding Items: declared type Object[]. */
    public static volatile Object[] objectItems =
        { new Item(60), new Item(70), new Item(80) };

    /** Mixed Object[] holding Items + a null: { Item(61), null, Item(63) }. */
    public static volatile Object[] objectMixed =
        { new Item(61), null, new Item(63) };

    /** A null Object[] reference (the array itself is null). */
    public static volatile Object[] nullObjectArray = null;

    // ---- 2D arrays (signature begins "[[", so the native to_vector<T>()
    //      signature branch must walk the OUTER array; each element is an
    //      INNER array OOP, re-wrapped as the requested element type) ---------
    // The native side does not descend into the inner rows here (that needs a
    // vector-of-vectors API the library does not expose); instead it proves the
    // OUTER dimension is read with the right COUNT and per-row null-ness, and
    // that an inner-null ROW decodes to a real nullptr slot.  This exercises the
    // signature[1] == '[' arm of the array walk.

    /** Canonical 2D Item[][]: 3 non-null rows of varying width. */
    public static volatile Item[][] grid2d =
        { { new Item(1) }, { new Item(2), new Item(3) }, { new Item(4) } };

    /** 2D Item[][] with a NULL middle row: { row, null, row }. */
    public static volatile Item[][] grid2dMixed =
        { { new Item(11) }, null, { new Item(12), new Item(13) } };

    /** Empty 2D Item[][] (outer length 0). */
    public static volatile Item[][] grid2dEmpty = new Item[0][];

    /** Single-row 2D Item[][]: one row of one element (degenerate 2-D shape). */
    public static volatile Item[][] grid2dSingle = { { new Item(91) } };

    /** 2D Item[][] with EVERY outer row null: { null, null }. */
    public static volatile Item[][] grid2dAllNullRows = new Item[2][];

    /** 2D Item[][] with an EMPTY (length-0) inner row in the middle:
        { row, emptyRow, row }.  The empty row is non-null but its inner length
        is 0 — distinct from a null row. */
    public static volatile Item[][] grid2dEmptyRow =
        { { new Item(92) }, new Item[0], { new Item(93) } };

    // ---- JAGGED 2D Item[][] for explicit inner-row descent ----------------
    // The native side walks the OUTER array, then walks each non-null ROW oop as
    // an array in its own right (array_length + get_array_element per inner slot),
    // asserting the differing inner widths {1, 2, 3} and the inner Item tags.

    /** Jagged Item[][]: rows of width 1 / 2 / 3 with unique tags per cell. */
    public static volatile Item[][] jaggedGrid =
        {
            { new Item(201) },
            { new Item(202), new Item(203) },
            { new Item(204), new Item(205), new Item(206) },
        };

    // ---- 2D String[][] -----------------------------------------------------
    // Same OUTER-walk + inner-row descent, but the inner element is a String OOP,
    // bridging the String[] read with the multi-dim object-array walk.  Includes a
    // jagged inner-width layout and a NULL middle row.

    /** Jagged String[][]: row widths 1 / 2, inner Strings. */
    public static volatile String[][] strGrid2d =
        { { "r0c0" }, { "r1c0", "r1c1" } };

    /** String[][] with a NULL middle row: { row, null, row }. */
    public static volatile String[][] strGrid2dMixed =
        { { "a" }, null, { "b", "c" } };

    // ---- 3D Object[][][] ---------------------------------------------------
    // Signature begins "[[[", so to_vector<T>() / the manual walk take the
    // signature[1]=='[' arm and read the OUTERMOST dimension; each element is a
    // 2-D Object[][] oop.  Proves the array walk is dimensionality-agnostic.

    /** Canonical 3D Object[][][]: 2 outer planes. */
    public static volatile Object[][][] cube3d =
        {
            { { new Item(301), new Item(302) }, { new Item(303) } },
            { { new Item(304) } },
        };

    // ---- Integer[] (BOXED primitive) ---------------------------------------
    // Declared java.lang.Integer[], so each non-null element is a boxed Integer
    // OOP whose `value` int field + intValue() the native side reads through a
    // java/lang/Integer wrapper.  Includes an interleaved null (boxing makes a
    // null element legal, unlike a primitive int[]).

    /** Canonical Integer[] (autoboxed), all non-null: { 7, 8, 9 }. */
    public static volatile Integer[] boxedInts = { 7, 8, 9 };

    /** Mixed Integer[] with a null: { 70, null, 90 }. */
    public static volatile Integer[] boxedMixed = { 70, null, 90 };

    // ---- Tagged[] (INTERFACE element type) ---------------------------------
    // Declared Tagged[] (an interface array) holding concrete Items: array
    // covariance.  The native item_object wrapper decodes each slot and reads its
    // tag, proving an interface-typed array reads identically to a class-typed one.

    /** Canonical Tagged[] holding Items: { Item(110), Item(120), Item(130) }. */
    public static volatile Tagged[] taggedItems =
        { new Item(110), new Item(120), new Item(130) };

    /** Mixed Tagged[]: { Item(111), null, Item(113) }. */
    public static volatile Tagged[] taggedMixed =
        { new Item(111), null, new Item(113) };

    // ---- POLYMORPHIC / INHERITED-element arrays ---------------------------
    // A Node[] holding a runtime MIX of the base Node and the derived Leaf, so a
    // single wrapper reads the INHERITED `tag` field off both concrete classes
    // (and a null slot stays a real nullptr).  The Tagged[] sibling holds the
    // same polymorphic mix through the interface element type.

    /** Polymorphic Node[]: { Node(140), Leaf(150,7), null, Node(160) }. */
    public static volatile Node[] polyNodes =
        { new Node(140), new Leaf(150, 7), null, new Node(160) };

    /** Tagged[] holding a Node + a Leaf (interface-typed polymorphic mix). */
    public static volatile Tagged[] taggedPoly =
        { new Node(170), new Leaf(180, 9) };

    /** A single Leaf in a Node[] — derived-only element via a base-typed array. */
    public static volatile Node[] leafOnly = { new Leaf(190, 11) };

    // ---- ABSTRACT-superclass-typed array (Number[] holding Integers) ------
    // Declared java.lang.Number[] (signature "[Ljava/lang/Number;") — an ABSTRACT
    // class element type — whose runtime elements are boxed Integers.  Proves the
    // "[L" signature branch keys on the descriptor, not on whether the L-class is
    // concrete / abstract / interface; the native side decodes each slot as a
    // java.lang.Integer and reads its value.
    public static volatile Number[] numberInts = { 21, 22, 23 };

    // ---- Field typed plain Object that HOLDS an array (covariance) ----------
    // The DECLARED field type is java.lang.Object (signature "Ljava/lang/Object;"),
    // but the assigned value is an Item[] — so field_oop() decodes to a live ARRAY
    // oop whose component type is Item.  The native side proves it can read the
    // backing array (length + element tags) even though the static field type is a
    // scalar reference, and that the component type is recoverable at runtime.

    /** A scalar Object field whose runtime value is an Item[]{Item(401),Item(402)}. */
    public static volatile Object objectHoldingArray =
        new Item[]{ new Item(401), new Item(402) };

    /** A scalar Object field whose runtime value is a String[]{"oh0","oh1"}. */
    public static volatile Object objectHoldingStringArray =
        new String[]{ "oh0", "oh1" };

    // ---- LARGE Item[] (length 1000) ----------------------------------------
    // Stresses the per-element decode loop and the result-vector reserve at a
    // realistic length.  Element i carries tag == i, so the native side can
    // spot-check the first / a middle / the last element's identity by value.

    /** Large Item[] of length 1000; element i has tag == i. */
    public static volatile Item[] largeItems = buildLargeItems(1000);

    private static Item[] buildLargeItems(final int n)
    {
        final Item[] out = new Item[n];
        for (int i = 0; i < n; i++)
        {
            out[i] = new Item(i);
        }
        return out;
    }

    // ---- Published values for native cross-checks -------------------------
    // Each Item carries a UNIQUE tag, so the native side proves it decoded the
    // right object by reading tag through the wrapper (field AND getTag method).
    // No identityHashCode is published because the zero-JNI native layer has no
    // primitive to recompute it; tag-uniqueness + re-read determinism are the
    // identity oracle instead.

    /** Self-reference so the native side can read the INSTANCE fields. */
    public static volatile FieldArraysObject self;

    /** Lengths re-published from Java so native count checks have a Java oracle. */
    public static volatile int staticStringsLen;
    public static volatile int mixedStringsLen;
    public static volatile int staticItemsLen;
    public static volatile int mixedItemsLen;
    public static volatile int objectItemsLen;
    public static volatile int objectMixedLen;
    public static volatile int grid2dLen;
    public static volatile int grid2dMixedLen;
    public static volatile int jaggedGridLen;
    public static volatile int jaggedRow0Len;
    public static volatile int jaggedRow1Len;
    public static volatile int jaggedRow2Len;
    public static volatile int strGrid2dLen;
    public static volatile int cube3dLen;
    public static volatile int boxedIntsLen;
    public static volatile int boxedMixedLen;
    public static volatile int taggedItemsLen;
    public static volatile int taggedMixedLen;
    public static volatile int largeItemsLen;
    public static volatile int objectHoldingArrayLen;
    public static volatile int polyNodesLen;
    public static volatile int taggedPolyLen;
    public static volatile int numberIntsLen;
    public static volatile int grid2dSingleLen;
    public static volatile int grid2dAllNullRowsLen;
    public static volatile int grid2dEmptyRowLen;
    public static volatile int cube3dPlane0Len;
    public static volatile int cube3dPlane0Row0Len;

    /** Hookable instance method — the native module hooks this to prove the
        fixture is live and to obtain a `self` that can read instance fields. */
    public int touch(final int delta)
    {
        return this.instItems.length + delta;
    }

    private static void publishLengths()
    {
        staticStringsLen = staticStrings.length;
        mixedStringsLen  = mixedStrings.length;
        staticItemsLen   = staticItems.length;
        mixedItemsLen    = mixedItems.length;
        objectItemsLen   = objectItems.length;
        objectMixedLen   = objectMixed.length;
        grid2dLen        = grid2d.length;
        grid2dMixedLen   = grid2dMixed.length;
        jaggedGridLen    = jaggedGrid.length;
        jaggedRow0Len    = jaggedGrid[0].length;
        jaggedRow1Len    = jaggedGrid[1].length;
        jaggedRow2Len    = jaggedGrid[2].length;
        strGrid2dLen     = strGrid2d.length;
        cube3dLen        = cube3d.length;
        boxedIntsLen     = boxedInts.length;
        boxedMixedLen    = boxedMixed.length;
        taggedItemsLen   = taggedItems.length;
        taggedMixedLen   = taggedMixed.length;
        largeItemsLen    = largeItems.length;
        objectHoldingArrayLen = ((Object[]) objectHoldingArray).length;
        polyNodesLen          = polyNodes.length;
        taggedPolyLen         = taggedPoly.length;
        numberIntsLen         = numberInts.length;
        grid2dSingleLen       = grid2dSingle.length;
        grid2dAllNullRowsLen  = grid2dAllNullRows.length;
        grid2dEmptyRowLen     = grid2dEmptyRow.length;
        cube3dPlane0Len       = cube3d[0].length;
        cube3dPlane0Row0Len   = ((Object[]) cube3d[0][0]).length;
    }

    static
    {
        // Publish the length oracles at CLASS-INIT time (this static block runs
        // when Main.loadFixtures() Class.forName's the fixture, long before any
        // native test runs).  The native count checks (str_*_count_matches_java,
        // item_manual_*_count_matches_java) read these as a Java oracle from the
        // side-effect-free PART A / PART B reads, which execute BEFORE the probe
        // handshake in PART C.  Publishing only inside the probe's run() left the
        // oracles at their default 0 for those earlier reads, so every
        // size-vs-oracle check compared 3 == 0 and failed.  The values are
        // compile-time-constant array lengths, so eager publication is correct.
        publishLengths();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldArraysObject.go && !FieldArraysObject.done;
            }

            @Override
            public void run()
            {
                final FieldArraysObject instance = new FieldArraysObject();
                FieldArraysObject.self = instance;
                // Idempotent re-publish (lengths are constants); kept so the
                // oracle is correct even if a future probe mutates the arrays.
                publishLengths();
                // Drive a real bytecode dispatch so the interpreter hook fires.
                FieldArraysObject.observed = instance.touch(1000);
                FieldArraysObject.done = true;
            }
        });
    }
}
