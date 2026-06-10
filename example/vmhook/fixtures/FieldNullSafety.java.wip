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
                // Real bytecode dispatch: invokestatic mutateOkInt() which
                // executes a putstatic on okInt that the native side then reads
                // back through the valid field_proxy::get() path.
                FieldNullSafety.mutateOkInt();
                FieldNullSafety.done = true;
            }
        });
    }
}
