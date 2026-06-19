package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_set_size_guard feature (area: fields).
 *
 * Focus: the SIZE / TYPE GUARD on field_proxy::set() (vmhook.hpp ~11956-12091,
 * audit/findings/field_proxy_set_size_guard.md) and -- the part no sibling
 * module proves -- that a write to field X NEVER clobbers an adjacent field Y.
 *
 * Division of labour vs the sibling field modules:
 *   - field_static is the SET + Java-readback + GCC-portability authority for
 *     correct-width writes; it touches the guard but only on STATIC mirror slots
 *     and never proves spatial non-interference between two real fields.
 *   - field_primitives_get / field_object_ref own the GET decode paths.
 *   - THIS fixture is the GUARD + ADJACENCY (anti-clobber) authority:
 *       (a) one field of EVERY primitive width (Z B C S I J F D) plus a
 *           reference field, written natively at the correct width (round-trip),
 *       (b) for each primitive, a MISTYPED native write (too-wide / too-narrow /
 *           wrong-kind / non-primitive) whose REFUSAL or behaviour the module
 *           characterises, and -- the headline --
 *       (c) ADJACENCY PAIRS: each guard field is declared immediately before a
 *           same-width "guardAfter" sentinel so the native side can prove (via
 *           raw_address() deltas) the two are neighbours in the object layout,
 *           then show a too-wide write to the first leaves the sentinel intact
 *           (an unguarded 8-byte write into a 4-byte slot would have smashed it).
 *
 * The witness machinery mirrors FieldStatic: the probe (mode 1) snapshots every
 * field into a parallel "seen*" field using GENUINE getfield/getstatic +
 * putstatic bytecode, so the native side reads back what the JVM ITSELF observed
 * for each native write -- not merely what a C++ memory peek reports.  A second
 * Java view (mode 2 getters) returns the live values so the module can also pull
 * them through static_method("...")->call().
 *
 * ADJACENCY DESIGN (instance fields):
 *   HotSpot groups same-width fields together and lays them out in (broadly)
 *   declaration order within a width class.  We declare each clobber target
 *   immediately followed by a same-type sentinel:
 *       int  clobI; int  clobIAfter;        long clobJ; long clobJAfter; ...
 *   The native side reads raw_address() of both halves of a pair; when they are
 *   exactly `width` bytes apart it asserts the STRONG anti-clobber invariant
 *   (mistyped write to clobX leaves clobXAfter byte-for-byte unchanged).  If a
 *   given JDK/GC lays them non-adjacently, the module degrades to an [INFO] note
 *   and still asserts each field individually retained its value -- it never
 *   fails on a layout it cannot control, and never crashes the JVM.
 *
 * PORTABILITY / ENCODING:
 *   - All char values are numeric / \\uXXXX (lexer-level), so javac agrees on
 *     Cp1252 (Windows) and UTF-8 (Linux/macOS).
 *   - The single String field is ASCII and is only read, never written through
 *     the in-place backing path, so no interned-literal hazard applies here.
 *   - Java 8 syntax only (anonymous Probe class; no var / lambdas / switch-expr).
 */
