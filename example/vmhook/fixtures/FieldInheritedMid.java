package vmhook.fixtures;

/**
 * Middle class of the field_inherited hierarchy (area: fields).
 *
 *     FieldInheritedBase   <-  FieldInheritedMid   <-  FieldInherited
 *
 * Deliberately declares NO field whose name collides with a Base or child
 * field.  Its single own instance field (midOwnInt) and own static (sMid) are
 * found by walking exactly ONE Klass::get_super() link up from a FieldInherited
 * instance — the depth-1 case between the depth-0 (own) and depth-2 (grandparent)
 * cases the native module pins.  By contributing no shadowing slot, Mid also
 * guarantees that the Base shadow slots are reachable only by the FULL two-link
 * walk, so a regression that stopped the walk early would surface here.
 *
 * Package-private top-level class (its public-named file matches the type); it
 * carries no Harness.Probe, so Main.loadFixtures() merely loads it.  Java 8.
 */
class FieldInheritedMid extends FieldInheritedBase
{
    static final int  MID_INT_INIT    = 0x00C0FFEE;
    static final int  MID_INT_RUNTIME = 0x77777777;
    static final int  STAT_MID_INIT    = 400;
    static final int  STAT_MID_RUNTIME = 0x7373;

    static final long   MID_LONG_INIT = 0x00FACADE00FACADEL;
    static final String MID_STR_INIT  = "mid-str";

    // Depth-1 own slots of the two narrow primitive types the parent level did
    // not yet cover (boolean Z, byte B), so the single-super-link walk is proven
    // for those widths as well as the I / J / String it already exercises.
    static final boolean MID_BOOL_INIT = true;
    static final byte    MID_BYTE_INIT = (byte) 0x6D;   // 'm'-ish sentinel = 109
    // A WIDE (J) own static inherited via a single super link.
    static final long    STAT_MID_LONG_INIT = 0x00ABCDEF00ABCDEFL;

    // Own instance field — inherited by the child via a single super link.
    public int midOwnInt = MID_INT_INIT;

    // Own WIDE (J) and REFERENCE (String) instance fields — inherited by the
    // child via a single super link too, so the depth-1 walk is proven for a
    // non-int slot AND a compressed-OOP reference, not only the 4-byte int.
    public long   midOwnLong = MID_LONG_INIT;
    public String midOwnStr  = MID_STR_INIT;

    // Own NARROW (Z / B) instance fields — depth-1 inherited by the child.
    public boolean midOwnBool = MID_BOOL_INIT;
    public byte    midOwnByte = MID_BYTE_INIT;

    // Own static — inherited static reached by a single super link.
    public static int sMid = STAT_MID_INIT;

    // Own WIDE (J) static — depth-1 inherited static on the class mirror.
    public static long sMidLong = STAT_MID_LONG_INIT;

    // Touch the new own slots so javac does not warn them unused under -Werror-y
    // builds and they are guaranteed present in the layout.
    long midSum()
    {
        return this.midOwnInt + this.midOwnLong + this.midOwnStr.length()
             + (this.midOwnBool ? 1 : 0) + this.midOwnByte
             + FieldInheritedMid.sMidLong;
    }
}
