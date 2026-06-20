package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_primitives_set feature: the SET mirror of
 * FieldPrimitivesGet.  It exercises field_proxy::set() (vmhook.hpp ~12051-12194)
 * for EVERY JVM primitive descriptor (Z B S C I J F D) at regular AND boundary
 * values, written by the NATIVE side and then OBSERVED by genuine Java bytecode.
 *
 * Division of labour (so this overlaps no sibling module):
 *   - FieldPrimitivesGet owns the GET decode paths (native reads class-init
 *     constants + runtime putstatic/putfield writes back through get()).
 *   - FieldSetGuard owns the SIZE / TYPE guard, the spatial anti-clobber proof,
 *     and the wrong-kind / non-primitive rejection characterisation.
 *   - THIS fixture is the SET VALUE-MATRIX authority: for every primitive width
 *     it lets the native side WRITE every boundary value (0, 1, -1, min, max,
 *     and for F/D the +/-0.0, +/-Inf, canonical/signaling/payload NaN, denormal,
 *     MIN_NORMAL bit patterns; for C the BMP / high-bit / surrogate code units;
 *     for B/S the sign-boundary bytes) into BOTH a static slot and an instance
 *     slot, then proves -- through real getstatic/getfield bytecode executed on
 *     the Java thread -- that the JVM ITSELF sees exactly the native-written
 *     value.  This is the part a pure C++ memory peek cannot prove.
 *
 * HOW "Java saw the native write" is proven:
 *   Every settable field NAME has a parallel `seen<Name>` witness field.  The
 *   probe's run() (mode 1) copies each field into its witness via genuine
 *   getstatic/getfield + putstatic, capturing F/D as RAW bits through
 *   Float.floatToRawIntBits / Double.doubleToRawLongBits (so a NaN payload or
 *   the signaling-NaN bit is preserved verbatim, never canonicalised by the
 *   `==` path).  The native side, AFTER the probe completes, reads the witness
 *   fields back and asserts they equal what it wrote.  A compact boolean[]
 *   `eq` witness ALSO records, per field, whether the snapshot equalled the
 *   exact native target -- a second, independent confirmation channel.
 *
 * SENTINELS: every field is pre-initialised in the class initializer to a value
 * the native side will NEVER write, so a no-op set() (e.g. silently refused)
 * is caught as "field still holds its sentinel".
 *
 * ENCODING NOTE: every char value uses a numeric / \\uXXXX form (lexer-level),
 * so this fixture compiles identically under javac on Windows (Cp1252) and
 * Linux/macOS (UTF-8); the CI invokes javac without an explicit -encoding flag.
 *
 * JAVA 8 SOURCE ONLY: anonymous Probe class; no var / records / switch-expr /
 * text blocks / List.of / Stream.toList / any post-8 java.* API.
 *
 * INHERITANCE: this class extends FieldPrimitivesSetBase (declared at the foot
 * of this file).  The base owns one INHERITED settable field per primitive
 * width, both static (bsZ..bsD) and instance (biZ..biD); the native side writes
 * them through the SUBCLASS wrapper (static_field / get_field on
 * FieldPrimitivesSet), exercising field_proxy::set()'s superclass-chain offset
 * resolution -- a write into a field the wrapper class does NOT itself declare.
 * Each inherited field is pre-set to its own sentinel so a refused / no-op write
 * is detectable, and the base exposes getters + snapshot witnesses so the JVM's
 * OWN bytecode confirms the inherited write landed.
 *
 * VOLATILE: a parallel set of `volatile` primitive fields (vsZ..vsD static,
 * viZ..viD instance) proves field_proxy::set() writes the same plain slot for a
 * volatile field -- ACC_VOLATILE is a JMM access-ordering attribute, not a
 * storage-layout one, so a raw memcpy lands identically and every subsequent
 * Java getstatic/getfield observes it.
 */
public final class FieldPrimitivesSet extends FieldPrimitivesSetBase
{
    // -- go / done handshake + scenario selector driven by run_probe --------
    public static volatile boolean go;
    public static volatile boolean done;
    public static volatile int     mode;

