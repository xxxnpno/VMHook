package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "poly_inherited_oop" feature (area: fields + methods).
 *
 * The live-JVM counterpart of the legacy example.cpp test_poly_probe: a
 * B-extends-A object exercised through vmhook so the native module can prove
 * vmhook::find_field's Klass::get_super() super-chain walk resolves an INHERITED
 * INSTANCE field, and that an inherited method is found (and, when the call gate
 * is exported, called) through the same walk.
 *
 *     A                         (super — declares the protected instance field
 *        ^  extends              protectedInt = 1337 and the protected instance
 *     B                          method protectedAdd(int) = protectedInt + x)
 *                               (sub — declares its OWN field bInt = 42)
 *
 * A and B are NESTED static classes of this fixture, so javac emits
 * PolyInherited$A and PolyInherited$B; the native module registers wrappers on
 * "vmhook/fixtures/PolyInherited$A" and "vmhook/fixtures/PolyInherited$B".
 *
 * Mirrors the legacy A/B exactly (same field name protectedInt, same value 1337,
 * same own field bInt = 42, same method protectedAdd whose body is
 * protectedInt + x) so the native constants match the canonical values and the
 * inherited-method call returns protectedAdd(3) == 1340.
 *
 * What the native module drives through this fixture:
 *   - get the live B instance (exposed as a static field),
 *   - read B's OWN bInt (super walk depth 0) and the INHERITED protected
 *     A.protectedInt (super walk depth 1) THROUGH the B klass — the offset must
 *     resolve to A's declared field,
 *   - find + (best-effort, gated on the call gate) call the inherited
 *     protectedAdd(3) and read back 1340.
 *
 * The go/done handshake lets the native side run the Java witness (Java reads
 * the same three quantities through real getfield / invokevirtual bytecode), so
 * the module can cross-check that the JVM itself sees identical values.  This
 * fixture is the only test thread for read-only ops, matching vmhook's
 * documented single-reader contract.  Java 8 syntax only (anonymous Probe; no
 * var / lambdas / records / switch-expressions).
 */
public final class PolyInherited
{
    // -- go / done handshake (native raises go; the probe latches done) --------
    public static volatile boolean go;
    public static volatile boolean done;

    // ---- Java-side witnesses: the probe sets these by reading the SAME three
    //      quantities through genuine bytecode, so the native module can confirm
    //      the JVM observes identical values to vmhook's offset reads. ----------
    public static volatile boolean sawOwnField;          // bInt == 42
    public static volatile boolean sawInheritedField;    // protectedInt == 1337
    public static volatile boolean sawInheritedMethod;   // protectedAdd(3) == 1340

    // ---- Witnesses for the EXHAUSTIVE expansion (deep hierarchy, shadow,
    //      reference shapes, polymorphic type).  Each is latched by the probe
    //      through genuine bytecode so the native module can cross-check that
    //      the JVM observes the identical values vmhook reads by raw offset. ----
    public static volatile boolean sawDeepFields;        // L1..L4 ints via L4 view
    public static volatile boolean sawDeepRefs;          // inherited String/array/null/self refs
    public static volatile boolean sawShadowSub;         // ShadowSub sees CHILD shadow slots
    public static volatile boolean sawShadowBase;        // base-typed read sees BASE shadow slots
    public static volatile boolean sawPolyConcrete;      // L1-typed field holds an L4 at runtime

    // ---- Canonical values (mirrored on the native side) --------------------
    public static final int PROTECTED_INT     = 1337;    // A.protectedInt init
    public static final int B_INT             = 42;      // B.bInt init
    public static final int ADD_ARG           = 3;       // protectedAdd argument
    public static final int ADD_RESULT        = 1340;    // protectedAdd(3) result

    // ---- Canonical values for the deep 4-level hierarchy (L1<-L2<-L3<-L4).
    //      Far apart so a wrong-offset read up the chain can never be a near-miss.
    public static final int  L1_INT           = 0x0A1A0001;   // declared on L1 (depth 3 from L4)
    public static final int  L2_INT           = 0x0B2B0002;   // declared on L2 (depth 2 from L4)
    public static final int  L3_INT           = 0x0C3C0003;   // declared on L3 (depth 1 from L4)
    public static final int  L4_INT           = 0x0D4D0004;   // declared on L4 (depth 0 / own)
    public static final int  L2_REF_VAL       = 0x0E5E0005;   // l2Ref's own l1Int
    public static final String L1_STR_VALUE   = "l1-inherited-string";
    public static final int  L1_ARR_ELEM0     = 0x51510001;   // l1Arr[0]
    public static final int  L1_ARR_ELEM1     = 0x51510002;   // l1Arr[1]
    public static final int  L1_ARR_LEN       = 2;

