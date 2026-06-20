package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the watch_static_field feature (area: watchers / hardware
 * debug registers).
 *
 * watch_static_field installs a hardware data breakpoint (one of the CPU's
 * four DR0-DR3 slots) on a Java static field's backing storage.  The trap
 * fires SYNCHRONOUSLY on whichever thread executes the write, *during* the
 * write instruction, and the native callback runs inside a vectored
 * exception handler on that same thread.  vmhook arms the trap on every
 * thread that exists at install time -- which includes THIS Harness loop
 * thread (and the persistent worker thread this fixture starts in its
 * static initializer) -- so a putstatic executed inside run() below traps
 * immediately.
 *
 * The native module:
 *   (a) installs a watch on a watched static field BEFORE raising `go`,
 *   (b) raises `go`; the Harness loop (on the Java thread) runs run(), which
 *       does N genuine putstatic writes to the watched field -- each write
 *       trapping and invoking the native callback synchronously,
 *   (c) polls `done`, then reads back its atomic fire-counters and the last
 *       value the callback observed, asserting the callback saw the field's
 *       NEW value.
 *
 * Because the DR trap is synchronous on the writing thread, every write the
 * module wants observed must happen inside a SINGLE run() invocation (the
 * `done` flag latches).  The module selects which field(s) to drive via the
 * `mode` selector; it sets `mode` and clears `done` on the rising edge of
 * `go`.
 *
 * COVERAGE this fixture enables (one mode per scenario):
 *   * FIVE independent watched int fields (counterA..counterE) for the
 *     four-hardware-slot characterisation (fill 4, refuse 5th, free + re-add)
 *     and the slot<->address independence proofs;
 *   * EVERY DR-watchable width: boolean/byte (1), short/char (2), int/float
 *     (4), long/double (8) and an Object reference (compressed-oop slot, 4),
 *     each written a known monotone sequence so the callback's observed NEW
 *     value is deterministic;
 *   * a SAME-VALUE field written the identical value repeatedly, to let the
 *     module characterise whether an unchanged store still traps;
 *   * a PRE-EXISTING worker thread (started in <clinit>, so it is armed at
 *     install time) that writes a field on demand -> proves the trap fires on
 *     whichever thread issues the store, not just the Harness loop thread;
 *   * a NEWLY-created thread (spawned inside run(), AFTER the watch is armed)
 *     that writes a field -> characterises the documented "threads created
 *     after install are not armed" limitation;
 *   * a lazily-initialised nested class (LazyInit) whose <clinit> writes a
 *     watched field, so the module can prove an armed watch SURVIVES a class
 *     initialisation event happening on the writing thread.
 *
 * Java 8 syntax only (anonymous Probe / Runnable classes; no var / lambda /
 * switch-expr / try-with-resources on non-AutoCloseable).
 */
