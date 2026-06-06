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
    public static volatile int mode;

    // =====================================================================
    //  STATIC fields — resolved through the class mirror (no live oop needed).
    // =====================================================================
    public static int    sTag      = 0x5A5A5A5A;      // 1515870810
    public static long   sBig      = 0x0123456789ABCDEFL;
    public static boolean sFlag    = true;
    public static char   sChar     = 0x20AC;          // euro sign
    public static String sName     = freshAscii("wrapper-static");

    // A static reference to a sibling object (object-reference static field).
    public static WrapperPattern sRef = new WrapperPattern(0x100);

    // =====================================================================
    //  INSTANCE fields — resolved through a live oop.  Values DIFFER from the
    //  static ones so a static/instance mix-up is caught immediately.
    // =====================================================================
    public int    iId    = 0x0BADF00D;                // 195948557
    public long   iValue = 1000L;                     // mutated by bump() at runtime
    public boolean iFlag = false;
    public String iLabel = freshAscii("wrapper-instance");

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
