package vmhook.fixtures;

/**
 * Grandparent of the field_inherited hierarchy (area: fields).
 *
 *     FieldInheritedBase   <-  FieldInheritedMid   <-  FieldInherited
 *
 * This class declares the fields that the native field_inherited module reaches
 * by walking TWO Klass::get_super() links up from a FieldInherited instance:
 *   - protected / public / package-private / private INSTANCE fields, so the
 *     test proves vmhook::find_field ignores Java access control (it reads by
 *     raw offset) for every access level,
 *   - protected / public / private STATIC fields, exercising the same super
 *     walk on the java.lang.Class mirror,
 *   - shadow slots (shadowedInt / shadowedStr / sShadow) whose NAMES are
 *     re-declared on the child, so a base-typed read of a child object must see
 *     THESE (grandparent) slots while a child-typed read sees the child's.
 *
 * Package-private top-level class (its public-named file matches the type); it
 * carries no Harness.Probe, so Main.loadFixtures() merely loads it.  Java 8.
 */
class FieldInheritedBase implements FieldInheritedIface
{
    // ---- Init + runtime constants the child's mutators write ---------------
    static final int PROT_INT_INIT    = 1337;        // canonical inherited value
    static final int PROT_INT_RUNTIME = 0xABCD;
    static final int PUB_INT_INIT     = 2674;
    static final int PUB_INT_RUNTIME  = 0x1234;
    static final int PKG_INT_INIT     = 0x0BADCAFE;
    static final int PRV_INT_INIT     = 0x0DEFACED;

    static final int  STAT_PROT_INIT    = 100;
    static final int  STAT_PROT_RUNTIME = 0x5151;
    static final int  STAT_PUB_INIT     = 200;
    static final int  STAT_PUB_RUNTIME  = 0x6262;
    static final int  STAT_PRV_INIT     = 300;

    // ---- INSTANCE fields at every access level (find_field ignores access) -
    protected int    protectedInt = PROT_INT_INIT;   // inherited PROTECTED
    public    int    publicInt    = PUB_INT_INIT;    // inherited PUBLIC
              int    packageInt   = PKG_INT_INIT;    // inherited PACKAGE-PRIVATE
    private   int    privateInt   = PRV_INT_INIT;    // inherited PRIVATE (base-only)

    // A representative non-int inherited primitive + an inherited reference,
    // so the super walk is covered for a wide primitive AND a compressed-OOP
    // reference decode (not just I-typed slots).
    protected long   baseLong     = 0x00BA5E_0000BA5EL;
    public    String baseStr      = "base-str";

    // ---- One inherited field of EVERY remaining JVM primitive type, plus an
    //      array reference, so the depth-2 super walk + field_proxy::get() are
    //      proven for Z B C S F D and the array-OOP decode path -- not only the
    //      I / J / Ljava/lang/String; signatures the rest of the fixture covers.
    //      Init values are mirrored verbatim on the native side.
    public    boolean baseBool   = true;                 // "Z"
    public    byte    baseByte   = (byte) 0x5A;          // "B"  (= 90)
    public    char    baseChar   = 'Q';                  // "C"  (= 0x0051 = 81)
    public    short   baseShort  = (short) 0x1234;       // "S"  (= 4660)
    public    float   baseFloat  = 2.5f;                 // "F"  (exact in IEEE-754)
    public    double  baseDouble = 1.5d;                 // "D"  (exact in IEEE-754)
    public    int[]   baseIntArray = { 11, 22, 33 };     // "[I" (inherited array ref)

