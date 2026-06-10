package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_null_safety feature: field_proxy / object-accessor
 * ROBUSTNESS on degenerate inputs.
 *
 * The native module's job is mostly negative — it proves that the library's
 * field-read / field-lookup surface NEVER crashes and ALWAYS returns the
 * documented fallback when handed garbage (absent names, empty / absurdly long
 * names, a field_proxy built from a null pointer, a wrapper built from a null
 * oop, a deliberately-wrong signature, hundreds of repeated failed lookups).
 *
 * This Java side therefore only needs to provide a small, KNOWN-GOOD surface so
 * the native side can prove two things at once:
 *   (1) the happy path still works  — a real static field (okInt / okStr / ...)
 *       and a real instance field read back their exact baked-in values, and
 *   (2) the degenerate cases are NON-DESTRUCTIVE — after every batch of bogus
 *       calls the SAME real fields still read back correctly (no cache
 *       poisoning, no global state corruption, no clobbered storage).
 *
 * A live `instance` is published so the native side can build an instance
 * wrapper (for the instance-field happy path AND for the null-oop contrast: a
 * static field read through a null-oop wrapper must still succeed via the
 * java.lang.Class mirror, while an instance field read through it must fail
 * gracefully).
 *
 * The probe mutates `okInt` through GENUINE putstatic bytecode so the native
 * side can confirm the valid read path reflects live, post-dispatch JVM state
 * even after all the degenerate calls — i.e. the guards never wedged the field.
 *
 * GC-SETTLE / MIRROR-TENURE (mode 2): the native module's static-field VALUE
 * reads resolve `java.lang.Class mirror + offset` and then do a RAW read of that
 * address (the library's field_proxy::get() memcpy is not GC-safe — the same
 * accepted raw-oop gap deflaked in collection_list).  This class's mirror is a
 * young, relocatable heap oop the FIRST time field_null_safety touches it; a
 * young GC firing between resolve and read leaves the captured address pointing
 * at the pre-relocation copy (stale bytes => a wrong value; an unmapped old page
 * => a fault).  This was invisible on the old CI runner but the windows·msvc
 * 14.51 runner's codegen/timing widened the window enough to corrupt the first
 * few reads (and, ~1/4 of runs, fault the JVM) — while windows·clang and every
 * linux/macos JDK-24/25/26 config stayed green.  Mode 2 allocates a little
 * garbage and calls System.gc() twice so this class's mirror is PROMOTED to the
 * old generation (where G1 will not relocate it) BEFORE the native side reads
 * any static value.  After that the raw reads are stable and every native check
 * can stay a HARD assertion.  Mirrors Warmup.java's runGcSettle().
 *
 * ENCODING NOTE: the one non-ASCII char value uses a \\uXXXX escape (resolved by
 * the Java lexer regardless of source-file encoding), so this fixture compiles
 * identically under javac on Windows (Cp1252) and Linux/macOS (UTF-8); CI
 * invokes javac with no explicit -encoding flag.  Java-8 source level only.
 */
