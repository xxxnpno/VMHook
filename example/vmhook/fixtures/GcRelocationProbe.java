package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the GC RELOCATION DETECTOR (area: gc / jni.global_ref).
 *
 * vmhook::vm_capabilities() reports the collector, the barrier shape,
 * UseCompressedOops and UseCompactObjectHeaders; vmhook::gc_epoch() samples
 * CollectedHeap::_total_collections paired with the gc-active flag; and
 * vmhook::jni::global_ref records that epoch at construction so oop() returns
 * null -- reporting is_stale() -- once a collection has happened, instead of
 * handing back an address a relocating collector may have invalidated.
 *
 * The native module vmhook/tests/jvm/modules/gc_relocation_detector.cpp proves
 * the detector fires on a REAL relocation.  global_ref is explicitly NOT a GC
 * root, so the object under test must be kept alive from JAVA -- that is what
 * {@link #subject} is for.  Because the static field is a real root, the
 * collector UPDATES it when it moves the object, so re-reading its oop after a
 * forced collection is an independent, native-side-verifiable witness that the
 * object physically moved.  Without that witness a passing test would prove
 * nothing (see docs/research/build_and_validation_manual.md 5.3).
 *
 * mode selector (native sets `mode` + clears `done` on the rising edge of go):
 *   1 = arm.  Fill eden with garbage that immediately dies, THEN publish a
 *       fresh subject into the static field.  A subsequent full collection has
 *       to evacuate/compact it out of eden, which is what makes the address
 *       change observable.
 *   2 = collect.  Allocate real churn and force System.gc() twice.  CI runs the
 *       target JVM with -Xmx4g -Xmn3g, so a 3 GB young generation absorbs
 *       incidental allocation and a NATURAL young evacuation is unlikely; the
 *       explicit full collection is what does the moving here.
 *
 * Java 8 syntax only (anonymous Probe class; no var/lambda/switch-expr).
 * ASCII only -- javac on JDK 8 / Windows reads these sources as Cp1252.
 */
public final class GcRelocationProbe
{
    // -- go / done handshake driven by the native module via run_probe --------
    /** Native raises this to request the probe action; clears it afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector; native sets it on the rising edge of go (1 or 2). */
    public static volatile int mode;

    /** How many times mode 2 has driven System.gc(). */
    public static volatile int gcRounds;

    /** How many times mode 1 has published a fresh subject. */
    public static volatile int armRounds;

    /**
     * The object under test, held ONLY by this static field.  global_ref is a
     * detector, not a root, so this reference is what keeps the object alive
     * across the collection -- and, because the collector updates a real root,
     * re-reading this field's oop natively after the collection reveals the
     * object's NEW address.
     */
    public static volatile Object subject;

    /**
     * Two ADJACENT reference (oop) instance fields.  Their offsets differ by
     * the in-heap oop slot width: 4 with -XX:+UseCompressedOops, 8 without.
     * The native module reads both offsets out of the klass metadata as an
     * independent cross-check of vm_capabilities().compressed_oops -- a
     * measurement of the VM rather than a second reading of the same flag.
     */
    public Object refA;

    /** @see #refA */
    public Object refB;

    /** Sentinel payload, so a relocated object can be proven still intact. */
    public int sentinel;

    /** ()V -- leaves sentinel at 0. */
    public GcRelocationProbe()
    {
    }

    /** (I)V -- stamps the sentinel the native side reads back. */
    public GcRelocationProbe(final int value)
    {
        this.sentinel = value;
    }

    /** The value mode 1 stamps into every published subject. */
    public static final int SENTINEL = 0x5A5A;

    /**
     * Allocates garbage that is dead the instant it returns.  Kept in its own
     * method so the JIT cannot trivially scalar-replace the whole loop.
     */
    private static int burn(final int rounds, final int bytes)
    {
        int witness = 0;
        for (int round = 0; round < rounds; round++)
        {
            final byte[] garbage = new byte[bytes];
            garbage[0] = (byte) round;
            garbage[garbage.length - 1] = (byte) round;
            witness += garbage[0] + garbage[garbage.length - 1];
        }
        return witness;
    }

    // -- Probe self-registration ---------------------------------------------
    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return GcRelocationProbe.go && !GcRelocationProbe.done;
            }

            @Override
            public void run()
            {
                if (GcRelocationProbe.mode == 1)
                {
                    // Dirty eden FIRST so the fresh subject lands after a band
                    // of soon-to-be-dead objects; the next full collection then
                    // has to move it.
                    if (burn(128, 64 * 1024) == Integer.MIN_VALUE)
                    {
                        throw new IllegalStateException("unreachable");
                    }
                    GcRelocationProbe.subject = new GcRelocationProbe(SENTINEL);
                    GcRelocationProbe.armRounds++;
                }
                else if (GcRelocationProbe.mode == 2)
                {
                    // Real churn, then two explicit full collections.  A forced
                    // System.gc() is only a hint, so the native side never
                    // assumes it ran -- it checks the epoch counter.
                    if (burn(256, 64 * 1024) == Integer.MIN_VALUE)
                    {
                        throw new IllegalStateException("unreachable");
                    }
                    System.gc();
                    System.gc();
                    GcRelocationProbe.gcRounds++;
                }
                GcRelocationProbe.done = true;
            }
        });
    }
}