    // =====================================================================
    //  SENTINEL constants -- the value each field holds before any native
    //  write.  Distinct from every value the native side will write, so a
    //  refused / no-op set() leaves a detectable fingerprint.  Declared HERE,
    //  ahead of the volatile/inherited fields below, so their initializers can
    //  reference them by simple name without an illegal forward reference.
    // =====================================================================
    public static final boolean SENT_Z   = false;          // native always writes from a real bool
    public static final byte    SENT_B   = (byte) 0x5A;     // 90
    public static final short   SENT_S   = (short) 0x5AA5;  // 23205
    public static final char    SENT_C   = 0x5A5A;          // arbitrary BMP unit
    public static final int     SENT_I   = 0x5A5A5A5A;
    public static final long    SENT_J   = 0x5A5A5A5A5A5A5A5AL;
    public static final int     SENT_F_BITS = 0x5A5A5A5A;   // a quiet, finite float pattern
    public static final long    SENT_D_BITS = 0x5A5A5A5A5A5A5A5AL;

    // =====================================================================
    //  VOLATILE settable fields, one per primitive, pre-set to the sentinel.
    //  ACC_VOLATILE only constrains JMM access ordering, never the in-heap slot
    //  layout, so field_proxy::set()'s raw memcpy must land identically to a
    //  plain field and every later getstatic/getfield must observe it.
    // =====================================================================
    public static volatile boolean vsZ = SENT_Z;
    public static volatile byte    vsB = SENT_B;
    public static volatile short   vsS = SENT_S;
    public static volatile char    vsC = SENT_C;
    public static volatile int     vsI = SENT_I;
    public static volatile long    vsJ = SENT_J;
    public static volatile float   vsF = Float.intBitsToFloat(SENT_F_BITS);
    public static volatile double  vsD = Double.longBitsToDouble(SENT_D_BITS);

    public volatile boolean viZ = SENT_Z;
    public volatile byte    viB = SENT_B;
    public volatile short   viS = SENT_S;
    public volatile char    viC = SENT_C;
    public volatile int     viI = SENT_I;
    public volatile long    viJ = SENT_J;
    public volatile float   viF = Float.intBitsToFloat(SENT_F_BITS);
    public volatile double  viD = Double.longBitsToDouble(SENT_D_BITS);

    // Witnesses for the VOLATILE-field write (mode 1 snapshot), F/D as raw bits.
    public static int  seenVsCBits;   // vsC as plain int (unsigned)
    public static long seenVsJ;
    public static int  seenVsFBits;
    public static long seenVsDBits;
    public static int  seenViCBits;
    public static long seenViJ;
    public static int  seenViFBits;
    public static long seenViDBits;

    // VOLATILE getters (Java's own bytecode reads each volatile field).
    public static boolean getVsZ()     { return vsZ; }
    public static byte    getVsB()     { return vsB; }
    public static short   getVsS()     { return vsS; }
    public static int     getVsC()     { return vsC; }           // char -> unsigned int
    public static int     getVsI()     { return vsI; }
    public static long    getVsJ()     { return vsJ; }
    public static int     getVsFBits() { return Float.floatToRawIntBits(vsF); }
    public static long    getVsDBits() { return Double.doubleToRawLongBits(vsD); }
    public static boolean getViZ()     { return instance.viZ; }
    public static byte    getViB()     { return instance.viB; }
    public static short   getViS()     { return instance.viS; }
    public static int     getViC()     { return instance.viC; }  // char -> unsigned int
    public static int     getViI()     { return instance.viI; }
    public static long    getViJ()     { return instance.viJ; }
    public static int     getViFBits() { return Float.floatToRawIntBits(instance.viF); }
    public static long    getViDBits() { return Double.doubleToRawLongBits(instance.viD); }

    // INHERITED getters: Java bytecode reads the base-declared field through the
    // subclass instance (genuine getfield / getstatic, super-chain offset).
    public static int  getBsCBits() { return bsC; }              // inherited static char
    public static long getBsJ()     { return bsJ; }              // inherited static long
    public static int  getBsFBits() { return Float.floatToRawIntBits(bsF); }
    public static long getBsDBits() { return Double.doubleToRawLongBits(bsD); }
    public static boolean getBiZ()  { return instance.biZ; }     // inherited instance bool
    public static byte    getBiB()  { return instance.biB; }
    public static short   getBiS()  { return instance.biS; }
    public static int     getBiC()  { return instance.biC; }     // char -> unsigned int
    public static int     getBiI()  { return instance.biI; }
    public static long    getBiJ()  { return instance.biJ; }
    public static int     getBiFBits() { return Float.floatToRawIntBits(instance.biF); }
    public static long    getBiDBits() { return Double.doubleToRawLongBits(instance.biD); }

