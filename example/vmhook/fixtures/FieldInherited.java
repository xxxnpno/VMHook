package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_inherited feature (area: fields).
 *
 * Exercises vmhook::find_field's Klass::get_super() super-chain walk
 * (vmhook.hpp:10756) end-to-end on a real THREE-level Java hierarchy:
 *
 *     FieldInheritedBase            (grandparent — protected/public/package/
 *        ^  extends                  private instance + static fields, plus the
 *     FieldInheritedMid             base copies of the shadow slots)
 *        ^  extends                 (parent — one own instance + one own static,
 *     FieldInherited                 NO shadowing names, so Base slots need the
 *                                    FULL two-link walk)
 *
 * This class (the registered fixture) owns the go/done/mode handshake, declares
 * the child's OWN field, and RE-DECLARES the shadow slots so child-wins
 * shadowing is observable across two inheritance levels.  The native module:
 *   - reads the child's OWN field (super walk depth 0),
 *   - reads the parent's field (depth 1) and grandparent's fields (depth 2) at
 *     every access level (find_field reads by offset, ignoring access control),
 *   - reads the SAME child object through a child-typed wrapper (sees the CHILD
 *     shadow slot) and through a base-typed wrapper (sees the BASE shadow slot),
 *   - reads inherited STATIC fields and a shadowed static,
 *   - probes an absent name (walk reaches Object -> nullopt),
 *   - then drives this fixture's three runtime modes and reads the mutated slots
 *     back, proving find_field resolves LIVE post-dispatch state and that the
 *     child shadow write never touches an unrelated base object's slot.
 *
 * Shadow values are far apart (BASE_* vs CHILD_*) so a misdirected read can
 * never be a near-miss.  Char/String shadows cover the reference (compressed
 * OOP) decode path through the super walk.  Java 8 syntax only.
 */
public final class FieldInherited extends FieldInheritedMid
{
    // -- go / done / mode handshake (native sets mode + clears done first) ----
    public static volatile boolean go;
    public static volatile boolean done;

    /**
     * Scenario selector. The native module sets this BEFORE raising go.
     *   1 = mutate the child's OWN + the inherited (Mid + Base) instance slots
     *       via putfield  (read-back proves find_field resolves live state)
     *   2 = mutate the child's shadow slot AND a separate base object's base
     *       shadow slot (proves the two slots are physically distinct)
     *   3 = mutate inherited + shadowed STATIC slots via putstatic
     *   4 = mutate the child's NARROW (byte) + WIDE (long) shadow slots via
     *       putfield (proves child-wins shadowing for non-int widths)
     */
    public static volatile int mode;

    // ---- Shared constants (mirrored on the native side) --------------------
    public static final int  OWN_INT_INIT       = 0x0C1D0001;   // child's own field
    public static final int  OWN_INT_RUNTIME    = 0x0C1DBEEF;   // mode 1 writes this

    public static final int  BASE_SHADOW_INT    = 1111;         // == Base.shadowedInt
    public static final int  CHILD_SHADOW_INT   = 9999;         // child shadowedInt
    public static final int  CHILD_SHADOW_RUNTIME = 4242;       // mode 2 child write
    public static final int  INDEP_BASE_SHADOW  = 7007;         // mode 2 base-obj write

    // Shadow slots of OTHER widths (byte / long): the child re-declares names
    // already present on Base, so child-wins shadowing is observable for a
    // NARROW and a WIDE primitive too.  Far-apart sentinels (Base copies are
    // 0x11 / 0x00BA5E).
    public static final byte BASE_SHADOW_BYTE   = (byte) 0x11;  // == Base.shadowedByte
    public static final byte CHILD_SHADOW_BYTE  = (byte) 0x77;  // child shadowedByte
    public static final long BASE_SHADOW_LONG   = 0x00BA5EL;    // == Base.shadowedLong
    public static final long CHILD_SHADOW_LONG  = 0x7CC1D7CC1DL;// child shadowedLong

    public static final int  STATIC_SHADOW_BASE    = 555;       // == Base.sShadow
    public static final int  STATIC_SHADOW_CHILD   = 777;       // child sShadow
    public static final int  STATIC_SHADOW_RUNTIME = 3030;      // mode 3 child write

    // mode 1 also writes these inherited non-int slots, so the live read-back
    // proves the super walk resolves the live offset for Z / F / D too (not only
    // I).  Init values for these live on FieldInheritedBase; the runtimes differ.
    public static final boolean BASE_BOOL_RUNTIME   = false;    // init is true
    public static final float   BASE_FLOAT_RUNTIME  = 6.25f;    // init is 2.5f
    public static final double  BASE_DOUBLE_RUNTIME = 9.75d;    // init is 1.5d