    // ---- BOUNDARY / edge-valued inherited slots (depth-2 super walk) --------
    // Min/max + sign-boundary values for every fixed-width primitive, so the
    // depth-2 walk's offset read is proven to honour the FULL bit pattern (no
    // truncation, no spurious sign extension) for each width.  Mirrored verbatim
    // on the native side.  char is the only UNSIGNED 16-bit slot, so 0xFFFF must
    // read back as 65535, never -1.
    public    int     baseIntMin   = Integer.MIN_VALUE;  // 0x80000000
    public    int     baseIntMax   = Integer.MAX_VALUE;  // 0x7FFFFFFF
    public    int     baseIntNeg   = -1;                 // 0xFFFFFFFF (all-ones)
    public    long    baseLongMin  = Long.MIN_VALUE;     // 0x8000000000000000
    public    long    baseLongMax  = Long.MAX_VALUE;     // 0x7FFFFFFFFFFFFFFF
    public    byte    baseByteMin  = Byte.MIN_VALUE;     // -128
    public    byte    baseByteMax  = Byte.MAX_VALUE;     // 127
    public    byte    baseByteNeg  = (byte) -1;          // 0xFF -> -1 (signed B)
    public    short   baseShortMin = Short.MIN_VALUE;    // -32768
    public    short   baseShortMax = Short.MAX_VALUE;    // 32767
    public    char    baseCharMax  = (char) 0xFFFF;      // 65535 (UNSIGNED)
    public    char    baseCharZero = (char) 0;           // 0
    public    float   baseFloatNeg = -123.5f;            // exact, signed
    public    double  baseDoubleNeg= -987.625d;          // exact, signed
    public    float   baseFloatZero= 0.0f;               // +0.0
    public    double  baseDoubleBig= 1.0e300d;           // exact-ish large magnitude

    // Inherited REFERENCE ARRAY of String ("[Ljava/lang/String;"), so the
    // depth-2 walk is proven for an OBJECT-array OOP decode (not only "[I").
    public    String[] baseStrArray = { "alpha", "beta", "gamma" };

    // Inherited reference array of int[][] is overkill for heap; a single inner
    // String[] suffices to exercise the object-array element decode.

    // ---- STATIC fields at every access level -------------------------------
    protected static int sProtected = STAT_PROT_INIT;
    public    static int sPublic    = STAT_PUB_INIT;
    private   static int sPrivate   = STAT_PRV_INIT;

    // ---- Shadow slots: the child RE-DECLARES these same names --------------
    public  int    shadowedInt = 1111;      // FieldInherited.BASE_SHADOW_INT
    public  String shadowedStr = "base";    // base copy of the shadowed String
    public  static int sShadow  = 555;      // FieldInherited.STATIC_SHADOW_BASE

    // Additional shadow slots of OTHER widths (byte / long), re-declared on the
    // child too, so child-wins shadowing is proven for a NARROW and a WIDE
    // primitive — not just the 4-byte int.  Base copies here; child copies live
    // on FieldInherited with far-apart sentinel values.
    public  byte   shadowedByte = (byte) 0x11;  // FieldInherited.BASE_SHADOW_BYTE
    public  long   shadowedLong = 0x00BA5EL;    // FieldInherited.BASE_SHADOW_LONG

    // Touch private members + the every-type slots so javac does not warn them
    // unused under -Werror-y builds and so they are guaranteed present in the
    // layout.  Also reads the interface constant so its declaration is exercised.
    int sumPrivates()
    {
        int acc = this.privateInt + FieldInheritedBase.sPrivate + IFACE_CONST;
        acc += (this.baseBool ? 1 : 0) + this.baseByte + this.baseChar + this.baseShort;
        acc += (int) this.baseFloat + (int) this.baseDouble;
        acc += this.baseIntArray.length;
        // Touch the boundary + reference-array + extra-shadow slots so javac does
        // not warn them unused under -Werror-y builds and they are guaranteed
        // present in the layout.
        acc += this.baseIntMin + this.baseIntMax + this.baseIntNeg;
        acc += (int) (this.baseLongMin + this.baseLongMax);
        acc += this.baseByteMin + this.baseByteMax + this.baseByteNeg;
        acc += this.baseShortMin + this.baseShortMax;
        acc += this.baseCharMax + this.baseCharZero;
        acc += (int) (this.baseFloatNeg + this.baseDoubleNeg + this.baseFloatZero);
        acc += (int) this.baseDoubleBig;
        acc += this.baseStrArray.length;
        acc += this.shadowedByte + (int) this.shadowedLong;
        return acc;
    }
}