public final class WatchStaticField
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    /** Scenario selector; native sets it (and clears done) before raising go. */
    public static volatile int mode;

    // =====================================================================
    //  WATCHED int fields (counterA..counterE).  Each is written ONLY by
    //  writeIntField(...) via genuine putstatic, so the native hardware-DR
    //  watchpoint traps on every increment.  They start at 0; the module
    //  resets them through mode 0 before each scenario so fire-counts and
    //  last-observed values are deterministic.
    //
    //  int (4 bytes) is the canonical watched width: it exercises the DR
    //  LEN=four_bytes path and a putstatic is a single aligned 4-byte store.
    // =====================================================================
    public static volatile int counterA;
    public static volatile int counterB;
    public static volatile int counterC;
    public static volatile int counterD;
    public static volatile int counterE;

    // =====================================================================
    //  WIDTH-COVERAGE watched fields -- one per DR-watchable storage width.
    //  Each is written a strictly-increasing 1..WRITE_COUNT sequence (in its
    //  own type) so the callback's observed NEW value ends at a known final.
    //  (Object ref uses a sequence of distinct heap objects; its "value" is a
    //  compressed oop, asserted only as non-zero / changing by the module.)
    // =====================================================================
    public static volatile boolean wBool;    // 1-byte width (boolean storage)
    public static volatile byte   wByte;     // 1-byte width
    public static volatile short  wShort;    // 2-byte width
    public static volatile char   wChar;     // 2-byte width
    public static volatile float  wFloat;    // 4-byte width
    public static volatile long   wLong;     // 8-byte width
    public static volatile double wDouble;   // 8-byte width
    public static volatile Object wRef;      // compressed-oop reference slot

    /** SAME-VALUE field: written the identical value repeatedly (mode 18). */
    public static volatile int sameValueField;
    /** The constant value the SAME-VALUE field is repeatedly written. */
    public static final int SAME_VALUE = 7;

    /**
     * How many writes each "write" mode performs in one run().  The module
     * mirrors this constant and asserts the callback fired exactly this many
     * times (the trap is synchronous and one-per-write).
     */
    public static final int WRITE_COUNT = 12;

    /**
     * The exact value the LAST increment leaves in a driven int/width field,
     * so the native callback's "new value" argument can be asserted precisely.
     * The field is reset to 0 (mode 0) before each scenario, so after
     * WRITE_COUNT unit increments the final value is WRITE_COUNT.
     */
    public static final int FINAL_VALUE = WRITE_COUNT;

    /** Records, per scenario, how many putstatic writes run() actually did. */
    public static volatile int writesMade;

    /** True after loadFixtures()+static-init, so a native readiness check works. */
    public static volatile boolean ready = true;

    // =====================================================================
    //  Cross-thread coordination for the PRE-EXISTING worker thread.  The
    //  worker is started in <clinit> (BEFORE any native watch is installed),
    //  so vmhook arms the DR trap on it at install time.  run() hands it a
    //  command and busy-waits (the worker, not the Harness loop, performs the
    //  putstatic), so the write -- and its synchronous trap -- happen on the
    //  worker thread.
    // =====================================================================
    /** Worker command: 0 = idle; 1 = increment counterA WRITE_COUNT times. */
    private static volatile int     workerCmd;
    /** Set by run() to ask the worker to act; cleared by run() after. */
    private static volatile boolean workerRequest;
    /** Set true by the worker while it is executing a command. */
    private static volatile boolean workerDone;
    /** Latched true once the worker thread has actually started looping. */
    public  static volatile boolean workerAlive;

    // =====================================================================
    //  LazyInit: a nested class whose <clinit> writes a watched static field.
    //  It is NOT referenced by WatchStaticField.<clinit> (and loadFixtures
    //  only forName's top-level non-'$' classes), so it stays UNINITIALISED
    //  until the module/probe touches it -- letting the module watch the
    //  survival of an armed trap across a real class-initialisation event.
    // =====================================================================
    public static final class LazyInit
    {
        /** Written WRITE_COUNT times by this class's <clinit> below. */
        public static volatile int lazyClinitValue;

        static
        {
            // Genuine putstatic writes inside <clinit>, on whatever thread
            // first initialises the class (the Harness loop thread here).
            int v = 0;
            for (int i = 0; i < WRITE_COUNT; i++)
            {
                v = v + 1;
                lazyClinitValue = v;
            }
        }

        /** Touch point that forces <clinit> when first called. */
        public static int touch()
        {
            return lazyClinitValue;
        }

        private LazyInit() { }
    }

    private static void resetCounters()
    {
        counterA = 0;
        counterB = 0;
        counterC = 0;
        counterD = 0;
        counterE = 0;
        wBool = false;
        wByte = 0;
        wShort = 0;
        wChar = 0;
        wFloat = 0.0f;
        wLong = 0L;
        wDouble = 0.0;
        wRef = null;
        sameValueField = 0;
        writesMade = 0;
    }

    /**
     * Increments the selected int field WRITE_COUNT times with a genuine
     * putstatic each iteration (read-modify-write of a volatile int compiles
     * to getstatic/iadd/putstatic, so the store the DR watches really runs).
     * A tiny sleep between writes spaces the traps out and keeps the loop from
     * being optimised into a single store, so each increment is an independent
     * 4-byte store the hardware breakpoint fires on.
     */
    private static void writeIntField(final int which)
    {
        int made = 0;
        for (int i = 0; i < WRITE_COUNT; i++)
        {
            switch (which)
            {
                case 0: counterA = counterA + 1; break;
                case 1: counterB = counterB + 1; break;
                case 2: counterC = counterC + 1; break;
                case 3: counterD = counterD + 1; break;
                case 4: counterE = counterE + 1; break;
                default: break;
            }
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /**
     * Writes wBool WRITE_COUNT times (1-byte boolean storage).  A boolean slot
     * only ever holds 0 or 1, so we toggle true/false each iteration and leave
     * it TRUE on the last store (WRITE_COUNT is even, so the toggle below ends
     * on true).  Each putstatic is an independent 1-byte store the DR watch
     * fires on; the module asserts the final stored value is true (1) and the
     * watch fired, but does NOT assert a monotone numeric sequence (a boolean
     * cannot carry one).
     */
    private static void writeBoolField()
    {
        int made = 0;
        for (int i = 1; i <= WRITE_COUNT; i++)
        {
            wBool = (i % 2 == 0);   // last iteration i==WRITE_COUNT (even) -> true
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /** Writes wByte = 1..WRITE_COUNT (1-byte width). */
    private static void writeByteField()
    {
        int made = 0;
        for (int i = 1; i <= WRITE_COUNT; i++)
        {
            wByte = (byte) i;
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /** Writes wShort = 1..WRITE_COUNT (2-byte width). */
    private static void writeShortField()
    {
        int made = 0;
        for (int i = 1; i <= WRITE_COUNT; i++)
        {
            wShort = (short) i;
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /** Writes wChar = 1..WRITE_COUNT (2-byte width). */
    private static void writeCharField()
    {
        int made = 0;
        for (int i = 1; i <= WRITE_COUNT; i++)
        {
            wChar = (char) i;
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /** Writes wFloat = 1.0..WRITE_COUNT (4-byte width). */
    private static void writeFloatField()
    {
        int made = 0;
        for (int i = 1; i <= WRITE_COUNT; i++)
        {
            wFloat = (float) i;
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /** Writes wLong = 1..WRITE_COUNT (8-byte width). */
    private static void writeLongField()
    {
        int made = 0;
        for (int i = 1; i <= WRITE_COUNT; i++)
        {
            wLong = (long) i;
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /** Writes wDouble = 1.0..WRITE_COUNT (8-byte width). */
    private static void writeDoubleField()
    {
        int made = 0;
        for (int i = 1; i <= WRITE_COUNT; i++)
        {
            wDouble = (double) i;
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /**
     * Writes wRef with WRITE_COUNT DISTINCT new objects (compressed-oop slot).
     * Each store changes the 4-byte narrow-oop in the slot, so the width-4
     * write watch traps once per store.
     */
    private static void writeRefField()
    {
        int made = 0;
        for (int i = 0; i < WRITE_COUNT; i++)
        {
            wRef = new int[] { i };   // a fresh heap object each iteration
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /**
     * Writes the SAME value (SAME_VALUE) WRITE_COUNT times.  Whether an
     * unchanged store still traps is hardware/JIT-dependent; the module only
     * characterises the count (>= 0) and asserts the final value, never a
     * hard fire count.
     */
    private static void writeSameValue()
    {
        int made = 0;
        for (int i = 0; i < WRITE_COUNT; i++)
        {
            sameValueField = SAME_VALUE;
            made++;
            sleep1();
        }
        writesMade = made;
    }

    /**
     * Spawns a BRAND-NEW thread (created here, AFTER the native watch was
     * installed) that increments counterA WRITE_COUNT times, and joins it.
     * Threads created after install are NOT armed by the current library, so
     * the module characterises this as "writes do not fire".
     */
    private static void writeFromNewThread()
    {
        final Thread t = new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                for (int i = 0; i < WRITE_COUNT; i++)
                {
                    counterA = counterA + 1;
                    sleep1();
                }
            }
        }, "wsf-new-thread");
        t.start();
        try { t.join(5000); } catch (final InterruptedException ie) { /* ignore */ }
        writesMade = WRITE_COUNT;
    }

    /**
     * Forces LazyInit.<clinit> (a real class-initialisation event on this
     * thread) and then writes counterA WRITE_COUNT times.  Lets the module
     * prove an armed counterA watch SURVIVES the class init.
     */
    private static void triggerClassInitThenWrite()
    {
        // First touch -> triggers LazyInit.<clinit> exactly once.
        final int seen = LazyInit.touch();
        // Then drive counterA so the module sees its watch still fires.
        int made = 0;
        for (int i = 0; i < WRITE_COUNT; i++)
        {
            counterA = counterA + 1;
            made++;
            sleep1();
        }
        // Park the observed value somewhere harmless so the JIT cannot elide
        // the touch() call.
        if (seen < 0)
        {
            writesMade = -1;
        }
        else
        {
            writesMade = made;
        }
    }

    private static void sleep1()
    {
        try { Thread.sleep(1); } catch (final InterruptedException ie) { /* ignore */ }
    }

    static
    {
        // Start the PRE-EXISTING worker thread FIRST, before registering the
        // probe, so it exists (and is armable) the moment the native module
        // installs a watch.  It is a daemon so it never blocks JVM shutdown.
        final Thread worker = new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                workerAlive = true;
                for (;;)
                {
                    if (workerRequest && !workerDone)
                    {
                        final int cmd = workerCmd;
                        if (cmd == 1)
                        {
                            for (int i = 0; i < WRITE_COUNT; i++)
                            {
                                counterA = counterA + 1;
                                sleep1();
                            }
                            writesMade = WRITE_COUNT;
                        }
                        workerDone = true;
                    }
                    sleep1();
                }
            }
        }, "wsf-worker");
        worker.setDaemon(true);
        worker.start();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return WatchStaticField.go && !WatchStaticField.done;
            }

            @Override
            public void run()
            {
                switch (WatchStaticField.mode)
                {
                    case 0:  resetCounters();            break; // reset only
                    case 1:  writeIntField(0);           break; // drive counterA
                    case 2:  writeIntField(1);           break; // drive counterB
                    case 3:  writeIntField(2);           break; // drive counterC
                    case 4:  writeIntField(3);           break; // drive counterD
                    case 5:  writeIntField(4);           break; // drive counterE

                    case 10: writeByteField();           break; // 1-byte
                    case 11: writeShortField();          break; // 2-byte
                    case 12: writeCharField();           break; // 2-byte
                    case 13: writeFloatField();          break; // 4-byte
                    case 14: writeLongField();           break; // 8-byte
                    case 15: writeDoubleField();         break; // 8-byte
                    case 16: writeBoolField();           break; // 1-byte boolean
                    case 17: writeRefField();            break; // object ref (4)
                    case 18: writeSameValue();           break; // same-value

                    case 20: driveWorkerCounterA();      break; // pre-existing thread
                    case 21: writeFromNewThread();        break; // new thread (post-install)
                    case 22: triggerClassInitThenWrite(); break; // <clinit> survival
                    default: break;
                }
                WatchStaticField.done = true;
            }
        });
    }

    /**
     * Asks the pre-existing worker thread to increment counterA WRITE_COUNT
     * times, and blocks (on the Harness loop thread) until it finishes.  The
     * putstatic -- and its DR trap -- thus happen on the WORKER thread.
     */
    private static void driveWorkerCounterA()
    {
        workerDone = false;
        workerCmd = 1;
        workerRequest = true;
        // Bounded wait so a stuck worker never wedges the Harness loop.
        for (int i = 0; i < 5000; i++)
        {
            if (workerDone)
            {
                break;
            }
            sleep1();
        }
        workerRequest = false;
        workerCmd = 0;
    }
}