    // =====================================================================
    //  SENTINEL constants (SENT_Z .. SENT_D_BITS) are declared ABOVE, ahead of
    //  the volatile/inherited fields, so those initializers can reference them
    //  without an illegal forward reference.
    // =====================================================================

    // =====================================================================
    //  STATIC settable fields, one per primitive, pre-set to the sentinel.
    //  The native side writes a matrix of boundary values into THESE across
    //  several phases; the LAST value written before the snapshot is what the
    //  witness records.
    // =====================================================================
    public static boolean sZ = SENT_Z;
    public static byte    sB = SENT_B;
    public static short   sS = SENT_S;
    public static char    sC = SENT_C;
    public static int     sI = SENT_I;
    public static long    sJ = SENT_J;
    public static float   sF = Float.intBitsToFloat(SENT_F_BITS);
    public static double  sD = Double.longBitsToDouble(SENT_D_BITS);

    // =====================================================================
    //  INSTANCE settable fields, mirror of the static set.
    // =====================================================================
    public boolean iZ = SENT_Z;
    public byte    iB = SENT_B;
    public short   iS = SENT_S;
    public char    iC = SENT_C;
    public int     iI = SENT_I;
    public long    iJ = SENT_J;
    public float   iF = Float.intBitsToFloat(SENT_F_BITS);
    public double  iD = Double.longBitsToDouble(SENT_D_BITS);

    // =====================================================================
    //  ANTI-CLOBBER neighbours.  For each width, a settable target sits
    //  between two same-width sentinels.  The native side writes ONLY the
    //  middle; the witness proves both neighbours kept their declared value.
    //  (Spatial proof via raw_address() lives in the C++ module; the Java
    //  witness is the "JVM-observed" confirmation channel.)
    // =====================================================================
    public int  clobBefore = 0x11111111;
    public int  clobMid    = 0x22222222;   // native writes this one
    public int  clobAfter  = 0x33333333;

    public long clobBeforeJ = 0x1111111111111111L;
    public long clobMidJ     = 0x2222222222222222L; // native writes this one
    public long clobAfterJ   = 0x3333333333333333L;

    // =====================================================================
    //  FINAL instance fields, one per primitive.  `final` is purely a Java
    //  language / verifier constraint (ACC_FINAL); it changes NOTHING about
    //  the in-heap storage layout -- the slot is a plain offset like any other.
    //  The native side writes these through field_proxy::set() (raw memory) to
    //  CHARACTERISE that direct memory access bypasses the final guarantee the
    //  JVM enforces on putfield bytecode.  Each is pre-set to a sentinel so a
    //  successful native write is detectable.  These are deliberately NOT
    //  blank-final + constructor-assigned (which the JIT may constant-fold);
    //  giving them a non-constant initializer via the helper keeps every read
    //  a genuine getfield so the snapshot/getter observes the native write.
    // =====================================================================
    public final boolean finZ = idBool(SENT_Z);
    public final byte    finB = idByte(SENT_B);
    public final short   finS = idShort(SENT_S);
    public final char    finC = idChar(SENT_C);
    public final int     finI = idInt(SENT_I);
    public final long    finJ = idLong(SENT_J);
    public final float   finF = idFloat(Float.intBitsToFloat(SENT_F_BITS));
    public final double  finD = idDouble(Double.longBitsToDouble(SENT_D_BITS));

    // Identity helpers: a non-constant initializer for the final fields above,
    // so javac cannot inline the constant and every Java read is a real getfield
    // (the native write must be visible to executing bytecode, not folded away).
    private static boolean idBool(boolean v)  { return v; }
    private static byte    idByte(byte v)     { return v; }
    private static short   idShort(short v)   { return v; }
    private static char    idChar(char v)     { return v; }
    private static int     idInt(int v)       { return v; }
    private static long    idLong(long v)     { return v; }
    private static float   idFloat(float v)   { return v; }
    private static double  idDouble(double v) { return v; }