public final class FieldNullSafety
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    /**
     * Action selector; the native side sets it BEFORE raising `go` so one probe
     * cycle drives exactly the work it wants.
     *   1 = mutateOkInt(): genuine putstatic on okInt (the live-state proof).
     *   2 = gcSettle(): allocate garbage + System.gc() x2 so THIS class's
     *       java.lang.Class mirror tenures to old-gen before native value reads
     *       (see the GC-SETTLE / MIRROR-TENURE note in the class doc above).
     * Defaults to 1 so an unset/legacy driver keeps the original behaviour.
     */
    public static volatile int mode = 1;

    /** Allocation iterations for the GC-settle action (mirrors Warmup.java). */
    public static final int GC_GARBAGE_ALLOCS = 32;

    // =====================================================================
    //  KNOWN-GOOD static fields — one of every primitive width plus a String
    //  and an int[] reference, so the native side can prove the happy path of
    //  EACH signature class survives intact next to the degenerate probes.
    //  Values are distinctive so a stale / clobbered read is unmistakable.
    // =====================================================================
    public static boolean okBool  = true;
    public static byte    okByte  = (byte) 0x7B;                 //  123
    public static short   okShort = (short) 0x1234;             //  4660
    public static char    okChar  = 0x00E9;                      // 'e-acute' (>0x7F)
    public static int     okInt   = 1234;                        //  the headline real field
    public static long    okLong  = 0x0123456789ABCDEFL;
    public static float   okFloat = Float.intBitsToFloat(0x3FC00000); // 1.5f
    public static double  okDouble = Double.longBitsToDouble(0x400921FB54442D18L); // Math.PI
    public static String  okStr   = "ok";
    public static int[]   okArr   = new int[] { 10, 20, 30 };

    // A second int used as a "canary": never written by the probe, so the
    // native side can confirm degenerate writes to a null proxy did not spill
    // into an unrelated real field.
    public static int canaryInt   = 0x600DC0DE;

    // =====================================================================
    //  KNOWN-GOOD instance fields — proves the instance-dispatch lookup path
    //  is equally robust, and gives the null-oop contrast its target.
    // =====================================================================
    public boolean iBool = true;
    public int     iInt  = 0x0BADF00D;                          // 195948557
    public long    iLong = 0x7FFFFFFFFFFFFFFFL;                 // Long.MAX_VALUE
    public String  iStr  = "inst";

    // =====================================================================
    //  RUNTIME field written by the probe via genuine putstatic bytecode.
    //  okInt is rewritten to this value so the native side proves the valid
    //  read path reflects live JVM state AFTER all the degenerate calls.
    // =====================================================================
    public static final int RUNTIME_OK_INT = 0x0BEEF99;        // 200044441

    /** Held so the native side can build an instance wrapper. */
    public static FieldNullSafety instance = new FieldNullSafety();

    /** A witness the probe bumps, so the native side has a second liveness signal. */
    private static int probeRuns = 0;

    /** Java-side getter so the native side can cross-check okInt via real bytecode. */
    public static int getOkInt()
    {
        return okInt;
    }

    /** Java-side getter for the canary (proves bytecode sees it unchanged too). */
    public static int getCanaryInt()
    {
        return canaryInt;
    }

    /**
     * Rewrites okInt through a genuine putstatic so the native side reads back
     * live, post-dispatch JVM state.  Returns the count of probe invocations.
     */
    public static int mutateOkInt()
    {
        okInt = RUNTIME_OK_INT;   // putstatic
        probeRuns = probeRuns + 1;
        return probeRuns;
    }

    /**
     * Allocates a little real garbage and calls System.gc() twice so a
     * collection actually happens (even under -XX:+DisableExplicitGC the garbage
     * forces one), promoting this class's live java.lang.Class mirror to the old
     * generation.  Once tenured, G1 will not relocate the mirror during the
     * young GCs that fire while the native module runs, so the native side's raw
     * `mirror + offset` static reads are stable.  See the class-doc note and
     * Warmup.runGcSettle().  Bounded and cheap so it completes well inside the
     * native probe's poll window.
     */
    public static void gcSettle()
    {
        for (int n = 0; n < GC_GARBAGE_ALLOCS; ++n)
        {
            final byte[] junk = new byte[64 * 1024];
            junk[0] = (byte) n;
        }
        System.gc();
        System.gc();
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldNullSafety.go && !FieldNullSafety.done;
            }

            @Override
            public void run()
            {
                switch (FieldNullSafety.mode)
                {
                    case 2:
                        // Tenure this class's mirror BEFORE the native value
                        // reads so the (GC-unsafe) raw reads cannot race a young
                        // GC relocating the mirror oop.
                        FieldNullSafety.gcSettle();
                        break;
                    case 1:
                    default:
                        // Real bytecode dispatch: invokestatic mutateOkInt()
                        // executes a putstatic on okInt that the native side
                        // then reads back through the valid field_proxy::get().
                        FieldNullSafety.mutateOkInt();
                        break;
                }
                FieldNullSafety.done = true;
            }
        });
    }
}
