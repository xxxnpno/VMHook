package vmhook.fixtures;

/**
 * Superclass of the field_static fixture (area: fields).
 *
 *     FieldStaticBase   <-   FieldStatic
 *
 * The field_static module is the static-field SET + Java-readback authority.
 * This grandparent exists so that module can prove the ONE inheritance angle
 * the sibling field_inherited module does NOT cover for statics: a native
 * static_field("name")->SET of an INHERITED static (declared here, reached
 * through the FieldStatic subclass wrapper) lands on the DECLARING class's
 * java.lang.Class mirror — not the subclass mirror — and is then visible to
 * Java's own getstatic.  (field_inherited only reads inherited statics; it
 * never writes one through the portable accessor and reads it back.)
 *
 * vmhook::find_field records the declaring klass for an inherited static
 * (vmhook.hpp: field_entry_t::declaring_klass) and the static get/set paths
 * resolve `declaring_klass->get_java_mirror()` + offset.  Writing the subclass
 * mirror instead would land in the wrong (smaller, differently-laid-out) oop —
 * exactly the regression these inherited-SET checks pin.
 *
 * The native side writes each `inh*` field via static_field(name)->set(v)
 * BEFORE raising go (field_proxy::set mutates the mirror slot directly, no
 * bytecode).  The FieldStatic probe (mode 5) then snapshots every inherited
 * static into a `seenInh*` witness using genuine getstatic/putstatic, so the
 * module reads back exactly what the JVM observed; getters here additionally
 * let the module pull each value through static_method("getInhX")->call().
 *
 * Initial values are deliberately DISTINCT from the values native writes, so a
 * silently-dropped (or mis-targeted) write is caught: the witness would still
 * equal the initial value.  All char values are numeric; no interned-literal
 * String SET target (the only inherited String here is GET-only / reference
 * replace, never an in-place write).  Java 8 syntax only; no Harness.Probe of
 * its own (loaded as the superclass when FieldStatic is initialised).
 */
class FieldStaticBase
{
    // ---- inherited static SET targets, one per primitive width + a ref -----
    //      native writes the value in the trailing comment; init is distinct.
    public    static boolean inhZ = false;          // native writes true
    protected static byte    inhB = 0;              // native writes Byte.MIN_VALUE (-128)
    private   static char    inhC = 0x0000;         // native writes 0xFFFF
              static short   inhS = 0;              // native writes Short.MIN_VALUE
    public    static int     inhI = 0;              // native writes 0x0C0FFEE1
    protected static long    inhJ = 0L;             // native writes Long.MAX_VALUE
    private   static float   inhF = 0.0f;           // native writes a known bit pattern
              static double  inhD = 0.0;            // native writes a known bit pattern

    // An inherited static OBJECT reference (replaced natively via unique_ptr).
    // Starts as objBaseA; native rewrites it to objBaseB, then to null.
    public static FieldStaticBase inhRef = null;    // set in <clinit> below

    // Two distinct base instances the native side flips inhRef between.  Tagged
    // so the read-back is exact, never "non-null and hope".
    public static final FieldStaticBase objBaseA = new FieldStaticBase();
    public static final FieldStaticBase objBaseB = new FieldStaticBase();

    // An inherited static final CONSTANT (compile-time inlined), so the module
    // can prove the inherited-constant mirror-slot characterization too.
    public static final int INH_CONST_I = 0x0BADBABE;

    public int baseTag = 0;

    // ---- witnesses the FieldStatic probe (mode 5) snapshots via getstatic ---
    public static boolean seenInhZ;
    public static byte    seenInhB;
    public static char    seenInhC;
    public static short   seenInhS;
    public static int     seenInhI;
    public static long    seenInhJ;
    public static int     seenInhFBits;     // Float.floatToRawIntBits(inhF)
    public static long    seenInhDBits;     // Double.doubleToRawLongBits(inhD)
    public static boolean seenInhRefIsB;    // inhRef == objBaseB at snapshot
    public static boolean seenInhRefIsNull;
    public static int     seenInhRefTag;    // inhRef == null ? -1 : inhRef.baseTag

    // ---- getters (genuine getstatic) so native can pull values back through
    //      static_method("getInhX")->call() — the portable static path. -------
    public static boolean getInhZ() { return inhZ; }
    public static byte    getInhB() { return inhB; }
    public static char    getInhC() { return inhC; }
    public static short   getInhS() { return inhS; }
    public static int     getInhI() { return inhI; }
    public static long    getInhJ() { return inhJ; }
    public static int     getInhFBits() { return Float.floatToRawIntBits(inhF); }
    public static long    getInhDBits() { return Double.doubleToRawLongBits(inhD); }
    public static int     getInhRefTag() { return inhRef == null ? -1 : inhRef.baseTag; }
    public static boolean inhRefIsB() { return inhRef == objBaseB; }
    public static boolean inhRefIsNull() { return inhRef == null; }
    // Inlined (constant-folded) vs reflective read of the inherited constant —
    // the inlined getter keeps the literal; reflection sees a mirror-slot write.
    public static int     getInhConstInlined() { return INH_CONST_I; }
    public static int     getInhConstReflect()
    {
        try { return FieldStaticBase.class.getField("INH_CONST_I").getInt(null); }
        catch (Exception e) { return -1; }
    }

    // Snapshot every inherited static into its witness using real bytecode.
    // Driven by the FieldStatic probe's mode 5.
    static void snapshotInherited()
    {
        seenInhZ = inhZ;
        seenInhB = inhB;
        seenInhC = inhC;
        seenInhS = inhS;
        seenInhI = inhI;
        seenInhJ = inhJ;
        seenInhFBits = Float.floatToRawIntBits(inhF);
        seenInhDBits = Double.doubleToRawLongBits(inhD);
        seenInhRefIsB = (inhRef == objBaseB);
        seenInhRefIsNull = (inhRef == null);
        seenInhRefTag = (inhRef == null) ? -1 : inhRef.baseTag;
    }

    // Restore the inherited SET targets to their known initial values (so the
    // module could re-run); also re-publishes objBaseA into inhRef.
    static void resetInherited()
    {
        inhZ = false;
        inhB = 0;
        inhC = 0x0000;
        inhS = 0;
        inhI = 0;
        inhJ = 0L;
        inhF = 0.0f;
        inhD = 0.0;
        inhRef = objBaseA;
    }

    static
    {
        objBaseA.baseTag = 0xA;
        objBaseB.baseTag = 0xB;
        inhRef = objBaseA;
    }
}