    // =====================================================================
    //  SEQUENTIAL-WRITE instance fields at DISTINCT offsets.  The native side
    //  writes seqA then seqB; both must be independently observable (a write to
    //  one never disturbs the other), proving distinct-offset writes do not
    //  cross-clobber and that the second write does not undo the first.
    // =====================================================================
    public int seqA = 0x5A5A0001;
    public int seqB = 0x5A5A0002;

    // =====================================================================
    //  WITNESS fields -- the snapshot the probe (mode 1) writes via genuine
    //  getstatic/getfield + putstatic.  F/D captured as RAW bits.
    // =====================================================================
    public static boolean seenSZ;
    public static byte    seenSB;
    public static short   seenSS;
    public static char    seenSC;
    public static int     seenSI;
    public static long    seenSJ;
    public static int     seenSFBits;   // Float.floatToRawIntBits(sF)
    public static long    seenSDBits;   // Double.doubleToRawLongBits(sD)

    public static boolean seenIZ;
    public static byte    seenIB;
    public static short   seenIS;
    public static char    seenIC;
    public static int     seenII;
    public static long    seenIJ;
    public static int     seenIFBits;
    public static long    seenIDBits;

    public static int  seenClobBefore;
    public static int  seenClobMid;
    public static int  seenClobAfter;
    public static long seenClobBeforeJ;
    public static long seenClobMidJ;
    public static long seenClobAfterJ;

    // Witnesses for the FINAL-field write characterisation (mode 1 snapshot).
    public static boolean seenFinZ;
    public static byte    seenFinB;
    public static short   seenFinS;
    public static char    seenFinC;
    public static int     seenFinI;
    public static long    seenFinJ;
    public static int     seenFinFBits;
    public static long    seenFinDBits;

    // Witnesses for the SEQUENTIAL-write instance fields (mode 1 snapshot).
    public static int seenSeqA;
    public static int seenSeqB;

    // =====================================================================
    //  EXACT-EQUALITY witness array.  The native side, before driving the
    //  snapshot, programs the EXPECTED value of every field into the
    //  `expected*` slots below; the probe (mode 2) then compares each live
    //  field against its expected value and records the boolean result into
    //  `eq[i]`.  This is a SECOND, Java-evaluated confirmation that the field
    //  equals the native target -- independent of the native re-read of the
    //  raw witness fields.  Indices are documented by EQ_* constants.
    // =====================================================================
    public static final int EQ_COUNT = 18;
    public static final boolean[] eq = new boolean[EQ_COUNT];

    public static final int EQ_SZ = 0;
    public static final int EQ_SB = 1;
    public static final int EQ_SS = 2;
    public static final int EQ_SC = 3;
    public static final int EQ_SI = 4;
    public static final int EQ_SJ = 5;
    public static final int EQ_SF = 6;   // raw-bits compare
    public static final int EQ_SD = 7;   // raw-bits compare
    public static final int EQ_IZ = 8;
    public static final int EQ_IB = 9;
    public static final int EQ_IS = 10;
    public static final int EQ_IC = 11;
    public static final int EQ_II = 12;
    public static final int EQ_IJ = 13;
    public static final int EQ_IF = 14;  // raw-bits compare
    public static final int EQ_ID = 15;  // raw-bits compare
    public static final int EQ_CLOB_NEIGHBOURS   = 16; // both int neighbours intact
    public static final int EQ_CLOB_NEIGHBOURS_J = 17; // both long neighbours intact

    // The native-programmed expected values for the mode-2 Java compare.
    public static boolean expSZ;
    public static byte    expSB;
    public static short   expSS;
    public static char    expSC;
    public static int     expSI;
    public static long    expSJ;
    public static int     expSFBits;
    public static long    expSDBits;
    public static boolean expIZ;
    public static byte    expIB;
    public static short   expIS;
    public static char    expIC;
    public static int     expII;
    public static long    expIJ;
    public static int     expIFBits;
    public static long    expIDBits;

    /** Held so the native side can build an instance wrapper for instance sets. */
    public static FieldPrimitivesSet instance = new FieldPrimitivesSet();

