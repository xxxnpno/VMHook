package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the wrapper_pattern feature: the vmhook::object&lt;T&gt; /
 * object_base wrapper pattern itself.
 *
 * The native module builds wrappers around live OOPs of THIS class and proves
 * the wrapper contract end to end:
 *   - construct a wrapper from a live oop (static_field("instance")-&gt;get()),
 *   - get_instance() (via EXPLICIT base qualification, because the module's
 *     wrapper deliberately declares a STATIC get_instance() that shadows the
 *     inherited instance accessor) returns exactly that oop,
 *   - a wrapper built from nullptr has a null instance pointer yet
 *     static_field / static_method still resolve through the class mirror,
 *   - instance vs static field/method dispatch, signature()/is_static(),
 *   - copy / move of the wrapper preserve / transfer the oop,
 *   - equality of two wrappers to the SAME instance (raw-oop identity; the
 *     wrapper has no operator==, which the module characterizes),
 *   - get_field on a null-oop wrapper returns nullopt gracefully,
 *   - a run_probe that mutates an instance field (real putfield bytecode) and
 *     the wrapper reads the new value.
 *
 * The class is intentionally rich: static + instance fields (primitive,
 * boundary, and reference), static + instance methods including OVERLOADS so
 * the name+signature resolver is exercised, two distinct published instances
 * plus an alias to the first instance (for the equality test), and a runtime-
 * mutated instance field driven by genuine invokevirtual / putfield.
 *
 * EXHAUSTIVE-WRAPPER additions (so the native module can prove the object&lt;T&gt;
 * contract over EVERY object shape, not just this class):
 *   - a nested wrapped reference type {@link Node} reachable through an instance
 *     field, and a {@code Node} that itself holds another {@code Node}
 *     (wrapper-of-wrapper / nested get through a field-decoded wrapper),
 *   - an INTERFACE-typed instance field ({@link Greeter}) holding a concrete
 *     impl ({@link Hello}) — a wrapper registered for the concrete impl reads
 *     it and dispatches the interface method,
 *   - an ENUM-constant reference ({@link Suit}) reachable as a static field — a
 *     wrapper registered for the enum reads an enum-body field and dispatches an
 *     enum instance method,
 *   - a primitive ARRAY field ({@code int[] numbers}) the module walks
 *     element-wise (an array oop is wrappable as a generic object_base, but its
 *     ELEMENTS are read through the array helpers),
 *   - a {@code java.lang.Object}-typed field holding {@code this} so the module
 *     can decode it through the generic polymorphic base and downcast,
 *   - a {@code mode == 1} probe branch that drives {@code System.gc()} so the
 *     native side can characterise a wrapper outliving a GC nudge.
 *
 * String fields are built with new String(char[]) (private backing) so any
 * in-place vmhook string write could never corrupt an interned literal; this
 * fixture never writes them, but the convention is kept for safety parity with
 * the other fixtures.
 *
 * JAVA 8 SOURCE ONLY: anonymous Probe class, no var / records / switch-expr /
 * text-blocks / lambdas-in-fields / post-8 APIs.  Non-ASCII char values use
 * numeric / \\uXXXX escapes so javac agrees under Cp1252 (Windows) and UTF-8.
 */
