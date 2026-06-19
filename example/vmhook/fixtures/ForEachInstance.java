package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the vmhook::for_each_instance&lt;T&gt; feature (area: heap scan).
 *
 * vmhook::for_each_instance&lt;T&gt;() walks the JVM's collected-heap reservation
 * (Universe::_collectedHeap::_reserved) linearly in 4&nbsp;KiB chunks via a safe
 * read, decodes each candidate oop's narrow-klass pointer at +8, and hands the
 * native visitor a freshly-allocated {@code std::unique_ptr<T>} for every header
 * whose klass matches the registered wrapper.  It is a CONSERVATIVE, best-effort
 * raw-memory scan: it is NOT GC-cooperative and runs without a safepoint, so a
 * concurrent GC, a region-based heap (G1) with unmapped pages, or a coloured-
 * pointer collector (ZGC/Shenandoah) can legitimately make it MISS any given
 * object.  Critically, though, every object it DOES report is a real one — it
 * never fabricates a false instance.
 *
 * Design — this class IS the dedicated, count-controlled instance type the
 * native side scans for.  The native module asks the JVM (via the standard
 * go/done probe) to populate {@link #PINNED}, a static array that holds exactly
 * {@link #PIN_COUNT} freshly-allocated ForEachInstance objects strongly
 * reachable for the entire scan, then performs the heap walk.  Because:
 *   - ONLY the probe ever constructs a ForEachInstance, and
 *   - it constructs exactly {@code PIN_COUNT} of them into {@code PINNED},
 * the JVM heap contains at most {@code PIN_COUNT} live instances of this class.
 * That gives the native side two RELIABLE bounds to assert (visited &gt; 0 and
 * visited &le; PIN_COUNT — the scanner can never see more instances than exist,
 * because it reports only real headers) while leaving "found a SPECIFIC pinned
 * instance" / "found ALL of them" as best-effort observations the conservative
 * scan does not guarantee.
 *
 * Each pinned instance carries a unique {@link #id} (0..PIN_COUNT-1) and a
 * constant {@link #marker} sentinel so the native side can, best-effort, read a
 * matched object's fields back through the wrapper and confirm it is genuinely
 * one of ours (marker matches, id in range) rather than merely a klass-pointer
 * coincidence.
 *
 * EXACT-KLASS-VS-SUBCLASS (batch-13).  The scan matches the decoded narrow-klass
 * pointer for EXACT pointer equality against the registered wrapper's klass, so a
 * scan keyed on this base class reports ONLY direct ForEachInstance instances and
 * NEVER a derived {@link Sub} (whose header carries a different klass pointer).
 * To exercise that we also pin a small handful of {@link Sub} instances in
 * {@link #PINNED_SUB}; a base-typed scan must not report them, and a Sub-typed
 * scan reports only Sub headers (each stamped with {@link #SUB_MARKER}).
 *
 * NEVER-INSTANTIATED (batch-13).  {@link Empty} is a dedicated nested type that
 * is loaded (the native side registers and resolves its klass) but NEVER
 * constructed, so its true live-instance count is zero.  A scan keyed on it must
 * complete cleanly without crash/hang and report no genuine Empty instance — the
 * crash-safety-on-a-never-instantiated-class case.
 *
 * No hook is involved: for_each_instance is a pure VMStructs heap read the
 * native module calls straight from its worker thread (exactly like the legacy
 * inline example.cpp test_for_each_instance), so this fixture needs no trigger()
 * and no JavaThread handshake beyond the go/done allocate-and-pin step.
 *
 * Java 8 syntax only (no var / lambda-in-field / records / switch-expr).
 */
public class ForEachInstance
{
    // ── go / done handshake driven by the native module via run_probe ──────────
    /** Native raises this to request the allocate-and-pin action; clears it after. */
    public static volatile boolean go;

    /** The probe action sets this true once the array is fully populated. */
    public static volatile boolean done;

    /** The exact number of live instances the probe pins for the scan. */
    public static final int PIN_COUNT = 100;

    /** A distinctive constant every pinned instance stamps into {@link #marker}. */
    public static final int MARKER = 0xFE10FE10;

    /** How many derived {@link Sub} instances the probe pins (heap-modest handful). */
    public static final int SUB_PIN_COUNT = 4;

    /** Distinctive marker every pinned {@link Sub} stamps; distinct from {@link #MARKER}. */
    public static final int SUB_MARKER = 0x5B5B5B5B;

    /**
     * Strong references that keep every constructed instance alive across the
     * native heap walk.  Populated exactly once by the probe; never cleared while
     * the scan runs, so the GC cannot reclaim (or, on a moving collector, is far
     * less likely to relocate) the objects mid-scan.
     */
    public static volatile ForEachInstance[] PINNED;

    /**
     * Strong references keeping the derived {@link Sub} instances live across the
     * scan.  Holds exactly {@link #SUB_PIN_COUNT} elements once published.  A
     * base-typed scan must never report any of these (different klass pointer); a
     * Sub-typed scan reports only these.
     */
    public static volatile Sub[] PINNED_SUB;

    /** Set true by the probe once PINNED is fully built (native reads it as a sanity flag). */
    public static volatile int pinnedCount;

    /** Set by the probe to the number of {@link Sub} instances actually pinned. */
    public static volatile int pinnedSubCount;

    // ── Instance fields the native side reads back, best-effort, through the wrapper ──
    /** Unique 0..PIN_COUNT-1 index of this instance within {@link #PINNED}. */
    public final int id;

    /** Constant sentinel = {@link #MARKER}; lets native confirm a match is really ours. */
    public final int marker;

    /**
     * Base constructor.  Kept package-private (not private) so the nested
     * {@link Sub} can chain to it while still preventing any code OUTSIDE this
     * fixture from creating a ForEachInstance — the live-instance count therefore
     * equals exactly what the probe pins (the native side's reliable upper bound).
     */
    ForEachInstance(final int id)
    {
        this.id = id;
        this.marker = MARKER;
    }

    /** Internal ctor used by {@link Sub} so its {@link #marker} reads {@link #SUB_MARKER}. */
    ForEachInstance(final int id, final int markerValue)
    {
        this.id = id;
        this.marker = markerValue;
    }

    /**
     * Direct subclass used to exercise the EXACT-klass match.  A Sub header carries
     * a different narrow-klass pointer than a base ForEachInstance, so a base-typed
     * for_each_instance scan must never report a Sub, and a Sub-typed scan reports
     * only Sub instances (each stamped with {@link #SUB_MARKER}).
     */
    public static final class Sub extends ForEachInstance
    {
        /** A field unique to Sub so the wrapper layout differs structurally too. */
        public final int subTag;

        Sub(final int id)
        {
            super(id, SUB_MARKER);
            this.subTag = id ^ SUB_MARKER;
        }
    }

    /**
     * A dedicated, loaded-but-NEVER-instantiated nested type.  The native side
     * registers and resolves its klass, then scans for it: the true live count is
     * zero, so the scan must terminate cleanly and report no genuine Empty.  Its
     * single field exists only to give the type a non-trivial layout.
     */
    public static final class Empty
    {
        public final int unused;

        private Empty()
        {
            this.unused = 0;
        }
    }

    /**
     * Allocate exactly {@link #PIN_COUNT} base instances into {@link #PINNED} plus
     * {@link #SUB_PIN_COUNT} derived {@link Sub} instances into {@link #PINNED_SUB},
     * each with a unique id, and publish them.  {@link Empty} is deliberately never
     * constructed.  Idempotent: a second call with the arrays already built is a
     * no-op (so a re-driven probe never inflates the live count).  Runs on the
     * Harness tick (Java) thread.
     */
    private static void allocateAndPin()
    {
        if (PINNED != null)
        {
            return; // already pinned — keep the live count at exactly PIN_COUNT
        }
        final ForEachInstance[] arr = new ForEachInstance[PIN_COUNT];
        for (int i = 0; i < PIN_COUNT; i++)
        {
            arr[i] = new ForEachInstance(i);
        }
        final Sub[] subs = new Sub[SUB_PIN_COUNT];
        for (int i = 0; i < SUB_PIN_COUNT; i++)
        {
            subs[i] = new Sub(i);
        }
        // Publish last so the volatile writes make every element visible together.
        PINNED = arr;
        PINNED_SUB = subs;
        pinnedCount = PIN_COUNT;
        pinnedSubCount = SUB_PIN_COUNT;
    }

    // ── Probe self-registration ─────────────────────────────────────────────────
    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ForEachInstance.go && !ForEachInstance.done;
            }

            @Override
            public void run()
            {
                allocateAndPin();
                ForEachInstance.done = true;
            }
        });
    }
}