    // =====================================================================
    //  Java GETTERS (static_method readback path).  Java's OWN bytecode reads
    //  each field and returns it, so the native side can also pull the value
    //  through static_method("...")->call() -- a third confirmation that the
    //  write is visible to executing Java code, not just to a memory peek.
    //  char getters return int (widened unsigned) so the native uint16 compare
    //  is exact; F/D getters return raw bits so a NaN payload is preserved.
    // =====================================================================
    public static boolean getSZ()       { return sZ; }
    public static byte    getSB()       { return sB; }
    public static short   getSS()       { return sS; }
    public static int     getSC()       { return sC; }            // char -> unsigned int
    public static int     getSI()       { return sI; }
    public static long    getSJ()       { return sJ; }
    public static int     getSFBits()   { return Float.floatToRawIntBits(sF); }
    public static long    getSDBits()   { return Double.doubleToRawLongBits(sD); }

    public static boolean getIZ()       { return instance.iZ; }
    public static byte    getIB()       { return instance.iB; }
    public static short   getIS()       { return instance.iS; }
    public static int     getIC()       { return instance.iC; }
    public static int     getII()       { return instance.iI; }
    public static long    getIJ()       { return instance.iJ; }
    public static int     getIFBits()   { return Float.floatToRawIntBits(instance.iF); }
    public static long    getIDBits()   { return Double.doubleToRawLongBits(instance.iD); }

    public static int  getClobBefore()  { return instance.clobBefore; }
    public static int  getClobMid()     { return instance.clobMid; }
    public static int  getClobAfter()   { return instance.clobAfter; }
    public static long getClobBeforeJ() { return instance.clobBeforeJ; }
    public static long getClobMidJ()    { return instance.clobMidJ; }
    public static long getClobAfterJ()  { return instance.clobAfterJ; }

    // FINAL-field getters: each is a genuine getfield through the held instance,
    // so the value returned is exactly what executing bytecode reads AFTER the
    // native raw-memory write (the final-write characterisation).  F/D as raw bits.
    public static boolean getFinZ()     { return instance.finZ; }
    public static byte    getFinB()     { return instance.finB; }
    public static short   getFinS()     { return instance.finS; }
    public static int     getFinC()     { return instance.finC; }            // char -> unsigned int
    public static int     getFinI()     { return instance.finI; }
    public static long    getFinJ()     { return instance.finJ; }
    public static int     getFinFBits() { return Float.floatToRawIntBits(instance.finF); }
    public static long    getFinDBits() { return Double.doubleToRawLongBits(instance.finD); }

    // SEQUENTIAL-write getters (distinct-offset instance fields).
    public static int getSeqA() { return instance.seqA; }
    public static int getSeqB() { return instance.seqB; }

    // Float/double value-class predicates evaluated by JAVA bytecode (so the
    // native side can assert isNaN / isInfinite / signbit survived a real
    // getstatic, independent of any raw-bits canonicalisation concern).
    public static boolean sFIsNaN()        { return Float.isNaN(sF); }
    public static boolean sFIsPosInf()     { return Float.isInfinite(sF) && sF > 0.0f; }
    public static boolean sFIsNegInf()     { return Float.isInfinite(sF) && sF < 0.0f; }
    public static boolean sDIsNaN()        { return Double.isNaN(sD); }
    public static boolean sDIsPosInf()     { return Double.isInfinite(sD) && sD > 0.0; }
    public static boolean sDIsNegInf()     { return Double.isInfinite(sD) && sD < 0.0; }
    // -0.0 detection: equals 0 but raw bits carry the sign bit.
    public static boolean sFIsNegZero()    { return sF == 0.0f && Float.floatToRawIntBits(sF) == 0x80000000; }
    public static boolean sDIsNegZero()    { return sD == 0.0 && Double.doubleToRawLongBits(sD) == 0x8000000000000000L; }

