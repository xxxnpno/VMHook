package vmhook.fixtures;

/**
 * Mutable side-effect tallies for the method_explicit_signature fixture that are
 * written from places which cannot host mutable static state directly:
 *
 *   * interface DEFAULT methods (in {@link MethodExplicitSigIface}) — interface
 *     fields are implicitly {@code public static final}, so a default method
 *     cannot mutate an interface-declared scalar; it writes here instead.
 *
 * Keeping these as plain {@code static volatile} scalars on a normal class lets
 * the native side read each with the clean {@code static_field("x")->get()}
 * one-liner (no array-element decode), exactly like every other per-overload
 * tally in {@link MethodExplicitSig}.
 *
 * This class registers no probe and has no instances.  Java 8 syntax only.
 */
final class MethodExplicitSigCounters
{
    private MethodExplicitSigCounters() { }

    /** ifaceDefault(I)I executed; records last int arg. */
    public static volatile int ifaceDefaultIntSeen;
    /** ifaceDefault(J)J executed; records last long arg. */
    public static volatile long ifaceDefaultLongSeen;
    /** ifaceAbstract(I)I (concrete override on MethodExplicitSig) executed. */
    public static volatile int ifaceAbstractSeen;

    /** dup(I)I instance overload executed; records last int arg. */
    public static volatile int dupInstanceSeen;
    /** dup(I)I STATIC overload executed; records last int arg. */
    public static volatile int dupStaticSeen;

    /** Per-shape tallies for the descriptor-shape selector family. */
    public static volatile float  shapeFloatSeen;
    public static volatile double shapeDoubleSeen;
    public static volatile int    shapeBoolSeen;   // 1 when shapes(Z) ran with true
    public static volatile int    shapeByteSeen;
    public static volatile int    shapeShortSeen;
    public static volatile int    shapeCharSeen;
    public static volatile int    shapeIntArrSeen;   // sum of int[] elements seen
    public static volatile long   shapeLongArrSeen;  // sum of long[] elements seen
    public static volatile int    shapeStrArrSeen;   // count of String[] elements seen
    public static volatile int    shapeFourArgSeen;  // a+b+c+d of shapes(IIII)

    /** <init>(int) ran; records the arg. */
    public static volatile int initIntSeen;
    /** <init>(String) ran; records the arg. */
    public static volatile String initStrSeen;
}
