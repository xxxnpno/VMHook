package vmhook;

/**
 * Base-class probe target.  Verifies that vmhook can:
 *   - Read every visibility level (public, package-private, protected, private).
 *   - Walk the superclass chain when {@link B} extends this class.
 *   - Call a constructor that increments a static counter (used by the
 *     make_unique tests on the C++ side).
 */
public class A
{
    // ── Fields exercised by the C++ wrapper ────────────────────────────────
    public        String string         = "test";
    public static int    counter        = 0;
    public        int    field          = 0;
    public        int    val            = 0;

    // Protected fields used by the inheritance/polymorphism probe.  B
    // inherits them; vmhook's field lookup walks the super chain so B's
    // wrapper can read them through its own klass*.
    protected     int    protectedInt    = 1337;
    protected     String protectedString = "from_A";

    // ── Constructors ───────────────────────────────────────────────────────
    /** Default constructor; bumps the static counter. */
    public A()
    {
        counter++;
    }

    /** Convenience constructor used by the make_unique tests. */
    public A(final String initialString)
    {
        this.string = initialString;
        counter++;
    }

    // ── Methods ────────────────────────────────────────────────────────────
    /** Reads the protected int.  B inherits this method too. */
    protected int protectedAdd(final int x)
    {
        return x + this.protectedInt;
    }
}