    /**
     * Snapshots every settable field into its `seen*` witness using genuine
     * getstatic / getfield + putstatic bytecode.  F/D are captured as RAW bits
     * so a signaling-NaN bit or NaN payload survives unchanged.  This is what
     * the native side reads back to prove the JVM observed the native writes.
     */
    public void snapshotAll()
    {
        // static slots
        seenSZ     = sZ;
        seenSB     = sB;
        seenSS     = sS;
        seenSC     = sC;
        seenSI     = sI;
        seenSJ     = sJ;
        seenSFBits = Float.floatToRawIntBits(sF);
        seenSDBits = Double.doubleToRawLongBits(sD);

        // instance slots (genuine getfield through the held instance)
        seenIZ     = instance.iZ;
        seenIB     = instance.iB;
        seenIS     = instance.iS;
        seenIC     = instance.iC;
        seenII     = instance.iI;
        seenIJ     = instance.iJ;
        seenIFBits = Float.floatToRawIntBits(instance.iF);
        seenIDBits = Double.doubleToRawLongBits(instance.iD);

        // anti-clobber neighbours
        seenClobBefore  = instance.clobBefore;
        seenClobMid     = instance.clobMid;
        seenClobAfter   = instance.clobAfter;
        seenClobBeforeJ = instance.clobBeforeJ;
        seenClobMidJ    = instance.clobMidJ;
        seenClobAfterJ  = instance.clobAfterJ;

        // final-field characterisation witnesses (genuine getfield on finalfields)
        seenFinZ     = instance.finZ;
        seenFinB     = instance.finB;
        seenFinS     = instance.finS;
        seenFinC     = instance.finC;
        seenFinI     = instance.finI;
        seenFinJ     = instance.finJ;
        seenFinFBits = Float.floatToRawIntBits(instance.finF);
        seenFinDBits = Double.doubleToRawLongBits(instance.finD);

        // sequential-write witnesses
        seenSeqA = instance.seqA;
        seenSeqB = instance.seqB;

        // volatile witnesses (genuine getstatic / getfield on volatile slots)
        seenVsCBits = vsC;
        seenVsJ     = vsJ;
        seenVsFBits = Float.floatToRawIntBits(vsF);
        seenVsDBits = Double.doubleToRawLongBits(vsD);
        seenViCBits = instance.viC;
        seenViJ     = instance.viJ;
        seenViFBits = Float.floatToRawIntBits(instance.viF);
        seenViDBits = Double.doubleToRawLongBits(instance.viD);

        // inherited witnesses (super-chain getstatic / getfield through the
        // held subclass instance, which IS-A FieldPrimitivesSetBase).
        instance.snapshotInherited(instance);
    }

    /**
     * Compares every live field against the native-programmed `exp*` value and
     * records the boolean result into eq[].  F/D compared via RAW bits so a
     * NaN payload counts as equal only when bit-identical.  Evaluated entirely
     * in Java bytecode -- an independent confirmation channel.
     */
    public void compareAll()
    {
        eq[EQ_SZ] = (sZ == expSZ);
        eq[EQ_SB] = (sB == expSB);
        eq[EQ_SS] = (sS == expSS);
        eq[EQ_SC] = (sC == expSC);
        eq[EQ_SI] = (sI == expSI);
        eq[EQ_SJ] = (sJ == expSJ);
        eq[EQ_SF] = (Float.floatToRawIntBits(sF) == expSFBits);
        eq[EQ_SD] = (Double.doubleToRawLongBits(sD) == expSDBits);

        eq[EQ_IZ] = (instance.iZ == expIZ);
        eq[EQ_IB] = (instance.iB == expIB);
        eq[EQ_IS] = (instance.iS == expIS);
        eq[EQ_IC] = (instance.iC == expIC);
        eq[EQ_II] = (instance.iI == expII);
        eq[EQ_IJ] = (instance.iJ == expIJ);
        eq[EQ_IF] = (Float.floatToRawIntBits(instance.iF) == expIFBits);
        eq[EQ_ID] = (Double.doubleToRawLongBits(instance.iD) == expIDBits);

        eq[EQ_CLOB_NEIGHBOURS] =
                (instance.clobBefore == 0x11111111) && (instance.clobAfter == 0x33333333);
        eq[EQ_CLOB_NEIGHBOURS_J] =
                (instance.clobBeforeJ == 0x1111111111111111L) && (instance.clobAfterJ == 0x3333333333333333L);
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldPrimitivesSet.go && !FieldPrimitivesSet.done;
            }

            @Override
            public void run()
            {
                // Real bytecode dispatch.  The native side selects the action
                // via `mode` on the rising edge of go:
                //   mode 1 -> snapshot every field into its seen* witness.
                //   mode 2 -> compare every field against the native expected.
                // Both paths execute genuine getstatic/getfield bytecode, so the
                // values they read are exactly what the JVM observed for the
                // native field_proxy::set() writes.
                final int m = FieldPrimitivesSet.mode;
                if (m == 2)
                {
                    FieldPrimitivesSet.instance.compareAll();
                }
                else
                {
                    FieldPrimitivesSet.instance.snapshotAll();
                }
                FieldPrimitivesSet.done = true;
            }
        });
    }
}