    // ---- Canonical values for the shadow (hidden-field) pair ----------------
    public static final int  BASE_SHADOW_INT  = 1000;         // Shadow.shadowedInt
    public static final int  SUB_SHADOW_INT   = 2000;         // ShadowSub.shadowedInt
    public static final String BASE_SHADOW_STR = "base-shadow";
    public static final String SUB_SHADOW_STR  = "sub-shadow";

    // ---- Canonical value for the polymorphic field's concrete L4 ------------
    public static final int  POLY_L4_INT      = 0x0F6F0006;   // l4Poly's l4Int

    /**
     * Super class — declares the inherited protected instance field and the
     * inherited protected instance method.  Mirrors legacy vmhook.A exactly.
     */
    static class A
    {
        protected int protectedInt = PROTECTED_INT;

        protected int protectedAdd(final int x)
        {
            return this.protectedInt + x;
        }
    }

    /**
     * Sub class — adds its OWN instance field.  Mirrors legacy vmhook.B (the
     * own int field bInt = 42 alongside the inherited protectedInt).
     */
    static class B extends A
    {
        int bInt = B_INT;
    }

    // ---- The live B instance the native side wraps (as a B-typed wrapper) ----
    public static final B bInstance = new B();

    // ======================================================================
    //  DEEP 4-LEVEL HIERARCHY  L1 <- L2 <- L3 <- L4.
    //
    //  Each level declares its OWN int field (so the native module can read a
    //  field DECLARED AT EACH LEVEL through the deepest L4 wrapper and prove the
    //  super walk resolves the right offset/value at depths 0..3).  L1 also
    //  declares the inherited REFERENCE-shape fields the module reads THROUGH the
    //  L4 subclass: a String ref, an int[] (array) ref, and a null ref.  L4
    //  declares a SELF-reference field (wired in its ctor) so a self-ref read
    //  through the deepest subclass decodes back to the receiver.
    // ======================================================================

    /** Grandparent^2 — owns the int field at depth 3 and the inherited refs. */
    static class L1
    {
        int    l1Int  = L1_INT;                 // inherited at depth 3 from L4
        String l1Str  = L1_STR_VALUE;           // inherited String ref
        int[]  l1Arr  = new int[] { L1_ARR_ELEM0, L1_ARR_ELEM1 };  // inherited array ref
        Object l1Null = null;                   // inherited null ref (must decode to null)
    }

    /** Grandparent — int at depth 2, plus an L1-typed object reference. */
    static class L2 extends L1
    {
        int l2Int = L2_INT;                     // inherited at depth 2 from L4

        /** A non-null object reference declared at depth 2 (holds a plain L1). */
        L1 l2Ref = makeL1Ref();
    }

    /** Parent — int at depth 1. */
    static class L3 extends L2
    {
        int l3Int = L3_INT;                     // inherited at depth 1 from L4
    }

    /** The deepest subclass — its OWN int (depth 0) and a self-reference. */
    static class L4 extends L3
    {
        int l4Int = L4_INT;                     // own field, super walk depth 0

        /** A field holding THIS instance; wired in the ctor (self-ref angle). */
        L4 l4Self;

        L4()
        {
            this.l4Self = this;
        }
    }

    /** Builds the plain L1 that l2Ref points at, with a distinguishing l1Int. */
    private static L1 makeL1Ref()
    {
        final L1 r = new L1();
        r.l1Int = L2_REF_VAL;                   // tell it apart from the L4's own L1 slot
        return r;
    }

    // ---- The live L4 instance the native side wraps (L4-typed view) ----------
    public static final L4 l4Instance = new L4();

    // ======================================================================
    //  POLYMORPHIC ACTUAL TYPE.  A field DECLARED as the base type L1 but
    //  HOLDING an L4 at runtime: the native read must return the CONCRETE L4
    //  oop (runtime klass L4), and an L4-typed wrapper over that oop must read
    //  L4's own field — proving the decode is by the live object, not the
    //  declared field type.
    // ======================================================================