public final class WrapperPattern
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;
    // Scenario selector (native sets it + clears done on the rising edge of go).
    //   0 = bump(2345) on `instance` (the runtime-mutation / hook target).
    //   1 = drive System.gc() twice (the wrapper-outlives-a-GC-nudge angle) and
    //       publish the post-GC witnesses below.
    public static volatile int mode;

    // -- GC-probe witnesses (published by the mode==1 action, Java-side) ------
    /** True once the gc() probe has run (so the native gate is exact). */
    public static volatile boolean gcProbeRan;
    /** instance.iId re-read Java-side AFTER System.gc() (still 0x0BADF00D). */
    public static volatile int gcInstanceIdAfter;
    /** node.nId re-read Java-side AFTER System.gc() (still NODE_ID). */
    public static volatile int gcNodeIdAfter;

    // =====================================================================
    //  Nested wrapped reference type (emitted as WrapperPattern$Node).
    //  The native module registers a wrapper for it and reads it through an
    //  instance field of WrapperPattern; a Node that holds ANOTHER Node proves
    //  wrapper-of-wrapper (nested get through a field-decoded wrapper).
    // =====================================================================
    public static final class Node
    {
        public int    nId;
        public String nLabel;
        public Node   nextNode;   // a Node-typed field ON a Node (recursive ref)

        public Node(final int id, final String label)
        {
            this.nId      = id;
            this.nLabel   = label;
            this.nextNode = null;
        }

        /** Instance method dispatched through a field-decoded Node wrapper. */
        public int nodeId()
        {
            return this.nId;
        }
    }

    // =====================================================================
    //  An interface + concrete impl (WrapperPattern$Greeter / $Hello).  The
    //  `greeter` field is declared as the interface but holds a Hello at
    //  runtime; the native side registers a wrapper for the concrete impl,
    //  reads it through the interface-typed field, and dispatches greet().
    // =====================================================================
    public interface Greeter
    {
        String greet();
    }

    public static final class Hello implements Greeter
    {
        public String who;

        public Hello(final String who)
        {
            this.who = who;
        }

        @Override
        public String greet()
        {
            return "hello " + this.who;
        }
    }

    // =====================================================================
    //  An enum (WrapperPattern$Suit) with an instance field + an instance
    //  method.  Each constant is a distinct singleton object; the native side
    //  registers a wrapper for the enum klass, reads a constant through a
    //  static field, reads the enum-body field, and dispatches rank().
    // =====================================================================
    public enum Suit
    {
        HEARTS(3),
        SPADES(7);

        /** Instance field declared on the enum body (native reads this). */
        public final int rank;

        Suit(final int rank)
        {
            this.rank = rank;
        }

        /** Instance method on the enum (native dispatches it through a wrapper). */
        public int rank()
        {
            return this.rank;
        }
    }

    // =====================================================================
    //  STATIC fields — resolved through the class mirror (no live oop needed).
    // =====================================================================
    public static int    sTag      = 0x5A5A5A5A;      // 1515870810
    public static long   sBig      = 0x0123456789ABCDEFL;
    public static boolean sFlag    = true;
    public static char   sChar     = 0x20AC;          // euro sign
    public static String sName     = freshAscii("wrapper-static");

    // ---- EVERY remaining primitive width as a STATIC field -----------------
    // byte / short / float / double round out the value_t variant alternatives
    // (i8 idx1, i16 idx2, float idx5, double idx6) that the int/long/char/bool/
    // reference statics above did not exercise.
    public static byte   sByte     = (byte) -7;       // i8
    public static short  sShort    = (short) -3000;   // i16
    public static float  sFloat    = 2.5f;            // float (exact binary)
    public static double sDouble   = -1.25d;          // double (exact binary)

    // ---- BOUNDARY / EDGE static values (every width's extremes) ------------
    public static byte    sByteMin  = Byte.MIN_VALUE;     // -128
    public static byte    sByteMax  = Byte.MAX_VALUE;     // 127
    public static short   sShortMin = Short.MIN_VALUE;    // -32768
    public static short   sShortMax = Short.MAX_VALUE;    // 32767
    public static char    sCharZero = (char) 0;           // 0x0000
    public static char    sCharMax  = (char) 0xFFFF;      // 65535
    public static int     sIntMin   = Integer.MIN_VALUE;  // 0x80000000
    public static int     sIntMax   = Integer.MAX_VALUE;  // 0x7FFFFFFF
    public static long    sLongMin  = Long.MIN_VALUE;
    public static long    sLongMax  = Long.MAX_VALUE;
    public static float   sFloatNeg = -0.5f;
    public static double  sDoubleBig = 1.0e9d + 0.5d;     // 1000000000.5, exact

    // A static reference to a sibling object (object-reference static field).
    public static WrapperPattern sRef = new WrapperPattern(0x100);

    // A static ENUM-constant reference (the enum-singleton-through-a-wrapper
    // angle): resolves to the HEARTS singleton.
    public static Suit favoriteSuit = Suit.HEARTS;

    // ---- Deterministic constants the native side mirrors (exhaustive part) --
    public static final int    NODE_ID    = 0x0D0E;            // head Node.nId
    public static final int    NODE2_ID   = 0x0D0F;            // chained Node.nId
    public static final String HELLO_WHO  = "wrapper";          // Hello.who
    public static final int    NUM0       = 11;
    public static final int    NUM1       = 22;
    public static final int    NUM2       = 33;
    public static final int    NUMBERS_LEN = 3;
    public static final int    HEARTS_RANK = 3;                 // Suit.HEARTS.rank

    // =====================================================================
    //  INSTANCE fields — resolved through a live oop.  Values DIFFER from the
    //  static ones so a static/instance mix-up is caught immediately.
    // =====================================================================
    public int    iId    = 0x0BADF00D;                // 195948557
    public long   iValue = 1000L;                     // mutated by bump() at runtime
    public boolean iFlag = false;
    public String iLabel = freshAscii("wrapper-instance");

    // ---- EVERY remaining primitive width as an INSTANCE field --------------
    // Values DIFFER from the static counterparts so a static/instance mix-up is
    // caught immediately, and exercise every value_t alternative through a live
    // oop: byte (i8), short (i16), char (u16), float, double.
    public byte   iByte   = (byte) 42;                // i8
    public short  iShort  = (short) 12345;            // i16
    public char   iChar   = 'Z';                      // 0x005A
    public float  iFloat  = 0.75f;                    // float (exact binary)
    public double iDouble = 3.5d;                     // double (exact binary)

    // ---- INSTANCE-field SET() round-trip scratch (per-published-object) -----
    // Dedicated fields the native side WRITES through a wrapper's get_field(
    // "...")->set(v) and reads back, then restores; isolated from every value
    // assertion above so the write cannot perturb another check.  One per
    // primitive class the set() path supports.
    public int     scratchI = 0;
    public long    scratchJ = 0L;
    public boolean scratchZ = false;
    public byte    scratchB = (byte) 0;
    public short   scratchS = (short) 0;
    public char    scratchC = (char) 0;
    public float   scratchF = 0.0f;
    public double  scratchD = 0.0d;
    public String  scratchStr = freshAscii("scratch-initial");

    // ---- STATIC-field SET() round-trip scratch ------------------------------
    public static int sScratchI = 0;

    // =====================================================================
    //  EXHAUSTIVE-WRAPPER reference / array fields (every object shape).
    //  Eagerly initialised so the nested Node / Hello / Suit klasses are
    //  loaded (and the OOPs exist) before the native module runs — Main's
    //  loadFixtures() only forName's the TOP-LEVEL fixture, so a nested type
    //  reached only through a never-touched field would otherwise stay unloaded.
    // =====================================================================
    /** A nested wrapped reference (a WrapperPattern$Node) on an instance. */
    public Node node = makeChain();

    /** An INTERFACE-typed field holding a concrete impl (Greeter -> Hello). */
    public Greeter greeter = new Hello(HELLO_WHO);

    /** A primitive int[] the native side walks element-wise through helpers. */
    public int[] numbers = { NUM0, NUM1, NUM2 };

    /** A java.lang.Object-typed field holding `this` (polymorphic-base angle). */
    public Object selfAsObject = this;

    // =====================================================================
    //  Published singletons the native side wraps.
    //   instance       : the primary live object the module wraps.
    //   instance2      : a DISTINCT object (different identity / iId).
    //   sameAsInstance : ALIASES `instance` (same object) for the equality test.
    // =====================================================================
    public static WrapperPattern instance       = new WrapperPattern(0x0BADF00D);
    public static WrapperPattern instance2       = new WrapperPattern(0x0CAFE2);
    public static WrapperPattern sameAsInstance  = instance;

    private WrapperPattern(final int id)
    {
        this.iId = id;
    }

    // Default ctor for sRef's `new WrapperPattern(0x100)` path is the private
    // one above; expose a no-arg shape only through the int ctor to keep
    // construction explicit.

    // =====================================================================
    //  STATIC methods (incl. overloads) — static_method dispatch + signatures.
    // =====================================================================
    public static int staticTag()
    {
        return sTag;
    }

    public static String staticName()
    {
        return sName;
    }

    // Overloaded statics: combine(int) and combine(int,int) share a name but
    // differ in descriptor, so the name+signature resolver must pick correctly.
    public static int combine(final int a)
    {
        return a + 1;
    }

    public static int combine(final int a, final int b)
    {
        return a + b;
    }

    // ---- EVERY primitive-arg descriptor as an overload of `widen` -----------
    // Same name, distinct JVM descriptors: (Z)I (B)I (S)I (C)I (J)I (F)I (D)I.
    // The name+signature resolver must pick the EXACT descriptor; the name-only
    // resolver returns the first by name.  None are called natively — RESOLUTION
    // by exact descriptor is the universal (thread-free) invariant under test.
    public static int widen(final boolean v) { return v ? 1 : 0; }
    public static int widen(final byte v)    { return v; }
    public static int widen(final short v)   { return v; }
    public static int widen(final char v)    { return v; }
    public static int widen(final long v)    { return (int) v; }
    public static int widen(final float v)   { return (int) v; }
    public static int widen(final double v)  { return (int) v; }

    // =====================================================================
    //  INSTANCE methods (incl. overloads) — instance dispatch + signatures.
    // =====================================================================
    public int getId()
    {
        return this.iId;
    }

    public long getValue()
    {
        return this.iValue;
    }

    public String getLabel()
    {
        return this.iLabel;
    }

    // Overloaded instance methods: describe() vs describe(int).
    public int describe()
    {
        return this.iId;
    }

    public int describe(final int salt)
    {
        return this.iId + salt;
    }

    // ---- INSTANCE methods returning every primitive-width return type -------
    // Their RESOLUTION + signature()/is_reference() introspection is the
    // thread-free invariant the native side asserts (the values are reachable
    // through the matching instance FIELDS already; these pin the return
    // descriptors ()B ()S ()C ()F ()D for method_proxy::signature()).
    public byte   getByte()   { return this.iByte; }
    public short  getShort()  { return this.iShort; }
    public char   getChar()   { return this.iChar; }
    public float  getFloat()  { return this.iFloat; }
    public double getDouble() { return this.iDouble; }

    /**
     * Mutates the instance field iValue via genuine putfield bytecode and
     * returns the new value.  The native side reads iValue back through the
     * wrapper AFTER the probe, proving the wrapper reads live post-dispatch
     * state, not the class-initializer constant.
     */
    public long bump(final int delta)
    {
        this.iValue = this.iValue + delta;   // getfield + iadd + putfield
        return this.iValue;
    }

    // Builds a String with a private (non-interned) backing array.
    private static String freshAscii(final String literal)
    {
        return new String(literal.toCharArray());
    }

    // Builds a two-deep Node chain so the native side can prove wrapper-of-
    // wrapper: head.nextNode is itself a Node-typed field on a Node.
    private static Node makeChain()
    {
        final Node head = new Node(NODE_ID, freshAscii("node-head"));
        head.nextNode   = new Node(NODE2_ID, freshAscii("node-tail"));
        return head;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return WrapperPattern.go && !WrapperPattern.done;
            }

            @Override
            public void run()
            {
                if (WrapperPattern.mode == 1)
                {
                    // GC-nudge scenario: ask the JVM to collect (twice, to give a
                    // moving collector a chance to relocate live objects), then
                    // re-read the witness fields Java-side so the native module
                    // can characterise a wrapper held across the GC.  This is a
                    // best-effort hint; the native side never hard-asserts a moved
                    // oop, only the re-resolved read.
                    System.gc();
                    System.gc();
                    WrapperPattern.gcInstanceIdAfter = WrapperPattern.instance.iId;
                    WrapperPattern.gcNodeIdAfter     = WrapperPattern.instance.node.nId;
                    WrapperPattern.gcProbeRan        = true;
                    WrapperPattern.done = true;
                    return;
                }

                // Real bytecode dispatch so a native interpreter hook on bump()
                // fires on the Java thread, and putfield mutates iValue.  mode 0
                // bumps by a fixed delta; the native side knows the expected
                // post-state.  invokevirtual bump(...) -> getfield/iadd/putfield.
                WrapperPattern.instance.bump(2345);
                WrapperPattern.done = true;
            }
        });
    }
}