/**
 * Superclass of FieldPrimitivesSet.  Declares one INHERITED settable field per
 * primitive width, both static (bs*) and instance (bi*).  The native side writes
 * these through the SUBCLASS wrapper, so field_proxy::set() must resolve the
 * field offset by walking the superclass chain.  Each field carries its OWN
 * sentinel (distinct base pattern 0xB5) so a refused / no-op write into an
 * inherited slot is detectable, and a snapshot copies each into a witness so the
 * JVM's own getstatic/getfield confirms the inherited write landed.
 *
 * Package-private (not public) so it may share FieldPrimitivesSet.java; Java 8
 * source only.
 */
class FieldPrimitivesSetBase
{
    // Base sentinels -- distinct from the subclass SENT_* patterns so an
    // inherited write is unambiguous (0xB5 vs 0x5A nibble swaps).
    static final byte  BSENT_B = (byte) 0xB5;
    static final short BSENT_S = (short) 0xB5B5;
    static final char  BSENT_C = 0xB5B5;
    static final int   BSENT_I = 0xB5B5B5B5;
    static final long  BSENT_J = 0xB5B5B5B5B5B5B5B5L;
    static final int   BSENT_F_BITS = 0x3FB504F3; // ~sqrt(2): a quiet finite float
    static final long  BSENT_D_BITS = 0x3FF6A09E667F3BCDL;

    // Inherited STATIC settable fields.
    public static boolean bsZ = false;
    public static byte    bsB = BSENT_B;
    public static short   bsS = BSENT_S;
    public static char    bsC = BSENT_C;
    public static int     bsI = BSENT_I;
    public static long    bsJ = BSENT_J;
    public static float   bsF = Float.intBitsToFloat(BSENT_F_BITS);
    public static double  bsD = Double.longBitsToDouble(BSENT_D_BITS);

    // Inherited INSTANCE settable fields.
    public boolean biZ = false;
    public byte    biB = BSENT_B;
    public short   biS = BSENT_S;
    public char    biC = BSENT_C;
    public int     biI = BSENT_I;
    public long    biJ = BSENT_J;
    public float   biF = Float.intBitsToFloat(BSENT_F_BITS);
    public double  biD = Double.longBitsToDouble(BSENT_D_BITS);

    // Witnesses for the INHERITED-field write (mode 1 snapshot); F/D raw bits.
    public static boolean seenBsZ;
    public static byte    seenBsB;
    public static short   seenBsS;
    public static int     seenBsCBits;
    public static int     seenBsI;
    public static long    seenBsJ;
    public static int     seenBsFBits;
    public static long    seenBsDBits;
    public static boolean seenBiZ;
    public static byte    seenBiB;
    public static short   seenBiS;
    public static int     seenBiCBits;
    public static int     seenBiI;
    public static long    seenBiJ;
    public static int     seenBiFBits;
    public static long    seenBiDBits;

    /**
     * Snapshots every inherited field into its witness via genuine getstatic /
     * getfield bytecode (invoked from the subclass probe through the held
     * instance, so the getfield reads the inherited slot of a real subclass
     * object).  F/D captured as raw bits.
     */
    void snapshotInherited(FieldPrimitivesSetBase self)
    {
        seenBsZ     = bsZ;
        seenBsB     = bsB;
        seenBsS     = bsS;
        seenBsCBits = bsC;
        seenBsI     = bsI;
        seenBsJ     = bsJ;
        seenBsFBits = Float.floatToRawIntBits(bsF);
        seenBsDBits = Double.doubleToRawLongBits(bsD);

        seenBiZ     = self.biZ;
        seenBiB     = self.biB;
        seenBiS     = self.biS;
        seenBiCBits = self.biC;
        seenBiI     = self.biI;
        seenBiJ     = self.biJ;
        seenBiFBits = Float.floatToRawIntBits(self.biF);
        seenBiDBits = Double.doubleToRawLongBits(self.biD);
    }
}