    /** A distinct L4 the polyBase field points at (its own l4Int is POLY_L4_INT). */
    private static L4 makePolyL4()
    {
        final L4 r = new L4();
        r.l4Int = POLY_L4_INT;
        return r;
    }

    public static final L4 l4Poly = makePolyL4();

    /** Declared base type L1, runtime type L4 (polymorphic-field angle). */
    public static final L1 polyBase = l4Poly;

    // ======================================================================
    //  SHADOWED / HIDDEN FIELD.  Both Shadow and ShadowSub declare fields of
    //  the SAME name (shadowedInt / shadowedRef) at DIFFERENT values.  A
    //  ShadowSub-typed read must resolve the CHILD slot (declared-scope wins at
    //  the start klass); a Shadow-typed read of the SAME oop must resolve the
    //  BASE slot (its own declaration) — proving the two same-named slots are
    //  physically distinct and the walk's declared-scope disambiguation is
    //  correct.
    // ======================================================================

    /** Base of the shadow pair — declares the base copies of the shadow slots. */
    static class Shadow
    {
        int    shadowedInt = BASE_SHADOW_INT;
        String shadowedRef = BASE_SHADOW_STR;
    }

    /** Sub of the shadow pair — RE-DECLARES the same names with child values. */
    static class ShadowSub extends Shadow
    {
        int    shadowedInt = SUB_SHADOW_INT;
        String shadowedRef = SUB_SHADOW_STR;
    }

    // ---- The live ShadowSub the native side wraps two ways (sub + base) ------
    public static final ShadowSub shadowInstance = new ShadowSub();

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return PolyInherited.go && !PolyInherited.done;
            }

            @Override
            public void run()
            {
                // Java reads the same three quantities through real getfield /
                // invokevirtual bytecode, so the native module can prove the JVM
                // itself agrees with vmhook's offset-based reads.
                PolyInherited.sawOwnField        = (PolyInherited.bInstance.bInt == B_INT);
                PolyInherited.sawInheritedField  = (PolyInherited.bInstance.protectedInt == PROTECTED_INT);
                PolyInherited.sawInheritedMethod = (PolyInherited.bInstance.protectedAdd(ADD_ARG) == ADD_RESULT);

                // ---- Deep hierarchy: read the int declared at EACH of the four
                //      levels THROUGH the deepest L4 instance (real getfield). ---
                final L4 l4 = PolyInherited.l4Instance;
                PolyInherited.sawDeepFields =
                       (l4.l1Int == L1_INT)
                    && (l4.l2Int == L2_INT)
                    && (l4.l3Int == L3_INT)
                    && (l4.l4Int == L4_INT);

                // ---- Inherited reference-shape fields read through L4 ----------
                PolyInherited.sawDeepRefs =
                       L1_STR_VALUE.equals(l4.l1Str)            // inherited String ref
                    && (l4.l1Arr != null && l4.l1Arr.length == L1_ARR_LEN
                        && l4.l1Arr[0] == L1_ARR_ELEM0)         // inherited array ref
                    && (l4.l1Null == null)                      // inherited null ref
                    && (l4.l2Ref != null && l4.l2Ref.l1Int == L2_REF_VAL)  // inherited obj ref
                    && (l4.l4Self == l4);                       // self ref decodes to receiver

                // ---- Shadow pair: a ShadowSub-typed read sees the CHILD slots;
                //      a Shadow-typed read of the SAME object sees the BASE slots. -
                final ShadowSub sub = PolyInherited.shadowInstance;
                final Shadow    base = sub;                     // widened static type
                PolyInherited.sawShadowSub =
                       (sub.shadowedInt == SUB_SHADOW_INT)
                    && SUB_SHADOW_STR.equals(sub.shadowedRef);
                PolyInherited.sawShadowBase =
                       (base.shadowedInt == BASE_SHADOW_INT)    // getfield resolves Shadow.shadowedInt
                    && BASE_SHADOW_STR.equals(base.shadowedRef);

                // ---- Polymorphic field: declared L1, runtime L4 ---------------
                PolyInherited.sawPolyConcrete =
                       (PolyInherited.polyBase instanceof L4)
                    && (((L4) PolyInherited.polyBase).l4Int == POLY_L4_INT);

                PolyInherited.done = true;
            }
        });
    }
}