public final class FieldSetGuard
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    /** Scenario selector; native sets it before raising go. */
    public static volatile int mode;

    // =====================================================================
    //  CORRECT-WIDTH SET TARGETS -- one field of EVERY primitive width plus a
    //  reference field.  The native side writes each at its matching width and
    //  proves the value round-trips (and, via the seen* snapshot, that Java
    //  observed it).  Initial values are deliberately DISTINCT from what native
    //  writes so a silently-dropped write is caught.
    //
    //  These are STATIC so the native side reaches them from a static C++
    //  wrapper method (the GCC-portable static_field path), matching FieldStatic.
    // =====================================================================
    public static boolean okZ = false;        // native writes true
    public static byte    okB = 0;            // native writes (byte)0x7E (126)
    public static char    okC = 0x0000;       // native writes 0xBEEF
    public static short   okS = 0;            // native writes (short)0x7EEF
    public static int     okI = 0;            // native writes 0x0BADF00D
    public static long    okJ = 0L;           // native writes 0x0123456789ABCDEF
    public static float   okF = 0.0f;         // native writes a known bit pattern (1.5f)
    public static double  okD = 0.0;          // native writes a known bit pattern (PI)

    /** A read-only String the native side reads but never writes. */
    public static String  okStr = "guard";

    // =====================================================================
    //  BOUNDARY / EXTREME correct-width round-trip targets.  Each gets a
    //  correct-width native write of an extreme bit pattern (min / max /
    //  all-ones / NaN / Inf / -0.0 / denormal); the native side proves the value
    //  round-trips natively and (via the bnd* snapshot) that Java observed the
    //  exact bits -- a wide correct-width memcpy must preserve every bit, not just
    //  the low ones, at the numeric extremes where a sloppy write is most likely
    //  to drop a bit.  Initial values are 0 / distinct so a dropped write shows.
    // =====================================================================
    public static boolean bndZ = false;   // native writes false then true
    public static byte    bndB = 0;       // native writes (byte)0x80 = -128 (min)
    public static short   bndS = 0;       // native writes (short)0x8000 = -32768 (min)
    public static char    bndC = 0;       // native writes 0xFFFF (max char)
    public static int     bndI = 0;       // native writes 0x80000000 (Integer.MIN_VALUE)
    public static long    bndJ = 0L;      // native writes 0x8000000000000000 (Long.MIN_VALUE)
    public static float   bndF = 0.0f;    // native writes a float NaN bit pattern
    public static double  bndD = 0.0;     // native writes a double -0.0 bit pattern

    // =====================================================================
    //  TYPE-CONFUSION (same-width, wrong KIND) targets.  field_proxy::set's
    //  guard is a *size* guard only: set(float) into an "I" field passes (both
    //  4 bytes) and writes the IEEE-754 bit pattern; set(int32) into an "F"
    //  field likewise.  The native side performs these and CHARACTERISES the
    //  actual (bit-reinterpreting) behaviour -- not a bug this test can fix, so
    //  it records [INFO] and asserts the ACTUAL bytes that land.
    // =====================================================================
    public static int     confI = 0x00000000; // native writes (int-bits of) 1.5f via set(float)
    public static float   confF = 0.0f;        // native writes float-from-bits via set(int32 0x40490FDB)
    public static long    confJ = 0L;          // native writes (long-bits of) PI via set(double)
    public static double  confD = 0.0;         // native writes double-from-bits via set(int64)

    // =====================================================================
    //  GUARD targets: MISTYPED writes must be REFUSED, value left UNCHANGED.
    //   - gWideI : native set(int64) into "I" -> too wide  -> refused.
    //   - gWideB : native set(int64) into "B" -> too wide  -> refused.
    //   - gWideS : native set(int32) into "S" -> too wide  -> refused.
    //   - gNarrowJ: native set(int32) into "J" -> too narrow -> refused.
    //   - gNarrowD: native set(int32) into "D" -> too narrow -> refused.
    //   - gStrI  : native set(std::string) into "I" -> non-primitive -> refused.
    //   - gVecB  : native set(std::vector<int>) into "B" -> non-primitive -> refused.
    //   - gRefI  : native set(unique_ptr<wrapper>) into "I" -> non-primitive -> refused.
    //   - gCharByte: native set(C++ char) into "C" -> 1->2 widening shortcut lands full char.
    // =====================================================================
    public static int     gWideI    = 0x11223344;          // must remain unchanged
    public static byte    gWideB    = 0x5A;                 // must remain unchanged (90)
    public static short   gWideS    = 0x1234;              // must remain unchanged
    public static long    gNarrowJ  = 0x1122334455667788L;  // must remain unchanged
    public static double  gNarrowD  = Double.longBitsToDouble(0x3FF0000000000000L); // +1.0, unchanged
    public static int     gStrI     = 0x0BADBEEF;          // must remain unchanged
    public static byte    gVecB     = 0x33;                 // must remain unchanged (51)
    public static int     gRefI     = 0x600DC0DE;          // must remain unchanged
    public static char    gCharByte = 0x0000;               // native writes 'Z'(0x5A) via 1-byte char

    // =====================================================================
    //  ADJACENCY PAIRS (INSTANCE fields).  Each clobber target is declared
    //  immediately before a SAME-WIDTH sentinel.  The native side proves via
    //  raw_address() that the two are neighbours, then shows a mistyped (refused)
    //  write to the first leaves the sentinel intact.  Two same-width sentinels
    //  per target (Before/After) bracket it so a guard regression that writes one
    //  byte too far in EITHER direction is caught.
    //
    //  Distinct, non-zero sentinel values so any clobber is unmistakable.
    // =====================================================================
    // byte trio (1-byte width)
    public byte clobBBefore = 0x71;
    public byte clobB       = 0x11;
    public byte clobBAfter  = 0x72;

    // short trio (2-byte width)
    public short clobSBefore = 0x7AAA;
    public short clobS       = 0x1111;
    public short clobSAfter  = 0x7BBB;

    // int trio (4-byte width) -- the canonical "set(int64) smashes the neighbour"
    // case: an unguarded 8-byte write into clobI's 4-byte slot would overwrite
    // clobIAfter's 4 bytes.
    public int clobIBefore = 0x7AAA_AAAA;
    public int clobI       = 0x1111_1111;
    public int clobIAfter  = 0x7BBB_BBBB;

    // long trio (8-byte width)
    public long clobJBefore = 0x7AAAAAAAAAAAAAAAL;
    public long clobJ       = 0x1111111111111111L;
    public long clobJAfter  = 0x7BBBBBBBBBBBBBBBL;

    // char trio (2-byte width) -- proves the "C" 1->2 widening writes EXACTLY two
    // bytes and not three/one (sentinels on both sides).
    public char clobCBefore = 0x7AAA;
    public char clobC       = 0x1111;
    public char clobCAfter  = 0x7BBB;

    // =====================================================================
    //  NON-PRIMITIVE anti-clobber trio (4-byte int width).  The width-mismatch
    //  trios above prove a too-WIDE primitive write does not spill.  This trio
    //  proves the SYMMETRIC guard (string / vector / unique_ptr into a primitive)
    //  is ALSO anti-clobber: a refused non-primitive write into npClobI -- which
    //  in the unguarded legacy path would have reinterpreted the int bytes as a
    //  compressed OOP and written through a wild address -- leaves BOTH sentinels
    //  (and the target) byte-for-byte intact.  Distinct from the clobI trio so the
    //  two probes never perturb each other.
    // =====================================================================
    public int npClobIBefore = 0x6CCC_CCCC;
    public int npClobI       = 0x6DDD_DDDD;
    public int npClobIAfter  = 0x6EEE_EEEE;

    // A live instance the native side wraps to reach the clob* instance fields.
    public static FieldSetGuard instance = new FieldSetGuard();

    // Two distinct published instances for the reference-field round-trip.
    public static FieldSetGuard refA = new FieldSetGuard();
    public static FieldSetGuard refB = new FieldSetGuard();

    /** A settable object-reference static field: native rewrites refSlot A->B->null. */
    public static FieldSetGuard refSlot = refA;

    /** Tag so the two published instances are distinguishable when read back. */
    public int tag = 0;

    // =====================================================================
    //  WITNESS fields ("seen*") -- the probe (mode 1) copies each target into
    //  its witness using genuine getfield/getstatic + putstatic, so native reads
    //  back the JVM's own view.  Float/double captured as raw bits.
    // =====================================================================
    public static boolean seenOkZ;
    public static byte    seenOkB;
    public static char    seenOkC;
    public static short   seenOkS;
    public static int     seenOkI;
    public static long    seenOkJ;
    public static int     seenOkFBits;   // Float.floatToRawIntBits(okF)
    public static long    seenOkDBits;   // Double.doubleToRawLongBits(okD)

    public static int     seenConfIBits;   // confI as an int (already bits)
    public static int     seenConfFBits;   // Float.floatToRawIntBits(confF)
    public static long    seenConfJBits;   // confJ as a long (already bits)
    public static long    seenConfDBits;   // Double.doubleToRawLongBits(confD)

    public static int     seenGWideI;
    public static byte    seenGWideB;
    public static short   seenGWideS;
    public static long    seenGNarrowJ;
    public static long    seenGNarrowDBits;  // Double.doubleToRawLongBits(gNarrowD)
    public static int     seenGStrI;
    public static byte    seenGVecB;
    public static int     seenGRefI;
    public static char    seenGCharByte;

    // Adjacency sentinels, as seen by Java after the native writes.
    public static byte    seenClobBBefore;
    public static byte    seenClobB;
    public static byte    seenClobBAfter;
    public static short   seenClobSBefore;
    public static short   seenClobS;
    public static short   seenClobSAfter;
    public static int     seenClobIBefore;
    public static int     seenClobI;
    public static int     seenClobIAfter;
    public static long    seenClobJBefore;
    public static long    seenClobJ;
    public static long    seenClobJAfter;
    public static char    seenClobCBefore;
    public static char    seenClobC;
    public static char    seenClobCAfter;

    public static boolean seenRefSlotIsA;
    public static boolean seenRefSlotIsB;
    public static boolean seenRefSlotIsNull;
    public static int     seenRefSlotTag;

    // Boundary round-trip witnesses (float/double captured as raw bits).
    public static boolean bndSeenZ;
    public static byte    bndSeenB;
    public static short   bndSeenS;
    public static char    bndSeenC;
    public static int     bndSeenI;
    public static long    bndSeenJ;
    public static int     bndSeenFBits;   // Float.floatToRawIntBits(bndF)
    public static long    bndSeenDBits;   // Double.doubleToRawLongBits(bndD)

    // Non-primitive anti-clobber witnesses (instance trio snapshot).
    public static int     seenNpClobIBefore;
    public static int     seenNpClobI;
    public static int     seenNpClobIAfter;

    // ---- Java getters (pulled natively via static_method to prove the write is
    //      visible to executing Java bytecode, not just a memory peek) ----
    public static boolean getOkZ()  { return okZ; }
    public static int     getOkI()  { return okI; }
    public static long    getOkJ()  { return okJ; }
    public static int     getOkC()  { return okC; }       // char widened unsigned to int
    public static int     getGWideI()   { return gWideI; }
    public static long    getGNarrowJ() { return gNarrowJ; }
    public static int     getGStrI()    { return gStrI; }
    public static int     getGRefI()    { return gRefI; }
    public static int     getGCharByte(){ return gCharByte; }
    public static int     getRefSlotTag(){ return refSlot == null ? -1 : refSlot.tag; }
    public static boolean refSlotIsB()  { return refSlot == refB; }
    public static boolean refSlotIsNull(){ return refSlot == null; }
    public static int     getBndI()     { return bndI; }      // Integer.MIN_VALUE after native write
    public static long    getBndJ()     { return bndJ; }      // Long.MIN_VALUE after native write
    public static int     getBndC()     { return bndC; }      // 0xFFFF widened unsigned to int

    /**
     * Hookable instance method (an interpreter-hook anchor, parallel to
     * FieldStatic.touch).  Returns delta unchanged.  Not strictly required by the
     * current checks but kept so a future detour-based getter readback has a real
     * bytecode dispatch to ride on.
     */
    public int touch(final int delta)
    {
        return delta;
    }

    private static void snapshot()
    {
        // ---- correct-width targets ----
        seenOkZ = okZ;
        seenOkB = okB;
        seenOkC = okC;
        seenOkS = okS;
        seenOkI = okI;
        seenOkJ = okJ;
        seenOkFBits = Float.floatToRawIntBits(okF);
        seenOkDBits = Double.doubleToRawLongBits(okD);

        // ---- type-confusion targets (raw bit views) ----
        seenConfIBits = confI;
        seenConfFBits = Float.floatToRawIntBits(confF);
        seenConfJBits = confJ;
        seenConfDBits = Double.doubleToRawLongBits(confD);

        // ---- guard targets (must be unchanged) ----
        seenGWideI = gWideI;
        seenGWideB = gWideB;
        seenGWideS = gWideS;
        seenGNarrowJ = gNarrowJ;
        seenGNarrowDBits = Double.doubleToRawLongBits(gNarrowD);
        seenGStrI = gStrI;
        seenGVecB = gVecB;
        seenGRefI = gRefI;
        seenGCharByte = gCharByte;

        // ---- adjacency sentinels (instance fields) ----
        final FieldSetGuard s = instance;
        seenClobBBefore = s.clobBBefore;
        seenClobB       = s.clobB;
        seenClobBAfter  = s.clobBAfter;
        seenClobSBefore = s.clobSBefore;
        seenClobS       = s.clobS;
        seenClobSAfter  = s.clobSAfter;
        seenClobIBefore = s.clobIBefore;
        seenClobI       = s.clobI;
        seenClobIAfter  = s.clobIAfter;
        seenClobJBefore = s.clobJBefore;
        seenClobJ       = s.clobJ;
        seenClobJAfter  = s.clobJAfter;
        seenClobCBefore = s.clobCBefore;
        seenClobC       = s.clobC;
        seenClobCAfter  = s.clobCAfter;

        // ---- reference identity ----
        seenRefSlotIsA = (refSlot == refA);
        seenRefSlotIsB = (refSlot == refB);
        seenRefSlotIsNull = (refSlot == null);
        seenRefSlotTag = (refSlot == null) ? -1 : refSlot.tag;

        // ---- boundary round-trip targets (raw bit views for FP) ----
        bndSeenZ = bndZ;
        bndSeenB = bndB;
        bndSeenS = bndS;
        bndSeenC = bndC;
        bndSeenI = bndI;
        bndSeenJ = bndJ;
        bndSeenFBits = Float.floatToRawIntBits(bndF);
        bndSeenDBits = Double.doubleToRawLongBits(bndD);

        // ---- non-primitive anti-clobber trio (must all be unchanged) ----
        seenNpClobIBefore = s.npClobIBefore;
        seenNpClobI       = s.npClobI;
        seenNpClobIAfter  = s.npClobIAfter;
    }

    static
    {
        instance.tag = 0x1;
        refA.tag = 0xA;
        refB.tag = 0xB;

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldSetGuard.go && !FieldSetGuard.done;
            }

            @Override
            public void run()
            {
                switch (FieldSetGuard.mode)
                {
                    case 1:
                        snapshot();
                        break;
                    case 2:
                        // Real bytecode dispatch so the native interpreter hook on
                        // touch() fires (anchor for any detour-based readback).
                        instance.touch(0x5A);
                        break;
                    default:
                        break;
                }
                FieldSetGuard.done = true;
            }
        });
    }
}