    // mode 4 writes the child's NARROW (byte) and WIDE (long) shadow slots, so
    // the live read-back proves child-wins shadowing for those widths AND that
    // the same write leaves the hidden Base copies untouched.
    public static final byte CHILD_SHADOW_BYTE_RUNTIME = (byte) 0x3C;
    public static final long CHILD_SHADOW_LONG_RUNTIME = 0x0DEFACED0L;

    // ---- The child's OWN instance field (super walk stops at depth 0) -------
    protected int childOwnInt = OWN_INT_INIT;

    // ---- Child RE-DECLARES names already present on the grandparent ---------
    // (child slot wins for a child-typed read; the grandparent slot is still
    //  physically present and reachable through a base-typed wrapper).
    public int    shadowedInt = CHILD_SHADOW_INT;
    public String shadowedStr = "child";
    public byte   shadowedByte = CHILD_SHADOW_BYTE;   // narrow shadow
    public long   shadowedLong = CHILD_SHADOW_LONG;   // wide shadow

    // ---- Child shadows a grandparent STATIC of the same name ----------------
    public static int sShadow = STATIC_SHADOW_CHILD;

    // ---- A live child instance the native side wraps for instance reads -----
    public static final FieldInherited instance = new FieldInherited();

    // ---- An INDEPENDENT pure-base object, so mode 2 can prove the child's
    //      shadow write never reaches an unrelated base object's slot. --------
    public static final FieldInheritedBase baseInstance = new FieldInheritedBase();

    /** Reads the child's own field through real getfield (probe witness). */
    public int childOwn()
    {
        return this.childOwnInt;
    }

    /**
     * Touches the child shadow slots of every width so javac does not warn them
     * unused under -Werror-y builds and they are guaranteed present in the
     * child's layout (distinct from the Base copies).
     */
    public long childShadowSum()
    {
        return this.shadowedInt + this.shadowedByte + this.shadowedLong
             + (long) this.shadowedStr.length();
    }

    // ---- Runtime mutators driven by the probe via real bytecode ------------

    /** mode 1: putfield the child's own + the inherited (Mid + Base) instance slots. */
    public void mutateInstance()
    {
        this.childOwnInt  = OWN_INT_RUNTIME;                    // own (depth 0)
        this.midOwnInt    = FieldInheritedMid.MID_INT_RUNTIME;  // inherited depth 1
        this.protectedInt = FieldInheritedBase.PROT_INT_RUNTIME;// inherited depth 2
        this.publicInt    = FieldInheritedBase.PUB_INT_RUNTIME; // inherited depth 2
        // Inherited NON-int slots (depth 2), so the live read-back covers Z/F/D.
        this.baseBool     = BASE_BOOL_RUNTIME;
        this.baseFloat    = BASE_FLOAT_RUNTIME;
        this.baseDouble   = BASE_DOUBLE_RUNTIME;
    }

    /**
     * mode 2: write the CHILD shadow slot here, and the BASE shadow slot on an
     * independent base object.  If find_field conflated the two same-named
     * slots the native read-back would not separate them.
     */
    public void mutateShadowDistinct()
    {
        this.shadowedInt         = CHILD_SHADOW_RUNTIME;   // child slot
        baseInstance.shadowedInt = INDEP_BASE_SHADOW;      // unrelated base slot
    }

    /** mode 3: putstatic the inherited + shadowed static slots. */
    public static void mutateStatics()
    {
        FieldInheritedBase.sProtected = FieldInheritedBase.STAT_PROT_RUNTIME;
        FieldInheritedBase.sPublic    = FieldInheritedBase.STAT_PUB_RUNTIME;
        FieldInheritedMid.sMid        = FieldInheritedMid.STAT_MID_RUNTIME;
        sShadow                       = STATIC_SHADOW_RUNTIME; // child static slot
    }

    /**
     * mode 4: write the child's NARROW (byte) and WIDE (long) shadow slots via
     * putfield.  The hidden Base copies of the same names are NOT written here,
     * so a base-typed read-back must still see the Base init values — proving
     * child-wins shadowing for non-int widths and that the slots are physically
     * distinct at every width.
     */
    public void mutateShadowWidths()
    {
        this.shadowedByte = CHILD_SHADOW_BYTE_RUNTIME;
        this.shadowedLong = CHILD_SHADOW_LONG_RUNTIME;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldInherited.go && !FieldInherited.done;
            }

            @Override
            public void run()
            {
                switch (FieldInherited.mode)
                {
                    case 1:
                        FieldInherited.instance.mutateInstance();
                        break;
                    case 2:
                        FieldInherited.instance.mutateShadowDistinct();
                        break;
                    case 3:
                        FieldInherited.mutateStatics();
                        break;
                    case 4:
                        FieldInherited.instance.mutateShadowWidths();
                        break;
                    default:
                        break;
                }
                FieldInherited.done = true;
            }
        });
    }
}
